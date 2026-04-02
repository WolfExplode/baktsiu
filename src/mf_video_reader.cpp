#include "mf_video_reader.h"

#include "common.h"
#include "mp4_iso_duration.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <utility>

#if defined(_WIN32) && defined(USE_VIDEO)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlwapi.h>

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

double qpcMs(const LARGE_INTEGER& a, const LARGE_INTEGER& b)
{
    static LARGE_INTEGER freq;
    static bool haveFreq = false;
    if (!haveFreq) {
        QueryPerformanceFrequency(&freq);
        haveFreq = true;
    }
    return static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

// Flush video only first: Flush(ALL_STREAMS) can block ~1s draining multiplexed audio while this
// reader only ever uses ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM).
HRESULT flushSourceReaderVideoFirst(IMFSourceReader* reader)
{
    HRESULT hr = reader->Flush((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    if (FAILED(hr)) {
        hr = reader->Flush((DWORD)MF_SOURCE_READER_ALL_STREAMS);
        if (FAILED(hr)) {
            LOGW("IMFSourceReader::Flush failed: 0x{:x}", static_cast<unsigned>(hr));
        }
    }
    return hr;
}

// Video-only: deselect audio, subtitles, etc. so MF does not decode or buffer them (faster seek/scrub).
void disableNonVideoSourceReaderStreams(IMFSourceReader* reader)
{
    if (!reader) {
        return;
    }
    for (DWORD i = 0; i < 64u; ++i) {
        BOOL dummySel = FALSE;
        if (FAILED(reader->GetStreamSelection(i, &dummySel))) {
            break;
        }
        IMFMediaType* native = nullptr;
        const HRESULT hrType = reader->GetNativeMediaType(i, 0, &native);
        if (FAILED(hrType) || !native) {
            continue;
        }
        GUID major = GUID_NULL;
        const HRESULT hrMajor = native->GetGUID(MF_MT_MAJOR_TYPE, &major);
        native->Release();
        if (FAILED(hrMajor)) {
            continue;
        }
        if (major != MFMediaType_Video) {
            reader->SetStreamSelection(i, FALSE);
        }
    }
}

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
    // Prefer UTF-8 (import dialog / drag-drop / wmain argv). Windows narrow argv is often the
    // system ANSI code page — fall back so paths from CreateProcess still open correctly.
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), -1, nullptr, 0);
    UINT cp = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (n <= 0) {
        cp = CP_ACP;
        flags = 0;
        n = MultiByteToWideChar(cp, flags, utf8.c_str(), -1, nullptr, 0);
    }
    if (n <= 0) {
        return L"";
    }
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(cp, flags, utf8.c_str(), -1, &w[0], n);
    return w;
}

std::wstring utf8PathToFullPathWide(const std::string& utf8Path)
{
    std::wstring wpath = utf8ToWide(utf8Path);
    if (wpath.empty() && !utf8Path.empty()) {
        return L"";
    }
    wchar_t buf[32768];
    DWORD len = GetFullPathNameW(wpath.c_str(), static_cast<DWORD>(std::size(buf)), buf, nullptr);
    if (len == 0 || len >= std::size(buf)) {
        if (wpath.empty()) {
            return L"";
        }
        if (wcsncpy_s(buf, wpath.c_str(), _TRUNCATE) != 0) {
            return L"";
        }
    }
    return std::wstring(buf);
}

