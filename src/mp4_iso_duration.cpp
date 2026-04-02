#include "mp4_iso_duration.h"

#include "common.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#endif

namespace baktsiu
{
namespace
{

#if defined(_WIN32)

std::wstring utf8ToWideLocal(const std::string& utf8)
{
    if (utf8.empty()) {
        return L"";
    }
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

std::wstring fullPathWideFromUtf8(const std::string& utf8Path)
{
    std::wstring wpath = utf8ToWideLocal(utf8Path);
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

#endif

uint32_t readU32be(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24u) | (static_cast<uint32_t>(p[1]) << 16u)
        | (static_cast<uint32_t>(p[2]) << 8u) | static_cast<uint32_t>(p[3]);
}

uint64_t readU64be(const uint8_t* p)
{
    return (static_cast<uint64_t>(readU32be(p)) << 32u) | static_cast<uint64_t>(readU32be(p + 4));
}

bool boxTypeIs(const uint8_t* type, const char* fourcc)
{
    return std::memcmp(type, fourcc, 4) == 0;
}

// mvhd / mdhd FullBox: version 0 or 1, then timescale + duration.
bool parseMdhdLikeDuration(const uint8_t* d, size_t len, double& outSec)
{
    if (len < 4) {
        return false;
    }
    const uint8_t ver = d[0];
    if (ver == 0 && len >= 24) {
        const uint32_t timescale = readU32be(d + 12);
        const uint32_t duration = readU32be(d + 16);
        if (timescale > 0 && duration > 0) {
            outSec = static_cast<double>(duration) / static_cast<double>(timescale);
            return outSec > 1e-6;
        }
    } else if (ver == 1 && len >= 36) {
        const uint32_t timescale = readU32be(d + 20);
        const uint64_t duration = readU64be(d + 24);
        if (timescale > 0 && duration > 0) {
            outSec = static_cast<double>(duration) / static_cast<double>(timescale);
            return outSec > 1e-6;
        }
    }
    return false;
}

template <typename Fn>
void forEachChildBox(const uint8_t* body, size_t bodyLen, Fn&& fn)
{
    size_t off = 0;
    while (off + 8 <= bodyLen) {
        uint64_t boxSize = readU32be(body + off);
        const uint8_t* boxType = body + off + 4;
        size_t header = 8;
        if (boxSize == 1) {
            if (off + 16 > bodyLen) {
                break;
            }
            boxSize = readU64be(body + off + 8);
            header = 16;
        } else if (boxSize == 0) {
            boxSize = bodyLen - off;
        }
        if (boxSize < header || off + boxSize > bodyLen) {
            break;
        }
        const uint8_t* inner = body + off + header;
        const size_t innerLen = static_cast<size_t>(boxSize - header);
        fn(boxType, inner, innerLen);
        off += static_cast<size_t>(boxSize);
    }
}

bool scanMdiaForMdhd(const uint8_t* mdiaBody, size_t mdiaLen, double& outSec)
{
    bool ok = false;
    forEachChildBox(mdiaBody, mdiaLen, [&](const uint8_t* type, const uint8_t* inner, size_t innerLen) {
        if (boxTypeIs(type, "mdhd") && innerLen >= 4) {
            double d = 0.0;
            if (parseMdhdLikeDuration(inner, innerLen, d)) {
                if (d > outSec) {
                    outSec = d;
                }
                ok = true;
            }
        }
    });
    return ok;
}

bool scanTrakForMdhd(const uint8_t* trakBody, size_t trakLen, double& outSec)
{
    bool ok = false;
    forEachChildBox(trakBody, trakLen, [&](const uint8_t* type, const uint8_t* inner, size_t innerLen) {
        if (boxTypeIs(type, "mdia")) {
            if (scanMdiaForMdhd(inner, innerLen, outSec)) {
                ok = true;
            }
        }
    });
    return ok;
}

bool findDurationInMoovBody(const uint8_t* moovBody, size_t moovLen, double& outSec)
{
    outSec = 0.0;
    double mvhdDur = 0.0;
    bool haveMvhd = false;
    double mdhdMax = 0.0;
    bool haveMdhd = false;

    forEachChildBox(moovBody, moovLen, [&](const uint8_t* type, const uint8_t* inner, size_t innerLen) {
        if (boxTypeIs(type, "mvhd") && innerLen >= 4) {
            if (parseMdhdLikeDuration(inner, innerLen, mvhdDur)) {
                haveMvhd = true;
            }
        } else if (boxTypeIs(type, "trak")) {
            double t = 0.0;
            if (scanTrakForMdhd(inner, innerLen, t) && t > mdhdMax) {
                mdhdMax = t;
                haveMdhd = true;
            }
        }
    });

    if (haveMvhd && mvhdDur > 1e-6) {
        outSec = mvhdDur;
        return true;
    }
    if (haveMdhd && mdhdMax > 1e-6) {
        outSec = mdhdMax;
        return true;
    }
    return false;
}

bool parseTopLevelForMoov(const uint8_t* data, size_t len, double& outSec)
{
    size_t off = 0;
    while (off + 8 <= len) {
        uint64_t boxSize = readU32be(data + off);
        const uint8_t* boxType = data + off + 4;
        size_t header = 8;
        if (boxSize == 1) {
            if (off + 16 > len) {
                break;
            }
            boxSize = readU64be(data + off + 8);
            header = 16;
        } else if (boxSize == 0) {
            boxSize = len - off;
        }
        if (boxSize < header || off + boxSize > len) {
            break;
        }
        const uint8_t* inner = data + off + header;
        const size_t innerLen = static_cast<size_t>(boxSize - header);
        if (boxTypeIs(boxType, "moov")) {
            return findDurationInMoovBody(inner, innerLen, outSec);
        }
        off += static_cast<size_t>(boxSize);
    }
    return false;
}

}  // namespace

bool pathLooksIsoBmff(const std::string& p)
{
    std::string lower = p;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const char* ext[] = {".mp4", ".m4v", ".mov"};
    for (const char* e : ext) {
        const size_t el = std::strlen(e);
        if (lower.size() >= el && lower.compare(lower.size() - el, el, e) == 0) {
            return true;
        }
    }
    return false;
}

double tryReadIsoBmffDurationSecondsFromUtf8Path(const std::string& utf8Path)
{
#if !defined(_WIN32)
    (void)utf8Path;
    return 0.0;
#else
    if (!pathLooksIsoBmff(utf8Path)) {
        return 0.0;
    }
    const std::wstring wide = fullPathWideFromUtf8(utf8Path);
    if (wide.empty()) {
        LOGW("[duration][iso] \"{}\": empty path after UTF-8->wide / GetFullPathName", utf8Path);
        return 0.0;
    }
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, wide.c_str(), L"rb") != 0 || !fp) {
        LOGW("[duration][iso] \"{}\": fopen failed (check path / permissions)", utf8Path);
        return 0.0;
    }
    if (_fseeki64(fp, 0, SEEK_END) != 0) {
        LOGW("[duration][iso] \"{}\": fseek(END) failed", utf8Path);
        std::fclose(fp);
        return 0.0;
    }
    const long long fileLen = _ftelli64(fp);
    // Only read head/tail windows; file size can be huge — do not reject large files.
    if (fileLen < 32) {
        LOGW("[duration][iso] \"{}\": file too small ({} bytes)", utf8Path, fileLen);
        std::fclose(fp);
        return 0.0;
    }
    constexpr long long kMaxHeadTailBytes = 16LL * 1024 * 1024;
    const size_t readSize = static_cast<size_t>((std::min)(kMaxHeadTailBytes, fileLen));
    std::vector<uint8_t> head(readSize);
    if (_fseeki64(fp, 0, SEEK_SET) != 0) {
        LOGW("[duration][iso] \"{}\": fseek(SET) failed", utf8Path);
        std::fclose(fp);
        return 0.0;
    }
    const size_t got = std::fread(head.data(), 1, readSize, fp);
    if (got < 32) {
        LOGW("[duration][iso] \"{}\": fread head got {} bytes", utf8Path, got);
        std::fclose(fp);
        return 0.0;
    }
    head.resize(got);

