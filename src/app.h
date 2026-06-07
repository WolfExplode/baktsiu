#ifndef BAKTSIU_APP_H_
#define BAKTSIU_APP_H_

#include "common.h"
#include "media_source.h"
#include "shader.h"
#include "static_image_source.h"
#include "texture.h"
#include "texture_pool.h"
#include "video_source.h"
#include "view.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

struct  GLFWwindow;
struct  ImFont;
struct  ImGuiIO;

namespace baktsiu
{

// Basic entity of command stack. It stores relevant data for undo instructions.
struct Action
{
    static uint8_t extractImageId(uint16_t value) {
        return (value & 0xFF00) >> 8;
    }

    static uint8_t extractLayerIndex(uint16_t value) {
        return value & 0xFF;
    }

    static uint16_t composeImageIndex(uint8_t id, uint8_t order) {
        return order| id << 8;
    }

    enum class Type : char
    {
        Unknown,
        Add,
        Remove,
        Move,
    };

    Action() 
    {
        reset();
    }

    Action(Type t, int topImageIdx, int cmpImageIdx) 
        : type(t), prevTopImageIdx(topImageIdx), prevCmpImageIdx(cmpImageIdx)
    {
    }

    void reset() 
    {
        type = Type::Unknown;
        filepathArray.clear();
        imageIdxArray.clear();
        prevTopImageIdx = prevCmpImageIdx = -1;
    }

    Type type = Type::Unknown;
    std::vector<std::string>    filepathArray;
    std::vector<uint16_t>       imageIdxArray;
    int prevTopImageIdx = -1;
    int prevCmpImageIdx = -1;
};


// Flags of the composition of image viewport.
enum class CompositeFlags : char
{
    Top         = 0x0,  // Display top (the selected) image only.
    Split       = 0x1,  // Scaled the compared image and put it underneath the top image.
    SideBySide  = 0x2,  // Display top and compared images side by side.
};

ENUM_CLASS_OPERATORS(CompositeFlags);

enum class PixelMarkerFlags : char
{
    None        = 0x00,
    Difference  = 0x01, // Draw pixel differences with a ramp of magenta.
    DiffHeatMap = 0x02, // Draw pixel differences in color ramp.
    DiffMask    = 0x03,
    Overflow    = 0x04, // Draw overflow pixels in red
    Underflow   = 0x08, // Draw underflow pixels in blue
    Default     = Difference | Overflow | Underflow,
};

ENUM_CLASS_OPERATORS(PixelMarkerFlags);


// The class of viewer functionalities.
//
// It contains two display layers on viewport. Top image is the one selected in 
// image property window at right hand side. In compare mode, we could move the
// splitter to swipe top image.
class App
{
public:
    static const char* kImagePropWindowName;
    static const char* kImageRemoveDlgTitle;
    static const char* kClearImagesDlgTitle;

public:
    bool    initialize(const char* title, int width, int height);

    void    importImageFiles(const std::vector<std::string>& filepathArray, 
                bool recordAction, std::vector<uint16_t>* imageIdxArray = nullptr);

    void    openVideoFile(const std::string& filepath);

    void    openVideoCompare(const std::string& leftPath, const std::string& rightPath);

    // Add every path to the media list (order preserved), set L/R to the first two distinct items, open.
    void    openVideoPaths(const std::vector<std::string>& paths);

    void    run(CompositeFlags initFlags = CompositeFlags::Top);

    void    release();

private:
    void    setThemeColors();

    void    initLogger();

    // Initialize related bitmap and uv info for text.
    void    initDigitCharData(const unsigned char* data);

    void    renderToolbar();

    // Create toggle handle for the property panel.
    // Returns true if the handle is hovered by the mouse cursor.
    bool    renderPropWindowHandle(float handleWidth, bool& showPropWindow);

    void    renderPropertiesPanel();

    void    renderFooter();

    void    renderInfoPopup(const char* name);

    void    showImageProperties();

    void    showVideoProperties();

    // Show image name overlays viewport.
    void    showImageNameOverlays();

    void    showHeatRangeOverlay(const Vec2f& pos, float width);

