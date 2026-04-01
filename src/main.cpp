#include <iostream>
#include <string>
#include <vector>

#include "app.h"
#include "docopt/docopt.h"
#include "mf_video_reader.h"

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


int main(int argc, char** argv)
{
    baktsiu::App app;
    std::vector<std::string> argList = { argv + 1, argv + argc };

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

// return 0;

    constexpr bool showHelpWhenRequest = true;
    PushRangeMarker("Parse Option");
    std::map<std::string, docopt::value> args
        = docopt::docopt(USAGE, argList, showHelpWhenRequest, "Bak-Tsiu v" VERSION);
    PopRangeMarker();

    if (app.initialize(u8"目睭 Bak Tsiu", 1280, 720))
    {
        if (args["<name>"]) {
            std::vector<std::string> names = args["<name>"].asStringList();
            std::vector<std::string> imagePaths;
            std::vector<std::string> videoPaths;
            for (const std::string& n : names) {
                if (baktsiu::MFVideoReader::isSupportedExtension(n)) {
                    videoPaths.push_back(n);
                } else {
                    imagePaths.push_back(n);
                }
            }
            if (!videoPaths.empty()) {
                app.openVideoFile(videoPaths[0]);
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
