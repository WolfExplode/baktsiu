# libjxl (Windows, MSVC)

JPEG XL decode support for Bak-Tsiu. Populate this folder from vcpkg:

```powershell
vcpkg install libjxl:x64-windows
```

Then copy (or refresh) from your vcpkg install tree, e.g. `installed\x64-windows`:

- `include/jxl/` → `third_party/libjxl/include/jxl/`
- `lib/jxl*.lib` → `third_party/libjxl/lib/`
- Runtime DLLs → `third_party/libjxl/bin/` (`jxl*.dll`, `brotli*.dll`, `hwy.dll`, `lcms2-2.dll`)

CMake: `-DUSE_JXL=ON -DJXL_ROOT=<path-to-this-folder>` (default when unset).