std::wstring pathToFileUrl(const std::string& utf8Path)
{
    std::wstring full = utf8PathToFullPathWide(utf8Path);
    if (full.empty() && !utf8Path.empty()) {
        return L"";
    }

    // Raw file:///C:/.../name with spaces breaks MFCreateSourceReaderFromURL (0x80070002). Let the
    // shell API build a proper file: URL, or percent-encode UTF-8 as fallback.
    wchar_t shellUrl[32768];
    DWORD shellUrlChars = static_cast<DWORD>(std::size(shellUrl));
    const HRESULT hrPath = UrlCreateFromPathW(full.c_str(), shellUrl, &shellUrlChars, 0);
    if (SUCCEEDED(hrPath) && shellUrlChars > 1u) {
        return std::wstring(shellUrl);
    }

    std::wstring pathFwd(full);
    for (auto& c : pathFwd) {
        if (c == L'\\') {
            c = L'/';
        }
    }
    const int nU8 = WideCharToMultiByte(CP_UTF8, 0, pathFwd.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (nU8 <= 1) {
        return L"";
    }
    std::string u8(static_cast<size_t>(nU8 - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, pathFwd.c_str(), -1, &u8[0], nU8, nullptr, nullptr);

    std::string enc;
    enc.reserve(u8.size() * 3u);
    static const char* kHex = "0123456789ABCDEF";
    for (unsigned char ch : u8) {
        if (std::isalnum(ch) != 0 || ch == '-' || ch == '.' || ch == '_' || ch == '~' || ch == '/'
            || ch == ':') {
            enc += static_cast<char>(ch);
        } else {
            enc += '%';
            enc += kHex[ch >> 4];
            enc += kHex[ch & 0xFu];
        }
    }
    std::wstring out;
    out.reserve(8u + enc.size());
    out.append(L"file:///");
    for (char c : enc) {
        out += static_cast<wchar_t>(static_cast<unsigned char>(c));
    }
    return out;
}

IMFAttributes* createSourceReaderAttributes()
{
    IMFAttributes* attr = nullptr;
    if (FAILED(MFCreateAttributes(&attr, 8))) {
        return nullptr;
    }
    // Prefer DXVA/D3D decoder MFTs when available (output may still be system memory for IMFSourceReader).
    if (FAILED(attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE))) {
        attr->Release();
        return nullptr;
    }
    return attr;
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

// Duration at open: MF_PD_DURATION, then ISO BMFF moov (.mp4/.m4v/.mov) when MF omits it.
// lazyProbePresentationDuration (see MFVideoReader) runs only if duration is still unknown after open
// and the path is not ISO BMFF — then we may scan the video stream toward EOS (time/sample capped).
double readPresentationDuration(IMFSourceReader* reader, HRESULT* outHr = nullptr, VARTYPE* outVt = nullptr)
{
    if (outHr) {
        *outHr = E_FAIL;
    }
    if (outVt) {
        *outVt = VT_EMPTY;
    }
    PROPVARIANT var;
    PropVariantInit(&var);
    double sec = 0.0;
    const HRESULT hr =
        reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var);
    if (outHr) {
        *outHr = hr;
    }
    if (outVt) {
        *outVt = var.vt;
    }
    // MF sometimes stores presentation duration as VT_UI8 (100-ns units), not only VT_I8.
    if (SUCCEEDED(hr)) {
        if (var.vt == VT_I8) {
            sec = static_cast<double>(var.hVal.QuadPart) / 10000000.0;
        } else if (var.vt == VT_UI8) {
            sec = static_cast<double>(var.uhVal.QuadPart) / 10000000.0;
        }
    }
    PropVariantClear(&var);
    return sec;
}

constexpr double kDurationEpsilonSec = 0.001;

// Clamp requested media time slightly inside presentation end (seeking to exact duration often fails).
double seekEndCapSec(double durationSec, double frameDurationSec)
{
    if (durationSec <= kDurationEpsilonSec) {
        return durationSec;
    }
    const double endSlack = std::max(frameDurationSec * 3.0, 0.1);
    return std::max(0.0, durationSec - endSlack);
}

void resolveMetadataDurationSec(
    IMFSourceReader* reader, const std::string& utf8Path, double& outSec, bool& outReliable)
{
    HRESULT mfHr = E_FAIL;
    VARTYPE mfVt = VT_EMPTY;
    outSec = readPresentationDuration(reader, &mfHr, &mfVt);
    LOGI(
        "[duration] \"{}\": MF_PD_DURATION hr=0x{:x} vt={} -> {:.6f}s",
        utf8Path, static_cast<unsigned>(mfHr), static_cast<unsigned>(mfVt), outSec);

    if (outSec <= kDurationEpsilonSec) {
        if (pathLooksIsoBmff(utf8Path)) {
            const double isoDur = tryReadIsoBmffDurationSecondsFromUtf8Path(utf8Path);
            if (isoDur > kDurationEpsilonSec) {
                outSec = isoDur;
            } else {
                LOGW(
                    "[duration] \"{}\": ISO returned no duration (see [duration][iso] lines above for head/tail/moov)",
                    utf8Path);
            }
        } else {
            LOGI(
                "[duration] \"{}\": ISO BMFF not attempted (not .mp4/.m4v/.mov); stream lazy-probe may run later",
                utf8Path);
        }
    }
    outReliable = (outSec > kDurationEpsilonSec);
    LOGI("[duration] \"{}\": metadata result reliable={} durationSec={:.6f}", utf8Path, outReliable, outSec);
}

struct ProbeVideoEndResult {
    double endSec = 0.0;
    bool reachedEos = false;
};

// Walk the video stream to the last sample time (MF_PD_DURATION is often missing on MP4).
// If we stop on time/sample limits before EOS, endSec is only a partial max timestamp — do not use as duration.
ProbeVideoEndResult probeVideoEndSec(IMFSourceReader* reader)
{
    ProbeVideoEndResult out;
    HRESULT hr = S_OK;
    flushSourceReaderVideoFirst(reader);

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
    constexpr int kMaxProbeSamples = 65536;
    constexpr double kProbeTimeBudgetMs = 2000.0;
    LARGE_INTEGER probeT0;
    LARGE_INTEGER probeFreq;
    QueryPerformanceFrequency(&probeFreq);
    QueryPerformanceCounter(&probeT0);

    while (n < kMaxProbeSamples) {
        LARGE_INTEGER probeNow;
        QueryPerformanceCounter(&probeNow);
        const double elapsedMs =
            static_cast<double>(probeNow.QuadPart - probeT0.QuadPart) * 1000.0
            / static_cast<double>(probeFreq.QuadPart);
        if (elapsedMs >= kProbeTimeBudgetMs) {
            LOGW("probeVideoEndSec: time budget {:.0f} ms reached at {} samples (no EOS — duration not trusted)",
                kProbeTimeBudgetMs, n);
            break;
        }
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
                LONGLONG t = 0;
                if (SUCCEEDED(sample->GetSampleTime(&t)) && t >= 0) {
                    if (t > maxHns) {
                        maxHns = t;
                    }
                } else if (ts > maxHns) {
                    maxHns = ts;
                }
                sample->Release();
            }
            out.reachedEos = true;
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

    flushSourceReaderVideoFirst(reader);
    PropVariantInit(&var);
    var.vt = VT_I8;
    var.hVal.QuadPart = 0;
    reader->SetCurrentPosition(GUID_NULL, var);
    PropVariantClear(&var);

    if (n >= kMaxProbeSamples && !out.reachedEos) {
        LOGW("probeVideoEndSec: sample cap {} reached without EOS (duration not trusted)", kMaxProbeSamples);
    }

    LARGE_INTEGER probeT1;
    QueryPerformanceCounter(&probeT1);
    out.endSec = static_cast<double>(maxHns) / 10000000.0;
    LOGI("probeVideoEndSec: {:.2f} ms ({} samples, maxT={:.3f}s, eos={})", qpcMs(probeT0, probeT1), n, out.endSec,
        out.reachedEos);

    return out;
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
            if (r.m_gpuNv12Path) {
                r.m_nv12.resize(nv12Need);
                std::memcpy(r.m_nv12.data(), yPlane, nv12Need);
                r.m_nv12YBufRows = nv12BufH;
                if (r.m_nv12DiagLogFrames > 0) {
                    --r.m_nv12DiagLogFrames;
                    const uint8_t* py = r.m_nv12.data();
                    const uint8_t* puv = py + yPlaneBytes;
                    auto rowMinMax = [&](int yRow) -> std::pair<uint8_t, uint8_t> {
                        uint8_t lo = 255;
                        uint8_t hi = 0;
                        const uint8_t* row = py + static_cast<size_t>(yRow) * stride;
                        for (int x = 0; x < r.m_width; ++x) {
                            const uint8_t v = row[static_cast<size_t>(x)];
                            lo = (std::min)(lo, v);
                            hi = (std::max)(hi, v);
                        }
                        return {lo, hi};
                    };
                    const int yMid = r.m_height > 1 ? r.m_height / 2 : 0;
                    const int yLast = r.m_height > 0 ? r.m_height - 1 : 0;
                    const auto mm0 = rowMinMax(0);
                    const auto mmM = rowMinMax(yMid);
                    const auto mmL = rowMinMax(yLast);
                    LOGI(
                        "NV12 cpu buffer diag: pic={}x{} stride={} bufH={} curLen={} "
                        "Y row0 min/max={}/{} mid[{}]={}/{} last={}/{} UV[0..3]={:02x}{:02x}{:02x}{:02x} "
                        "displayW={:.2f} PAR={}:{}",
                        r.m_width, r.m_height, r.m_strideBytes, nv12BufH, static_cast<unsigned>(curLen),
                        mm0.first, mm0.second, yMid, mmM.first, mmM.second, mmL.first, mmL.second, puv[0],
                        puv[1], puv[2], puv[3], r.displayWidthForAspect(), r.m_pixelAspectNum,
                        r.m_pixelAspectDen);
                }
            } else {
                nv12ToRgba(yPlane, uvPlane, r.m_strideBytes, r.m_width, r.m_height, r.m_rgba.data());
            }
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

float MFVideoReader::displayWidthForAspect() const
{
    const int den = (m_pixelAspectDen > 0) ? m_pixelAspectDen : 1;
    const int num = (m_pixelAspectNum > 0) ? m_pixelAspectNum : 1;
    return static_cast<float>(m_width) * static_cast<float>(num) / static_cast<float>(den);
}

float MFVideoReader::displayHeightForAspect() const
{
    return static_cast<float>(m_height);
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
    LARGE_INTEGER tOpenStart;
    QueryPerformanceCounter(&tOpenStart);

    close();

    mfStartupAddRef();
    if (g_mfStartupRef <= 0) {
        return false;
    }

    LARGE_INTEGER t0;
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t0);

    IMFAttributes* srAttr = createSourceReaderAttributes();
    const bool hwTransformsAttrRequested = (srAttr != nullptr);
    IMFSourceReader* reader = nullptr;
    HRESULT hr = E_FAIL;
    const std::wstring fullWide = utf8PathToFullPathWide(utf8Path);
    if (!fullWide.empty()) {
        IMFByteStream* byteStream = nullptr;
        hr = MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST, MF_FILEFLAGS_NONE,
            fullWide.c_str(), &byteStream);
        if (SUCCEEDED(hr) && byteStream) {
            hr = MFCreateSourceReaderFromByteStream(byteStream, srAttr, &reader);
            if (FAILED(hr) && srAttr) {
                hr = MFCreateSourceReaderFromByteStream(byteStream, nullptr, &reader);
            }
            byteStream->Release();
        }
    }
    if (FAILED(hr) || !reader) {
        const std::wstring url = pathToFileUrl(utf8Path);
        hr = MFCreateSourceReaderFromURL(url.c_str(), srAttr, &reader);
        if (FAILED(hr) && srAttr) {
            hr = MFCreateSourceReaderFromURL(url.c_str(), nullptr, &reader);
        }
    }
    if (srAttr) {
        srAttr->Release();
        srAttr = nullptr;
    }
    if (FAILED(hr) || !reader) {
        LOGE("MFCreateSourceReader (file/URL) failed: 0x{:x}", static_cast<unsigned>(hr));
        mfStartupRelease();
        return false;
    }

    disableNonVideoSourceReaderStreams(reader);

    m_reader = reader;
    QueryPerformanceCounter(&t1);
    const double msMfCreate = qpcMs(t0, t1);

    QueryPerformanceCounter(&t0);

    m_pixelAspectNum = 1;
    m_pixelAspectDen = 1;
    m_gpuNv12Path = false;
    IMFMediaType* nativeType = nullptr;
    if (SUCCEEDED(reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &nativeType))
        && nativeType) {
        UINT64 par = 0;
        if (SUCCEEDED(nativeType->GetUINT64(MF_MT_PIXEL_ASPECT_RATIO, &par))) {
            const UINT32 n = static_cast<UINT32>(par >> 32);
            const UINT32 d = static_cast<UINT32>(par & 0xffffffffu);
            if (n > 0u && d > 0u) {
                m_pixelAspectNum = static_cast<int>(n);
                m_pixelAspectDen = static_cast<int>(d);
            }
        }
        nativeType->Release();
    }

    if (!configureOutputType(reader, MFVideoFormat_NV12)) {
        if (!configureOutputType(reader, MFVideoFormat_RGB32)) {
            LOGE("Could not set video output type (NV12/RGB32)");
            close();
            return false;
        }
        m_outputRgb32 = true;
        m_gpuNv12Path = false;
    } else {
        m_outputRgb32 = false;
        m_gpuNv12Path = true;
    }

    if (!readVideoOutputInfo(reader, m_width, m_height, m_strideBytes, m_outputRgb32, m_frameDurationSec)) {
        LOGE("Could not read video dimensions");
        close();
        return false;
    }

    if (m_pixelAspectNum == 1 && m_pixelAspectDen == 1) {
        IMFMediaType* curType = nullptr;
        if (SUCCEEDED(reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &curType))
            && curType) {
            UINT64 par = 0;
            if (SUCCEEDED(curType->GetUINT64(MF_MT_PIXEL_ASPECT_RATIO, &par))) {
                const UINT32 n = static_cast<UINT32>(par >> 32);
                const UINT32 d = static_cast<UINT32>(par & 0xffffffffu);
                if (n > 0u && d > 0u) {
                    m_pixelAspectNum = static_cast<int>(n);
                    m_pixelAspectDen = static_cast<int>(d);
                }
            }
            curType->Release();
        }
    }

    constexpr int kMaxVideoDim = 8192;
    if (m_width <= 0 || m_height <= 0 || m_width > kMaxVideoDim || m_height > kMaxVideoDim) {
        LOGE("Video dimensions out of supported range: {}x{}", m_width, m_height);
        close();
        return false;
    }

    QueryPerformanceCounter(&t1);
    const double msCfg = qpcMs(t0, t1);

    QueryPerformanceCounter(&t0);
    resolveMetadataDurationSec(reader, utf8Path, m_durationSec, m_durationReliable);
    m_lazyDurationProbeDone = false;
    m_skipLazyStreamProbe = pathLooksIsoBmff(utf8Path);
    QueryPerformanceCounter(&t1);
    const double msMeta = qpcMs(t0, t1);

    if (m_outputRgb32 || !m_gpuNv12Path) {
        m_rgba.resize(static_cast<size_t>(m_width) * static_cast<size_t>(m_height) * 4u);
    } else {
        m_rgba.clear();
        m_nv12.clear();
        m_nv12YBufRows = 0;
    }

    QueryPerformanceCounter(&t0);
    seek(0.0);
    if (!decodeFrame()) {
        LOGE("Failed to decode first video frame (codec/buffer layout may be unsupported)");
        close();
        return false;
    }
    QueryPerformanceCounter(&t1);
    const double msFirstDecode = qpcMs(t0, t1);

    LARGE_INTEGER tOpenEnd;
    QueryPerformanceCounter(&tOpenEnd);
    LOGI(
        "MFVideoReader open \"{}\": total {:.2f} ms (mfCreate {:.2f}, cfg {:.2f}, meta {:.2f}, firstDecode "
        "{:.2f}) nv12Gpu={} hwTransformsAttr={} durationReliable={} durationSec={:.6f} skipLazyStreamProbe={}",
        utf8Path, qpcMs(tOpenStart, tOpenEnd), msMfCreate, msCfg, msMeta, msFirstDecode, m_gpuNv12Path,
        hwTransformsAttrRequested,
        m_durationReliable, m_durationSec, m_skipLazyStreamProbe);
    if (m_gpuNv12Path) {
        m_nv12DiagLogFrames = 3;
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
    m_pixelAspectNum = 1;
    m_pixelAspectDen = 1;
    m_durationSec = m_positionSec = 0.0;
    m_durationReliable = false;
    m_frameDurationSec = 1.0 / 30.0;
    m_gpuNv12Path = false;
    m_lazyDurationProbeDone = false;
    m_skipLazyStreamProbe = false;
    m_nv12.clear();
    m_nv12YBufRows = 0;
    m_nv12DiagLogFrames = 0;
    m_rgba.clear();
}

