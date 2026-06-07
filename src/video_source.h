#ifndef BAKTSIU_VIDEO_SOURCE_H_
#define BAKTSIU_VIDEO_SOURCE_H_

#include "media_source.h"
#include "mpv_gl_player.h"

#include <memory>
#include <string>

struct GLFWwindow;

namespace baktsiu
{

// MediaSource backed by a libmpv video stream.
// Owns the MpvGlPlayer and the GL texture the player renders into.
// All methods follow the MpvGlPlayer convention: must be called from the GL thread.
class VideoSource : public MediaSource
{
public:
    VideoSource();
    ~VideoSource() override;

    VideoSource(const VideoSource&)            = delete;
    VideoSource& operator=(const VideoSource&) = delete;

    // Construct a stub (path stored, no decoder opened yet).
    explicit VideoSource(std::string path);

    bool open(GLFWwindow* window, const std::string& path);
    void close();
    bool isOpen() const;

    // ---- MediaSource interface ----
    GLuint      texId()    const override { return mTexture; }
    Vec2f       size()     const override;
    std::string filename() const override;
    std::string filepath() const override;
    uint8_t     id()       const override { return 0; }
    bool        isVideo()  const override { return true; }

    // ---- Texture lifecycle ----
    // (Re)create the GL texture to match the current player dimensions.
    void ensureTexture();
    // Render the latest decoded frame into the internal texture.
    void upload();

    // ---- Forward the full MpvGlPlayer API so existing call sites compile unchanged ----
    int    width()  const;
    int    height() const;
    float  displayWidthForAspect()  const;
    float  displayHeightForAspect() const;
    double durationSec()            const;
    bool   hasReliableDuration()    const;
    double positionSec()            const;
    double frameDurationSec()       const;
    void   seek(double seconds, bool flushStreams = true);
    bool   decodeFrame(bool fastEventDrain = false);
    bool   decodeFrameThrough(double targetSec, int maxReadSamplesCap = 0);
    bool   lazyProbePresentationDuration();
    void   setPipelineTimingLog(bool on);
    float  lastSeekMs()               const;
    float  lastSeekFlushMs()          const;
    float  lastSeekSetPosMs()         const;
    float  lastDecodeThroughMs()      const;
    int    lastDecodeThroughReads()   const;
    bool   lastDecodeThroughHitCap()  const;

private:
    std::unique_ptr<MpvGlPlayer> mPlayer;
    GLuint      mTexture  = 0;
    std::string mFilepath;
};

}  // namespace baktsiu

#endif  // BAKTSIU_VIDEO_SOURCE_H_
