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
    double positionSec() const { return m_positionSec; }
    double frameDurationSec() const { return m_frameDurationSec; }

    void seek(double seconds);
    // Pull next video sample into internal RGBA; returns true if pixels changed.
    bool decodeFrame();
    // After seek/scrub: advance through decoded output until frame time >= targetSec (MF often starts at a keyframe).
    bool decodeFrameThrough(double targetSec);

    const uint8_t* rgbaBuffer() const { return m_rgba.data(); }

private:
    friend struct MFVideoReaderDecodeDetail;
    void* m_reader = nullptr;  // IMFSourceReader* (Win32 + USE_VIDEO only)

    int m_width = 0;
    int m_height = 0;
    int m_pixelAspectNum = 1;
    int m_pixelAspectDen = 1;
    int m_strideBytes = 0;
    double m_durationSec = 0.0;
    double m_positionSec = 0.0;
    double m_frameDurationSec = 1.0 / 30.0;
    bool m_outputRgb32 = true;

    std::vector<uint8_t> m_rgba;
};

}  // namespace baktsiu

#endif
