#include "mpv_gl_player.h"

#include "common.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32) && defined(USE_VIDEO)

#include <chrono>

extern "C" {
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>
}

namespace baktsiu
{
namespace
{
constexpr double kOpenTimeoutSec = 60.0;

const char* mpvEventIdLabel(mpv_event_id id)
{
    switch (id) {
    case MPV_EVENT_NONE:
        return "NONE";
    case MPV_EVENT_SHUTDOWN:
        return "SHUTDOWN";
    case MPV_EVENT_LOG_MESSAGE:
        return "LOG_MESSAGE";
    case MPV_EVENT_START_FILE:
        return "START_FILE";
    case MPV_EVENT_END_FILE:
        return "END_FILE";
    case MPV_EVENT_FILE_LOADED:
        return "FILE_LOADED";
    case MPV_EVENT_IDLE:
        return "IDLE";
    case MPV_EVENT_TICK:
        return "TICK";
    case MPV_EVENT_VIDEO_RECONFIG:
        return "VIDEO_RECONFIG";
    case MPV_EVENT_AUDIO_RECONFIG:
        return "AUDIO_RECONFIG";
    case MPV_EVENT_PROPERTY_CHANGE:
        return "PROPERTY_CHANGE";
    default:
        return "OTHER";
    }
}

void* mpvGlGetProcAddress(void* /*ctx*/, const char* name)
{
    return reinterpret_cast<void*>(glfwGetProcAddress(name));
}

}  // namespace

MpvGlPlayer::MpvGlPlayer() = default;

MpvGlPlayer::~MpvGlPlayer()
{
    close();
}

void MpvGlPlayer::destroyMpv()
{
    if (m_mrc) {
        mpv_render_context_free(static_cast<mpv_render_context*>(m_mrc));
        m_mrc = nullptr;
    }
    if (m_mpv) {
        mpv_destroy(static_cast<mpv_handle*>(m_mpv));
        m_mpv = nullptr;
    }
    if (m_glFbo != 0) {
        glDeleteFramebuffers(1, &m_glFbo);
        m_glFbo = 0;
    }
    m_fboTex = 0U;
    m_window = nullptr;
    m_width = m_height = 0;
    m_durationSec = 0.0;
    m_durationReliable = false;
    m_positionSec = 0.0;
    m_frameDurationSec = 1.0 / 30.0;
}

std::string MpvGlPlayer::normalizeFsPath(const std::string& utf8Path)
{
    std::string s = utf8Path;
    for (char& c : s) {
        if (c == '\\') {
            c = '/';
        }
    }
    return s;
}

bool MpvGlPlayer::isSupportedExtension(const std::string& filepath)
{
    static const char* kExt[] = {".mp4",  ".m4v",  ".mov",  ".webm", ".mkv",  ".avi",  ".wmv",
                                 ".flv",  ".mpg",  ".mpeg", ".ogv",  ".ts",   ".m2ts", ".3gp",
                                 ".asf",  ".divx", ".xvid", ".y4m"};
    const std::size_t dot = filepath.rfind('.');
    if (dot == std::string::npos) {
        return false;
    }
    std::string ext = filepath.substr(dot);
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    for (const char* e : kExt) {
        if (ext == e) {
            return true;
        }
    }
    return false;
}

bool MpvGlPlayer::createRenderContext()
{
    auto* mpv = static_cast<mpv_handle*>(m_mpv);
    mpv_opengl_init_params glInit = {mpvGlGetProcAddress, nullptr};
    mpv_render_param params[] = {
        {static_cast<mpv_render_param_type>(MPV_RENDER_PARAM_API_TYPE),
            const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
        {static_cast<mpv_render_param_type>(MPV_RENDER_PARAM_OPENGL_INIT_PARAMS), &glInit},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    mpv_render_context* mrc = nullptr;
    if (mpv_render_context_create(&mrc, mpv, params) < 0) {
        m_mrc = nullptr;
        return false;
    }
    m_mrc = mrc;
    return true;
}

void MpvGlPlayer::drainEvents(double timeoutSec)
{
    auto* mpv = static_cast<mpv_handle*>(m_mpv);
    if (!mpv) {
        return;
    }
    const double t0 = glfwGetTime();
    for (;;) {
        const double elapsed = glfwGetTime() - t0;
        const double remain = timeoutSec - elapsed;
        const double wait = (remain > 0.05) ? 0.05 : ((remain > 0.0) ? remain : 0.0);
        mpv_event* ev = mpv_wait_event(mpv, wait);
        if (ev->event_id == MPV_EVENT_NONE) {
            if (elapsed >= timeoutSec) {
                break;
            }
            continue;
        }
        if (ev->event_id == MPV_EVENT_SHUTDOWN) {
            break;
        }
    }
}

void MpvGlPlayer::pumpRenderContext()
{
    auto* mrc = static_cast<mpv_render_context*>(m_mrc);
    if (mrc) {
        (void)mpv_render_context_update(mrc);
    }
}

bool MpvGlPlayer::waitFileLoaded(GLuint scratchRgbTex)
{
    auto* mpv = static_cast<mpv_handle*>(m_mpv);
    if (!mpv) {
        return false;
    }
    const double t0 = glfwGetTime();
    int nonNoneCount = 0;
    const char* lastNonNone = "(none)";
    double lastBeat = t0;
    while (glfwGetTime() - t0 < kOpenTimeoutSec) {
        pumpRenderContext();
        if (scratchRgbTex != 0U) {
            renderToFbo(scratchRgbTex, 640, 360);
        }
        mpv_event* ev = mpv_wait_event(mpv, 0.2);
        if (ev->event_id == MPV_EVENT_NONE) {
            const double now = glfwGetTime();
            if (now - lastBeat >= 1.0) {
                lastBeat = now;
                LOGI(
                    "[mpv-open] wait FILE_LOADED: {:.1f}s elapsed (non-none events: {}, last: {}, scratchTex={})",
                    now - t0, nonNoneCount, lastNonNone, scratchRgbTex != 0U ? 1 : 0);
            }
            continue;
        }
        lastNonNone = mpvEventIdLabel(ev->event_id);
        ++nonNoneCount;
        if (nonNoneCount <= 16) {
            LOGI("[mpv-open] mpv event #{}: {} ({})", nonNoneCount, lastNonNone, static_cast<int>(ev->event_id));
        }
        if (ev->event_id == MPV_EVENT_FILE_LOADED) {
            LOGI("[mpv-open] FILE_LOADED after {:.3f}s ({} non-none events)", glfwGetTime() - t0, nonNoneCount);
            return true;
        }
        if (ev->event_id == MPV_EVENT_END_FILE) {
            auto* ef = static_cast<mpv_event_end_file*>(ev->data);
            if (ef && ef->reason != MPV_END_FILE_REASON_REDIRECT) {
                LOGW("[mpv-open] END_FILE reason={} after {:.3f}s", static_cast<int>(ef->reason), glfwGetTime() - t0);
                return false;
            }
        }
        if (ev->event_id == MPV_EVENT_SHUTDOWN) {
            LOGW("[mpv-open] SHUTDOWN while waiting for load after {:.3f}s", glfwGetTime() - t0);
            return false;
        }
    }
    LOGW("[mpv-open] timeout waiting FILE_LOADED ({:.1f}s, {} non-none, last: {})", kOpenTimeoutSec, nonNoneCount,
        lastNonNone);
    return false;
}

void MpvGlPlayer::refreshDuration()
{
    auto* mpv = static_cast<mpv_handle*>(m_mpv);
    if (!mpv) {
        return;
    }
    double d = 0.0;
    if (mpv_get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &d) >= 0 && std::isfinite(d) && d > 0.001) {
        m_durationSec = d;
        m_durationReliable = true;
    } else {
        m_durationSec = 0.0;
        m_durationReliable = false;
    }
}

void MpvGlPlayer::refreshFrameDuration()
{
    auto* mpv = static_cast<mpv_handle*>(m_mpv);
    if (!mpv) {
        return;
    }
    double fps = 0.0;
    if (mpv_get_property(mpv, "container-fps", MPV_FORMAT_DOUBLE, &fps) >= 0 && std::isfinite(fps)
        && fps > 0.5 && fps < 500.0) {
        m_frameDurationSec = 1.0 / fps;
    } else {
        m_frameDurationSec = 1.0 / 30.0;
    }
}

void MpvGlPlayer::refreshTimePos()
{
    auto* mpv = static_cast<mpv_handle*>(m_mpv);
    if (!mpv) {
        return;
    }
    double t = 0.0;
    if (mpv_get_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &t) >= 0 && std::isfinite(t)) {
        m_positionSec = t;
    }
}

void MpvGlPlayer::refreshVideoGeometry()
{
    auto* mpv = static_cast<mpv_handle*>(m_mpv);
    if (!mpv) {
        return;
    }
    int64_t dw = 0;
    int64_t dh = 0;
    int64_t w = 0;
    int64_t h = 0;
    if (mpv_get_property(mpv, "video-params/dw", MPV_FORMAT_INT64, &dw) >= 0 && dw > 0
        && mpv_get_property(mpv, "video-params/dh", MPV_FORMAT_INT64, &dh) >= 0 && dh > 0) {
        m_width = static_cast<int>(dw);
        m_height = static_cast<int>(dh);
        return;
    }
    if (mpv_get_property(mpv, "video-params/w", MPV_FORMAT_INT64, &w) >= 0 && w > 0
        && mpv_get_property(mpv, "video-params/h", MPV_FORMAT_INT64, &h) >= 0 && h > 0) {
        m_width = static_cast<int>(w);
        m_height = static_cast<int>(h);
    }
}

bool MpvGlPlayer::open(GLFWwindow* window, const std::string& utf8Path)
{
    namespace chrono = std::chrono;
    const auto wallOpen0 = chrono::steady_clock::now();

    close();
    if (!window || utf8Path.empty()) {
        return false;
    }
    m_window = window;

    const std::string path = normalizeFsPath(utf8Path);
    LOGI("[mpv-open] begin \"{}\"", path);

    mpv_handle* mpv = mpv_create();
    if (!mpv) {
        LOGW("[mpv-open] mpv_create failed");
        return false;
    }
    m_mpv = mpv;

    mpv_set_option_string(mpv, "terminal", "no");
    mpv_set_option_string(mpv, "msg-level", "all=no");
    mpv_set_option_string(mpv, "vo", "libmpv");
    mpv_set_option_string(mpv, "audio", "no");
    mpv_set_option_string(mpv, "ao", "null");
    mpv_set_option_string(mpv, "pause", "yes");
    mpv_set_option_string(mpv, "keep-open", "yes");
    mpv_set_option_string(mpv, "hwdec", "auto-safe");

    if (mpv_initialize(mpv) < 0) {
        LOGW("[mpv-open] mpv_initialize failed");
        destroyMpv();
        return false;
    }

    if (!createRenderContext()) {
        LOGW("[mpv-open] mpv_render_context_create failed");
        destroyMpv();
        return false;
    }
    GLuint scratchRgb = 0U;
    glGenTextures(1, &scratchRgb);
    if (scratchRgb != 0U) {
        glBindTexture(GL_TEXTURE_2D, scratchRgb);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 640, 360, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    const char* cmd[] = {"loadfile", path.c_str(), "replace", nullptr};
    if (mpv_command(mpv, cmd) < 0) {
        LOGW("[mpv-open] loadfile command failed");
        if (scratchRgb != 0U) {
            glDeleteTextures(1, &scratchRgb);
        }
        destroyMpv();
        return false;
    }
    LOGI("[mpv-open] loadfile issued, waiting...");

    if (!waitFileLoaded(scratchRgb)) {
        if (scratchRgb != 0U) {
            glDeleteTextures(1, &scratchRgb);
        }
        destroyMpv();
        return false;
    }

    const auto tGeom0 = chrono::steady_clock::now();
    int geomItersUsed = 200;
    for (int i = 0; i < 200; ++i) {
        pumpRenderContext();
        if (scratchRgb != 0U) {
            renderToFbo(scratchRgb, 640, 360);
        }
        refreshDuration();
        refreshFrameDuration();
        refreshVideoGeometry();
        if (m_width > 0 && m_height > 0) {
            geomItersUsed = i + 1;
            break;
        }
        if (i == 0 || i == 50 || i == 100 || i == 199) {
            LOGI("[mpv-open] geometry probe iter {}: size {}x{} durRel={} {:.3f}s", i, m_width, m_height,
                m_durationReliable ? 1 : 0,
                chrono::duration<double>(chrono::steady_clock::now() - tGeom0).count());
        }
        drainEvents(0.05);
    }

    if (scratchRgb != 0U) {
        glDeleteTextures(1, &scratchRgb);
    }

    const bool usedGeomFallback = (m_width <= 0 || m_height <= 0);
    if (usedGeomFallback) {
        m_width = 640;
        m_height = 360;
        LOGW("[mpv-open] video-params never appeared; using fallback 640x360 after {} iters ({:.3f}s)", geomItersUsed,
            chrono::duration<double>(chrono::steady_clock::now() - tGeom0).count());
    } else {
        LOGI("[mpv-open] geometry ok {}x{} after {} iters ({:.3f}s)", m_width, m_height, geomItersUsed,
            chrono::duration<double>(chrono::steady_clock::now() - tGeom0).count());
    }

    refreshTimePos();
    LOGI("[mpv-open] done in {:.3f}s — duration={:.3f}s reliable={} pos={:.3f}s", //
        chrono::duration<double>(chrono::steady_clock::now() - wallOpen0).count(), m_durationSec,
        m_durationReliable ? 1 : 0, m_positionSec);
    return true;
}

void MpvGlPlayer::close()
{
    destroyMpv();
}

bool MpvGlPlayer::isOpen() const
{
    return m_mpv != nullptr;
}

float MpvGlPlayer::displayWidthForAspect() const
{
    return static_cast<float>((std::max)(m_width, 1));
}

float MpvGlPlayer::displayHeightForAspect() const
{
    return static_cast<float>((std::max)(m_height, 1));
}

void MpvGlPlayer::seek(double /*seconds*/, bool flushStreams)
{
    m_seekFlush = flushStreams;
}

void MpvGlPlayer::applySeek(double targetSec, int maxReadSamplesCap)
{
    auto* mpv = static_cast<mpv_handle*>(m_mpv);
    if (!mpv) {
        return;
    }
    namespace chrono = std::chrono;

    const bool exact = m_seekFlush && maxReadSamplesCap <= 0;
    char tbuf[64];
    std::snprintf(tbuf, sizeof tbuf, "%.6f", targetSec);
    const char* mode = exact ? "exact" : "keyframes";
    const char* args[] = {"seek", tbuf, "absolute", mode, nullptr};

    if (!m_logTiming) {
        mpv_command(mpv, args);
        drainEvents(0.05);
        m_lastSeekMs = 0.f;
        m_lastSeekFlushMs = 0.f;
        m_lastSeekSetPosMs = 0.f;
        return;
    }

    const auto tCmd0 = chrono::steady_clock::now();
    mpv_command(mpv, args);
    const auto tCmd1 = chrono::steady_clock::now();
    drainEvents(0.05);
    const auto tDrain1 = chrono::steady_clock::now();

    m_lastSeekSetPosMs = chrono::duration<float, std::milli>(tCmd1 - tCmd0).count();
    m_lastSeekFlushMs = chrono::duration<float, std::milli>(tDrain1 - tCmd1).count();
    m_lastSeekMs = m_lastSeekSetPosMs + m_lastSeekFlushMs;
}

bool MpvGlPlayer::decodeFrameThrough(double targetSec, int maxReadSamplesCap)
{
    if (!m_mpv) {
        return false;
    }
    m_lastDecodeThroughHitCap = maxReadSamplesCap > 0;
    m_lastDecodeThroughReads = 1;
    applySeek(targetSec, maxReadSamplesCap);

    namespace chrono = std::chrono;
    const auto t0 = m_logTiming ? chrono::steady_clock::now() : chrono::steady_clock::time_point {};
    refreshTimePos();
    refreshVideoGeometry();
    if (m_logTiming) {
        m_lastDecodeThroughMs =
            chrono::duration<float, std::milli>(chrono::steady_clock::now() - t0).count();
    } else {
        m_lastDecodeThroughMs = 0.f;
    }
    return true;
}

bool MpvGlPlayer::decodeFrame()
{
    auto* mpv = static_cast<mpv_handle*>(m_mpv);
    if (!mpv) {
        return false;
    }
    int eof = 0;
    if (mpv_get_property(mpv, "eof-reached", MPV_FORMAT_FLAG, &eof) >= 0 && eof) {
        return false;
    }
    if (mpv_command_string(mpv, "frame-step") < 0) {
        return false;
    }
    drainEvents(0.05);
    refreshTimePos();
    return true;
}

void MpvGlPlayer::renderToFbo(GLuint texture, int w, int h)
{
    auto* mrc = static_cast<mpv_render_context*>(m_mrc);
    auto* mpv = static_cast<mpv_handle*>(m_mpv);
    if (!mrc || !mpv || texture == 0 || w <= 0 || h <= 0) {
        return;
    }

    GLint prevFbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevFbo);

    if (m_glFbo == 0) {
        glGenFramebuffers(1, &m_glFbo);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_glFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    m_fboTex = texture;
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
        return;
    }

    mpv_opengl_fbo fbo = {static_cast<int>(m_glFbo), w, h, static_cast<int>(GL_RGBA8)};
    int flipY = (fbo.fbo == 0) ? 1 : 0;
    mpv_render_param rparams[] = {
        {static_cast<mpv_render_param_type>(MPV_RENDER_PARAM_OPENGL_FBO), &fbo},
        {static_cast<mpv_render_param_type>(MPV_RENDER_PARAM_FLIP_Y), &flipY},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };

    (void)mpv_render_context_update(mrc);
    mpv_render_context_render(mrc, rparams);
    refreshTimePos();

    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
}

void MpvGlPlayer::renderToTexture(GLuint texture)
{
    renderToFbo(texture, m_width, m_height);
}

bool MpvGlPlayer::lazyProbePresentationDuration()
{
    return false;
}

void MpvGlPlayer::setPipelineTimingLog(bool on)
{
    m_logTiming = on;
}

}  // namespace baktsiu

