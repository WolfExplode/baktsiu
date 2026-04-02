#ifndef BAKTSIU_MF_VIDEO_READER_H_
#define BAKTSIU_MF_VIDEO_READER_H_

#include <cstdint>
#include <string>
#include <vector>

namespace baktsiu
{

// Windows Media Foundation–backed reader (USE_VIDEO on Win32). Stub elsewhere.
class MFVideoReader
{
public:
    MFVideoReader();
    ~MFVideoReader();

    MFVideoReader(const MFVideoReader&) = delete;
    MFVideoReader& operator=(const MFVideoReader&) = delete;

    static bool isSupportedExtension(const std::string& filepath);

    bool open(const std::string& utf8Path);
    void close();
    bool isOpen() const;

    int width() const { return m_width; }
    int height() const { return m_height; }
    // Pixel aspect ratio times frame size, for layout (MF_MT_PIXEL_ASPECT_RATIO). Matches coded
    // dimensions when PAR is 1:1; anamorphic/square-frame portrait encodes use PAR so DAR is correct.
    float displayWidthForAspect() const;
    float displayHeightForAspect() const;
    double durationSec() const { return m_durationSec; }
    // True when duration came from MF_PD_DURATION, ISO BMFF parse, or a probe that reached EOS.
    // False when unknown or when a time/sample-limited probe did not finish the stream.
    bool hasReliableDuration() const { return m_durationReliable; }
    double positionSec() const { return m_positionSec; }
    double frameDurationSec() const { return m_frameDurationSec; }

    // flushStreams: false for scrub preview (skips Flush when safe). Backward seeks still flush so
    // SetCurrentPosition stays fast; small forward steps skip Flush+SetCurrentPosition and rely on
    // decodeFrameThrough(ReadSample) only.
    void seek(double seconds, bool flushStreams = true);
    // Pull next video sample into internal RGBA; returns true if pixels changed.
    bool decodeFrame();
    // After seek/scrub: advance through decoded output until frame time >= targetSec (MF often starts at a keyframe).
    // maxReadSamplesCap > 0 limits ReadSample iterations (faster scrub preview; may stop on a nearby frame).
    bool decodeFrameThrough(double targetSec, int maxReadSamplesCap = 0);

    const uint8_t* rgbaBuffer() const { return m_rgba.data(); }

    // NV12 GPU upload path: Y+UV in nv12Buffer(), dimensions via nv12StrideBytes / nv12YTexHeight.
    bool usesGpuNv12Path() const { return m_gpuNv12Path && !m_outputRgb32; }
    const uint8_t* nv12Buffer() const { return m_nv12.empty() ? nullptr : m_nv12.data(); }
    int nv12StrideBytes() const { return m_strideBytes; }
    int nv12YTexHeight() const { return m_nv12YBufRows; }
    int pixelAspectNum() const { return m_pixelAspectNum; }
    int pixelAspectDen() const { return m_pixelAspectDen; }

    // If presentation duration was still unknown at open(), may probe the stream once (expensive).
    // Skipped for .mp4/.m4v/.mov (moov parse already ran). Call from the main thread after first frames.
    // Returns true if the reader was rewound to t=0 (caller should resync, e.g. syncVideoDecodersToCompositionT).
    bool lazyProbePresentationDuration();

    // When true, seek/decodeFrameThrough record timings (see last* accessors). For scrub profiling.
    void setPipelineTimingLog(bool on);
    float lastSeekMs() const { return m_lastSeekMs; }
    // Sub-steps of last seek() (Win32 MF); 0 when stub or timing off.
    float lastSeekFlushMs() const { return m_lastSeekFlushMs; }
    float lastSeekSetPosMs() const { return m_lastSeekSetPosMs; }
    float lastDecodeThroughMs() const { return m_lastDecodeThroughMs; }
    int lastDecodeThroughReads() const { return m_lastDecodeThroughReads; }
    bool lastDecodeThroughHitCap() const { return m_lastDecodeThroughHitCap; }

private:
    friend struct MFVideoReaderDecodeDetail;
    void* m_reader = nullptr;  // IMFSourceReader* (Win32 + USE_VIDEO only)

    int m_width = 0;
    int m_height = 0;
    int m_pixelAspectNum = 1;
    int m_pixelAspectDen = 1;
    int m_strideBytes = 0;
    double m_durationSec = 0.0;
    bool m_durationReliable = false;
    double m_positionSec = 0.0;
    double m_frameDurationSec = 1.0 / 30.0;
    bool m_outputRgb32 = true;
    bool m_gpuNv12Path = false;
    bool m_lazyDurationProbeDone = false;
    // When true, lazyProbePresentationDuration does not scan the stream (ISO containers already had
    // moov parse at open; stream probe is for MKV/WebM and similar).
    bool m_skipLazyStreamProbe = false;

    std::vector<uint8_t> m_rgba;
    std::vector<uint8_t> m_nv12;
    int m_nv12YBufRows = 0;
    int m_nv12DiagLogFrames = 0;

    bool m_logPipelineTiming = false;
    float m_lastSeekMs = 0.f;
    float m_lastSeekFlushMs = 0.f;
    float m_lastSeekSetPosMs = 0.f;
    float m_lastDecodeThroughMs = 0.f;
    int m_lastDecodeThroughReads = 0;
    bool m_lastDecodeThroughHitCap = false;
};

}  // namespace baktsiu

#endif