bool MFVideoReader::isOpen() const
{
    return m_reader != nullptr;
}

bool MFVideoReader::lazyProbePresentationDuration()
{
    if (!m_reader || m_lazyDurationProbeDone) {
        return false;
    }
    if (m_durationSec > kDurationEpsilonSec) {
        m_lazyDurationProbeDone = true;
        return false;
    }
    if (m_skipLazyStreamProbe) {
        LOGI(
            "[duration] lazyProbe skipped: ISO BMFF path (stream EOS scan disabled after open); if duration "
            "unknown, moov was not in file head/tail parse window");
        m_lazyDurationProbeDone = true;
        return false;
    }
    m_lazyDurationProbeDone = true;
    IMFSourceReader* reader = static_cast<IMFSourceReader*>(m_reader);
    LOGI("[duration] lazyProbe: scanning video stream toward EOS (time/sample capped)");
    LARGE_INTEGER tProbe0;
    LARGE_INTEGER tProbe1;
    QueryPerformanceCounter(&tProbe0);
    const ProbeVideoEndResult pr = probeVideoEndSec(reader);
    QueryPerformanceCounter(&tProbe1);
    LOGI("lazyProbePresentationDuration: {:.2f} ms", qpcMs(tProbe0, tProbe1));
    if (pr.reachedEos && pr.endSec > 0.001) {
        m_durationSec = pr.endSec;
        m_durationReliable = true;
    } else if (!pr.reachedEos && pr.endSec > 0.001) {
        LOGW(
            "lazyProbePresentationDuration: incomplete scan (max sample time {:.3f}s) — not using as duration; "
            "use container metadata or a full read for long files",
            pr.endSec);
    }
    // probeVideoEndSec leaves the reader at t=0; caller must resync to the current composition time.
    return true;
}