    bool    showRemoveImageDlg(const char *name);

    bool    showClearImagesDlg(const char* title);

    void    showImportImageDlg();

    void    showExportSessionDlg();

    // Handle key pressed cases.
    // @note Few key pressed events related to image transformation are handled in updateImageTransform.
    void    onKeyPressed(const ImGuiIO&);

    void    updateImagePairFromPressedKeys();

    // Resize window once to the first successfully loaded media aspect ratio.
    void    applyInitialWindowAspectFromSize(float mediaW, float mediaH);

    // Update image transform from user's key stroke and mouse input.
    void    updateImageTransform(const ImGuiIO&, bool useColumnView);

    void    updateImageSplitterPos(ImGuiIO&);

    // Dispatch compute kernels for image statistics (histGpuTex + histCpu are a matched pair per stream).
    void    computeImageStatistics(
        const RenderTexture& texture, float valueScale, GLuint histGpuTex, std::array<int, 768>& histCpu);

    void computeVideoTextureHistogram(
        GLuint rgba8Tex, int width, int height, GLuint histGpuTex, std::array<int, 768>& histCpu);

    // Reset image transform to viewport center.
    void    resetImageTransform(const Vec2f& imgSize, bool fitWindow = false);

    // Synchronize local offset of column views.
    void    syncSideBySideView(const ImGuiIO& io);
    
    // Return pixel coordinates from mouse position.
    bool    getImageCoordinates(Vec2f viewportCoords, Vec2f& outImageCoords) const;

    // Compare: side 0 = left layer (uImage1), 1 = right (uImage2). Uses same ref/contain as present.frag.
    bool    getCompareSideImagePixelAtMouse(Vec2f imguiMousePos, int side, Vec2f& outImagePx) const;

    void    appendAction(Action&& action);

    void    undoAction();

    StaticImageSource*       getTopImage();
    const StaticImageSource* getTopImage() const;
    StaticImageSource*       getCmpImage();
    const StaticImageSource* getCmpImage() const;

    void    removeTopImage(bool recordAction);

    void    clearImages(bool recordAction);

    void    processTextureUploadTasks();

    void    onFileDrop(int count, const char* filepaths[]);

    // Open compare session.
    void    openSession(const std::string& filepath);

    // Save compare session with file extension .bts
    void    saveSession(const std::string& filepath);

    void    gradingTexImage(StaticImageSource& image, int renderTexIdx);

    // Return the width of property window at right hand side.
    float   getPropWindowWidth() const;

    int     getPixelMarkerFlags() const;

    bool    inCompareMode() const;

    bool    inSideBySideMode() const;

    bool    shouldShowSplitter() const;

    void    toggleSplitView();

    void    toggleSideBySideView();

    void    exitVideoMode();

    // Return the active left/right VideoSource from mMediaList, or null if not video mode.
    VideoSource*       videoSource();
    const VideoSource* videoSource() const;
    VideoSource*       videoSourceB();
    const VideoSource* videoSourceB() const;
    bool               isVideoMode() const;

    int     addVideoPath(const std::string& filepath);

    void    openVideosFromSelection();

    // After addVideoPath batch: in Top or SideBySide, focus list rows on the new paths; Split keeps L/R from fill logic.
    void    applyImportedVideoListSelection(const std::vector<int>& addedListIndices);

    void    renderVideoTransportBar(const ImGuiIO& io);

    void    tickAndUploadVideoFrame(float deltaTime);

    void    renderVideoBlit(const ImGuiIO& io);

    void    uploadVideoTexture();

    bool    videoCompareActive() const;

    // Match renderVideoBlit: drawable content rect below toolbar / above transport + footer.
    void    getVideoBlitContentLayout(const ImGuiIO& io, Vec2f& outContentOrigin, Vec2f& outContentSize) const;
    bool    getSingleVideoPixelAtMouse(const ImGuiIO& io, Vec2f& outImagePx) const;
    bool    getCompareSideVideoPixelAtMouse(const ImGuiIO& io, int side, Vec2f& outImagePx) const;

