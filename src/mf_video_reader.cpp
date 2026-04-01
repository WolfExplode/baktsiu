#include "mf_video_reader.h"

#include "common.h"

#include <algorithm>
#include <cctype>
#include <climits>

#if defined(_WIN32) && defined(USE_VIDEO)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propvarutil.h>

#include <cstring>

namespace baktsiu
{

namespace
{

int g_mfStartupRef = 0;

void mfStartupAddRef()
{
    if (g_mfStartupRef++ == 0) {
        HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(hr)) {
            LOGE("MFStartup failed: 0x{:x}", static_cast<unsigned>(hr));
            --g_mfStartupRef;
        }
    }
}

void mfStartupRelease()
{
    if (g_mfStartupRef > 0 && --g_mfStartupRef == 0) {
        MFShutdown();
    }
}

std::wstring utf8ToWide(const std::string& utf8)
{
    if (utf8.empty()) {
        return L"";
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (n <= 0) {
        return L"";
    }
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &w[0], n);
    return w;
}

std::wstring pathToFileUrl(const std::string& utf8Path)
{
    std::wstring wpath = utf8ToWide(utf8Path);
    wchar_t full[4096];
    DWORD len = GetFullPathNameW(wpath.c_str(), static_cast<DWORD>(std::size(full)), full, nullptr);
    if (len == 0 || len >= std::size(full)) {
        wcsncpy_s(full, wpath.c_str(), _TRUNCATE);
    }
    std::wstring path(full);
    for (auto& c : path) {
        if (c == L'\\') {
            c = L'/';
        }
    }
    return L"file:///" + path;
}

bool configureOutputType(IMFSourceReader* reader, const GUID& subtype)
{
    IMFMediaType* pPartial = nullptr;
    HRESULT hr = MFCreateMediaType(&pPartial);
    if (FAILED(hr)) {
        return false;
    }
    hr = pPartial->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) {
        hr = pPartial->SetGUID(MF_MT_SUBTYPE, subtype);
    }
    if (SUCCEEDED(hr)) {
        hr = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pPartial);
    }
    pPartial->Release();
    return SUCCEEDED(hr);
}

bool readVideoOutputInfo(IMFSourceReader* reader, int& outW, int& outH, int& strideBytes, bool& rgb32,
    double& frameDurationSec)
{
    frameDurationSec = 1.0 / 30.0;

    IMFMediaType* pType = nullptr;
    HRESULT hr = reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pType);
    if (FAILED(hr)) {
        return false;
    }
    GUID sub = {};
    if (FAILED(pType->GetGUID(MF_MT_SUBTYPE, &sub))) {
        pType->Release();
        return false;
    }
    rgb32 = (sub == MFVideoFormat_RGB32);

    UINT64 frameSize = 0;
    hr = pType->GetUINT64(MF_MT_FRAME_SIZE, &frameSize);
    if (FAILED(hr)) {
        pType->Release();
        return false;
    }
    // MF_MT_FRAME_SIZE: high DWORD = width, low DWORD = height (see MS docs).
    outW = static_cast<int>(frameSize >> 32);
    outH = static_cast<int>(frameSize & 0xffffffffu);

    UINT32 strideU = 0;
    hr = pType->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideU);
    int stride = 0;
    if (SUCCEEDED(hr) && strideU > 0u && strideU < 0x100000u) {
        stride = static_cast<int>(strideU);
    }
    const int minStride = rgb32 ? (outW * 4) : outW;
    if (stride < minStride) {
        stride = minStride;
    }
    // NV12/YUV: decoders pad row stride to a multiple of 16 (e.g. 852-wide → 864). MF_MT_DEFAULT_STRIDE
    // is often missing or equals picture width — using width as stride causes diagonal tearing.
    if (!rgb32 && stride == outW && (outW & 15) != 0) {
        stride = ((outW + 15) / 16) * 16;
    }
    strideBytes = stride;

    UINT32 rateNum = 0;
    UINT32 rateDen = 0;
    if (SUCCEEDED(MFGetAttributeRatio(pType, MF_MT_FRAME_RATE, &rateNum, &rateDen)) && rateDen > 0
        && rateNum > 0) {
        const double fps = static_cast<double>(rateNum) / static_cast<double>(rateDen);
        if (fps > 0.5 && fps < 480.0) {
            frameDurationSec = 1.0 / fps;
        }
    }

    pType->Release();
    return outW > 0 && outH > 0;
}