void MFVideoReader::setPipelineTimingLog(bool on)
{
    m_logPipelineTiming = on;
}

void MFVideoReader::seek(double seconds, bool flushStreams)
{
    if (!m_reader) {
        return;
    }
    LARGE_INTEGER tSeek0 {};
    LARGE_INTEGER tAfterFlush {};
    LARGE_INTEGER tSeek1 {};
    if (m_logPipelineTiming) {
        QueryPerformanceCounter(&tSeek0);
    }
    IMFSourceReader* reader = static_cast<IMFSourceReader*>(m_reader);
    constexpr double kMaxSeekSec = 86400.0 * 366.0 * 50.0;
    seconds = std::max(0.0, std::min(seconds, kMaxSeekSec));
    if (m_durationSec > kDurationEpsilonSec) {
        seconds = std::min(seconds, seekEndCapSec(m_durationSec, m_frameDurationSec));
    }

    const double pos0 = m_positionSec;
    constexpr double kTimeTol = 1.0 / 240.0;
    // Without Flush, SetCurrentPosition can block ~1s; forward within this window is cheaper via ReadSample.
    constexpr double kForwardCatchupSec = 1.35;
    const bool backward = seconds < pos0 - kTimeTol;
    const bool effectiveFlush = flushStreams || backward;

    if (!flushStreams && !backward && seconds >= pos0 - kTimeTol && seconds <= pos0 + kForwardCatchupSec) {
        if (m_logPipelineTiming) {
            QueryPerformanceCounter(&tSeek1);
            m_lastSeekFlushMs = 0.f;
            m_lastSeekSetPosMs = 0.f;
            m_lastSeekMs = static_cast<float>(qpcMs(tSeek0, tSeek1));
        }
        return;
    }

    if (effectiveFlush) {
        flushSourceReaderVideoFirst(reader);
    }
    if (m_logPipelineTiming) {
        QueryPerformanceCounter(&tAfterFlush);
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
    HRESULT hr = reader->SetCurrentPosition(GUID_NULL, var);
    PropVariantClear(&var);
    if (FAILED(hr)) {
        LOGD("SetCurrentPosition failed: 0x{:x}", static_cast<unsigned>(hr));
    }

    m_positionSec = seconds;
    if (m_logPipelineTiming) {
        QueryPerformanceCounter(&tSeek1);
        m_lastSeekFlushMs =
            effectiveFlush ? static_cast<float>(qpcMs(tSeek0, tAfterFlush)) : 0.f;
        m_lastSeekSetPosMs = static_cast<float>(qpcMs(tAfterFlush, tSeek1));
        m_lastSeekMs = static_cast<float>(qpcMs(tSeek0, tSeek1));
    }
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

bool MFVideoReader::decodeFrameThrough(double targetSec, int maxReadSamplesCap)
{
    if (!m_reader || m_width <= 0 || m_height <= 0) {
        return false;
    }
    LARGE_INTEGER tDec0 {};
    LARGE_INTEGER tDec1 {};
    if (m_logPipelineTiming) {
        QueryPerformanceCounter(&tDec0);
        m_lastDecodeThroughReads = 0;
        m_lastDecodeThroughHitCap = false;
    }
    IMFSourceReader* reader = static_cast<IMFSourceReader*>(m_reader);

    if (m_durationSec > kDurationEpsilonSec) {
        targetSec = std::min(targetSec, seekEndCapSec(m_durationSec, m_frameDurationSec));
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
    // Read budget must cover keyframe→target; scale with span. 130 ≈ headroom above nominal fps; 2048
    // base iterations for short clips. When duration unknown, span uses targetSec so long seeks work.
    const double spanForIterBudget =
        (m_durationSec > kDurationEpsilonSec) ? m_durationSec : (std::max)(targetSec, 0.0);
    const int kBudget = static_cast<int>(spanForIterBudget * 130.0 + 2048.0);
    int kMaxReadIterations = (std::min)(200000, (std::max)(16384, kBudget));
    if (maxReadSamplesCap > 0) {
        kMaxReadIterations = (std::min)(kMaxReadIterations, maxReadSamplesCap);
    }
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
            if (m_logPipelineTiming) {
                QueryPerformanceCounter(&tDec1);
                m_lastDecodeThroughMs = static_cast<float>(qpcMs(tDec0, tDec1));
                m_lastDecodeThroughReads = readIterations;
            }
            return false;
        }
        if (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (hold) {
                const bool ok = MFVideoReaderDecodeDetail::finalize(*this, hold, holdReaderTs);
                if (m_logPipelineTiming) {
                    QueryPerformanceCounter(&tDec1);
                    m_lastDecodeThroughMs = static_cast<float>(qpcMs(tDec0, tDec1));
                    m_lastDecodeThroughReads = readIterations;
                }
                return ok;
            }
            m_positionSec = m_durationSec > 0.0 ? m_durationSec : m_positionSec;
            if (m_logPipelineTiming) {
                QueryPerformanceCounter(&tDec1);
                m_lastDecodeThroughMs = static_cast<float>(qpcMs(tDec0, tDec1));
                m_lastDecodeThroughReads = readIterations;
            }
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
            const bool ok = MFVideoReaderDecodeDetail::finalize(*this, sample, ts);
            if (m_logPipelineTiming) {
                QueryPerformanceCounter(&tDec1);
                m_lastDecodeThroughMs = static_cast<float>(qpcMs(tDec0, tDec1));
                m_lastDecodeThroughReads = readIterations;
            }
            return ok;
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
        if (m_logPipelineTiming) {
            m_lastDecodeThroughHitCap = true;
        }
        const bool ok = MFVideoReaderDecodeDetail::finalize(*this, hold, holdReaderTs);
        if (m_logPipelineTiming) {
            QueryPerformanceCounter(&tDec1);
            m_lastDecodeThroughMs = static_cast<float>(qpcMs(tDec0, tDec1));
            m_lastDecodeThroughReads = readIterations;
        }
        return ok;
    }
    LOGW("decodeFrameThrough: no video sample after {} iterations", readIterations);
    if (m_logPipelineTiming) {
        QueryPerformanceCounter(&tDec1);
        m_lastDecodeThroughMs = static_cast<float>(qpcMs(tDec0, tDec1));
        m_lastDecodeThroughReads = readIterations;
    }
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

float MFVideoReader::displayWidthForAspect() const
{
    const int den = (m_pixelAspectDen > 0) ? m_pixelAspectDen : 1;
    const int num = (m_pixelAspectNum > 0) ? m_pixelAspectNum : 1;
    return static_cast<float>(m_width) * static_cast<float>(num) / static_cast<float>(den);
}

float MFVideoReader::displayHeightForAspect() const
{
    return static_cast<float>(m_height);
}

void MFVideoReader::close()
{
    m_width = m_height = 0;
    m_pixelAspectNum = 1;
    m_pixelAspectDen = 1;
    m_durationSec = m_positionSec = 0.0;
    m_durationReliable = false;
    m_frameDurationSec = 1.0 / 30.0;
    m_gpuNv12Path = false;
    m_lazyDurationProbeDone = false;
    m_skipLazyStreamProbe = false;
    m_nv12.clear();
    m_nv12YBufRows = 0;
    m_nv12DiagLogFrames = 0;
    m_rgba.clear();
}

bool MFVideoReader::isOpen() const
{
    return false;
}

void MFVideoReader::seek(double, bool)
{
}

bool MFVideoReader::decodeFrame()
{
    return false;
}

bool MFVideoReader::decodeFrameThrough(double, int)
{
    return false;
}

bool MFVideoReader::lazyProbePresentationDuration()
{
    return false;
}

void MFVideoReader::setPipelineTimingLog(bool)
{
}

}  // namespace baktsiu

#endif
