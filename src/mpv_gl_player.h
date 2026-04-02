#ifndef BAKTSIU_MPV_GL_PLAYER_H_
#define BAKTSIU_MPV_GL_PLAYER_H_

#include <GL/gl3w.h>

#include <cstdint>
#include <string>

struct GLFWwindow;

namespace baktsiu
{

// libmpv + mpv_render_context (OpenGL). All methods must be called from the thread that owns the GLFW GL context.
class MpvGlPlayer
{
public:
    MpvGlPlayer();
    ~MpvGlPlayer();

    MpvGlPlayer(const MpvGlPlayer&) = delete;
    MpvGlPlayer& operator=(const MpvGlPlayer&) = delete;

    static bool isSupportedExtension(const std::string& filepath);

    bool open(GLFWwindow* window, const std::string& utf8Path);
    void close();
    bool isOpen() const;

    int width() const { return m_width; }
    int height() const { return m_height; }
    float displayWidthForAspect() const;
    float displayHeightForAspect() const;
    double durationSec() const { return m_durationSec; }
    bool hasReliableDuration() const { return m_durationReliable; }
    double positionSec() const { return m_positionSec; }
    double frameDurationSec() const { return m_frameDurationSec; }

    // Records flush preference for the next decodeFrameThrough (Media Foundation used seek+decode in two steps).
    void seek(double seconds, bool flushStreams = true);
    bool decodeFrame();
    bool decodeFrameThrough(double targetSec, int maxReadSamplesCap = 0);

    void renderToTexture(GLuint texture);

    bool lazyProbePresentationDuration();

    void setPipelineTimingLog(bool on);
    float lastSeekMs() const { return m_lastSeekMs; }
    float lastSeekFlushMs() const { return m_lastSeekFlushMs; }
    float lastSeekSetPosMs() const { return m_lastSeekSetPosMs; }
    float lastDecodeThroughMs() const { return m_lastDecodeThroughMs; }
    int lastDecodeThroughReads() const { return m_lastDecodeThroughReads; }
    bool lastDecodeThroughHitCap() const { return m_lastDecodeThroughHitCap; }

private:
    void destroyMpv();
    bool createRenderContext();
    bool waitFileLoaded(GLuint scratchRgbTex);
    void drainEvents(double timeoutSec);
    void refreshDuration();
    void refreshFrameDuration();
    void refreshTimePos();
    void refreshVideoGeometry();
    void applySeek(double targetSec, int maxReadSamplesCap);
    void pumpRenderContext();
    // Fixed-size FBO blit for open-time pumping when video-params/w*h are not ready yet.
    void renderToFbo(GLuint texture, int w, int h);
    static std::string normalizeFsPath(const std::string& utf8Path);

    GLFWwindow* m_window = nullptr;
    void* m_mpv = nullptr;
    void* m_mrc = nullptr;

    GLuint m_glFbo = 0;
    GLuint m_fboTex = 0;

    int m_width = 0;
    int m_height = 0;
    double m_durationSec = 0.0;
    bool m_durationReliable = false;
    double m_positionSec = 0.0;
    double m_frameDurationSec = 1.0 / 30.0;
    bool m_seekFlush = true;

    bool m_logTiming = false;
    float m_lastSeekMs = 0.f;
    float m_lastSeekFlushMs = 0.f;
    float m_lastSeekSetPosMs = 0.f;
    float m_lastDecodeThroughMs = 0.f;
    int m_lastDecodeThroughReads = 0;
    bool m_lastDecodeThroughHitCap = false;
};

}  // namespace baktsiu

#endif
