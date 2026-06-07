#pragma warning(push)
#pragma warning(disable: 4819 4566)
#include "portable_file_dialogs.h"
#pragma warning(pop)

#include <algorithm>
#include <string>
#include <vector>

#pragma warning(push)
#pragma warning(disable: 4819 4566)
#include <fx/gltf.h>
#pragma warning(pop)

#include "app.h"

namespace
{

bool endsWith(const std::string& value, const std::string& ending)
{
    if (ending.size() > value.size()) {
        return false;
    }
    return std::equal(ending.rbegin(), ending.rend(), value.rbegin());
}

}  // namespace

namespace baktsiu
{

void App::appendAction(Action&& action)
{
    mActionStack.push_back(action);
    if (mActionStack.size() > 16) {
        mActionStack.pop_front();
    }
}

void App::undoAction()
{
    if (mActionStack.empty()) {
        return;
    } else if (mCurAction.type != Action::Type::Unknown) {
        return;
    }

    Action action = mActionStack.back();
    mActionStack.pop_back();

    if (action.type == Action::Type::Remove) {
        mCurAction = action;
        importImageFiles(action.filepathArray, false, &action.imageIdxArray);
    } else if (action.type == Action::Type::Add) {
        const int imageCount = static_cast<int>(mMediaList.size());
        for (int i = imageCount - 1; i >= 0; --i) {
            for (auto index : action.imageIdxArray) {
                uint8_t layerIndex = Action::extractLayerIndex(index);
                if (layerIndex == i) {
                    mMediaList.erase(mMediaList.begin() + layerIndex);
                    break;
                }
            }
        }

        mTopImageIndex = action.prevTopImageIdx;
        mCmpImageIndex = action.prevCmpImageIdx;

        if (mTopImageIndex == -1 && mCmpImageIndex == -1) {
            mCompositeFlags = CompositeFlags::Top;
        }

        mTexturePool.cleanUnusedTextures();

    } else if (action.type == Action::Type::Move) {
        std::vector<MediaUPtr> newMediaList;
        newMediaList.reserve(mMediaList.size());

        for (auto index : action.imageIdxArray) {
            uint8_t id = Action::extractImageId(index);
            for (auto& image : mMediaList) {
                if (image && image->id() == id) {
                    newMediaList.push_back(std::move(image));
                    break;
                }
            }
        }

        mMediaList.swap(newMediaList);
        mTopImageIndex = action.prevTopImageIdx;
        mCmpImageIndex = action.prevCmpImageIdx;
    }
}

void App::showImportImageDlg()
{
    static std::vector<std::string> filters = {
#ifdef USE_JXL
        "Supported Image Files", "*.bmp; *.exr; *.gif; *.jpg; *.jxl; *.hdr; *.png; *.bts; *.mp4; *.mov; *.wmv; *.avi; *.mkv; *.webm; *.m4v" ,
#else
        "Supported Image Files", "*.bmp; *.exr; *.gif; *.jpg; *.hdr; *.png; *.bts; *.mp4; *.mov; *.wmv; *.avi; *.mkv; *.webm; *.m4v" ,
#endif
        "BMP (*.BMP)", "*.bmp",
        "OpenEXR (*.EXR)", "*.exr",
        "GIF (*.GIF)", "*.gif",
        "JPEG (*.JPG, *.JPEG)", "*.jpg; *.jpeg",
#ifdef USE_JXL
        "JPEG XL (*.JXL)", "*.jxl",
#endif
        "HDR (*.HDR)", "*.hdr",
        "PNG (*.PNG)", "*.png",
        "Video (*.MP4, *.MOV, ...)", "*.mp4; *.mov; *.wmv; *.avi; *.mkv; *.webm; *.m4v",
        "Bak-Tsiu Session (*.BTS)", "*.bts",
    };

    std::vector<std::string> selection = pfd::open_file("Select image file(s)", "", filters, true).result();
    auto iter = selection.begin();
    while (iter != selection.end()) {
        if (endsWith(*iter, ".bts")) {
            openSession(*iter);
            iter = selection.erase(iter);
        } else {
            ++iter;
        }
    }

    std::vector<std::string> videoPaths;
    std::vector<std::string> imagePaths;
    for (const std::string& path : selection) {
        if (MpvGlPlayer::isSupportedExtension(path)) {
            videoPaths.push_back(path);
        } else {
            imagePaths.push_back(path);
        }
    }

    if (!videoPaths.empty()) {
#if defined(USE_VIDEO)
        std::vector<int> addedIdx;
        addedIdx.reserve(videoPaths.size());
        for (const std::string& p : videoPaths) {
            const int idx = addVideoPath(p);
            if (idx >= 0) {
                addedIdx.push_back(idx);
                if (mTopImageIndex < 0) {
                    mTopImageIndex = idx;
                } else if (mCmpImageIndex < 0 && idx != mTopImageIndex) {
                    mCmpImageIndex = idx;
                }
            }
        }
        applyImportedVideoListSelection(addedIdx);
        openVideosFromSelection();
#else
        LOGW("Video playback is disabled in this build (USE_VIDEO).");
#endif
    } else if (!imagePaths.empty()) {
        importImageFiles(imagePaths, true);
    }
}

void App::showExportSessionDlg()
{
    static std::vector<std::string> filters = {
        "Bak-Tsiu Session (*.BTS)", "*.bts",
    };

    std::string filepath = pfd::save_file("Export compare session", "", filters, true).result();

    if (!filepath.empty()) {
        if (!endsWith(filepath, ".bts")) {
            filepath += ".bts";
        }

        saveSession(filepath);
    }
}

void App::importImageFiles(const std::vector<std::string>& filepathArray,
                           bool recordAction, std::vector<uint16_t>* imageIdxArray)
{
    const size_t imageNum = filepathArray.size();

    if (imageNum == 0) {
        return;
    }

    Action action(Action::Type::Add, mTopImageIndex, mCmpImageIndex);
    std::vector<std::string> videoPaths;

    for (size_t i = 0; i < imageNum; ++i) {
        std::string path = filepathArray[i];
        std::replace(path.begin(), path.end(), '\\', '/');

        if (MpvGlPlayer::isSupportedExtension(path)) {
            videoPaths.push_back(path);
            continue;
        }

        if (!Texture::isSupported(path)) {
            LOGW("Unsupported image type for \"{}\"", path);
            continue;
        }

        uint8_t insertIdx = static_cast<uint8_t>(mMediaList.size());
        uint8_t id = 0;

        if (imageIdxArray) {
            uint16_t value = (*imageIdxArray)[i];
            id = Action::extractImageId(value);
            insertIdx = Action::extractLayerIndex(value);

            if (insertIdx <= mTopImageIndex) {
                ++mTopImageIndex;
            }

            if (insertIdx <= mCmpImageIndex) {
                ++mCmpImageIndex;
            }

            LOGI("Reimport {} to layer {}", path, insertIdx);
        } else {
            LOGI("Import {}", path);
        }

        auto newTexture = mTexturePool.acquireTexture(path);
        auto newImage = std::make_unique<StaticImageSource>(newTexture, id);
        const ImageType imageType = Texture::getImageType(path);
        if (imageType == ImageType::HDR || imageType == ImageType::OPENEXR) {
            newImage->setColorEncodingType(ColorEncodingType::Linear);
        }

        mMediaList.insert(mMediaList.begin() + insertIdx, std::move(newImage));

        action.filepathArray.push_back(path);
        action.imageIdxArray.push_back(Action::composeImageIndex(id, insertIdx));
        mUpdateImageSelection = true;
    }

    if (!videoPaths.empty()) {
#if defined(USE_VIDEO)
        std::vector<int> addedIdx;
        addedIdx.reserve(videoPaths.size());
        for (const std::string& p : videoPaths) {
            const int idx = addVideoPath(p);
            if (idx >= 0) {
                addedIdx.push_back(idx);
                if (mTopImageIndex < 0) {
                    mTopImageIndex = idx;
                } else if (mCmpImageIndex < 0 && idx != mTopImageIndex) {
                    mCmpImageIndex = idx;
                }
            }
        }
        applyImportedVideoListSelection(addedIdx);
        openVideosFromSelection();
#else
        LOGW("Video playback is disabled in this build (USE_VIDEO).");
#endif
    }

    if (recordAction) {
        appendAction(std::move(action));
    }
}

StaticImageSource* App::getTopImage()
{
    if (mTopImageIndex < 0 || static_cast<size_t>(mTopImageIndex) >= mMediaList.size()) {
        return nullptr;
    }
    auto* m = mMediaList[mTopImageIndex].get();
    return (m && !m->isVideo()) ? static_cast<StaticImageSource*>(m) : nullptr;
}

const StaticImageSource* App::getTopImage() const
{
    if (mTopImageIndex < 0 || static_cast<size_t>(mTopImageIndex) >= mMediaList.size()) {
        return nullptr;
    }
    const auto* m = mMediaList[mTopImageIndex].get();
    return (m && !m->isVideo()) ? static_cast<const StaticImageSource*>(m) : nullptr;
}

StaticImageSource* App::getCmpImage()
{
    if (mCmpImageIndex < 0 || static_cast<size_t>(mCmpImageIndex) >= mMediaList.size()) {
        return nullptr;
    }
    auto* m = mMediaList[mCmpImageIndex].get();
    return (m && !m->isVideo()) ? static_cast<StaticImageSource*>(m) : nullptr;
}

const StaticImageSource* App::getCmpImage() const
{
    if (mCmpImageIndex < 0 || static_cast<size_t>(mCmpImageIndex) >= mMediaList.size()) {
        return nullptr;
    }
    const auto* m = mMediaList[mCmpImageIndex].get();
    return (m && !m->isVideo()) ? static_cast<const StaticImageSource*>(m) : nullptr;
}

void App::removeTopImage(bool recordAction)
{
    auto image = std::move(mMediaList[mTopImageIndex]);
    mMediaList.erase(mMediaList.begin() + mTopImageIndex);

    if (recordAction) {
        Action action(Action::Type::Remove, mTopImageIndex, mCmpImageIndex);
        action.filepathArray.push_back(image->filepath());
        action.imageIdxArray.push_back(Action::composeImageIndex(image->id(), mTopImageIndex));
        appendAction(std::move(action));
    }

    image.reset();
    mTexturePool.cleanUnusedTextures();

    const int imageNum = static_cast<int>(mMediaList.size());
    if (imageNum < 2) {
        mCmpImageIndex = -1;
        mCompositeFlags = CompositeFlags::Top;
    }

    if (imageNum == 0) {
        mTopImageIndex = -1;
    } else {
        mTopImageIndex = std::min(mTopImageIndex, imageNum - 1);
    }
}

void App::clearImages(bool recordAction)
{
    if (recordAction) {
        Action action(Action::Type::Remove, mTopImageIndex, mCmpImageIndex);

        for (int index = 0; index < static_cast<int>(mMediaList.size()); ++index) {
            action.filepathArray.push_back(mMediaList[index]->filepath());
            action.imageIdxArray.push_back(Action::composeImageIndex(mMediaList[index]->id(), index));
        }

        mActionStack.push_back(action);
    }

    mMediaList.clear();
    mTopImageIndex = mCmpImageIndex = -1;
    mCompositeFlags = CompositeFlags::Top;
}

void App::onFileDrop(int count, const char* filepaths[])
{
    std::vector<std::string> filepathArray;
    filepathArray.reserve(count);
    std::vector<std::string> videoPaths;

    for (int i = 0; i < count; ++i) {
        const std::string& filepath = filepaths[i];

        if (endsWith(filepath, ".bts")) {
            openSession(filepath);
        } else if (MpvGlPlayer::isSupportedExtension(filepath)) {
            videoPaths.push_back(filepath);
        } else {
            filepathArray.push_back(filepath);
        }
    }

    if (!videoPaths.empty()) {
#if defined(USE_VIDEO)
        const bool hadVideoR = (mCmpImageIndex >= 0);
        std::vector<int> addedIdx;
        addedIdx.reserve(videoPaths.size());
        for (const std::string& p : videoPaths) {
            const int idx = addVideoPath(p);
            if (idx >= 0) {
                addedIdx.push_back(idx);
                if (mTopImageIndex < 0) {
                    mTopImageIndex = idx;
                } else if (mCmpImageIndex < 0 && idx != mTopImageIndex) {
                    mCmpImageIndex = idx;
                }
            }
        }
        applyImportedVideoListSelection(addedIdx);
        if (!hadVideoR && mCmpImageIndex >= 0 && mCompositeFlags == CompositeFlags::Top) {
            mCompositeFlags = CompositeFlags::Split;
        }
        openVideosFromSelection();
#else
        LOGW("Video playback is disabled in this build (USE_VIDEO).");
#endif
    } else if (!filepathArray.empty()) {
        importImageFiles(filepathArray, true);
    }
}

void App::openSession(const std::string& filepath)
{
    fx::gltf::Document sessionFile = fx::gltf::LoadFromText(filepath);
    if ("baktsiu" != sessionFile.asset.generator) {
        LOGW("Input is not a valid Bak-Tsiu session file");
        return;
    }

    std::vector<std::string> filepathArray;
    filepathArray.reserve(sessionFile.images.size());

    for (auto& imageProp : sessionFile.images) {
        filepathArray.push_back(imageProp.uri);
    }

    importImageFiles(filepathArray, true);
}

void App::saveSession(const std::string& filepath)
{
    fx::gltf::Document sessionFile;
    sessionFile.asset.generator = "baktsiu";

    for (auto& image : mMediaList) {
        fx::gltf::Image imageProp;
        imageProp.uri = image->filepath();
        sessionFile.images.push_back(imageProp);
    }

    fx::gltf::Save(sessionFile, filepath, false);
}

}  // namespace baktsiu
