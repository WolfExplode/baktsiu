# libmpv (Windows, MSVC)

Place a **shinchiro** `mpv-dev-*.7z` extract here so this folder matches `MPV_ROOT` in CMake.

Expected layout:

- `include/mpv/client.h`, `render.h`, `render_gl.h`, …
- `libmpv-2.dll`
- `libmpv-2.lib` — MSVC import library (generate from the DLL with `dumpbin` + `lib.exe` if you only have `libmpv.dll.a`)
- `libmpv-2.def` — optional; used to build `libmpv-2.lib`

CMake: `-DUSE_VIDEO=ON -DMPV_ROOT=<path-to-this-folder>` (or set the `MPV_ROOT` cache variable in your IDE).

Release downloads: [mpv-winbuild-cmake releases](https://github.com/shinchiro/mpv-winbuild-cmake/releases).
