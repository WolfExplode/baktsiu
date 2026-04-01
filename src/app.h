#ifndef BAKTSIU_APP_H_
#define BAKTSIU_APP_H_

#include "common.h"
#include "image.h"
#include "mf_video_reader.h"
#include "shader.h"
#include "texture.h"
#include "texture_pool.h"
#include "view.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

struct  GLFWwindow;
struct  ImFont;
struct  ImGuiIO;

namespace baktsiu
{

// Basic entity of command stack. It stores relevant data for undo instructions.
struct Action
{
    static uint8_t extractImageId(uint16_t value) {
        return (value & 0xFF00) >> 8;
    }

    static uint8_t extractLayerIndex(uint16_t value) {
        return value & 0xFF;
    }

    static uint16_t composeImageIndex(uint8_t id, uint8_t order) {
        return order| id << 8;
    }

    enum class Type : char
    {
        Unknown,
        Add,
        Remove,
        Move,
    };

    Action() 
    {
        reset();
    }

    Action(Type t, int topImageIdx, int cmpImageIdx) 
        : type(t), prevTopImageIdx(topImageIdx), prevCmpImageIdx(cmpImageIdx)
    {
    }

    void reset() 
    {
        type = Type::Unknown;
        filepathArray.clear();
        imageIdxArray.clear();
        prevTopImageIdx = prevCmpImageIdx = -1;
    }

    Type type = Type::Unknown;
    std::vector<std::string>    filepathArray;
    std::vector<uint16_t>       imageIdxArray;
    int prevTopImageIdx = -1;
    int prevCmpImageIdx = -1;
};


// Flags of the composition of image viewport.
enum class CompositeFlags : char
{
    Top         = 0x0,  // Display top (the selected) image only.
    Split       = 0x1,  // Scaled the compared image and put it underneath the top image.
    SideBySide  = 0x2,  // Display top and compared images side by side.
};

ENUM_CLASS_OPERATORS(CompositeFlags);

enum class PixelMarkerFlags : char
{
    None        = 0x00,
    Difference  = 0x01, // Draw pixel differences with a ramp of magenta.
    DiffHeatMap = 0x02, // Draw pixel differences in color ramp.
    DiffMask    = 0x03,
    Overflow    = 0x04, // Draw overflow pixels in red
    Underflow   = 0x08, // Draw underflow pixels in blue
    Default     = Difference | Overflow | Underflow,
};

ENUM_CLASS_OPERATORS(PixelMarkerFlags);


// The class of viewer functionalities.
//
// It contains two display layers on viewport. Top image is the one selected in 
// image property window at right hand side. In compare mode, we could move the
// splitter to swipe top image.
class App
{
public:
    static const char* kImagePropWindowName;
    static const char* kImageRemoveDlgTitle;
    static const char* kClearImagesDlgTitle;

public:
    bool    initialize(const char* title, int width, int height);

    void    importImageFiles(const std::vector<std::string>& filepathArray, 
                bool recordAction, std::vector<uint16_t>* imageIdxArray = nullptr);

    void    openVideoFile(const std::string& filepath);

    void    openVideoCompare(const std::string& leftPath, const std::string& rightPath);

    // Add every path to the media list (order preserved), set L/R to the first two distinct items, open.
    void    openVideoPaths(const std::vector<std::string>& paths);

    void    run(CompositeFlags initFlags = CompositeFlags::Top);

    void    release();

private:
    void    setThemeColors();

    void    initLogger();

    // Initialize related bitmap and uv info for text.
    void    initDigitCharData(const unsigned char* data);

    void    initToolbar();

    // Create toogle handle for image property window.
    // Return true if the handle is hovered by mouse cursor.
    bool    initImagePropWindowHandle(float handleWidth, bool& showPropWindow);

    void    initImagePropWindow();

    void    initFooter();

    void    initHomeWindow(const char* name);

    void    showImageProperties();

    // Show image name overlays viewport.
    void    showImageNameOverlays();

    void    showHeatRangeOverlay(const Vec2f& pos, float width);

    bool    showRemoveImageDlg(const char *name);

    bool    showClearImagesDlg(const char* title);

    void    showImportImageDlg();

    void    showExportSessionDlg();

    // Handle key pressed cases.
    // @note Few key pressed events related to image transformation are handled in updateImageTransform.
    void    onKeyPressed(const ImGuiIO&);

    void    updateImagePairFromPressedKeys();

    // Update image transform from user's key stroke and mouse input.
    void    updateImageTransform(const ImGuiIO&, bool useColumnView);

    void    updateImageSplitterPos(ImGuiIO&);

    // Dispatch compute kernels for image statistics.
    void    computeImageStatistics(const RenderTexture&, float valueScale);

    // Reset image transform to viewport center.
    void    resetImageTransform(const Vec2f& imgSize, bool fitWindow = false);

    // Synchronize local offset of column views.
    void    syncSideBySideView(const ImGuiIO& io);
    
    // Return pixel coordinates from mouse position.
    bool    getImageCoordinates(Vec2f viewportCoords, Vec2f& outImageCoords) const;

    // Compare: side 0 = left layer (uImage1), 1 = right (uImage2). Uses same ref/contain as present.frag.
    bool    getCompareSideImagePixelAtMouse(Vec2f imguiMousePos, int side, Vec2f& outImagePx) const;

    void    appendAction(Action&& action);

    void    undoAction();

    Image*  getTopImage();

    void    removeTopImage(bool recordAction);

    void    clearImages(bool recordAction);

    void    processTextureUploadTasks();

