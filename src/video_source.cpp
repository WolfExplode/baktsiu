#include "video_source.h"

#include "common.h"

#include <GLFW/glfw3.h>

#if defined(_WIN32) && defined(USE_VIDEO)

#include <string>

namespace baktsiu
{

VideoSource::VideoSource() = default;

VideoSource::VideoSource(std::string path)
    : mFilepath(std::move(path))
{
}

VideoSource::~VideoSource()
{
    close();
}

bool VideoSource::open(GLFWwindow* window, const std::string& path)
{
    mFilepath = path;
    mPlayer = std::make_unique<MpvGlPlayer>();
    if (!mPlayer->open(window, path)) {
        mPlayer.reset();
        return false;
    }
    ensureTexture();
    return true;
}

void VideoSource::close()
{
    if (mPlayer) {
        mPlayer->close();
    }
    if (mTexture != 0) {
        glDeleteTextures(1, &mTexture);
        mTexture = 0;
    }
}

bool VideoSource::isOpen() const
{
    return mPlayer && mPlayer->isOpen();
}

Vec2f VideoSource::size() const
{
    if (!mPlayer || !mPlayer->isOpen()) {
        return Vec2f(1.0f);
    }
    return Vec2f(mPlayer->displayWidthForAspect(), mPlayer->displayHeightForAspect());
}

std::string VideoSource::filename() const
{
    if (mFilepath.empty()) {
        return "none";
    }
    const auto sep = mFilepath.find_last_of("/\\");
    return (sep == std::string::npos) ? mFilepath : mFilepath.substr(sep + 1);
}

std::string VideoSource::filepath() const
{
    return mFilepath.empty() ? "none" : mFilepath;
}

void VideoSource::ensureTexture()
{
    if (mTexture != 0) {
        glDeleteTextures(1, &mTexture);
        mTexture = 0;
    }
    if (!mPlayer || !mPlayer->isOpen()) {
        return;
    }
    glGenTextures(1, &mTexture);
    glBindTexture(GL_TEXTURE_2D, mTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const int w = mPlayer->width();
    const int h = mPlayer->height();
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
    LOGI("OpenGL video texture: {}x{} display {:.0f}x{:.0f} \"{}\"", w, h,
        static_cast<double>(mPlayer->displayWidthForAspect()),
        static_cast<double>(mPlayer->displayHeightForAspect()),
        filename());
}

void VideoSource::upload()
{
    if (mTexture == 0 || !mPlayer || !mPlayer->isOpen()) {
        return;
    }
    mPlayer->renderToTexture(mTexture);
}

// ---- MpvGlPlayer forwarding ----

int    VideoSource::width()  const { return mPlayer ? mPlayer->width()  : 0; }
int    VideoSource::height() const { return mPlayer ? mPlayer->height() : 0; }

float  VideoSource::displayWidthForAspect()  const { return mPlayer ? mPlayer->displayWidthForAspect()  : 1.0f; }
float  VideoSource::displayHeightForAspect() const { return mPlayer ? mPlayer->displayHeightForAspect() : 1.0f; }

double VideoSource::durationSec()         const { return mPlayer ? mPlayer->durationSec()         : 0.0; }
bool   VideoSource::hasReliableDuration() const { return mPlayer ? mPlayer->hasReliableDuration() : false; }
double VideoSource::positionSec()         const { return mPlayer ? mPlayer->positionSec()         : 0.0; }
double VideoSource::frameDurationSec()    const { return mPlayer ? mPlayer->frameDurationSec()    : 1.0 / 30.0; }

void VideoSource::seek(double seconds, bool flushStreams)
{
    if (mPlayer) { mPlayer->seek(seconds, flushStreams); }
}

bool VideoSource::decodeFrame(bool fastEventDrain)
{
    return mPlayer ? mPlayer->decodeFrame(fastEventDrain) : false;
}

bool VideoSource::decodeFrameThrough(double targetSec, int maxReadSamplesCap)
{
    return mPlayer ? mPlayer->decodeFrameThrough(targetSec, maxReadSamplesCap) : false;
}

bool VideoSource::lazyProbePresentationDuration()
{
    return mPlayer ? mPlayer->lazyProbePresentationDuration() : false;
}

void VideoSource::setPipelineTimingLog(bool on)
{
    if (mPlayer) { mPlayer->setPipelineTimingLog(on); }
}

float VideoSource::lastSeekMs()              const { return mPlayer ? mPlayer->lastSeekMs()              : 0.f; }
float VideoSource::lastSeekFlushMs()         const { return mPlayer ? mPlayer->lastSeekFlushMs()         : 0.f; }
float VideoSource::lastSeekSetPosMs()        const { return mPlayer ? mPlayer->lastSeekSetPosMs()        : 0.f; }
float VideoSource::lastDecodeThroughMs()     const { return mPlayer ? mPlayer->lastDecodeThroughMs()     : 0.f; }
int   VideoSource::lastDecodeThroughReads()  const { return mPlayer ? mPlayer->lastDecodeThroughReads()  : 0; }
bool  VideoSource::lastDecodeThroughHitCap() const { return mPlayer ? mPlayer->lastDecodeThroughHitCap() : false; }

}  // namespace baktsiu

#else  // !(_WIN32 && USE_VIDEO)

namespace baktsiu
{

VideoSource::VideoSource() = default;

VideoSource::VideoSource(std::string path)
    : mFilepath(std::move(path))
{
}

VideoSource::~VideoSource() = default;

bool VideoSource::open(GLFWwindow*, const std::string& path) { mFilepath = path; return false; }
void VideoSource::close()   {}
bool VideoSource::isOpen()  const { return false; }

Vec2f       VideoSource::size()     const { return Vec2f(1.0f); }
std::string VideoSource::filename() const { return "none"; }
std::string VideoSource::filepath() const { return mFilepath.empty() ? "none" : mFilepath; }

void VideoSource::ensureTexture() {}
void VideoSource::upload()        {}

int    VideoSource::width()  const { return 0; }
int    VideoSource::height() const { return 0; }
float  VideoSource::displayWidthForAspect()  const { return 1.0f; }
float  VideoSource::displayHeightForAspect() const { return 1.0f; }
double VideoSource::durationSec()            const { return 0.0; }
bool   VideoSource::hasReliableDuration()    const { return false; }
double VideoSource::positionSec()            const { return 0.0; }
double VideoSource::frameDurationSec()       const { return 1.0 / 30.0; }
void   VideoSource::seek(double, bool)       {}
bool   VideoSource::decodeFrame(bool)        { return false; }
bool   VideoSource::decodeFrameThrough(double, int) { return false; }
bool   VideoSource::lazyProbePresentationDuration() { return false; }
void   VideoSource::setPipelineTimingLog(bool)      {}
float  VideoSource::lastSeekMs()              const { return 0.f; }
float  VideoSource::lastSeekFlushMs()         const { return 0.f; }
float  VideoSource::lastSeekSetPosMs()        const { return 0.f; }
float  VideoSource::lastDecodeThroughMs()     const { return 0.f; }
int    VideoSource::lastDecodeThroughReads()  const { return 0; }
bool   VideoSource::lastDecodeThroughHitCap() const { return false; }

}  // namespace baktsiu

#endif  // _WIN32 && USE_VIDEO
