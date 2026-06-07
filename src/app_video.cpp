#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#pragma warning(push)
#pragma warning(disable: 4819 4566)
#include <GL/gl3w.h>
#include <GLFW/glfw3.h>
#pragma warning(pop)

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "app.h"

namespace baktsiu
{

bool refPxToPixel(Vec2f refPx, Vec2f refSz, Vec2f actualSz, bool flipH, bool flipV, Vec2f& outPx);
}

}  // namespace baktsiu

namespace
{

using namespace baktsiu;

void formatVideoHms(double secIn, char* out, size_t outSize)
{
    if (outSize == 0) {
        return;
    }
    if (secIn < 0.0) {
        secIn = 0.0;
    }
    const double capped = std::min(secIn, static_cast<double>(LLONG_MAX / 16));
    const long long t = static_cast<long long>(std::floor(capped + 0.0005));
    const long long h = t / 3600LL;
    const long long m = (t % 3600LL) / 60LL;
    const long long s = t % 60LL;
    std::snprintf(out, outSize, "%lld:%02lld:%02lld", h, m, s);
}

constexpr double kVideoScrubUnknownMaxSec = 3600.0 * 24.0;
constexpr double kVideoForwardKeyframeSeekMinSec = 0.12;
constexpr double kVideoForwardKeyframeSeekMaxSec = 1.35;
constexpr double kVideoPlaybackWallDtCapSec = 0.25;
constexpr int kVideoMaxCompareStepsPerTick = 8;

#if defined(BAKTSIU_DEBUG_BUILD)
inline bool videoPipelineDbgEnabled(bool userFlag)
{
    return userFlag;
}
#else
inline bool videoPipelineDbgEnabled(bool)
{
    return false;
}
#endif

double videoScrubSpanSec(const VideoSource* r)
{
    if (!r || !r->isOpen()) {
        return 0.0;
    }
    const double d = r->durationSec();
    if (r->hasReliableDuration() && d > 0.001) {
        return d;
    }
    return kVideoScrubUnknownMaxSec;
}

double mediaTimeFromComposition(double T, double startS, double durationSec)
{
    double t = T - startS;
    if (durationSec > 1e-6) {
        if (t < 0.0) {
            t = 0.0;
        } else if (t > durationSec) {
            t = durationSec;
        }
    } else {
        t = 0.0;
    }
    return t;
}

double mediaSyncEpsilonSec(const VideoSource* r)
{
    if (!r || !r->isOpen()) {
        return 1e-4;
    }
    double fd = r->frameDurationSec();
    if (fd < 1e-6 || fd > 1.0) {
        fd = 1.0 / 30.0;
    }
    return (std::max)(1e-4, fd * 0.25);
}

double saneFrameDurSec(const VideoSource* r)
{
    double fd = (r && r->isOpen()) ? r->frameDurationSec() : 0.0;
    if (fd < 1e-6 || fd > 1.0) {
        fd = 1.0 / 30.0;
    }
    return fd;
}

struct VideoScrubDecodeHint {
    bool run = false;
    int maxReadCapPerStream = 0;
};

VideoScrubDecodeHint videoScrubDecodeHint(bool itemActive, bool itemActivated, bool itemClicked, bool valueChanged,
    bool itemDeactivated, double imguiTimeSec, double& lastDecodeTimeSec)
{
    VideoScrubDecodeHint out;
    constexpr double kMinIntervalSec = 1.0 / 20.0;
    if (itemDeactivated || itemActivated || itemClicked) {
        lastDecodeTimeSec = imguiTimeSec;
        out.run = true;
        out.maxReadCapPerStream = 0;
        return out;
    }
    if (itemActive && valueChanged) {
        if (imguiTimeSec - lastDecodeTimeSec >= kMinIntervalSec) {
            lastDecodeTimeSec = imguiTimeSec;
            out.run = true;
            out.maxReadCapPerStream = 0;
            return out;
        }
    }
    return out;
}

#if defined(USE_VIDEO)
constexpr float kVideoTransportBarHeightCompare = 60.0f;
constexpr float kVideoTransportBarHeightSingle = 36.0f;

bool videoContainPixelFromLocal(baktsiu::Vec2f localBL, baktsiu::Vec2f panelPx, baktsiu::Vec2f vidSize, baktsiu::Vec2f& outPx)
{
    using namespace baktsiu;
    const float vw = (std::max)(vidSize.x, 1.f);
    const float vh = (std::max)(vidSize.y, 1.f);
    const float pw = (std::max)(panelPx.x, 1.f);
    const float ph = (std::max)(panelPx.y, 1.f);
    const float s = (std::min)(pw / vw, ph / vh);
    const Vec2f disp(vw * s / pw, vh * s / ph);
    const Vec2f off((1.f - disp.x) * 0.5f, (1.f - disp.y) * 0.5f);
    const Vec2f vidUv((localBL.x - off.x) / (std::max)(disp.x, 1e-5f),
        (localBL.y - off.y) / (std::max)(disp.y, 1e-5f));
    if (vidUv.x < -1e-4f || vidUv.x > 1.f + 1e-4f || vidUv.y < -1e-4f || vidUv.y > 1.f + 1e-4f) {
        return false;
    }
    outPx.x = std::floor(vidUv.x * vw);
    outPx.y = std::floor(vidUv.y * vh);
    outPx.x = glm::clamp(outPx.x, 0.f, (std::max)(vw - 1.f, 0.f));
    outPx.y = glm::clamp(outPx.y, 0.f, (std::max)(vh - 1.f, 0.f));
    return true;
}
#endif  // USE_VIDEO

std::string normalizeVideoPath(std::string p)
{
    std::replace(p.begin(), p.end(), '\\', '/');
    return p;
}

}  // namespace