    void    onFileDrop(int count, const char* filepaths[]);

    // Open compare session.
    void    openSession(const std::string& filepath);

    // Save compare session with file extension .bts
    void    saveSession(const std::string& filepath);

    void    gradingTexImage(Image& image, int renderTexIdx);

    // Return the width of property window at right hand side.
    float   getPropWindowWidth() const;

    int     getPixelMarkerFlags() const;

    bool    inCompareMode() const;

    bool    inSideBySideMode() const;

    bool    shouldShowSplitter() const;

    void    toggleSplitView();

    void    toggleSideBySideView();

    void    exitVideoMode();

    int     addVideoPath(const std::string& filepath);

    void    openVideosFromSelection();

    void    initVideoTransportBar(const ImGuiIO& io);

    void    tickAndUploadVideoFrame(float deltaTime);

    void    renderVideoBlit(const ImGuiIO& io);

    void    recreateVideoTexture();

    void    uploadVideoTexture();

    bool    videoCompareActive() const;

    void    recomputeVideoScrubBounds(double& outMin, double& outMax) const;

    void    clampVideoCompositionT();

    void    syncVideoDecodersToCompositionT();

    // Video scrub: direction -1 = earlier, +1 = later.
    void    stepVideoScrubFiveSeconds(int direction);
    void    stepVideoScrubOneFrame(int direction);

    void    swapVideoSides();

    void    recreateVideoTextureB();

    void    uploadVideoTextureB();

    using ImageUPtr = std::unique_ptr<Image>;

    std::vector<ImageUPtr>      mImageList;
    std::deque<Action>          mActionStack;
    Action                      mCurAction;
    TexturePool                 mTexturePool;

    GLFWwindow*     mWindow = nullptr;
    ImFont*         mSmallFont = nullptr;
    ImFont*         mSmallIconFont = nullptr;
    GLuint          mFontTexture = 0;
    
    std::vector<Vec4f> mCharUvRanges;   // UV bbox of each digit in font texture.
    std::vector<Vec4f> mCharUvXforms;   // UV offset of each digit in font texture.

    RenderTexture   mRenderTextures[2];   // The intermediate output for input image.
    int             mTopImageRenderTexIdx = 0;

    Shader          mGradingShader;
    Shader          mPresentShader;
    Shader          mStatisticsShader;
    GLuint          mTexHistogram;
    Sampler         mPointSampler;
    
    std::array<int, 768> mHistogram;

    CompositeFlags      mCompositeFlags = CompositeFlags::Top;
    PixelMarkerFlags    mPixelMarkerFlags = PixelMarkerFlags::Default;

    // Image transformation
    View        mView;
    View        mColumnViews[2];    // Views for side by side mode.
    float       mImageScale = 1.0f;
    float       mPrevImageScale = -1.0f;

    int         mTopImageIndex = -1;
    int         mCmpImageIndex = -1;

    int         mCurrentPresentMode = 0;
    int         mOutTransformType = 0;
    char        mImageScaleInfo[64];

    float       mToolbarHeight;
    float       mFooterHeight;

    float       mViewSplitPos = 0.5f;   // The horizontal position of viewport splitter.
    float       mPropWindowHSplitRatio = 0.65f;

    Vec3f       mPixelBorderHighlightColor = Vec3f(0.153f, 0.980f, 0.718f);
    float       mDisplayGamma = 2.2f;
    float       mExposureValue = 0.0f;

    bool        mIsMovingSplitter = false;
    bool        mIsScalingImage = false;
    bool        mCtrlMmbZoomPivotLocked = false;
    Vec2f       mCtrlMmbZoomPivot = Vec2f(-1.0f);
    bool        mCtrlMmbZoomRightColumn = false;
    bool        mEnableToneMapping = false;
    bool        mShowPixelValues = false;
    bool        mAboutToTerminate = false;

    bool        mShowImagePropWindow = false;
    bool        mPopupImagePropWindow = false;
    bool        mUseLinearFilter = true;
    bool        mShowImageNameOverlay = true;
    bool        mShowPixelMarker = false;
    bool        mSupportComputeShader = false;
    bool        mUpdateImageSelection = false;

    bool        mVideoMode = false;
    bool        mVideoPlaying = false;
    // Defer lazy duration probe so the first paint after open is not blocked by a full-stream read.
    int         mVideoLazyDurationGraceFrames = 0;
    double      mVideoPlaybackTimeBank = 0.0;
    double      mVideoScrubValue = 0.0;
    std::unique_ptr<MFVideoReader> mVideoReader;
    std::unique_ptr<MFVideoReader> mVideoReaderB;
    std::vector<std::string> mVideoPaths;
    int         mVideoIndexL = -1;
    int         mVideoIndexR = -1;
    std::string mVideoPathL;
    std::string mVideoPathR;
    GLuint      mVideoTexture = 0;
    GLuint      mVideoTextureB = 0;
    double      mVideoCompositionT = 0.0;
    double      mVideoStartL = 0.0;
    double      mVideoStartR = 0.0;
    // Presentation-only left/right swap for instant switching while playing.
    // When true, the renderer (and UI "L/R" meaning) are swapped, but the underlying decoders
    // remain in their original slots (mVideoReader/mVideoReaderB).
    bool        mVideoSwapPresentationLR = false;
    Shader      mVideoBlitShader;
    bool        mVideoShaderReady = false;

#if defined(_WIN32) && defined(USE_VIDEO)
    bool        mVideoComInitialized = false;
#endif
};

}  // namespace baktsiu
#endif // BAKTSIU_APP_H_