    void    syncVideoViewsLayout(const ImGuiIO& io);
    void    resetVideoViewTransform(bool fitWindow = false);
    Vec2f   videoRefDisplaySizeForLayout() const;

    void    recomputeVideoScrubBounds(double& outMin, double& outMax) const;

    void    clampVideoCompositionT();

    // maxDecodeReadCapPerStream 0 = full decodeFrameThrough budget; >0 caps ReadSample count per stream (scrub preview).
    void    syncVideoDecodersToCompositionT(int maxDecodeReadCapPerStream = 0);

    void    resetVideoDecoderSyncCache();

    // Video scrub: direction -1 = earlier, +1 = later.
    void    stepVideoScrubFiveSeconds(int direction);
    void    stepVideoScrubOneFrame(int direction);

    void    swapVideoSides();

    void    uploadVideoTextureB();

    using MediaUPtr = std::unique_ptr<MediaSource>;

    std::vector<MediaUPtr>      mMediaList;
    std::deque<Action>          mActionStack;
    Action                      mCurAction;
    TexturePool                 mTexturePool;

    GLFWwindow*     mWindow = nullptr;
    ImFont*         mSmallFont = nullptr;
    ImFont*         mSmallIconFont = nullptr;
    GLuint          mFontTexture = 0;
    
    std::vector<Vec4f> mCharUvRanges;   // UV bbox of each digit in font texture.
    std::vector<Vec4f> mCharUvXforms;   // UV offset of each digit in font texture.

    RenderTexture   mRenderTextures[2];   // The intermediate output for input image.
    int             mTopImageRenderTexIdx = 0;

    Shader          mGradingShader;
    Shader          mPresentShader;
    Shader          mStatisticsShader;
#if defined(USE_VIDEO)
    Shader          mVideoStatisticsShader;
    bool            mVideoRgbHistogramComputeReady = false;
#endif
    GLuint          mTexHistogram = 0;
    GLuint          mTexHistogramB = 0;
    Sampler         mPointSampler;
    
    std::array<int, 768> mHistogram;
    std::array<int, 768> mHistogramB;

    CompositeFlags      mCompositeFlags = CompositeFlags::Top;
    PixelMarkerFlags    mPixelMarkerFlags = PixelMarkerFlags::Default;

    // Image / video transformation (unified — video uses the same mView/mColumnViews)
    View        mView;
    View        mColumnViews[2];    // Views for side by side mode.
    float       mImageScale = 1.0f;
    float       mPrevScale = -1.0f;     // pre-sniper scale; shared between image and video modes
    // Fit reset must run after syncVideoViewsLayout() so bottom padding includes the transport bar.
    bool        mPendingViewFitReset = false;

    int         mTopImageIndex = -1;
    int         mCmpImageIndex = -1;

    int         mCurrentPresentMode = 0;
    int         mOutTransformType = 0;
    char        mImageScaleInfo[64];

    float       mToolbarHeight;
    float       mFooterHeight;

    float       mViewSplitPos = 0.5f;   // The horizontal position of viewport splitter.
    float       mPropWindowHSplitRatio = 0.65f;

    Vec3f       mPixelBorderHighlightColor = Vec3f(0.153f, 0.980f, 0.718f);
    float       mDisplayGamma = 2.2f;
    float       mExposureValue = 0.0f;

    // RMB window-drag state (was static in run())
    bool        mRmbWindowDragActive = false;
    double      mRmbDragStartCursorScreenX = 0.0;
    double      mRmbDragStartCursorScreenY = 0.0;
    int         mRmbDragStartWindowX = 0;
    int         mRmbDragStartWindowY = 0;

    bool        mIsMovingSplitter = false;
    bool        mIsScalingImage = false;
    bool        mCtrlMmbZoomPivotLocked = false;
    Vec2f       mCtrlMmbZoomPivot = Vec2f(-1.0f);
    bool        mCtrlMmbZoomRightColumn = false;
    bool        mEnableToneMapping = false;
    bool        mShowPixelValues = false;
    bool        mAboutToTerminate = false;

    // Pixel-sniper zoom
    bool        mIsInSniperMode = false;