double readPresentationDuration(IMFSourceReader* reader)
{
    PROPVARIANT var;
    PropVariantInit(&var);
    double sec = 0.0;
    HRESULT hr = reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var);
    if (SUCCEEDED(hr) && var.vt == VT_I8) {
        sec = static_cast<double>(var.hVal.QuadPart) / 10000000.0;
    }
    PropVariantClear(&var);
    return sec;
}

// Walk the video stream to the last sample time (MF_PD_DURATION is often missing on MP4).
double probeVideoEndSec(IMFSourceReader* reader)
{
    HRESULT hr = reader->Flush((DWORD)MF_SOURCE_READER_ALL_STREAMS);
    if (FAILED(hr)) {
        reader->Flush((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    }

    PROPVARIANT var;
    PropVariantInit(&var);
    var.vt = VT_I8;
    // Fast path: jump near the end (often rejected on MP4 — then we scan from 0).
    var.hVal.QuadPart = static_cast<LONGLONG>(7.0 * 86400.0 * 10000000.0);
    hr = reader->SetCurrentPosition(GUID_NULL, var);
    PropVariantClear(&var);
    const bool jumpedNearEnd = SUCCEEDED(hr);
    if (!jumpedNearEnd) {
        PropVariantInit(&var);
        var.vt = VT_I8;
        var.hVal.QuadPart = 0;
        hr = reader->SetCurrentPosition(GUID_NULL, var);
        PropVariantClear(&var);
        if (FAILED(hr)) {
            LOGW("probeVideoEndSec: SetCurrentPosition(0) failed: 0x{:x}", static_cast<unsigned>(hr));
        }
    }

    LONGLONG maxHns = 0;
    int n = 0;
    constexpr int kMaxProbeSamples = 500000;
    while (n < kMaxProbeSamples) {
        ++n;
        IMFSample* sample = nullptr;
        DWORD streamFlags = 0;
        LONGLONG ts = 0;
        DWORD actualStream = 0;
        hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &actualStream, &streamFlags, &ts,
            &sample);
        if (FAILED(hr)) {
            break;
        }
        if (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (sample) {
                sample->Release();
            }
            break;
        }
        if (streamFlags & MF_SOURCE_READERF_STREAMTICK) {
            continue;
        }
        if (!sample) {
            continue;
        }
        LONGLONG t = 0;
        if (SUCCEEDED(sample->GetSampleTime(&t)) && t >= 0) {
            if (t > maxHns) {
                maxHns = t;
            }
        } else if (ts > 0 && ts > maxHns) {
            maxHns = ts;
        }
        sample->Release();
    }

    hr = reader->Flush((DWORD)MF_SOURCE_READER_ALL_STREAMS);
    if (FAILED(hr)) {
        reader->Flush((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    }
    PropVariantInit(&var);
    var.vt = VT_I8;
    var.hVal.QuadPart = 0;
    reader->SetCurrentPosition(GUID_NULL, var);
    PropVariantClear(&var);

    return static_cast<double>(maxHns) / 10000000.0;
}

inline uint8_t clampByte(int v)
{
    if (v < 0) {
        return 0;
    }
    if (v > 255) {
        return 255;
    }
    return static_cast<uint8_t>(v);
}

int resolveRgb32StrideFromBuffer(DWORD curLen, int w, int h, int typeStride)
{
    const int minS = w * 4;
    if (h <= 0 || minS <= 0) {
        return typeStride;
    }
    const DWORD dh = static_cast<DWORD>(h);
    if (curLen < dh * static_cast<DWORD>(minS)) {
        return typeStride;
    }
    if (curLen % dh != 0) {
        return typeStride;
    }
    const int cand = static_cast<int>(curLen / dh);
    if (cand >= minS && cand < 0x400000) {
        return cand;
    }
    return typeStride;
}

int resolveNv12StrideFromBuffer(DWORD curLen, int w, int h, int typeStride)
{
    if (h <= 0 || w <= 0) {
        return typeStride;
    }
    const DWORD denom = static_cast<DWORD>(h) + (static_cast<DWORD>(h) + 1u) / 2u;
    if (denom == 0 || curLen < denom) {
        return typeStride;
    }
    if (curLen % denom != 0) {
        return typeStride;
    }
    const int cand = static_cast<int>(curLen / denom);
    if (cand >= w && cand < 0x400000) {
        return cand;
    }
    return typeStride;
}

// NV12 size = stride * (lumaRows + chromaRows), chromaRows = (lumaRows+1)/2. Decoders often pad luma height to
// a multiple of 16 (e.g. 260 → 272) while MF_MT_FRAME_SIZE still reports 260 — UV plane offset must use padded rows.
int findNv12LumaHeightForRowCount(DWORD totalRowBlocks, int hPic)
{
    for (int H = hPic; H <= hPic + 192; ++H) {
        const DWORD rows = static_cast<DWORD>(H) + (static_cast<DWORD>(H) + 1u) / 2u;
        if (rows == totalRowBlocks) {
            return H;
        }
    }
    return -1;
}

// Smallest luma height >= hPic that explains curLen; sets stride and buffer luma height.
bool pickNv12BufferLayout(DWORD curLen, int w, int hPic, int typeStride, int& outStride, int& outBufH)
{
    for (int H = hPic; H <= hPic + 192; ++H) {
        const DWORD denom = static_cast<DWORD>(H) + (static_cast<DWORD>(H) + 1u) / 2u;
        if (denom == 0 || curLen % denom != 0) {
            continue;
        }
        const int stride = static_cast<int>(curLen / denom);
        if (stride < w || stride >= 0x400000) {
            continue;
        }
        if (static_cast<DWORD>(stride) * denom != curLen) {
            continue;
        }
        outStride = stride;
        outBufH = H;
        return true;
    }
    return false;
}

void nv12ToRgba(const uint8_t* yPlane, const uint8_t* uvPlane, int yStride, int width, int height, uint8_t* dstRgba)
{
    for (int y = 0; y < height; ++y) {
        const uint8_t* rowY = yPlane + y * yStride;
        const uint8_t* rowUv = uvPlane + (y / 2) * yStride;
        uint8_t* rowOut = dstRgba + y * width * 4;
        for (int x = 0; x < width; ++x) {
            int Y = static_cast<int>(rowY[x]) - 16;
            int U = static_cast<int>(rowUv[(x / 2) * 2]) - 128;
            int V = static_cast<int>(rowUv[(x / 2) * 2 + 1]) - 128;
            int c = 298 * Y;
            int r = (c + 409 * V + 128) >> 8;
            int g = (c - 100 * U - 208 * V + 128) >> 8;
            int b = (c + 516 * U + 128) >> 8;
            rowOut[x * 4 + 0] = clampByte(r);
            rowOut[x * 4 + 1] = clampByte(g);
            rowOut[x * 4 + 2] = clampByte(b);
            rowOut[x * 4 + 3] = 255;
        }
    }
}

}  // namespace

// IMFSourceReader output → m_rgba (after nv12ToRgba); friend of MFVideoReader.
struct MFVideoReaderDecodeDetail
{
    static bool finalize(MFVideoReader& r, IMFSample* sample, LONGLONG readerTs)
    {
        LONGLONG sampleTimeHns = 0;
        const HRESULT hrPts = sample->GetSampleTime(&sampleTimeHns);

        IMFMediaBuffer* buf = nullptr;
        HRESULT hr = sample->ConvertToContiguousBuffer(&buf);
        sample->Release();
        if (FAILED(hr) || !buf) {
            return false;
        }

        BYTE* data = nullptr;
        DWORD maxLen = 0;
        DWORD curLen = 0;
        hr = buf->Lock(&data, &maxLen, &curLen);
        if (FAILED(hr) || !data) {
            buf->Release();
            return false;
        }

        int strideBytes = r.m_strideBytes;
        const int minPitch = r.m_outputRgb32 ? (r.m_width * 4) : r.m_width;

        bool have2dStride = false;
        LONG pitch2d = 0;
        IMF2DBuffer* p2d = nullptr;
        if (SUCCEEDED(buf->QueryInterface(IID_IMF2DBuffer, reinterpret_cast<void**>(&p2d))) && p2d) {
            BYTE* scan0 = nullptr;
            LONG pitch = 0;
            if (SUCCEEDED(p2d->GetScanline0AndPitch(&scan0, &pitch)) && pitch >= minPitch && pitch < 0x400000) {
                pitch2d = pitch;
                have2dStride = true;
            }
            p2d->Release();
        }

        int nv12BufH = r.m_height;
        if (r.m_outputRgb32) {
            if (have2dStride) {
                strideBytes = static_cast<int>(pitch2d);
            } else {
                strideBytes = resolveRgb32StrideFromBuffer(curLen, r.m_width, r.m_height, r.m_strideBytes);
            }
        } else {
            if (!pickNv12BufferLayout(curLen, r.m_width, r.m_height, r.m_strideBytes, strideBytes, nv12BufH)) {
                if (have2dStride) {
                    strideBytes = static_cast<int>(pitch2d);
                    if (curLen % static_cast<DWORD>(strideBytes) == 0) {
                        const DWORD totalRows = curLen / static_cast<DWORD>(strideBytes);
                        const int found = findNv12LumaHeightForRowCount(totalRows, r.m_height);
                        if (found >= 0) {
                            nv12BufH = found;
                        }
                    }
                } else {
                    strideBytes = resolveNv12StrideFromBuffer(curLen, r.m_width, r.m_height, r.m_strideBytes);
                }
            }
        }
        r.m_strideBytes = strideBytes;

        const size_t stride = static_cast<size_t>(strideBytes);
        const size_t picH = static_cast<size_t>(r.m_height);
        const size_t w = static_cast<size_t>(r.m_width);

        if (r.m_outputRgb32) {
            const size_t rowNeed = stride;
            const size_t lastRowEnd = (picH > 0) ? ((picH - 1u) * stride + w * 4u) : 0u;
            if (curLen < lastRowEnd || rowNeed < w * 4u) {
                LOGW("RGB32 buffer too small or stride invalid: curLen={}, need>={}, stride={}, {}x{}",
                    static_cast<unsigned>(curLen), static_cast<unsigned>(lastRowEnd), r.m_strideBytes, r.m_width,
                    r.m_height);
                buf->Unlock();
                buf->Release();
                return false;
            }
            for (int y = 0; y < r.m_height; ++y) {
                const uint8_t* src = data + static_cast<size_t>(y) * stride;
                uint8_t* dst = r.m_rgba.data() + y * r.m_width * 4;
                for (int x = 0; x < r.m_width; ++x) {
                    dst[x * 4 + 0] = src[x * 4 + 2];
                    dst[x * 4 + 1] = src[x * 4 + 1];
                    dst[x * 4 + 2] = src[x * 4 + 0];
                    dst[x * 4 + 3] = src[x * 4 + 3];
                }
            }
        } else {
            const size_t bufH = static_cast<size_t>(nv12BufH);
            const size_t yPlaneBytes = stride * bufH;
            const size_t uvRows = (bufH + 1u) / 2u;
            const size_t uvPlaneBytes = stride * uvRows;
            const size_t nv12Need = yPlaneBytes + uvPlaneBytes;
            if (curLen < nv12Need) {
                LOGW("NV12 buffer too small: curLen={}, need>={}, stride={}, bufH={} pic={}x{}",
                    static_cast<unsigned>(curLen), static_cast<unsigned>(nv12Need), r.m_strideBytes,
                    nv12BufH, r.m_width, r.m_height);
                buf->Unlock();
                buf->Release();
                return false;
            }
            const uint8_t* yPlane = data;
            const uint8_t* uvPlane = data + yPlaneBytes;
            nv12ToRgba(yPlane, uvPlane, r.m_strideBytes, r.m_width, r.m_height, r.m_rgba.data());
        }

        buf->Unlock();
        buf->Release();

        if (SUCCEEDED(hrPts)) {
            r.m_positionSec = static_cast<double>(sampleTimeHns) / 10000000.0;
        } else if (readerTs > 0) {
            r.m_positionSec = static_cast<double>(readerTs) / 10000000.0;
        } else {
            r.m_positionSec += r.m_frameDurationSec;
        }
        return true;
    }
};

MFVideoReader::MFVideoReader() = default;

MFVideoReader::~MFVideoReader()
{
    close();
}

bool MFVideoReader::isSupportedExtension(const std::string& filepath)
{
    static const char* kExt[] = {".mp4", ".mov", ".wmv", ".avi", ".mkv", ".webm", ".m4v"};
    std::string lower = filepath;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* e : kExt) {
        size_t el = std::strlen(e);
        if (lower.size() >= el && lower.compare(lower.size() - el, el, e) == 0) {
            return true;
        }
    }
    return false;
}

bool MFVideoReader::open(const std::string& utf8Path)
{
    close();

    mfStartupAddRef();
    if (g_mfStartupRef <= 0) {
        return false;
    }

    std::wstring url = pathToFileUrl(utf8Path);
    IMFSourceReader* reader = nullptr;
    HRESULT hr = MFCreateSourceReaderFromURL(url.c_str(), nullptr, &reader);
    if (FAILED(hr)) {
        LOGE("MFCreateSourceReaderFromURL failed: 0x{:x}", static_cast<unsigned>(hr));
        mfStartupRelease();
        return false;
    }

    m_reader = reader;

    if (!configureOutputType(reader, MFVideoFormat_RGB32)) {
        if (!configureOutputType(reader, MFVideoFormat_NV12)) {
            LOGE("Could not set video output type (RGB32/NV12)");
            close();
            return false;
        }
        m_outputRgb32 = false;
    } else {
        m_outputRgb32 = true;
    }

    if (!readVideoOutputInfo(reader, m_width, m_height, m_strideBytes, m_outputRgb32, m_frameDurationSec)) {
        LOGE("Could not read video dimensions");
        close();
        return false;
    }

    constexpr int kMaxVideoDim = 8192;
    if (m_width <= 0 || m_height <= 0 || m_width > kMaxVideoDim || m_height > kMaxVideoDim) {
        LOGE("Video dimensions out of supported range: {}x{}", m_width, m_height);
        close();
        return false;
    }

    m_durationSec = readPresentationDuration(reader);
    if (m_durationSec <= 0.001) {
        const double probed = probeVideoEndSec(reader);
        if (probed > 0.001) {
            m_durationSec = probed;
        }
    }

    m_rgba.resize(static_cast<size_t>(m_width) * static_cast<size_t>(m_height) * 4u);

    seek(0.0);
    if (!decodeFrame()) {
        LOGE("Failed to decode first video frame (codec/buffer layout may be unsupported)");
        close();
        return false;
    }
    return true;
}

void MFVideoReader::close()
{
#if defined(_WIN32) && defined(USE_VIDEO)
    // Only balance mfStartupAddRef from a successful open(). Calling mfStartupRelease when
    // m_reader is null would wrongly MFShutdown() while another MFVideoReader still exists
    // (e.g. second reader's open() begins with close() on a default-constructed instance).
    if (m_reader) {
        static_cast<IMFSourceReader*>(m_reader)->Release();
        m_reader = nullptr;
        mfStartupRelease();
    }
#else
    m_reader = nullptr;
#endif
    m_width = m_height = m_strideBytes = 0;
    m_durationSec = m_positionSec = 0.0;
    m_frameDurationSec = 1.0 / 30.0;
    m_rgba.clear();
}

bool MFVideoReader::isOpen() const
{
    return m_reader != nullptr;
}

void MFVideoReader::seek(double seconds)
{
    if (!m_reader) {
        return;
    }
    IMFSourceReader* reader = static_cast<IMFSourceReader*>(m_reader);
    constexpr double kMaxSeekSec = 86400.0 * 366.0 * 50.0;
    seconds = std::max(0.0, std::min(seconds, kMaxSeekSec));
    // Seeking exactly to probed duration often fails (0xc00d36e5); stay slightly inside the stream.
    if (m_durationSec > 0.001) {
        const double endSlack = std::max(m_frameDurationSec * 3.0, 0.1);
        const double seekCap = std::max(0.0, m_durationSec - endSlack);
        seconds = std::min(seconds, seekCap);
    }

    // Flush all streams so audio/subtitle queues do not leave the reader out of sync after a seek.
    HRESULT hr = reader->Flush((DWORD)MF_SOURCE_READER_ALL_STREAMS);
    if (FAILED(hr)) {
        hr = reader->Flush((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        if (FAILED(hr)) {
            LOGW("IMFSourceReader::Flush failed: 0x{:x}", static_cast<unsigned>(hr));
        }
    }

    const double hnsF = seconds * 10000000.0;
    LONGLONG hns = 0;
    if (hnsF >= static_cast<double>(LLONG_MAX)) {
        hns = LLONG_MAX;
    } else {
        hns = static_cast<LONGLONG>(hnsF + 0.5);
    }

    PROPVARIANT var;
    PropVariantInit(&var);
    var.vt = VT_I8;
    var.hVal.QuadPart = hns;
    hr = reader->SetCurrentPosition(GUID_NULL, var);
    PropVariantClear(&var);
    if (FAILED(hr)) {
        LOGD("SetCurrentPosition failed: 0x{:x}", static_cast<unsigned>(hr));
    }

    m_positionSec = seconds;
}

bool MFVideoReader::decodeFrame()
{
    if (!m_reader || m_width <= 0 || m_height <= 0) {
        return false;
    }
    IMFSourceReader* reader = static_cast<IMFSourceReader*>(m_reader);

    IMFSample* sample = nullptr;
    DWORD streamFlags = 0;
    LONGLONG ts = 0;
    DWORD actualStream = 0;

    int readIterations = 0;
    constexpr int kMaxReadIterations = 4096;
    while (readIterations < kMaxReadIterations) {
        ++readIterations;
        if (sample) {
            sample->Release();
            sample = nullptr;
        }
        HRESULT hr = reader->ReadSample(
            (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
            0,
            &actualStream,
            &streamFlags,
            &ts,
            &sample);
        if (FAILED(hr)) {
            LOGE("ReadSample failed: 0x{:x}", static_cast<unsigned>(hr));
            return false;
        }
        if (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
            m_positionSec = m_durationSec > 0.0 ? m_durationSec : m_positionSec;
            return false;
        }
        if (streamFlags & MF_SOURCE_READERF_STREAMTICK) {
            continue;
        }
        if (sample) {
            break;
        }
    }
    if (!sample) {
        LOGW("ReadSample: no video sample after {} iterations", readIterations);
        return false;
    }

    return MFVideoReaderDecodeDetail::finalize(*this, sample, ts);
}

bool MFVideoReader::decodeFrameThrough(double targetSec)
{
    if (!m_reader || m_width <= 0 || m_height <= 0) {
        return false;
    }
    IMFSourceReader* reader = static_cast<IMFSourceReader*>(m_reader);

    if (m_durationSec > 0.001) {
        const double endSlack = std::max(m_frameDurationSec * 3.0, 0.1);
        const double targetCap = std::max(0.0, m_durationSec - endSlack);
        targetSec = std::min(targetSec, targetCap);
    }

    const double targetHnsD = targetSec * 10000000.0;
    const LONGLONG targetHns = targetHnsD >= static_cast<double>(LLONG_MAX)
        ? LLONG_MAX
        : static_cast<LONGLONG>(targetHnsD + 0.5);
    const LONGLONG epsHns = (std::max)(10000LL, static_cast<LONGLONG>(m_frameDurationSec * 5000000.0));

    IMFSample* hold = nullptr;
    LONGLONG holdReaderTs = 0;

    IMFSample* sample = nullptr;
    DWORD streamFlags = 0;
    LONGLONG ts = 0;
    DWORD actualStream = 0;

    int readIterations = 0;
    const int kBudget = static_cast<int>(m_durationSec * 130.0 + 2048.0);
    const int kMaxReadIterations = (std::min)(200000, (std::max)(16384, kBudget));
    while (readIterations < kMaxReadIterations) {
        ++readIterations;
        if (sample) {
            sample->Release();
            sample = nullptr;
        }
        HRESULT hr = reader->ReadSample(
            (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
            0,
            &actualStream,
            &streamFlags,
            &ts,
            &sample);
        if (FAILED(hr)) {
            LOGE("ReadSample failed: 0x{:x}", static_cast<unsigned>(hr));
            if (hold) {
                hold->Release();
            }
            return false;
        }
        if (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (hold) {
                return MFVideoReaderDecodeDetail::finalize(*this, hold, holdReaderTs);
            }
            m_positionSec = m_durationSec > 0.0 ? m_durationSec : m_positionSec;
            return false;
        }
        if (streamFlags & MF_SOURCE_READERF_STREAMTICK) {
            continue;
        }
        if (!sample) {
            continue;
        }

        LONGLONG t = 0;
        const HRESULT hrPts = sample->GetSampleTime(&t);
        if (FAILED(hrPts)) {
            t = ts;
        }

        if (t + epsHns >= targetHns) {
            if (hold) {
                hold->Release();
                hold = nullptr;
            }
            return MFVideoReaderDecodeDetail::finalize(*this, sample, ts);
        }

        if (hold) {
            hold->Release();
        }
        hold = sample;
        holdReaderTs = ts;
        sample = nullptr;
    }

    if (hold) {
        LOGW("decodeFrameThrough: hit iteration cap, using last sample before target");
        return MFVideoReaderDecodeDetail::finalize(*this, hold, holdReaderTs);
    }
    LOGW("decodeFrameThrough: no video sample after {} iterations", readIterations);
    return false;
}

}  // namespace baktsiu

#else  // !(_WIN32 && USE_VIDEO)

namespace baktsiu
{

MFVideoReader::MFVideoReader() = default;

MFVideoReader::~MFVideoReader()
{
    close();
}

bool MFVideoReader::isSupportedExtension(const std::string&)
{
    return false;
}

bool MFVideoReader::open(const std::string&)
{
    return false;
}

void MFVideoReader::close()
{
    m_width = m_height = 0;
    m_durationSec = m_positionSec = 0.0;
    m_frameDurationSec = 1.0 / 30.0;
    m_rgba.clear();
}

bool MFVideoReader::isOpen() const
{
    return false;
}

void MFVideoReader::seek(double)
{
}

bool MFVideoReader::decodeFrame()
{
    return false;
}

bool MFVideoReader::decodeFrameThrough(double)
{
    return false;
}

}  // namespace baktsiu

#endif