namespace baktsiu
{

VideoSource* App::videoSource()
{
    if (mTopImageIndex < 0 || static_cast<size_t>(mTopImageIndex) >= mMediaList.size()) {
        return nullptr;
    }
    auto* m = mMediaList[mTopImageIndex].get();
    return (m && m->isVideo()) ? static_cast<VideoSource*>(m) : nullptr;
}

const VideoSource* App::videoSource() const
{
    if (mTopImageIndex < 0 || static_cast<size_t>(mTopImageIndex) >= mMediaList.size()) {
        return nullptr;
    }
    const auto* m = mMediaList[mTopImageIndex].get();
    return (m && m->isVideo()) ? static_cast<const VideoSource*>(m) : nullptr;
}

VideoSource* App::videoSourceB()
{
    if (mCmpImageIndex < 0 || static_cast<size_t>(mCmpImageIndex) >= mMediaList.size()) {
        return nullptr;
    }
    auto* m = mMediaList[mCmpImageIndex].get();
    return (m && m->isVideo()) ? static_cast<VideoSource*>(m) : nullptr;
}

const VideoSource* App::videoSourceB() const
{
    if (mCmpImageIndex < 0 || static_cast<size_t>(mCmpImageIndex) >= mMediaList.size()) {
        return nullptr;
    }
    const auto* m = mMediaList[mCmpImageIndex].get();
    return (m && m->isVideo()) ? static_cast<const VideoSource*>(m) : nullptr;
}

bool App::isVideoMode() const
{
    const VideoSource* vs = videoSource();
    return vs != nullptr && vs->isOpen();
}

bool App::videoCompareActive() const
{
#if !defined(USE_VIDEO)
    return false;
#else
    const VideoSource* rB = videoSourceB();
    return rB != nullptr && rB->isOpen();
#endif
}

#if defined(USE_VIDEO)
Vec2f App::videoRefDisplaySizeForLayout() const
{
    const VideoSource* r = videoSource();
    const VideoSource* rB = videoSourceB();
    if (!r || !r->isOpen()) {
        return Vec2f(1.0f, 1.0f);
    }
    const bool cmp = videoCompareActive();
    if (!cmp) {
        return Vec2f(r->displayWidthForAspect(), r->displayHeightForAspect());
    }
    if (mCompositeFlags == CompositeFlags::Top) {
        const bool dispSwap = mVideoSwapPresentationLR;
        const VideoSource* shown = dispSwap ? rB : r;
        if (!shown || !shown->isOpen()) {
            return Vec2f(1.0f, 1.0f);
        }
        return Vec2f(shown->displayWidthForAspect(), shown->displayHeightForAspect());
    }
    const bool dispSwap = mVideoSwapPresentationLR;
    const VideoSource* leftP = dispSwap ? rB : r;
    const VideoSource* rightP = dispSwap ? r : rB;
    Vec2f ref(1.0f, 1.0f);
    if (leftP && leftP->isOpen()) {
        ref.x = (std::max)(ref.x, leftP->displayWidthForAspect());
        ref.y = (std::max)(ref.y, leftP->displayHeightForAspect());
    }
    if (rightP && rightP->isOpen()) {
        ref.x = (std::max)(ref.x, rightP->displayWidthForAspect());
        ref.y = (std::max)(ref.y, rightP->displayHeightForAspect());
    }
    return ref;
}

void App::resetVideoViewTransform(bool fitWindow)
{
    resetImageTransform(Vec2f(1.0f), fitWindow);
}

void App::syncVideoViewsLayout(const ImGuiIO& io)
{
    const VideoSource* r = videoSource();
    if (!r || !r->isOpen()) {
        return;
    }
    const bool cmp = videoCompareActive();
#if defined(BAKTSIU_DEBUG_BUILD)
    constexpr float kVideoTransportPipelineDbgRow = 26.0f;
#else
    constexpr float kVideoTransportPipelineDbgRow = 0.0f;
#endif
    const float transportBarH =
        (cmp ? kVideoTransportBarHeightCompare : kVideoTransportBarHeightSingle) + kVideoTransportPipelineDbgRow;
    const Vec4f pad(mToolbarHeight, 0.0f, mFooterHeight + transportBarH, 0.0f);
    mView.setViewportPadding(pad);
    mColumnViews[0].setViewportPadding(pad);
    mColumnViews[1].setViewportPadding(pad);

    const Vec2f ref = videoRefDisplaySizeForLayout();
    const bool videoSbs = cmp && mCompositeFlags == CompositeFlags::SideBySide;
    if (videoSbs) {
        const float leftColumnWidth = io.DisplaySize.x * mViewSplitPos;
        mColumnViews[0].resize(Vec2f(leftColumnWidth, io.DisplaySize.y));
        mColumnViews[1].resize(Vec2f(io.DisplaySize.x - leftColumnWidth, io.DisplaySize.y));
        mColumnViews[0].setImageSize(ref);
        mColumnViews[1].setImageSize(ref);
    } else {
        mView.resize(io.DisplaySize);
        mView.setImageSize(ref);
    }
}

void App::getVideoBlitContentLayout(const ImGuiIO& io, Vec2f& outContentOrigin, Vec2f& outContentSize) const
{
    const bool cmp = videoCompareActive();
#if defined(BAKTSIU_DEBUG_BUILD)
    constexpr float kVideoTransportPipelineDbgRow = 26.0f;
#else
    constexpr float kVideoTransportPipelineDbgRow = 0.0f;
#endif
    const float transportBarH =
        (cmp ? kVideoTransportBarHeightCompare : kVideoTransportBarHeightSingle) + kVideoTransportPipelineDbgRow;
    const float contentH = io.DisplaySize.y - mToolbarHeight - mFooterHeight - transportBarH;
    outContentOrigin = Vec2f(0.0f, mToolbarHeight);
    outContentSize = Vec2f(io.DisplaySize.x, (std::max)(1.0f, contentH));
}

bool App::getSingleVideoPixelAtMouse(const ImGuiIO& io, Vec2f& outImagePx) const
{
    const VideoSource* r = videoSource();
    if (!r || !r->isOpen()) {
        return false;
    }
    const Vec2f wh(io.MousePos.x, io.DisplaySize.y - io.MousePos.y);
    const bool cmp = videoCompareActive();
#if defined(BAKTSIU_DEBUG_BUILD)
    constexpr float kVideoTransportPipelineDbgRow = 26.0f;
#else
    constexpr float kVideoTransportPipelineDbgRow = 0.0f;
#endif
    const float transportBarH =
        (cmp ? kVideoTransportBarHeightCompare : kVideoTransportBarHeightSingle) + kVideoTransportPipelineDbgRow;
    const float y0 = mFooterHeight + transportBarH;
    const float y1 = io.DisplaySize.y - mToolbarHeight;
    if (wh.x < 0.f || wh.y < y0 || wh.x >= io.DisplaySize.x || wh.y > y1) {
        return false;
    }

    const bool vSbs = cmp && mCompositeFlags == CompositeFlags::SideBySide;
    bool outside = false;
    Vec2f refPx;
    if (vSbs) {
        const float splitPx = glm::round(io.DisplaySize.x * mViewSplitPos);
        if (wh.x < splitPx) {
            refPx = mColumnViews[0].getImageCoords(wh, &outside);
        } else {
            Vec2f whR(wh.x - splitPx, wh.y);
            refPx = mColumnViews[1].getImageCoords(whR, &outside);
        }
    } else {
        refPx = mView.getImageCoords(wh, &outside);
    }
    if (outside) {
        return false;
    }
    const Vec2f refSz = videoRefDisplaySizeForLayout();
    const Vec2f texSz(r->displayWidthForAspect(), r->displayHeightForAspect());
    return refPxToPixel(refPx, refSz, texSz, mFlipPresentMediaHorz[0], mFlipPresentMediaVert[0], outImagePx);
}

bool App::getCompareSideVideoPixelAtMouse(const ImGuiIO& io, int side, Vec2f& outImagePx) const
{
    const VideoSource* r = videoSource();
    const VideoSource* rB = videoSourceB();
    if (!videoCompareActive() || !r || !r->isOpen() || !rB || !rB->isOpen()) {
        return false;
    }
    if (side != 0 && side != 1) {
        return false;
    }
    const Vec2f wh(io.MousePos.x, io.DisplaySize.y - io.MousePos.y);
#if defined(BAKTSIU_DEBUG_BUILD)
    constexpr float kVideoTransportPipelineDbgRow = 26.0f;
#else
    constexpr float kVideoTransportPipelineDbgRow = 0.0f;
#endif
    const float transportBarH = 60.0f + kVideoTransportPipelineDbgRow;
    const float y0 = mFooterHeight + transportBarH;
    const float y1 = io.DisplaySize.y - mToolbarHeight;
    if (wh.x < 0.f || wh.y < y0 || wh.x >= io.DisplaySize.x || wh.y > y1) {
        return false;
    }

    const bool vSbs = mCompositeFlags == CompositeFlags::SideBySide;
    const float splitPx = glm::round(io.DisplaySize.x * mViewSplitPos);
    if (vSbs) {
        if (side == 0 && wh.x >= splitPx) {
            return false;
        }
        if (side == 1 && wh.x < splitPx) {
            return false;
        }
    } else {
        const float spx = glm::clamp(mViewSplitPos, 0.02f, 0.98f) * io.DisplaySize.x;
        if (side == 0 && wh.x >= spx) {
            return false;
        }
        if (side == 1 && wh.x < spx) {
            return false;
        }
    }

    bool outside = false;
    Vec2f refPx;
    if (vSbs) {
        if (side == 0) {
            refPx = mColumnViews[0].getImageCoords(wh, &outside);
        } else {
            Vec2f whR(wh.x - splitPx, wh.y);
            refPx = mColumnViews[1].getImageCoords(whR, &outside);
        }
    } else {
        refPx = mView.getImageCoords(wh, &outside);
    }
    if (outside) {
        return false;
    }

    const bool dispSwap = mVideoSwapPresentationLR;
    const VideoSource* leftP = dispSwap ? rB : r;
    const VideoSource* rightP = dispSwap ? r : rB;
    const VideoSource* sideP = (side == 0) ? leftP : rightP;
    if (!sideP || !sideP->isOpen()) {
        return false;
    }
    const Vec2f texSz(sideP->displayWidthForAspect(), sideP->displayHeightForAspect());
    const Vec2f refSz = videoRefDisplaySizeForLayout();
    return refPxToPixel(refPx, refSz, texSz, mFlipPresentMediaHorz[side], mFlipPresentMediaVert[side], outImagePx);
}
#else  // !USE_VIDEO

Vec2f App::videoRefDisplaySizeForLayout() const { return Vec2f(1.0f, 1.0f); }
void App::resetVideoViewTransform(bool) {}
void App::syncVideoViewsLayout(const ImGuiIO&) {}
void App::getVideoBlitContentLayout(const ImGuiIO& io, Vec2f& outContentOrigin, Vec2f& outContentSize) const
{
    outContentOrigin = Vec2f(0.0f, mToolbarHeight);
    outContentSize = Vec2f(io.DisplaySize.x, (std::max)(1.0f, io.DisplaySize.y - mToolbarHeight - mFooterHeight));
}
bool App::getSingleVideoPixelAtMouse(const ImGuiIO&, Vec2f&) const { return false; }
bool App::getCompareSideVideoPixelAtMouse(const ImGuiIO&, int, Vec2f&) const { return false; }

#endif  // USE_VIDEO

void App::recomputeVideoScrubBounds(double& outMin, double& outMax) const
{
#if !defined(USE_VIDEO)
    outMin = 0.0;
    outMax = 1.0;
#else
    const VideoSource* r = videoSource();
    const VideoSource* rB = videoSourceB();
    if (videoCompareActive() && r && r->isOpen()) {
        const double spanL = videoScrubSpanSec(r);
        const double spanR = videoScrubSpanSec(rB);
        outMin = (std::min)(mVideoStartL, mVideoStartR);
        outMax = (std::max)(mVideoStartL + spanL, mVideoStartR + spanR);
        if (outMax < outMin + 1e-3) {
            outMax = outMin + 1.0;
        }
    } else if (r && r->isOpen()) {
        outMin = 0.0;
        outMax = videoScrubSpanSec(r);
    } else {
        outMin = 0.0;
        outMax = 1.0;
    }
#endif
}

void App::clampVideoCompositionT()
{
#if defined(USE_VIDEO)
    double tMin = 0.0;
    double tMax = 1.0;
    recomputeVideoScrubBounds(tMin, tMax);
    if (mVideoCompositionT < tMin) {
        mVideoCompositionT = tMin;
    }
    if (mVideoCompositionT > tMax) {
        mVideoCompositionT = tMax;
    }
#endif
}

void App::syncVideoDecodersToCompositionT(int maxDecodeReadCapPerStream)
{
#if !defined(USE_VIDEO)
    (void)maxDecodeReadCapPerStream;
    return;
#else
#if defined(BAKTSIU_DEBUG_BUILD)
    namespace chrono = std::chrono;
    const auto tSync0 = chrono::steady_clock::now();
#endif
    VideoSource* r = videoSource();
    VideoSource* rB = videoSourceB();
    if (!r || !r->isOpen()) {
        return;
    }
    if (videoPipelineDbgEnabled(mVideoLogPipelineTiming)) {
        mVideoDbgLastUploadLMs = 0.f;
        mVideoDbgLastUploadRMs = 0.f;
    }
    bool ranDecodeL = false;
    bool ranDecodeR = false;
    const double dL = videoScrubSpanSec(r);
    const double tL = mediaTimeFromComposition(mVideoCompositionT, mVideoStartL, dL);
    double tR = 0.0;
    bool hasR = false;

    const bool pipeBothSides = videoCompareActive() && inCompareMode();
    const bool visibleIsReaderB = mVideoSwapPresentationLR && videoCompareActive();
    const bool runSyncL = !videoCompareActive() || pipeBothSides || !visibleIsReaderB;
    const bool runSyncR = videoCompareActive() && rB && rB->isOpen()
        && (pipeBothSides || visibleIsReaderB);

    if (runSyncL) {
        const double epsL = mediaSyncEpsilonSec(r);
        const bool needL = (mVideoLastSyncedTargetMediaL < 0.0)
            || (std::fabs(tL - mVideoLastSyncedTargetMediaL) > epsL);
        if (needL) {
            r->seek(tL, maxDecodeReadCapPerStream <= 0);
            r->decodeFrameThrough(tL, maxDecodeReadCapPerStream);
            uploadVideoTexture();
            ranDecodeL = true;
            if (maxDecodeReadCapPerStream <= 0) {
                mVideoLastSyncedTargetMediaL = tL;
            }
        }
    }
    if (runSyncR) {
        const double dR = videoScrubSpanSec(rB);
        tR = mediaTimeFromComposition(mVideoCompositionT, mVideoStartR, dR);
        hasR = true;
        const double epsR = mediaSyncEpsilonSec(rB);
        const bool needR = (mVideoLastSyncedTargetMediaR < 0.0)
            || (std::fabs(tR - mVideoLastSyncedTargetMediaR) > epsR);
        if (needR) {
            rB->seek(tR, maxDecodeReadCapPerStream <= 0);
            rB->decodeFrameThrough(tR, maxDecodeReadCapPerStream);
            uploadVideoTextureB();
            ranDecodeR = true;
            if (maxDecodeReadCapPerStream <= 0) {
                mVideoLastSyncedTargetMediaR = tR;
            }
        }
    }
    if (!videoCompareActive()) {
        mVideoLastSyncedTargetMediaR = -1.0;
    }
    if (videoPipelineDbgEnabled(mVideoLogPipelineTiming) && (ranDecodeL || ranDecodeR)) {
        if (ranDecodeL) {
            LOGI(
                "[video-pipe] readCap={} L: seek={:.2f}ms (flush={:.2f} setPos={:.2f}) decode+buf={:.2f}ms "
                "reads={} hitIterCap={} upload={:.2f}ms nv12Out={}",
                maxDecodeReadCapPerStream, static_cast<double>(r->lastSeekMs()),
                static_cast<double>(r->lastSeekFlushMs()),
                static_cast<double>(r->lastSeekSetPosMs()),
                static_cast<double>(r->lastDecodeThroughMs()),
                r->lastDecodeThroughReads(), r->lastDecodeThroughHitCap() ? 1 : 0,
                static_cast<double>(mVideoDbgLastUploadLMs), 0);
        }
        if (ranDecodeR && rB && rB->isOpen()) {
            LOGI(
                "[video-pipe] readCap={} R: seek={:.2f}ms (flush={:.2f} setPos={:.2f}) decode+buf={:.2f}ms "
                "reads={} hitIterCap={} upload={:.2f}ms nv12Out={}",
                maxDecodeReadCapPerStream, static_cast<double>(rB->lastSeekMs()),
                static_cast<double>(rB->lastSeekFlushMs()),
                static_cast<double>(rB->lastSeekSetPosMs()),
                static_cast<double>(rB->lastDecodeThroughMs()),
                rB->lastDecodeThroughReads(), rB->lastDecodeThroughHitCap() ? 1 : 0,
                static_cast<double>(mVideoDbgLastUploadRMs), 0);
        }
    }

#if defined(BAKTSIU_DEBUG_BUILD)
    {
        const double posL = (r && r->isOpen()) ? r->positionSec() : 0.0;
        mVideoDbgLastDriftLMs = static_cast<float>((posL - tL) * 1000.0);
        if (hasR && rB && rB->isOpen()) {
            const double posR = rB->positionSec();
            mVideoDbgLastDriftRMs = static_cast<float>((posR - tR) * 1000.0);
        } else {
            mVideoDbgLastDriftRMs = 0.f;
        }
    }
    mVideoDbgLastSyncMs =
        chrono::duration<float, std::milli>(chrono::steady_clock::now() - tSync0).count();
#else
    (void)hasR;
    (void)tR;
#endif
#endif
}

void App::resetVideoDecoderSyncCache()
{
    mVideoLastSyncedTargetMediaL = -1.0;
    mVideoLastSyncedTargetMediaR = -1.0;
    mVideoLastScrubDecodeTime = -1.0e9;
    mVideoComparePlaybackBank = 0.0;
    mVideoCompareBankL = 0.0;
    mVideoCompareBankR = 0.0;
}

void App::stepVideoScrubFiveSeconds(int direction)
{
#if !defined(USE_VIDEO)
    (void)direction;
    return;
#else
    VideoSource* r = videoSource();
    if (!r || !r->isOpen() || direction == 0) {
        return;
    }
    constexpr double kJumpSec = 5.0;
    mVideoPlaying = false;
    mVideoPlaybackTimeBank = 0.0;
    mVideoComparePlaybackBank = 0.0;
    mVideoCompareBankL = 0.0;
    mVideoCompareBankR = 0.0;
    if (videoCompareActive()) {
        mVideoCompositionT += static_cast<double>(direction) * kJumpSec;
        clampVideoCompositionT();
        syncVideoDecodersToCompositionT();
        mVideoScrubValue = mVideoCompositionT;
        return;
    }
    const double pos = r->positionSec();
    const double metaDur = r->durationSec();
    double frameDur = r->frameDurationSec();
    if (frameDur < 1e-6 || frameDur > 1.0) {
        frameDur = 1.0 / 30.0;
    }
    double tMax = videoScrubSpanSec(r);
    if (r->hasReliableDuration() && metaDur > 0.001) {
        const double endSlack = std::max(frameDur * 3.0, 0.1);
        tMax = std::max(0.0, metaDur - endSlack);
    }
    double newPos = pos + static_cast<double>(direction) * kJumpSec;
    if (newPos < 0.0) {
        newPos = 0.0;
    } else if (newPos > tMax) {
        newPos = tMax;
    }
    if (videoPipelineDbgEnabled(mVideoLogPipelineTiming)) {
        mVideoDbgLastUploadLMs = 0.f;
    }
    r->seek(newPos);
    r->decodeFrameThrough(newPos);
    uploadVideoTexture();
    if (videoPipelineDbgEnabled(mVideoLogPipelineTiming)) {
        LOGI(
            "[video-pipe] readCap={} L: seek={:.2f}ms (flush={:.2f} setPos={:.2f}) decode+buf={:.2f}ms "
            "reads={} hitIterCap={} upload={:.2f}ms nv12Out={}",
            0, static_cast<double>(r->lastSeekMs()),
            static_cast<double>(r->lastSeekFlushMs()),
            static_cast<double>(r->lastSeekSetPosMs()),
            static_cast<double>(r->lastDecodeThroughMs()),
            r->lastDecodeThroughReads(), r->lastDecodeThroughHitCap() ? 1 : 0,
            static_cast<double>(mVideoDbgLastUploadLMs), 0);
    }
    mVideoScrubValue = newPos;
#endif
}

void App::stepVideoScrubOneFrame(int direction)
{
#if !defined(USE_VIDEO)
    (void)direction;
    return;
#else
    VideoSource* r = videoSource();
    VideoSource* rB = videoSourceB();
    if (!r || !r->isOpen() || direction == 0) {
        return;
    }
    mVideoPlaying = false;
    mVideoPlaybackTimeBank = 0.0;
    mVideoComparePlaybackBank = 0.0;
    mVideoCompareBankL = 0.0;
    mVideoCompareBankR = 0.0;
    if (videoCompareActive()) {
        double stepA = r->frameDurationSec();
        double stepB = (rB && rB->isOpen()) ? rB->frameDurationSec() : stepA;
        if (stepA < 1e-6 || stepA > 1.0) {
            stepA = 1.0 / 30.0;
        }
        if (stepB < 1e-6 || stepB > 1.0) {
            stepB = 1.0 / 30.0;
        }
        const double step = (std::min)(stepA, stepB);
        mVideoCompositionT += static_cast<double>(direction) * step;
        clampVideoCompositionT();
        syncVideoDecodersToCompositionT();
        mVideoScrubValue = mVideoCompositionT;
        return;
    }
    double step = r->frameDurationSec();
    if (step < 1e-6 || step > 1.0) {
        step = 1.0 / 30.0;
    }
    const double pos = r->positionSec();
    const double metaDur = r->durationSec();
    double tMax = videoScrubSpanSec(r);
    if (r->hasReliableDuration() && metaDur > 0.001) {
        const double endSlack = std::max(step * 3.0, 0.1);
        tMax = std::max(0.0, metaDur - endSlack);
    }
    double newPos = pos + static_cast<double>(direction) * step;
    if (newPos < 0.0) {
        newPos = 0.0;
    } else if (newPos > tMax) {
        newPos = tMax;
    }
    if (videoPipelineDbgEnabled(mVideoLogPipelineTiming)) {
        mVideoDbgLastUploadLMs = 0.f;
    }
    const double deltaSec = newPos - pos;
    const bool forwardKeyframeSeek = direction > 0 && deltaSec > kVideoForwardKeyframeSeekMinSec
        && deltaSec <= kVideoForwardKeyframeSeekMaxSec;
    r->seek(newPos, !forwardKeyframeSeek);
    r->decodeFrameThrough(newPos);
    uploadVideoTexture();
    if (videoPipelineDbgEnabled(mVideoLogPipelineTiming)) {
        LOGI(
            "[video-pipe] readCap={} L: seek={:.2f}ms (flush={:.2f} setPos={:.2f}) decode+buf={:.2f}ms "
            "reads={} hitIterCap={} upload={:.2f}ms nv12Out={}",
            0, static_cast<double>(r->lastSeekMs()),
            static_cast<double>(r->lastSeekFlushMs()),
            static_cast<double>(r->lastSeekSetPosMs()),
            static_cast<double>(r->lastDecodeThroughMs()),
            r->lastDecodeThroughReads(), r->lastDecodeThroughHitCap() ? 1 : 0,
            static_cast<double>(mVideoDbgLastUploadLMs), 0);
    }
    mVideoScrubValue = newPos;
#endif
}

void App::swapVideoSides()
{
#if !defined(USE_VIDEO)
    return;
#else
    if (!videoCompareActive()) {
        return;
    }
    mVideoSwapPresentationLR = !mVideoSwapPresentationLR;
#endif
}

int App::addVideoPath(const std::string& filepath)
{
#if !defined(USE_VIDEO)
    (void)filepath;
    return -1;
#else
    const std::string path = normalizeVideoPath(filepath);
    bool hasImages = false;
    for (auto& m : mMediaList) { if (m && !m->isVideo()) { hasImages = true; break; } }
    if (hasImages) {
        mMediaList.clear();
        mTopImageIndex = mCmpImageIndex = -1;
        mCompositeFlags = CompositeFlags::Top;
    }
    for (int i = 0; i < static_cast<int>(mMediaList.size()); ++i) {
        if (mMediaList[i] && mMediaList[i]->filepath() == path) {
            return i;
        }
    }
    mMediaList.push_back(std::make_unique<VideoSource>(path));
    return static_cast<int>(mMediaList.size()) - 1;
#endif
}

void App::applyImportedVideoListSelection(const std::vector<int>& addedListIndices)
{
#if !defined(USE_VIDEO)
    (void)addedListIndices;
    return;
#else
    if (addedListIndices.empty() || mCompositeFlags == CompositeFlags::Split) {
        return;
    }
    if (addedListIndices.size() == 1) {
        const int newIdx = addedListIndices[0];
        if (mTopImageIndex >= 0 && mTopImageIndex != newIdx) {
            mCmpImageIndex = newIdx;
        } else {
            mTopImageIndex = newIdx;
            mCmpImageIndex = -1;
        }
    } else {
        mTopImageIndex = addedListIndices[0];
        mCmpImageIndex = addedListIndices[1];
        if (mTopImageIndex == mCmpImageIndex) {
            mCmpImageIndex = -1;
        }
    }
    mVideoSwapPresentationLR = false;
#endif
}

void App::openVideosFromSelection()
{
#if !defined(USE_VIDEO)
    return;
#else
    const int n = static_cast<int>(mMediaList.size());
    if (n <= 0 || mTopImageIndex < 0 || mTopImageIndex >= n) {
        return;
    }

    const int idxL = mTopImageIndex;
    int idxR = (mCmpImageIndex >= 0 && mCmpImageIndex < n && mCmpImageIndex != idxL) ? mCmpImageIndex : -1;
    if (idxR < 0 && n == 2
        && (mCompositeFlags == CompositeFlags::Split
            || mCompositeFlags == CompositeFlags::SideBySide)) {
        idxR = (idxL == 0) ? 1 : 0;
        mCmpImageIndex = idxR;
    }

    auto* stubL = static_cast<VideoSource*>(mMediaList[idxL].get());
    VideoSource* stubR = (idxR >= 0) ? static_cast<VideoSource*>(mMediaList[idxR].get()) : nullptr;
    const std::string pathL = stubL->filepath();
    const std::string pathR = stubR ? stubR->filepath() : std::string();

    VideoSource* curL = videoSource();
    VideoSource* curR = videoSourceB();
    if (curL && curL->isOpen() && curL->filepath() == pathL
        && ((idxR < 0 && (!curR || !curR->isOpen()))
            || (idxR >= 0 && curR && curR->isOpen() && curR->filepath() == pathR))) {
        if (idxR < 0 || videoCompareActive()) {
            syncVideoDecodersToCompositionT();
            mVideoLazyDurationGraceFrames = 0;
            return;
        }
    }

    if (isVideoMode() && videoCompareActive() && idxR >= 0
        && curL && curL->isOpen() && curR && curR->isOpen()) {
        const int pL = mVideoSwapPresentationLR ? mCmpImageIndex : mTopImageIndex;
        const int pR = mVideoSwapPresentationLR ? mTopImageIndex : mCmpImageIndex;
        if (pL >= 0 && pL < n && pR >= 0 && pR < n && pL != pR) {
            const std::string wantLeft = mMediaList[pL]->filepath();
            const std::string wantRight = mMediaList[pR]->filepath();
            const std::string openA = curL->filepath();
            const std::string openB = curR->filepath();
            const bool samePair =
                (wantLeft == openA && wantRight == openB) || (wantLeft == openB && wantRight == openA);
            if (samePair) {
                if (wantLeft == openA && wantRight == openB) {
                    mTopImageIndex = pL;
                    mCmpImageIndex = pR;
                    mVideoSwapPresentationLR = false;
                } else {
                    mTopImageIndex = pR;
                    mCmpImageIndex = pL;
                    mVideoSwapPresentationLR = true;
                }
                syncVideoDecodersToCompositionT();
                mVideoLazyDurationGraceFrames = 0;
                return;
            }
        }
    }

    if (idxR >= 0) {
        LOGI("[video-open] selection n={} idxL={} idxR={} (dual/compare) L=\"{}\" R=\"{}\"", n, idxL, idxR, pathL, pathR);
    } else {
        LOGI("[video-open] selection n={} idxL={} idxR=-1 (single) L=\"{}\"", n, idxL, pathL);
    }

    for (auto& m : mMediaList) {
        if (m && m->isVideo()) {
            static_cast<VideoSource*>(m.get())->close();
        }
    }
    mTexturePool.cleanUnusedTextures();

    const bool openedL = stubL->open(mWindow, pathL);
    bool openedR = false;
    if (idxR >= 0) {
        openedR = stubR->open(mWindow, pathR);
    }

    if (!openedL) {
        LOGW("Failed to open video \"{}\"", pathL);
        if (openedR && stubR) { stubR->close(); }
        return;
    }
    if (idxR >= 0 && !openedR) {
        LOGW("Failed to open video \"{}\"", pathR);
        stubL->close();
        return;
    }

    stubL->setPipelineTimingLog(videoPipelineDbgEnabled(mVideoLogPipelineTiming));
    if (stubR) { stubR->setPipelineTimingLog(videoPipelineDbgEnabled(mVideoLogPipelineTiming)); }

    mVideoPlaying = true;
    mVideoPlaybackTimeBank = 0.0;
    if (!videoCompareActive()) {
        mCompositeFlags = CompositeFlags::Top;
    }

    mVideoSwapPresentationLR = false;
    mPendingViewFitReset = true;
    applyInitialWindowAspectFromSize(stubL->displayWidthForAspect(), stubL->displayHeightForAspect());

    mVideoStartL = 0.0;
    mVideoStartR = 0.0;

    double tMin = 0.0;
    double tMax = 1.0;
    recomputeVideoScrubBounds(tMin, tMax);
    mVideoCompositionT = tMin;
    mVideoScrubValue = mVideoCompositionT;

    {
        namespace chrono = std::chrono;
        const auto tTexSync0 = chrono::steady_clock::now();
        stubL->ensureTexture();
        if (stubR) { stubR->ensureTexture(); }
        resetVideoDecoderSyncCache();
        syncVideoDecodersToCompositionT();
        LOGI("[video-open] recreate textures + first syncVideoDecodersToCompositionT: {:.3f}s",
            chrono::duration<double>(chrono::steady_clock::now() - tTexSync0).count());
    }

    mVideoLazyDurationGraceFrames = 0;
    if (stubL->isOpen() && stubL->durationSec() <= 0.001) {
        mVideoLazyDurationGraceFrames = 1;
    }
    if (videoCompareActive() && stubR && stubR->isOpen() && stubR->durationSec() <= 0.001) {
        mVideoLazyDurationGraceFrames = 1;
    }
#endif
}

void App::openVideoFile(const std::string& filepath)
{
#if !defined(USE_VIDEO)
    (void)filepath;
    LOGW("Video playback is disabled in this build (USE_VIDEO).");
#else
    const int idx = addVideoPath(filepath);
    if (idx < 0) {
        return;
    }
    mTopImageIndex = idx;
    if (mCmpImageIndex == mTopImageIndex) {
        mCmpImageIndex = -1;
    }
    openVideosFromSelection();
#endif
}

void App::openVideoCompare(const std::string& leftPath, const std::string& rightPath)
{
#if !defined(USE_VIDEO)
    (void)leftPath;
    (void)rightPath;
    LOGW("Video playback is disabled in this build (USE_VIDEO).");
#else
    const int idxL = addVideoPath(leftPath);
    const int idxR = addVideoPath(rightPath);
    if (idxL < 0 || idxR < 0) {
        return;
    }
    if (idxL == idxR) {
        LOGW("Video compare: left and right resolve to the same list row; opening as single view.");
        mTopImageIndex = idxL;
        mCmpImageIndex = -1;
        openVideosFromSelection();
        return;
    }
    mTopImageIndex = idxL;
    mCmpImageIndex = idxR;
    openVideosFromSelection();
    LOGI("Video compare: left \"{}\", right \"{}\"", mMediaList[idxL]->filepath(), mMediaList[idxR]->filepath());
#endif
}

void App::openVideoPaths(const std::vector<std::string>& paths)
{
#if !defined(USE_VIDEO)
    (void)paths;
    LOGW("Video playback is disabled in this build (USE_VIDEO).");
#else
    if (paths.empty()) {
        return;
    }
    std::vector<int> addedIdx;
    addedIdx.reserve(paths.size());
    for (const std::string& p : paths) {
        const int idx = addVideoPath(p);
        if (idx >= 0) {
            addedIdx.push_back(idx);
            if (mTopImageIndex < 0) {
                mTopImageIndex = idx;
            } else if (mCmpImageIndex < 0 && idx != mTopImageIndex) {
                mCmpImageIndex = idx;
            }
        }
    }
    applyImportedVideoListSelection(addedIdx);
    openVideosFromSelection();
#endif
}

void App::exitVideoMode()
{
#if !defined(USE_VIDEO)
    return;
#else
    mVideoPlaying = false;
    mViewportRmbScrubActive = false;
    mVideoResumePlaybackAfterViewportScrub = false;
    mPendingViewFitReset = false;
    mVideoLazyDurationGraceFrames = 0;
    mVideoPlaybackTimeBank = 0.0;
    resetVideoDecoderSyncCache();
    mVideoStartL = mVideoStartR = 0.0;
    mVideoCompositionT = 0.0;
    mVideoSwapPresentationLR = false;
    for (auto& m : mMediaList) {
        if (m && m->isVideo()) {
            static_cast<VideoSource*>(m.get())->close();
        }
    }
    mMediaList.clear();
    mTopImageIndex = mCmpImageIndex = -1;
    mCompositeFlags = CompositeFlags::Top;
#endif
}

void App::uploadVideoTexture()
{
#if !defined(USE_VIDEO)
    return;
#else
    VideoSource* r = videoSource();
    if (!r || !r->isOpen()) {
        return;
    }
    namespace chrono = std::chrono;
    const bool timeUpload = videoPipelineDbgEnabled(mVideoLogPipelineTiming);
    const auto tUpload0 = timeUpload ? chrono::steady_clock::now() : chrono::steady_clock::time_point {};
    r->upload();
    if (timeUpload) {
        mVideoDbgLastUploadLMs = chrono::duration<float, std::milli>(chrono::steady_clock::now() - tUpload0).count();
    }
#endif
}

void App::uploadVideoTextureB()
{
#if !defined(USE_VIDEO)
    return;
#else
    VideoSource* rB = videoSourceB();
    if (!rB || !rB->isOpen()) {
        return;
    }
    namespace chrono = std::chrono;
    const bool timeUpload = videoPipelineDbgEnabled(mVideoLogPipelineTiming);
    const auto tUpload0 = timeUpload ? chrono::steady_clock::now() : chrono::steady_clock::time_point {};
    rB->upload();
    if (timeUpload) {
        mVideoDbgLastUploadRMs = chrono::duration<float, std::milli>(chrono::steady_clock::now() - tUpload0).count();
    }
#endif
}

void App::tickAndUploadVideoFrame(float deltaTime)
{
#if !defined(USE_VIDEO)
    (void)deltaTime;
    return;
#else
    VideoSource* r = videoSource();
    VideoSource* rB = videoSourceB();
    if (!mVideoPlaying || !r || !r->isOpen()) {
        return;
    }
#if defined(BAKTSIU_DEBUG_BUILD)
    mVideoDbgLastTickDtMs = static_cast<float>(static_cast<double>(deltaTime) * 1000.0);
#endif
    if (videoCompareActive()) {
        double tMin = 0.0;
        double tMax = 1.0;
        recomputeVideoScrubBounds(tMin, tMax);
        const double stepA = saneFrameDurSec(r);
        const double stepB = saneFrameDurSec(rB);
        const double step = (std::min)(stepA, stepB);

        const double dt = (std::min)(static_cast<double>(deltaTime), kVideoPlaybackWallDtCapSec);
        mVideoComparePlaybackBank += dt;
        int n = static_cast<int>(std::floor((mVideoComparePlaybackBank / step) + 1e-9));
        if (n > kVideoMaxCompareStepsPerTick) {
            n = kVideoMaxCompareStepsPerTick;
        }
        if (n > 0) {
            mVideoComparePlaybackBank -= static_cast<double>(n) * step;
        }

        constexpr double kEndEps = 1.0 / 240.0;
        const double dL = videoScrubSpanSec(r);
        const double dR = videoScrubSpanSec(rB);
        constexpr double kNeedMediaStepEps = 1e-9;
        const bool pipeBothSides = inCompareMode();
        const bool visibleIsReaderB = mVideoSwapPresentationLR;
        const bool runDecodeL = pipeBothSides || !visibleIsReaderB;
        const bool runDecodeR =
            rB && rB->isOpen() && (pipeBothSides || visibleIsReaderB);
        if (n > 0) {
            for (int i = 0; i < n; ++i) {
                mVideoCompareBankL += step;
                mVideoCompareBankR += step;

                const double T = mVideoCompositionT;
                const double Tnext = T + step;
                const bool clampedL = mediaTimeFromComposition(Tnext, mVideoStartL, dL)
                    <= mediaTimeFromComposition(T, mVideoStartL, dL) + kNeedMediaStepEps;
                const bool clampedR = mediaTimeFromComposition(Tnext, mVideoStartR, dR)
                    <= mediaTimeFromComposition(T, mVideoStartR, dR) + kNeedMediaStepEps;

                const bool needL = !clampedL && mVideoCompareBankL >= stepA - 1e-9;
                const bool needR = !clampedR && mVideoCompareBankR >= stepB - 1e-9;
                if (needL) {
                    mVideoCompareBankL -= stepA;
                }
                if (needR) {
                    mVideoCompareBankR -= stepB;
                }

                if (needL && runDecodeL && !r->decodeFrame(true)) {
                    mVideoComparePlaybackBank = 0.0;
                    mVideoCompareBankL = 0.0;
                    mVideoCompareBankR = 0.0;
                    mVideoPlaying = false;
                    break;
                }
                if (needR && runDecodeR && !rB->decodeFrame(true)) {
                    mVideoComparePlaybackBank = 0.0;
                    mVideoCompareBankL = 0.0;
                    mVideoCompareBankR = 0.0;
                    mVideoPlaying = false;
                    break;
                }
                mVideoCompositionT += step;
                if (mVideoCompositionT >= tMax - kEndEps) {
                    mVideoCompositionT = tMax;
                    mVideoComparePlaybackBank = 0.0;
                    mVideoCompareBankL = 0.0;
                    mVideoCompareBankR = 0.0;
                    mVideoPlaying = false;
                    break;
                }
            }
            if (runDecodeL) {
                uploadVideoTexture();
            }
            if (runDecodeR) {
                uploadVideoTextureB();
            }
        }
        clampVideoCompositionT();
        if (mVideoCompositionT >= tMax - kEndEps) {
            mVideoCompositionT = tMax;
            mVideoPlaying = false;
            mVideoComparePlaybackBank = 0.0;
            mVideoCompareBankL = 0.0;
            mVideoCompareBankR = 0.0;
        }

#if defined(BAKTSIU_DEBUG_BUILD)
        if (videoPipelineDbgEnabled(mVideoLogPipelineTiming)) {
            const double now = ImGui::GetTime();
            if (now - mVideoDbgLastConsoleBeatSec >= 1.0) {
                mVideoDbgLastConsoleBeatSec = now;
                const double posL = (r && r->isOpen()) ? r->positionSec() : 0.0;
                const double posR = (rB && rB->isOpen()) ? rB->positionSec() : 0.0;
                LOGI("[video-compare] dt={:.1f}ms sync={:.1f}ms driftL={:+.1f}ms driftR={:+.1f}ms "
                     "T={:.3f}s posL={:.3f}s posR={:.3f}s readCap={}",
                    static_cast<double>(mVideoDbgLastTickDtMs),
                    static_cast<double>(mVideoDbgLastSyncMs),
                    static_cast<double>(mVideoDbgLastDriftLMs),
                    static_cast<double>(mVideoDbgLastDriftRMs),
                    mVideoCompositionT, posL, posR, n);
            }
        }
#endif
        mVideoScrubValue = mVideoCompositionT;
        return;
    }
    double frameDur = r->frameDurationSec();
    if (frameDur < 1e-6 || frameDur > 1.0) {
        frameDur = 1.0 / 30.0;
    }
    const double dt = (std::min)(static_cast<double>(deltaTime), kVideoPlaybackWallDtCapSec);
    mVideoPlaybackTimeBank += dt;
    constexpr int kMaxDecodeStepsPerFrame = 64;
    const double framesOwed = mVideoPlaybackTimeBank / frameDur;
    int n = static_cast<int>(std::floor(framesOwed + 1e-9));
    if (n > kMaxDecodeStepsPerFrame) {
        n = kMaxDecodeStepsPerFrame;
    }
    if (n > 0) {
        mVideoPlaybackTimeBank -= static_cast<double>(n) * frameDur;
        for (int i = 0; i < n; ++i) {
            if (!r->decodeFrame(true)) {
                mVideoPlaybackTimeBank = 0.0;
                mVideoPlaying = false;
                break;
            }
        }
        uploadVideoTexture();
    }
    mVideoScrubValue = r->positionSec();
#endif
}

void App::renderVideoBlit(const ImGuiIO& io)
{
#if !defined(USE_VIDEO)
    (void)io;
    return;
#else
    VideoSource* r = videoSource();
    VideoSource* rB = videoSourceB();
    if (!mVideoShaderReady || !r || !r->isOpen() || r->texId() == 0) {
        return;
    }
    const Vec2f viewportSize = io.DisplaySize * io.DisplayFramebufferScale;
    glViewport(0, 0, static_cast<GLsizei>(viewportSize.x), static_cast<GLsizei>(viewportSize.y));
    glClearColor(0.45f, 0.55f, 0.6f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);

    mVideoBlitShader.bind();
    const bool cmp = videoCompareActive();
#if defined(BAKTSIU_DEBUG_BUILD)
    constexpr float kVideoTransportPipelineDbgRow = 26.0f;
#else
    constexpr float kVideoTransportPipelineDbgRow = 0.0f;
#endif
    const float transportBarH =
        (cmp ? kVideoTransportBarHeightCompare : kVideoTransportBarHeightSingle) + kVideoTransportPipelineDbgRow;
    const float contentH =
        io.DisplaySize.y - mToolbarHeight - mFooterHeight - transportBarH;
    const Vec2f contentOrigin(0.0f, mToolbarHeight);
    const Vec2f contentSize(io.DisplaySize.x, std::max(1.0f, contentH));

    mVideoBlitShader.setUniform("uWindowSize", Vec2f(io.DisplaySize.x, io.DisplaySize.y));
    mVideoBlitShader.setUniform("uContentOrigin", contentOrigin);
    mVideoBlitShader.setUniform("uContentSize", contentSize);

    const float canvasY0 = mFooterHeight + transportBarH;
    const float canvasY1 = io.DisplaySize.y - mToolbarHeight;
    mVideoBlitShader.setUniform("uVideoCanvasRect", Vec4f(0.0f, canvasY0, io.DisplaySize.x, canvasY1));
    const Vec2f videoRef = videoRefDisplaySizeForLayout();
    mVideoBlitShader.setUniform("uVideoRefSize", videoRef);
    const bool videoSbsLayout = cmp && mCompositeFlags == CompositeFlags::SideBySide;
    if (videoSbsLayout) {
        mVideoBlitShader.setUniform("uVideoOffset", mColumnViews[0].getImageOffset());
        mVideoBlitShader.setUniform("uVideoOffsetExtra", mColumnViews[1].getImageOffset());
        mVideoBlitShader.setUniform("uVideoRelativeOffset",
            (mColumnViews[1].getLocalOffset() - mColumnViews[0].getLocalOffset())
                * mColumnViews[0].getImageScale());
        mVideoBlitShader.setUniform("uVideoImageScale", mColumnViews[0].getImageScale());
        mVideoBlitShader.setUniform("uVideoSideBySide", 1);
    } else {
        mVideoBlitShader.setUniform("uVideoOffset", mView.getImageOffset());
        mVideoBlitShader.setUniform("uVideoOffsetExtra", mView.getImageOffset());
        mVideoBlitShader.setUniform("uVideoRelativeOffset", Vec2f(0.0f));
        mVideoBlitShader.setUniform("uVideoImageScale", mView.getImageScale());
        mVideoBlitShader.setUniform("uVideoSideBySide", 0);
    }

    if (cmp && rB && rB->texId() != 0) {
        const bool dispSwap = mVideoSwapPresentationLR && videoCompareActive();
        const GLuint t0 = dispSwap ? rB->texId() : r->texId();
        const GLuint t1 = dispSwap ? r->texId() : rB->texId();
        const VideoSource* r0 = dispSwap ? rB : r;
        const VideoSource* r1 = dispSwap ? r : rB;

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, t0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, t1);

        int compareMode = 1;
        if (mCompositeFlags == CompositeFlags::Top) {
            compareMode = 0;
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, t0);
        } else if (mCompositeFlags == CompositeFlags::SideBySide) {
            compareMode = 2;
        }
        mVideoBlitShader.setUniform("uCompareMode", compareMode);
        mVideoBlitShader.setUniform("uVideo", 0);
        mVideoBlitShader.setUniform("uVideoR", 1);
        mVideoBlitShader.setUniform("uVideoSize",
            Vec2f(r0 ? r0->displayWidthForAspect() : 1.0f, r0 ? r0->displayHeightForAspect() : 1.0f));
        mVideoBlitShader.setUniform("uVideoSizeR",
            Vec2f(r1 ? r1->displayWidthForAspect() : 1.0f, r1 ? r1->displayHeightForAspect() : 1.0f));
        mVideoBlitShader.setUniform("uSplitPos", mViewSplitPos);
    } else {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, r->texId());
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, r->texId());

        mVideoBlitShader.setUniform("uCompareMode", 0);
        mVideoBlitShader.setUniform("uVideo", 0);
        mVideoBlitShader.setUniform("uVideoR", 1);
        mVideoBlitShader.setUniform("uVideoSize",
            Vec2f(r->displayWidthForAspect(), r->displayHeightForAspect()));
        mVideoBlitShader.setUniform("uVideoSizeR",
            Vec2f(r->displayWidthForAspect(), r->displayHeightForAspect()));
    }
    mVideoBlitShader.setUniform("uOutTransformType", mOutTransformType);
    mVideoBlitShader.setUniform("uDisplayGamma", mDisplayGamma);
    mVideoBlitShader.setUniform("uEV", mExposureValue);
    mVideoBlitShader.setUniform("uPresentMode", mCurrentPresentMode);
    mVideoBlitShader.setUniform("uApplyToneMapping", mEnableToneMapping);
    mVideoBlitShader.setUniform("uPixelMarkerFlags", getPixelMarkerFlags());
    mVideoBlitShader.setUniform("uFlipVideoL", mFlipPresentMediaVert[0]);
    mVideoBlitShader.setUniform("uFlipVideoR", cmp ? mFlipPresentMediaVert[1] : false);
    mVideoBlitShader.setUniform("uFlipVideoLH", mFlipPresentMediaHorz[0]);
    mVideoBlitShader.setUniform("uFlipVideoRH", cmp ? mFlipPresentMediaHorz[1] : false);
    mVideoBlitShader.drawTriangle();
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
#endif
}