    // Sub-pixel pan residual for Alt+MMB pixel-snap in column views
    Vec2f       mPanResidual = Vec2f(0.0f);

    // Auto-centering toolbar width measurement (was static in renderToolbar)
    float       mCenteredToolbarWidth = 506.0f;

    // Image-list drag-to-reorder (was static in renderPropertiesPanel)
    bool        mIsDraggingImageItem = false;
    std::unique_ptr<Action> mMoveAction;

    // Footer file-size cache: avoid stat() on every frame
    std::string     mFooterCachePathVL, mFooterCachePathVR;
    std::int64_t    mFooterCacheBytesVL = -1;
    std::int64_t    mFooterCacheBytesVR = -1;
    std::string     mFooterCachePathVSingle;
    std::int64_t    mFooterCacheBytesVSingle = -1;
    std::string     mFooterCachePathIL, mFooterCachePathIR;
    std::int64_t    mFooterCacheBytesIL = -1;
    std::int64_t    mFooterCacheBytesIR = -1;

    bool        mShowImagePropWindow = false;
    bool        mPopupImagePropWindow = false;
    bool        mUseLinearFilter = false;
    bool        mShowImageNameOverlay = true;
    bool        mShowPixelMarker = false;
    bool        mSupportComputeShader = false;
    bool        mUpdateImageSelection = false;

    // mTopImageIndex / mCmpImageIndex serve double duty:
    //   image mode  → index into mMediaList (StaticImageSource)
    //   video mode  → index into mMediaList (VideoSource, left / right decoder)
    // isVideoMode() / videoSource() / videoSourceB() encapsulate the distinction.

    bool        mVideoPlaying = false;
    bool        mInitialMediaAspectApplied = false;
    // Defer lazy duration probe so the first paint after open is not blocked by a full-stream read.
    int         mVideoLazyDurationGraceFrames = 0;
    double      mVideoPlaybackTimeBank = 0.0;
    double      mVideoComparePlaybackBank = 0.0;
    double      mVideoCompareBankL = 0.0;
    double      mVideoCompareBankR = 0.0;
    double      mVideoScrubValue = 0.0;
    double      mVideoCompositionT = 0.0;
    double      mVideoStartL = 0.0;
    double      mVideoStartR = 0.0;
    // Last targets used in syncVideoDecodersToCompositionT (skip redundant seek/decode when unchanged).
    double      mVideoLastSyncedTargetMediaL = -1.0;
    double      mVideoLastSyncedTargetMediaR = -1.0;
    // ImGui::GetTime() of last scrub seek+decode; throttles decode work while dragging the slider.
    double      mVideoLastScrubDecodeTime = -1.0e9;
    // Snapshot when a scrub drag begins; used to resume playback after release if it was playing.
    bool        mVideoPlayingBeforeScrub = false;
    // Right-drag scrub on the video viewport (not the transport slider).
    bool        mViewportRmbScrubActive = false;
    bool        mVideoResumePlaybackAfterViewportScrub = false;
    bool        mVideoLogPipelineTiming = false;
    float       mVideoDbgLastUploadLMs = 0.f;
    float       mVideoDbgLastUploadRMs = 0.f;
    float       mVideoDbgLastSyncMs = 0.f;
    float       mVideoDbgLastTickDtMs = 0.f;
    float       mVideoDbgLastDriftLMs = 0.f;
    float       mVideoDbgLastDriftRMs = 0.f;
    double      mVideoDbgLastConsoleBeatSec = -1.0e9;
    // Presentation-only left/right swap: renderer sees decoders flipped, but mTopImageIndex /
    // mCmpImageIndex keep pointing at the physical decoder slots (no reopen required).
    bool        mVideoSwapPresentationLR = false;
    // Vertical flip (invert along horizontal midline) for on-screen left / right media.
    bool        mFlipPresentMediaVert[2] = {false, false};
    // Horizontal flip (mirror along vertical midline) for on-screen left / right media.
    bool        mFlipPresentMediaHorz[2] = {false, false};
    Shader      mVideoBlitShader;
    bool        mVideoShaderReady = false;
};

}  // namespace baktsiu
#endif // BAKTSIU_APP_H_