#include <string>
#include <vector>

#include "app.h"
#include "docopt/docopt.h"
#include "mpv_gl_player.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <shellapi.h>
#include <windows.h>
#endif

static const char USAGE[] =
R"(Bak-Tsiu, examining every image details.

    Usage:
      baktsiu
      baktsiu [--split | --columns] <name>...
      baktsiu (-h | --help)
      baktsiu --version

    Options:
      -h --help     Show this screen.
      --version     Show version.
)";

#if defined(_WIN32)
static std::string wideArgToUtf8(const wchar_t* w)
{
    if (!w) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], n, nullptr, nullptr);
    return out;
}
#endif

static int runMain(std::vector<std::string> argList)
{
    baktsiu::App app;

#ifdef __APPLE__
    // If we launch from finder, there might be a "-psn" argument represents
    // a unique process serial number. We have to remove it before we parse
    // arguments with docopt.
    for (auto iter = argList.begin(); iter != argList.end(); ++iter) {
        if (iter->find("-psn") == 0) {
            argList.erase(iter);
            break;
        }
    }
#endif

    constexpr bool showHelpWhenRequest = true;
    PushRangeMarker("Parse Option");
    std::map<std::string, docopt::value> args =
        docopt::docopt(USAGE, argList, showHelpWhenRequest, "Bak-Tsiu v" VERSION);
    PopRangeMarker();

    if (app.initialize(u8"目睭 Bak Tsiu", 1280, 720)) {
        if (args["<name>"]) {
            std::vector<std::string> names = args["<name>"].asStringList();
            std::vector<std::string> imagePaths;
            std::vector<std::string> videoPaths;
            for (const std::string& n : names) {
                if (baktsiu::MpvGlPlayer::isSupportedExtension(n)) {
                    videoPaths.push_back(n);
                } else {
                    imagePaths.push_back(n);
                }
            }
            if (!videoPaths.empty()) {
                app.openVideoPaths(videoPaths);
            }
            if (!imagePaths.empty()) {
                app.importImageFiles(imagePaths, true);
            }
        }

        baktsiu::CompositeFlags composition = baktsiu::CompositeFlags::Top;
        if (args["--split"].asBool()) {
            composition = baktsiu::CompositeFlags::Split;
        } else if (args["--columns"].asBool()) {
            composition = baktsiu::CompositeFlags::SideBySide;
        }

        app.run(composition);
        app.release();
        return 0;
    }

    return 1;
}

int main(int argc, char** argv)
{
#if defined(_WIN32)
    (void)argc;
    (void)argv;
    int wargc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv) {
        return 1;
    }
    std::vector<std::string> argList;
    for (int i = 1; i < wargc; ++i) {
        argList.push_back(wideArgToUtf8(wargv[i]));
    }
    LocalFree(wargv);
    return runMain(std::move(argList));
#else
    return runMain({ argv + 1, argv + argc });
#endif
}