void App::renderVideoTransportBar(const ImGuiIO& io)
{
#if !defined(USE_VIDEO)
    (void)io;
    return;
#else
    VideoSource* r = videoSource();
    VideoSource* rB = videoSourceB();
    if (!r || !r->isOpen()) {
        return;
    }
    const bool cmp = videoCompareActive();
#if defined(BAKTSIU_DEBUG_BUILD)
    constexpr float kVideoTransportPipelineDbgRow = 26.0f;
#else
    constexpr float kVideoTransportPipelineDbgRow = 0.0f;
#endif
    const float barH =
        (cmp ? kVideoTransportBarHeightCompare : kVideoTransportBarHeightSingle) + kVideoTransportPipelineDbgRow;
    ImGui::SetNextWindowPos(ImVec2(0.0f, io.DisplaySize.y - mFooterHeight - barH));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, barH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("VideoTransport", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
        | ImGuiWindowFlags_NoSavedSettings);

    {
        const ImGuiStyle& st = ImGui::GetStyle();
        const float playPauseW =
            (std::max)(ImGui::CalcTextSize("Play").x, ImGui::CalcTextSize("Pause").x) + st.FramePadding.x * 2.0f;
        if (ImGui::Button(mVideoPlaying ? "Pause" : "Play", ImVec2(playPauseW, 0.0f))) {
            mVideoPlaying ^= true;
            mVideoPlaybackTimeBank = 0.0;
            mVideoComparePlaybackBank = 0.0;
            mVideoCompareBankL = 0.0;
            mVideoCompareBankR = 0.0;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
        exitVideoMode();
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }
    ImGui::SameLine();

    if (cmp) {
        const ImVec4 tintL(0.25f, 0.80f, 0.75f, 1.0f);
        const ImVec4 tintR(0.95f, 0.55f, 0.20f, 1.0f);

        double tMin = 0.0;
        double tMax = 1.0;
        recomputeVideoScrubBounds(tMin, tMax);

        ImGui::SetNextItemWidth(std::max(120.0f, io.DisplaySize.x - 380.0f));
        ImGui::PushID("videoscrubcmp");
        const bool valueChanged =
            ImGui::SliderScalar("##t", ImGuiDataType_Double, &mVideoScrubValue, &tMin, &tMax, "");

        if (tMax > tMin) {
            const ImVec2 rmin = ImGui::GetItemRectMin();
            const ImVec2 rmax = ImGui::GetItemRectMax();
            const float padY = 2.0f;
            const float y0 = rmin.y + padY;
            const float y1 = rmax.y - padY;
            auto drawRange = [&](double a, double b, const ImVec4& tint) {
                const double lo = (std::min)(a, b);
                const double hi = (std::max)(a, b);
                const double c0 = (std::max)(lo, tMin);
                const double c1 = (std::min)(hi, tMax);
                if (c1 <= c0) {
                    return;
                }
                const float u0 = static_cast<float>((c0 - tMin) / (tMax - tMin));
                const float u1 = static_cast<float>((c1 - tMin) / (tMax - tMin));
                const float x0 = rmin.x + (rmax.x - rmin.x) * glm::clamp(u0, 0.0f, 1.0f);
                const float x1 = rmin.x + (rmax.x - rmin.x) * glm::clamp(u1, 0.0f, 1.0f);
                const ImU32 fill = ImGui::ColorConvertFloat4ToU32(ImVec4(tint.x, tint.y, tint.z, 0.22f));
                const ImU32 edge = ImGui::ColorConvertFloat4ToU32(ImVec4(tint.x, tint.y, tint.z, 0.65f));
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), fill, 2.0f);
                ImGui::GetWindowDrawList()->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), edge, 2.0f);
            };

            const bool dispSwap = mVideoSwapPresentationLR && videoCompareActive();
            const double dA = r ? videoScrubSpanSec(r) : 0.0;
            const double dB = rB ? videoScrubSpanSec(rB) : 0.0;
            const double startLeft = dispSwap ? mVideoStartR : mVideoStartL;
            const double durLeft = dispSwap ? dB : dA;
            const double startRight = dispSwap ? mVideoStartL : mVideoStartR;
            const double durRight = dispSwap ? dA : dB;
            drawRange(startLeft, startLeft + durLeft, tintL);
            drawRange(startRight, startRight + durRight, tintR);
        }

        const bool itemActive = ImGui::IsItemActive();
        const bool itemActivated = ImGui::IsItemActivated();
        const bool itemClicked = ImGui::IsItemClicked();
        const bool itemDeactivated = ImGui::IsItemDeactivated();
        if (itemActivated) {
            mVideoPlayingBeforeScrub = mVideoPlaying;
        }
        if (itemActive || itemActivated || valueChanged || itemClicked || itemDeactivated) {
            mVideoCompositionT = mVideoScrubValue;
            clampVideoCompositionT();
            mVideoPlaybackTimeBank = 0.0;
            mVideoPlaying = false;
            const VideoScrubDecodeHint scrubHint = videoScrubDecodeHint(itemActive, itemActivated, itemClicked,
                valueChanged, itemDeactivated, ImGui::GetTime(), mVideoLastScrubDecodeTime);
            if (scrubHint.run) {
                syncVideoDecodersToCompositionT(scrubHint.maxReadCapPerStream);
            }
        }
        if (itemDeactivated && mVideoPlayingBeforeScrub) {
            mVideoPlaying = true;
            mVideoPlaybackTimeBank = 0.0;
            mVideoComparePlaybackBank = 0.0;
            mVideoCompareBankL = 0.0;
            mVideoCompareBankR = 0.0;
        }
        if (!itemActive && !mViewportRmbScrubActive) {
            mVideoScrubValue = mVideoCompositionT;
        }
        ImGui::PopID();

        ImGui::SameLine();
        const double tShown =
            (itemActive || mViewportRmbScrubActive) ? mVideoScrubValue : mVideoCompositionT;
        char tBuf[32];
        char maxBuf[32];
        formatVideoHms(tShown, tBuf, sizeof tBuf);
        formatVideoHms(tMax, maxBuf, sizeof maxBuf);
        const bool durRelL = r && r->isOpen() && r->hasReliableDuration() && r->durationSec() > 0.001;
        const bool durRelR = rB && rB->isOpen() && rB->hasReliableDuration() && rB->durationSec() > 0.001;
        const bool durRelCmp = durRelL && durRelR;
        if (durRelCmp) {
            ImGui::Text("T %s / %s", tBuf, maxBuf);
        } else {
            ImGui::Text("T %s / ?", tBuf);
        }
        if (!durRelCmp) {
            ImGui::SameLine();
            ImGui::TextDisabled("(duration unknown)");
        }

        auto offsetControl = [&](const char* label, double& v, double stepSec, const ImVec4& tint) -> bool {
            bool changed = false;
            ImGui::PushID(label);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(tint, "%s", label);
            ImGui::SameLine();

            const ImVec4 btn = ImVec4(tint.x * 0.85f, tint.y * 0.85f, tint.z * 0.85f, 1.0f);
            const ImVec4 btnHover = ImVec4(tint.x, tint.y, tint.z, 1.0f);
            const ImVec4 btnActive = ImVec4(tint.x * 1.05f, tint.y * 1.05f, tint.z * 1.05f, 1.0f);
            const ImVec4 frame = ImVec4(tint.x * 0.20f, tint.y * 0.20f, tint.z * 0.20f, 0.85f);
            ImGui::PushStyleColor(ImGuiCol_Button, btn);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, btnHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, btnActive);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, frame);
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, frame);
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, frame);

            if (ImGui::SmallButton("<")) {
                v -= stepSec;
                changed = true;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(92.0f);
            {
                const float dragSpeed = static_cast<float>(stepSec * (ImGui::GetIO().KeyShift ? 0.75 : 4.0));
                changed |= ImGui::DragScalar("##v", ImGuiDataType_Double, &v, dragSpeed, nullptr, nullptr, "%.3f", 1.0f);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Drag to scrub, Ctrl+Click to type");
                }

                const ImVec2 rmin = ImGui::GetItemRectMin();
                const ImVec2 rmax = ImGui::GetItemRectMax();
                const char* unit = "s";
                const ImVec2 unitSz = ImGui::CalcTextSize(unit);
                const float padX = 6.0f;
                const float x = rmax.x - unitSz.x - padX;
                const float y = rmin.y + (rmax.y - rmin.y - unitSz.y) * 0.5f;
                const ImU32 col = ImGui::ColorConvertFloat4ToU32(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::GetWindowDrawList()->AddText(ImVec2(x, y), col, unit);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(">")) {
                v += stepSec;
                changed = true;
            }

            ImGui::PopStyleColor(6);
            ImGui::PopID();
            return changed;
        };

        const bool dispSwap = mVideoSwapPresentationLR && videoCompareActive();
        const double stepA = (r && r->frameDurationSec() > 0.0) ? r->frameDurationSec() : 0.05;
        const double stepB = (rB && rB->frameDurationSec() > 0.0) ? rB->frameDurationSec() : 0.05;
        double& startLeft = dispSwap ? mVideoStartR : mVideoStartL;
        double& startRight = dispSwap ? mVideoStartL : mVideoStartR;
        const double stepLeft = dispSwap ? stepB : stepA;
        const double stepRight = dispSwap ? stepA : stepB;

        const bool changedL = offsetControl("L", startLeft, stepLeft, tintL);
        ImGui::SameLine();
        const bool changedR = offsetControl("R", startRight, stepRight, tintR);
        if (changedL || changedR) {
            clampVideoCompositionT();
            syncVideoDecodersToCompositionT();
            mVideoPlaying = false;
        }
    } else {
        const double pos = r->positionSec();
        const double metaDur = r->durationSec();
        const bool durRel = r->hasReliableDuration() && metaDur > 0.001;
        double sliderMaxSec =
            std::max({videoScrubSpanSec(r), pos + 0.01, mVideoScrubValue + 0.01, 0.25});
        if (durRel) {
            const double frameDur = r->frameDurationSec();
            const double endSlack = std::max(frameDur * 3.0, 0.1);
            const double seekableEnd = std::max(0.0, metaDur - endSlack);
            sliderMaxSec = std::max({seekableEnd, pos + 0.01, mVideoScrubValue + 0.01, 0.25});
        }
        const double sliderMinSec = 0.0;

        ImGui::SetNextItemWidth(std::max(120.0f, io.DisplaySize.x - 280.0f));
        ImGui::PushID("videoscrub");
        const bool valueChanged =
            ImGui::SliderScalar("##t", ImGuiDataType_Double, &mVideoScrubValue, &sliderMinSec, &sliderMaxSec, "");
        const bool itemActive = ImGui::IsItemActive();
        const bool itemActivated = ImGui::IsItemActivated();
        const bool itemClicked = ImGui::IsItemClicked();
        const bool itemDeactivated = ImGui::IsItemDeactivated();
        if (itemActivated) {
            mVideoPlayingBeforeScrub = mVideoPlaying;
        }
        if (itemActive || itemActivated || valueChanged || itemClicked || itemDeactivated) {
            mVideoPlaybackTimeBank = 0.0;
            mVideoPlaying = false;
            const VideoScrubDecodeHint scrubHint = videoScrubDecodeHint(itemActive, itemActivated, itemClicked,
                valueChanged, itemDeactivated, ImGui::GetTime(), mVideoLastScrubDecodeTime);
            if (scrubHint.run) {
                r->seek(mVideoScrubValue, scrubHint.maxReadCapPerStream <= 0);
                r->decodeFrameThrough(mVideoScrubValue, scrubHint.maxReadCapPerStream);
                uploadVideoTexture();
                if (videoPipelineDbgEnabled(mVideoLogPipelineTiming)) {
                    LOGI(
                        "[video-pipe] readCap={} L: seek={:.2f}ms (flush={:.2f} setPos={:.2f}) "
                        "decode+buf={:.2f}ms reads={} hitIterCap={} upload={:.2f}ms nv12Out={}",
                        scrubHint.maxReadCapPerStream, static_cast<double>(r->lastSeekMs()),
                        static_cast<double>(r->lastSeekFlushMs()),
                        static_cast<double>(r->lastSeekSetPosMs()),
                        static_cast<double>(r->lastDecodeThroughMs()),
                        r->lastDecodeThroughReads(), r->lastDecodeThroughHitCap() ? 1 : 0,
                        static_cast<double>(mVideoDbgLastUploadLMs), 0);
                }
            }
        }
        if (itemDeactivated && mVideoPlayingBeforeScrub) {
            mVideoPlaying = true;
            mVideoPlaybackTimeBank = 0.0;
            mVideoComparePlaybackBank = 0.0;
            mVideoCompareBankL = 0.0;
            mVideoCompareBankR = 0.0;
        }
        if (mVideoPlaying && !itemActive && !mViewportRmbScrubActive) {
            mVideoScrubValue = r->positionSec();
        }
        ImGui::PopID();

        ImGui::SameLine();
        const double tDisplayed = mVideoScrubValue;
        char posBuf[32];
        char durBuf[32];
        formatVideoHms(tDisplayed, posBuf, sizeof posBuf);
        formatVideoHms(metaDur, durBuf, sizeof durBuf);
        if (durRel) {
            ImGui::Text("%s / %s", posBuf, durBuf);
        } else {
            ImGui::Text("%s / ?", posBuf);
            ImGui::SameLine();
            ImGui::TextDisabled("(duration unknown)");
        }
    }

#if defined(BAKTSIU_DEBUG_BUILD)
    ImGui::Separator();
    if (ImGui::Checkbox("Log pipeline (console)##videopipe", &mVideoLogPipelineTiming)) {
        if (r) { r->setPipelineTimingLog(videoPipelineDbgEnabled(mVideoLogPipelineTiming)); }
        if (rB) { rB->setPipelineTimingLog(videoPipelineDbgEnabled(mVideoLogPipelineTiming)); }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("seek / decode+buf / upload ms");
    ImGui::SameLine();
    if (videoCompareActive()) {
        ImGui::TextDisabled("dt %.1fms sync %.1fms drift L %+0.1fms R %+0.1fms",
            static_cast<double>(mVideoDbgLastTickDtMs),
            static_cast<double>(mVideoDbgLastSyncMs),
            static_cast<double>(mVideoDbgLastDriftLMs),
            static_cast<double>(mVideoDbgLastDriftRMs));
    } else {
        ImGui::TextDisabled("dt %.1fms", static_cast<double>(mVideoDbgLastTickDtMs));
    }
#endif

    ImGui::End();
    ImGui::PopStyleVar();
#endif
}

}  // namespace baktsiu