#else  // !(_WIN32 && USE_VIDEO)

namespace baktsiu
{

MpvGlPlayer::MpvGlPlayer() = default;

MpvGlPlayer::~MpvGlPlayer()
{
    close();
}

void MpvGlPlayer::destroyMpv()
{
    if (m_glFbo != 0) {
        glDeleteFramebuffers(1, &m_glFbo);
        m_glFbo = 0;
    }
    m_fboTex = 0U;
    m_window = nullptr;
    m_mpv = nullptr;
    m_mrc = nullptr;
    m_width = m_height = 0;
    m_durationSec = 0.0;
    m_durationReliable = false;
    m_positionSec = 0.0;
    m_frameDurationSec = 1.0 / 30.0;
}

std::string MpvGlPlayer::normalizeFsPath(const std::string& utf8Path)
{
    return utf8Path;
}

bool MpvGlPlayer::isSupportedExtension(const std::string& /*filepath*/)
{
    return false;
}

bool MpvGlPlayer::createRenderContext()
{
    return false;
}

void MpvGlPlayer::drainEvents(double /*timeoutSec*/) {}

bool MpvGlPlayer::waitFileLoaded(GLuint /*scratchRgbTex*/)
{
    return false;
}

void MpvGlPlayer::refreshDuration() {}

void MpvGlPlayer::refreshFrameDuration() {}

void MpvGlPlayer::refreshTimePos() {}

void MpvGlPlayer::refreshVideoGeometry() {}

bool MpvGlPlayer::open(GLFWwindow* /*window*/, const std::string& /*utf8Path*/)
{
    return false;
}

void MpvGlPlayer::close()
{
    destroyMpv();
}

bool MpvGlPlayer::isOpen() const
{
    return false;
}

float MpvGlPlayer::displayWidthForAspect() const
{
    return 1.0f;
}

float MpvGlPlayer::displayHeightForAspect() const
{
    return 1.0f;
}

void MpvGlPlayer::seek(double /*seconds*/, bool flushStreams)
{
    m_seekFlush = flushStreams;
}

void MpvGlPlayer::applySeek(double /*targetSec*/, int /*maxReadSamplesCap*/) {}

bool MpvGlPlayer::decodeFrameThrough(double /*targetSec*/, int /*maxReadSamplesCap*/)
{
    return false;
}

bool MpvGlPlayer::decodeFrame()
{
    return false;
}

void MpvGlPlayer::renderToTexture(GLuint /*texture*/) {}

bool MpvGlPlayer::lazyProbePresentationDuration()
{
    return false;
}

void MpvGlPlayer::setPipelineTimingLog(bool /*on*/) {}

}  // namespace baktsiu

#endif  // _WIN32 && USE_VIDEO
