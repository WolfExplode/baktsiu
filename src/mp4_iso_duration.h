#ifndef BAKTSIU_MP4_ISO_DURATION_H_
#define BAKTSIU_MP4_ISO_DURATION_H_

#include <string>

namespace baktsiu
{

// True for extensions we treat as ISO BMFF (.mp4, .m4v, .mov).
bool pathLooksIsoBmff(const std::string& utf8Path);

// Best-effort duration from ISO BMFF (MP4/MOV) moov without decoding. Returns 0 on failure.
double tryReadIsoBmffDurationSecondsFromUtf8Path(const std::string& utf8Path);

}  // namespace baktsiu

#endif