    double dur = 0.0;
    if (parseTopLevelForMoov(head.data(), head.size(), dur)) {
        LOGI(
            "[duration][iso] \"{}\": moov in first {} bytes -> {:.6f}s (fileLen={})", utf8Path, got, dur,
            fileLen);
        std::fclose(fp);
        return dur;
    }
    LOGI(
        "[duration][iso] \"{}\": no moov in first {} bytes; trying tail window (readSize={} fileLen={})",
        utf8Path, got, readSize, fileLen);
    const long long tailStart = fileLen - static_cast<long long>(readSize);
    if (tailStart > 0 && static_cast<size_t>(fileLen) > got) {
        std::vector<uint8_t> tail(readSize);
        if (_fseeki64(fp, tailStart, SEEK_SET) == 0) {
            const size_t got2 = std::fread(tail.data(), 1, readSize, fp);
            if (got2 >= 32) {
                tail.resize(got2);
                if (parseTopLevelForMoov(tail.data(), tail.size(), dur)) {
                    LOGI(
                        "[duration][iso] \"{}\": moov in tail (offset {} size {}) -> {:.6f}s", utf8Path,
                        tailStart, got2, dur);
                    std::fclose(fp);
                    return dur;
                }
                LOGW(
                    "[duration][iso] \"{}\": tail window [{} .. {}] has no parsable moov/duration", utf8Path,
                    tailStart, tailStart + static_cast<long long>(got2));
            } else {
                LOGW("[duration][iso] \"{}\": fread tail got {} bytes", utf8Path, got2);
            }
        } else {
            LOGW("[duration][iso] \"{}\": fseek tail at {} failed", utf8Path, tailStart);
        }
    } else {
        LOGW(
            "[duration][iso] \"{}\": file fits in single head read; moov not found (non-ISO or corrupt moov)",
            utf8Path);
    }
    std::fclose(fp);
    return 0.0;
#endif
}

}  // namespace baktsiu
