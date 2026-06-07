#define IMGUI_DEFINE_MATH_OPERATORS
#include <icons_font_awesome5.h>
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#pragma warning(push)
#pragma warning(disable: 4819 4566)

#include <GL/gl3w.h>    // Initialize with gl3wInit()
#include <GLFW/glfw3.h>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#pragma warning(pop)

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include <stb_image.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include "app.h"
#include "colour.h"
#include "resources.h"

#ifdef EMBED_SHADERS
#include "shader_resources.h"
#define INIT_SHADER(shader, name, vtxName, fragName)\
shader.init(name, vtxName##_vert, fragName##_frag)
#else
#define STRING(s) #s
#define INIT_SHADER(shader, name, vtxName, fragName)\
shader.initFromFiles(name, "shaders/"##STRING(vtxName)##".vert", "shaders/"##STRING(fragName)##".frag")
#endif

#ifndef _MSC_VER
#define sprintf_s snprintf
#endif

#ifndef _WIN32
#include <sys/stat.h>
#else
#include <io.h>
#endif

namespace KeyCode
{
    // GLFW key values passed through ImGui_ImplGlfw.
    constexpr int Tab        = 0x102;
    constexpr int Backspace  = 0x103;
    constexpr int Delete     = 0x105;
    constexpr int Right      = 0x106;
    constexpr int Left       = 0x107;
    constexpr int Up         = 0x109;
    constexpr int F1         = 0x122;
    constexpr int F5         = 0x126;
    constexpr int KpDivide   = 0x14B;
    constexpr int KpSubtract = 0x14D;
    constexpr int KpAdd      = 0x14E;
    constexpr int Space      = 32;
    constexpr int Minus      = 0x2D;
    constexpr int Comma      = 0x2C;
    constexpr int Period     = 0x2E;
    constexpr int Slash      = 0x2F;
    constexpr int Equal      = 0x3D;  // '=' / zoom-in key
    constexpr int A          = 0x41;
    constexpr int C          = 0x43;
    constexpr int D          = 0x44;
    constexpr int E          = 0x45;
    constexpr int F          = 0x46;
    constexpr int L          = 0x4C;
    constexpr int O          = 0x4F;
    constexpr int Q          = 0x51;
    constexpr int R          = 0x52;
    constexpr int S          = 0x53;
    constexpr int W          = 0x57;
    constexpr int X          = 0x58;
    constexpr int Z          = 0x5A;
}  // namespace KeyCode

namespace
{

void APIENTRY glDebugOutput(GLenum source, GLenum type, GLuint id, GLenum severity,
    GLsizei length, const GLchar *message, void *userParam)
{
    // Ignore non-significant error/warning codes
    if (id == 131169 || id == 131185 || id == 131218 || id == 131204 || id == 131184) return;

    std::string sourceDesc;
    std::string typeDesc;
    std::string severityDesc;

    switch (source) {
        case GL_DEBUG_SOURCE_API:             { sourceDesc = "API"; break; }
        case GL_DEBUG_SOURCE_WINDOW_SYSTEM:   { sourceDesc = "Window System"; break; }
        case GL_DEBUG_SOURCE_SHADER_COMPILER: { sourceDesc = "Shader Compiler"; break; }
        case GL_DEBUG_SOURCE_THIRD_PARTY:     { sourceDesc = "Third Party"; break; }
        case GL_DEBUG_SOURCE_APPLICATION:     { sourceDesc = "Application"; break; }
        case GL_DEBUG_SOURCE_OTHER:           { sourceDesc = "Other"; break; }
    }

    switch (type) {
        case GL_DEBUG_TYPE_ERROR:               typeDesc = "Error"; break;
        case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR: typeDesc = "Deprecated Behaviour"; break;
        case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:  typeDesc = "Undefined Behaviour"; break;
        case GL_DEBUG_TYPE_PORTABILITY:         typeDesc = "Portability"; break;
        case GL_DEBUG_TYPE_PERFORMANCE:         typeDesc = "Performance"; break;
        case GL_DEBUG_TYPE_MARKER:              typeDesc = "Marker"; break;
        case GL_DEBUG_TYPE_PUSH_GROUP:          typeDesc = "Push Group"; break;
        case GL_DEBUG_TYPE_POP_GROUP:           typeDesc = "Pop Group"; break;
        case GL_DEBUG_TYPE_OTHER:               typeDesc = "Other"; break;
    }

    switch (severity) {
    case GL_DEBUG_SEVERITY_HIGH:         severityDesc = "high"; break;
    case GL_DEBUG_SEVERITY_MEDIUM:       severityDesc = "medium"; break;
    case GL_DEBUG_SEVERITY_LOW:          severityDesc = "low"; break;
    case GL_DEBUG_SEVERITY_NOTIFICATION: severityDesc = "notification"; break;
    }

    LOGW("{} {} No.{} ({}):\n{}", sourceDesc, typeDesc, id, severityDesc, message);
}



namespace
{

const char* const kInfoPopupName = "Info";

bool mergeWindowsCjkFallbackFont(ImGuiIO& io, ImFont* dstFont, float sizePx)
{
#if !defined(_WIN32)
    (void)io;
    (void)dstFont;
    (void)sizePx;
    return false;
#else
    if (!dstFont) {
        return false;
    }

    // Common Windows CJK fonts. Try in order and merge the first available one.
    static const char* kCandidates[] = {
        "C:/Windows/Fonts/msyh.ttc",      // Microsoft YaHei
        "C:/Windows/Fonts/msyhbd.ttc",    // Microsoft YaHei Bold
        "C:/Windows/Fonts/simhei.ttf",    // SimHei
        "C:/Windows/Fonts/simsun.ttc",    // SimSun
        "C:/Windows/Fonts/NotoSansCJK-Regular.ttc",
    };

    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.PixelSnapH = true;
    cfg.DstFont = dstFont;
    const ImWchar* ranges = io.Fonts->GetGlyphRangesChineseFull();

    for (const char* path : kCandidates) {
        if (_access(path, 0) != 0) {
            continue;
        }
        ImFont* merged = io.Fonts->AddFontFromFileTTF(path, sizePx, &cfg, ranges);
        if (merged) {
            LOGI("Merged CJK fallback font: {}", path);
            return true;
        }
    }
    LOGW("Unable to find/load a Windows CJK fallback font. Non-ASCII text may render as missing glyphs.");
    return false;
#endif
}

}  // namespace

}  // namespace

namespace baktsiu
{

bool refPxToPixel(Vec2f refPx, Vec2f refSz, Vec2f texSz, bool flipH, bool flipV, Vec2f& outPx)
{
    const Vec2f safeR = glm::max(refSz, Vec2f(1.f));
    const Vec2f safeT = glm::max(texSz, Vec2f(1.f));
    const float sc = (std::min)(safeR.x / safeT.x, safeR.y / safeT.y);
    const Vec2f disp = safeT * sc;
    const Vec2f ox = (safeR - disp) * 0.5f;
    Vec2f uv((refPx.x - ox.x) / (std::max)(disp.x, 1e-6f), (refPx.y - ox.y) / (std::max)(disp.y, 1e-6f));
    if (uv.x < -1e-4f || uv.x > 1.f + 1e-4f || uv.y < -1e-4f || uv.y > 1.f + 1e-4f) {
        return false;
    }
    if (flipH) {
        uv.x = 1.0f - uv.x;
    }
    if (flipV) {
        uv.y = 1.0f - uv.y;
    }
    outPx.x = std::floor(uv.x * texSz.x);
    outPx.y = std::floor(uv.y * texSz.y);
    outPx.x = glm::clamp(outPx.x, 0.f, (std::max)(texSz.x - 1.f, 0.f));
    outPx.y = glm::clamp(outPx.y, 0.f, (std::max)(texSz.y - 1.f, 0.f));
    return true;
}

const char* App::kImagePropWindowName = "ImagePropWindow";
const char* App::kImageRemoveDlgTitle = "Bak Tsiu##RemoveImage";
const char* App::kClearImagesDlgTitle = "Bak Tsiu##ClearImageLayers";

// Return UV BBox of given character in font texture.
inline Vec4f getCharUvRange(const stbtt_bakedchar& ch, float mapWidth)
{
    return Vec4f(ch.x0, ch.y0, ch.x1, ch.y1) / mapWidth;
}

// Return UV offset of given character in font texture.
inline Vec4f getCharUvXform(const stbtt_bakedchar& ch, float charHeight)
{
    Vec4f xform;
    xform.x = (ch.x1 - ch.x0) / ch.xadvance;
    xform.y = (ch.y1 - ch.y0) / charHeight;
    xform.z = ch.xoff / ch.xadvance;

    // The bitmap is baked upside down. For each char, the yoff is the value from 
    // image bottom to the baseline pixel. Ex. if yoff is -7, means the baseline
    // is at +7 pixels in y-axis. To compute the offset uv coordinates within
    // that char only, we calculate the pixels number from char bottom to baseline
    // which is (charHeight + ch.yoff), then divide by charHeight to compute the
    // normalized uv coordinates.
    xform.w = (charHeight + ch.yoff) / charHeight;
    return xform;
}


void    App::setThemeColors()
{
    ImGui::StyleColorsDark();
    ImVec4* colors = ImGui::GetStyle().Colors;

    colors[ImGuiCol_WindowBg] = ImVec4(0.184f, 0.184f, 0.184f, 1.0f);
    colors[ImGuiCol_MenuBarBg] = colors[ImGuiCol_WindowBg];

    colors[ImGuiCol_FrameBg] = ImVec4(0.187f, 0.321f, 0.418f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.288f, 0.485f, 0.637f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.137f, 0.663f, 0.812f, 0.75f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.0f, 0.439f, 0.702f, 0.965f);

    colors[ImGuiCol_TitleBgActive] = ImVec4(0.0f, 0.439f, 0.702f, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(0.0f, 0.439f, 0.702f, 0.5f);
    colors[ImGuiCol_HeaderActive] = colors[ImGuiCol_ButtonActive];
    colors[ImGuiCol_HeaderHovered] = colors[ImGuiCol_ButtonHovered];
    
    colors[ImGuiCol_TabActive] = colors[ImGuiCol_ButtonActive];
    colors[ImGuiCol_TabHovered] = colors[ImGuiCol_ButtonHovered];

    colors[ImGuiCol_SeparatorActive] = ImVec4(0.0f, 0.439f, 0.702f, 0.8f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.137f, 0.663f, 0.812f, 0.8f);
    colors[ImGuiCol_SliderGrab] = colors[ImGuiCol_ButtonActive];
    colors[ImGuiCol_SliderGrabActive] = colors[ImGuiCol_ButtonHovered];
}

void App::initLogger()
{
    auto console = spdlog::stdout_color_mt("console");
    console->set_pattern("[BakTsiu][%^%l%$] %v");

#ifdef _DEBUG
    console->set_level(spdlog::level::debug);
#else
    console->set_level(spdlog::level::info);
#endif

    spdlog::set_default_logger(console);
}

bool App::initialize(const char* title, int width, int height)
{
    initLogger();

    glfwSetErrorCallback([](int error, const char* description) {
        LOGW("GLFW error {}: {}", error, description);
    });

    if (!glfwInit()) {
        LOGE("Failed to initialize glfw");
        return false;
    }

    // Decide GL+GLSL versions
#if __APPLE__
    // GL 3.2 + GLSL 150
    const char* glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // Required on Mac
#else
    // GL 3.0 + GLSL 130
    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    //glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
    //glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // 3.0+ only
    //glfwWindowHint(GLFW_DECORATED, false);
    glfwWindowHint(GLFW_SRGB_CAPABLE, GL_FALSE);
#endif

#ifdef _DEBUG
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GL_TRUE);
#endif

    // Create window with graphics context
    mWindow = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (mWindow == nullptr) {
        return false;
    }

    glfwSetWindowUserPointer(mWindow, this);
    glfwSetDropCallback(mWindow, [](GLFWwindow* window, int count, const char* paths[]) {
        App* self = static_cast<App*>(glfwGetWindowUserPointer(window));
        self->onFileDrop(count, paths);
    });

    GLFWimage images[1];
    // Icon is converted to byte array in pre-build stage. It's content is defined in resources.cpp.
    images[0].pixels = stbi_load_from_memory(title_bar_icon_png, title_bar_icon_png_size, &images[0].width, &images[0].height, nullptr, 0);
    glfwSetWindowIcon(mWindow, 1, images);
    glfwMakeContextCurrent(mWindow);
    glfwSwapInterval(1); // Enable vsync

    // Initialize OpenGL loader
    if (gl3wInit() != 0) {
        LOGE("Failed to initialize OpenGL loader");
        return false;
    }

    mSupportComputeShader = glfwExtensionSupported("GL_ARB_compute_shader");
    LOGI("Support compute shader: {}", mSupportComputeShader);

#ifdef _DEBUG
    // Check output frame buffer has no gamma color encoding, since we done it in our shader of image presentation.
    GLint encoding;
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_FRONT_LEFT, GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &encoding);
    assert(encoding == GL_LINEAR);

    GLint flags;
    glGetIntegerv(GL_CONTEXT_FLAGS, &flags);
    if (flags & GL_CONTEXT_FLAG_DEBUG_BIT) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(glDebugOutput, nullptr);
        glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, GL_TRUE);
    }
#endif

    ScopeMarker("Initialization");

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

    setThemeColors();

    ImGui_ImplGlfw_InitForOpenGL(mWindow, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    mToolbarHeight = 32.0f;
    mFooterHeight = 20.0f;
    Vec4f padding(mToolbarHeight, 0.0f, mFooterHeight, 0.0f);
    mView.setViewportPadding(padding);
    mColumnViews[0].setViewportPadding(padding);
    mColumnViews[1].setViewportPadding(padding);

    // Fonts are converted into data arrays in pre-built stage. It uses bin2c.cmake to convert
    // resources/fonts/*.ttf into corresponding data arrays. Here we simply add fonts from 
    // memory. But in order to keep ownership of font data, we need to set FontDataOwnedByAtlas
    // to false, otherwise ImGui would release font data and cause crash when closing app window.
    // https://github.com/ocornut/imgui/issues/220
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;
    mSmallFont = io.Fonts->AddFontFromMemoryTTF(roboto_regular_ttf, roboto_regular_ttf_size, 13.0f, &config);
    io.FontDefault = io.Fonts->AddFontFromMemoryTTF(roboto_regular_ttf, roboto_regular_ttf_size, 15.0f, &config);
    mergeWindowsCjkFallbackFont(io, mSmallFont, 13.0f);
    mergeWindowsCjkFallbackFont(io, io.FontDefault, 15.0f);

    config.MergeMode = true;
    config.PixelSnapH = true;
    config.GlyphMinAdvanceX = 16.0f; // Use if you want to make the icon monospaced
    static const ImWchar icon_ranges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
    io.Fonts->AddFontFromMemoryTTF(fa_solid_900_ttf, fa_solid_900_ttf_size, config.GlyphMinAdvanceX, &config, icon_ranges);
    
    // Create another font with small icons.
    config.MergeMode = false;
    config.GlyphMinAdvanceX = 0.0f;
    mSmallIconFont = io.Fonts->AddFontFromMemoryTTF(roboto_regular_ttf, roboto_regular_ttf_size, 15.0f, &config);

    config.MergeMode = true;
    config.GlyphMinAdvanceX = 14.0f;
    io.Fonts->AddFontFromMemoryTTF(fa_solid_900_ttf, fa_solid_900_ttf_size, config.GlyphMinAdvanceX, &config, icon_ranges);

    // Initialize bit map texture for shader to render pixel's RGB values.
    initDigitCharData((unsigned char*)robotomono_regular_ttf);

    bool status = INIT_SHADER(mPresentShader, "present", quad, present);
    CHECK_AND_RETURN_IT(status, "Failed to initialize present shader");

    status = INIT_SHADER(mGradingShader, "color_grading", quad, color_grading);
    CHECK_AND_RETURN_IT(status, "Failed to initialize color grading shader");

#if defined(USE_VIDEO)
    status = INIT_SHADER(mVideoBlitShader, "video_blit", video_blit, video_blit);
    if (!status) {
        LOGW("Failed to initialize video blit shader");
    } else {
        mVideoShaderReady = true;
    }
#endif

    if (mSupportComputeShader) {
        glGenTextures(1, &mTexHistogram);
        glBindTexture(GL_TEXTURE_2D, mTexHistogram);
        // Setup filtering parameters for display
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32I, static_cast<GLint>(mHistogram.size()), 1);
        glGenTextures(1, &mTexHistogramB);
        glBindTexture(GL_TEXTURE_2D, mTexHistogramB);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32I, static_cast<GLint>(mHistogramB.size()), 1);
        glBindTexture(GL_TEXTURE_2D, 0);
        status = mStatisticsShader.initCompute("statistics", statistics_comp);
#if defined(USE_VIDEO) && defined(EMBED_SHADERS)
        try {
            mVideoRgbHistogramComputeReady =
                mVideoStatisticsShader.initCompute("statistics_rgba8", statistics_rgba8_comp);
        } catch (const std::exception& e) {
            LOGW("Failed to init video histogram compute shader: {}", e.what());
            mVideoRgbHistogramComputeReady = false;
        }
#endif
    }

    mPointSampler.initialize(GL_NEAREST, GL_NEAREST);

    return status;
}

void App::initDigitCharData(const unsigned char* data)
{
    constexpr int bitmapWidth = 256;
    unsigned char bitmap[bitmapWidth * bitmapWidth];
    constexpr float fontHeight = 84.0f;

    // Bake bitmap from ASCII code 46 to 58: ./0123456789
    constexpr int charNumber = 12;
    constexpr int firstAsciiCodeIdx = 46;
    stbtt_bakedchar cdata[charNumber];
    int result = stbtt_BakeFontBitmap(data, 0, fontHeight, bitmap, bitmapWidth, bitmapWidth, firstAsciiCodeIdx, charNumber, cdata);
    if (result < 0) {
        LOGW("Failed to initialized font!");
    }

    // Push data for digit 0-9 whose ASCII code is [48, 57]
    for (int c = 48; c < 58; c++) {
        const auto& ch = cdata[c - firstAsciiCodeIdx];
        mCharUvRanges.push_back(getCharUvRange(ch, bitmapWidth));
        mCharUvXforms.push_back(getCharUvXform(ch, fontHeight));
    }

    // Push data for decimal point '.'
    mCharUvRanges.push_back(getCharUvRange(cdata[0], bitmapWidth));
    mCharUvXforms.push_back(getCharUvXform(cdata[0], fontHeight));

    glGenTextures(1, &mFontTexture);
    glBindTexture(GL_TEXTURE_2D, mFontTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, bitmapWidth, bitmapWidth, 0, GL_RED, GL_UNSIGNED_BYTE, bitmap);

    // Generate mipmap for the text texture
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 3);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenerateMipmap(GL_TEXTURE_2D);
}

void App::release()
{
#if defined(USE_VIDEO)
    exitVideoMode();
    if (mVideoShaderReady) {
        mVideoBlitShader.release();
        mVideoShaderReady = false;
    }
    mVideoStatisticsShader.release();
    mVideoRgbHistogramComputeReady = false;
#endif

    // Cleanup
    mMediaList.clear();
    mPointSampler.release();

    mPresentShader.release();
    mGradingShader.release();

    if (mTexHistogram != 0) {
        glDeleteTextures(1, &mTexHistogram);
        mTexHistogram = 0;
    }
    if (mTexHistogramB != 0) {
        glDeleteTextures(1, &mTexHistogramB);
        mTexHistogramB = 0;
    }
    mStatisticsShader.release();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(mWindow);
    glfwTerminate();
}

// This is executed in main thread (which has GL context).
void App::processTextureUploadTasks()
{
    TextureList newTextureList = mTexturePool.upload();

    bool isUndo = (mCurAction.type != Action::Type::Unknown);
    
    if (isUndo && mTexturePool.hasNoPendingTasks()) {
        if (mCurAction.type == Action::Type::Remove) {
            const int curImageNum = static_cast<int>(mMediaList.size());
            mTopImageIndex = std::min(mCurAction.prevTopImageIdx, curImageNum - 1);
            mCmpImageIndex = std::min(mCurAction.prevCmpImageIdx, curImageNum - 1);
            mUpdateImageSelection = false;
        }
        
        mCurAction.reset();
    }

    if (newTextureList.empty() && !mUpdateImageSelection) {
        return;
    }

    // When we finish importing images, we switch top image to the latest one.
    if (!isUndo && mTexturePool.hasNoPendingTasks()) {
        mTopImageIndex = static_cast<int>(mMediaList.size()) - 1;
        resetImageTransform(getTopImage()->size());
        if (!mInitialMediaAspectApplied) {
            const Vec2f firstImageSize = getTopImage()->size();
            applyInitialWindowAspectFromSize(firstImageSize.x, firstImageSize.y);
        }

        if (mCmpImageIndex == -1 && mTopImageIndex >= 1) {
            mCmpImageIndex = 0;
            if (mCompositeFlags == CompositeFlags::Top) {
                mCompositeFlags = CompositeFlags::Split;
            }
        }

        mUpdateImageSelection = false;
    }
}

void App::applyInitialWindowAspectFromSize(float mediaW, float mediaH)
{
    if (mInitialMediaAspectApplied || !mWindow) {
        return;
    }
    if (!(mediaW > 0.0f) || !(mediaH > 0.0f)) {
        return;
    }

    const float aspect = mediaW / mediaH;
    if (!(aspect > 0.0f)) {
        return;
    }

    int curW = 0;
    int curH = 0;
    glfwGetWindowSize(mWindow, &curW, &curH);
    if (curW <= 0 || curH <= 0) {
        return;
    }

    const float area = static_cast<float>(curW) * static_cast<float>(curH);
    int targetW = static_cast<int>(std::round(std::sqrt(area * aspect)));
    int targetH = static_cast<int>(std::round(static_cast<float>(targetW) / aspect));

    const GLFWvidmode* mode = glfwGetVideoMode(glfwGetPrimaryMonitor());
    if (mode) {
        const int maxW = (std::max)(640, static_cast<int>(std::round(mode->width * 0.95)));
        const int maxH = (std::max)(360, static_cast<int>(std::round(mode->height * 0.90)));
        if (targetW > maxW) {
            targetW = maxW;
            targetH = static_cast<int>(std::round(static_cast<float>(targetW) / aspect));
        }
        if (targetH > maxH) {
            targetH = maxH;
            targetW = static_cast<int>(std::round(static_cast<float>(targetH) * aspect));
        }
    }

    targetW = glm::clamp(targetW, 640, 8192);
    targetH = glm::clamp(targetH, 360, 8192);
    if (targetW <= 0 || targetH <= 0) {
        return;
    }

    glfwSetWindowSize(mWindow, targetW, targetH);
    mInitialMediaAspectApplied = true;
}

void App::run(CompositeFlags initFlags)
{
    bool shouldChangeComposition = (initFlags != CompositeFlags::Top);

    // Spawn worker threads to handle import images.
    const unsigned int workerNum = std::thread::hardware_concurrency();
    mTexturePool.initialize(workerNum);

    while (!glfwWindowShouldClose(mWindow)) {
        glfwPollEvents();

        processTextureUploadTasks();
        if (shouldChangeComposition && mMediaList.size() >= 2) {
            mCompositeFlags = initFlags;
            shouldChangeComposition = false;
        }

        const bool videoActive = isVideoMode();

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        renderToolbar();
        renderFooter();
        renderPropertiesPanel();
        
        ImGuiIO& io = ImGui::GetIO();

        // PureRef-style window drag: hold RMB and drag anywhere to move the OS window.
        if (ImGui::IsMouseClicked(1)) {
            mRmbWindowDragActive = true;
            int winX = 0;
            int winY = 0;
            double cursorX = 0.0;
            double cursorY = 0.0;
            glfwGetWindowPos(mWindow, &winX, &winY);
            glfwGetCursorPos(mWindow, &cursorX, &cursorY);
            mRmbDragStartWindowX = winX;
            mRmbDragStartWindowY = winY;
            mRmbDragStartCursorScreenX = static_cast<double>(winX) + cursorX;
            mRmbDragStartCursorScreenY = static_cast<double>(winY) + cursorY;
        }
        if (ImGui::IsMouseReleased(1)) {
            mRmbWindowDragActive = false;
        }
        if (mRmbWindowDragActive && ImGui::IsMouseDown(1)) {
            int winX = 0;
            int winY = 0;
            double cursorX = 0.0;
            double cursorY = 0.0;
            glfwGetWindowPos(mWindow, &winX, &winY);
            glfwGetCursorPos(mWindow, &cursorX, &cursorY);
            const double cursorScreenX = static_cast<double>(winX) + cursorX;
            const double cursorScreenY = static_cast<double>(winY) + cursorY;
            glfwSetWindowPos(
                mWindow,
                mRmbDragStartWindowX + static_cast<int>(std::round(cursorScreenX - mRmbDragStartCursorScreenX)),
                mRmbDragStartWindowY + static_cast<int>(std::round(cursorScreenY - mRmbDragStartCursorScreenY)));
        }

        // Advance animated textures (e.g. GIF) in image mode.
        for (auto& media : mMediaList) {
            if (!media || media->isVideo()) {
                continue;
            }
            auto* si = static_cast<StaticImageSource*>(media.get());
            if (Texture* tex = si->getTexture()) {
                tex->tick(io.DeltaTime);
            }
        }

        // Use WantCaptureMouse + GLFW focus, not "!AnyWindowFocused". Toolbar items call
        // SetItemDefaultFocus(), so an ImGui window always has focus at startup and would
        // block pan/scroll until the user clicked away — even with the mouse over the image.
        if (glfwGetWindowAttrib(mWindow, GLFW_FOCUSED) && !io.WantCaptureMouse) {
            updateImageSplitterPos(io);
        }

        onKeyPressed(io);
        auto* topImage = getTopImage();
        Vec2f imageSize = topImage ? topImage->size() : Vec2f(1.0f);
        Vec2f refImageSize = imageSize;

        const bool useColumnView = inSideBySideMode();
        if (!videoActive) {
            const bool enableCompareView = inCompareMode();
            if (enableCompareView && mCmpImageIndex >= 0 && topImage) {
                auto* cmpImage = getCmpImage();
                if (cmpImage) {
                    const Vec2f cmpSize = cmpImage->size();
                    refImageSize.x = (std::max)(refImageSize.x, cmpSize.x);
                    refImageSize.y = (std::max)(refImageSize.y, cmpSize.y);
                }
            }

            if (useColumnView) {
                const float leftColumnWidth = io.DisplaySize.x * mViewSplitPos;
                mColumnViews[0].resize(Vec2f(leftColumnWidth, io.DisplaySize.y));
                mColumnViews[1].resize(Vec2f(io.DisplaySize.x - leftColumnWidth, io.DisplaySize.y));
                mColumnViews[0].setImageSize(enableCompareView ? refImageSize : imageSize);
                mColumnViews[1].setImageSize(enableCompareView ? refImageSize : imageSize);
            } else {
                mView.resize(io.DisplaySize);
                mView.setImageSize(enableCompareView ? refImageSize : imageSize);
            }

            updateImageTransform(io, useColumnView);
            if (shouldShowSplitter() && mShowImageNameOverlay) {
                showImageNameOverlays();
            }

            if (enableCompareView && ((getPixelMarkerFlags() & 0x2) > 0)) {
                Vec2f heatbarPos(8.0f, io.DisplaySize.y - mFooterHeight - 8.0f - 20.0f);
                showHeatRangeOverlay(heatbarPos, 150.0f);
            }
        }
#if defined(USE_VIDEO)
        else {
            if (mVideoLazyDurationGraceFrames > 0) {
                --mVideoLazyDurationGraceFrames;
            }
            syncVideoViewsLayout(io);
            if (mPendingViewFitReset) {
                resetVideoViewTransform(true);
                mPendingViewFitReset = false;
            }
            const bool vSbs = videoCompareActive() && mCompositeFlags == CompositeFlags::SideBySide;
            updateImageTransform(io, vSbs);
            mImageScale = vSbs ? mColumnViews[0].getImageScale() : mView.getImageScale();
            tickAndUploadVideoFrame(io.DeltaTime);
            renderVideoTransportBar(io);
            if (mShowImageNameOverlay) {
                showImageNameOverlays();
            }
        }
#endif
        
        // Since GLFW doesn't support cursor of resize all, thus we use imgui to draw that cursor.
        // Caution: the cursor is hidden when using imgui's drawn cursor, when the root window is unfocused.
        io.MouseDrawCursor = (ImGui::GetMouseCursor() == ImGuiMouseCursor_ResizeAll);

        //ImGui::ShowDemoWindow();

        ImGui::Render();

        if (!videoActive) {
            if (topImage && topImage->texId() != 0) {
                gradingTexImage(*topImage, mTopImageRenderTexIdx);
                if (mShowImagePropWindow && mSupportComputeShader) {
                    float valueScale = topImage->getColorEncodingType() == ColorEncodingType::Linear ? 1.0f : 255.0f;
                    computeImageStatistics(
                        mRenderTextures[mTopImageRenderTexIdx], valueScale, mTexHistogram, mHistogram);
                }
            }

            const bool enableCompareView = inCompareMode();
            if (enableCompareView && mCmpImageIndex >= 0) {
                auto* cmpImage = getCmpImage();
                if (cmpImage->texId() != 0) {
                    gradingTexImage(*cmpImage, mTopImageRenderTexIdx ^ 1);
                    if (mShowImagePropWindow && mSupportComputeShader) {
                        const float valueScaleB =
                            cmpImage->getColorEncodingType() == ColorEncodingType::Linear ? 1.0f : 255.0f;
                        computeImageStatistics(
                            mRenderTextures[mTopImageRenderTexIdx ^ 1], valueScaleB, mTexHistogramB, mHistogramB);
                    }
                }
            }

            // We have to apply framebuffer scale for hidh DPI display.
            const Vec2f viewportSize = io.DisplaySize * io.DisplayFramebufferScale;
            glViewport(0, 0, static_cast<GLsizei>(viewportSize.x), static_cast<GLsizei>(viewportSize.y));
            glClearColor(0.45f, 0.55f, 0.6f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glDepthMask(GL_FALSE);
            glDisable(GL_DEPTH_TEST);

            // Render image viewer display
            // TODO: Use depth culling to avoid over-drawing.
            mPresentShader.bind();
            glActiveTexture(GL_TEXTURE0);

            // We have to forcely use nearest filter to properly show numerical values within a pixel.
            const View& topView = useColumnView ? mColumnViews[0] : mView;
            const View& bottomView = useColumnView ? mColumnViews[1] : mView;
            const float imageScale = topView.getImageScale();
            const bool forceNearestFilter = imageScale > 5.0f;

            Vec2f tex1Present = topImage ? topImage->size() : Vec2f(1.0f);
            Vec2f tex2Present(1.0f);
            auto* cmpForPresent = (enableCompareView && mCmpImageIndex >= 0)
                ? getCmpImage()
                : nullptr;
            if (cmpForPresent) {
                tex2Present = cmpForPresent->size();
            }
            Vec2f refForPresent = (enableCompareView && cmpForPresent) ? refImageSize : tex1Present;

            if (topImage) {
                mRenderTextures[mTopImageRenderTexIdx].bindAsInput(mUseLinearFilter && !forceNearestFilter);

                mPresentShader.setUniform("uImageSize", refForPresent * imageScale);
                mPresentShader.setUniform("uOffset", topView.getImageOffset());
                mPresentShader.setUniform("uImage1", 0);
            } else {
                mPresentShader.setUniform("uImageSize", Vec2f(0.0f));
            }

            mPresentShader.setUniform("uRefImageSize", refForPresent);
            mPresentShader.setUniform("uTex1Size", tex1Present);
            mPresentShader.setUniform("uTex2Size", tex2Present);

            mPresentShader.setUniform("uEnablePixelHighlight", !mIsMovingSplitter && !mIsScalingImage);
            mPresentShader.setUniform("uCursorPos", Vec2f(io.MousePos.x, io.DisplaySize.y - io.MousePos.y) + Vec2f(0.5f));
            mPresentShader.setUniform("uSideBySide", mCompositeFlags == CompositeFlags::SideBySide);
            mPresentShader.setUniform("uPixelMarkerFlags", getPixelMarkerFlags());
            mPresentShader.setUniform("uPresentMode", mCurrentPresentMode);
            mPresentShader.setUniform("uOutTransformType", mOutTransformType);
            mPresentShader.setUniform("uWindowSize", Vec2f(io.DisplaySize));
            mPresentShader.setUniform("uImageScale", imageScale);
            mPresentShader.setUniform("uSplitPos", enableCompareView ? mViewSplitPos : 1.0f);
            mPresentShader.setUniform("uDisplayGamma", mDisplayGamma);
            mPresentShader.setUniform("uApplyToneMapping", mEnableToneMapping);
            mPresentShader.setUniform("uFlipImage1", mFlipPresentMediaVert[0]);
            mPresentShader.setUniform("uFlipImage2", enableCompareView ? mFlipPresentMediaVert[1] : false);
            mPresentShader.setUniform("uFlipImage1H", mFlipPresentMediaHorz[0]);
            mPresentShader.setUniform("uFlipImage2H", enableCompareView ? mFlipPresentMediaHorz[1] : false);
            mPresentShader.setUniform("uCharUvRanges", mCharUvRanges);
            mPresentShader.setUniform("uCharUvXforms", mCharUvXforms);
            mPresentShader.setUniform("uPixelBorderHighlightColor", mPixelBorderHighlightColor);

            if (enableCompareView && mCmpImageIndex >= 0) {
                glActiveTexture(GL_TEXTURE1);
                mRenderTextures[mTopImageRenderTexIdx ^ 1].bindAsInput(mUseLinearFilter && !forceNearestFilter);
                mPresentShader.setUniform("uImage2", 1);
                mPresentShader.setUniform("uOffsetExtra", bottomView.getImageOffset());
                mPresentShader.setUniform("uRelativeOffset", (bottomView.getLocalOffset() - topView.getLocalOffset()) * mImageScale);
            }

            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, mFontTexture);
            mPresentShader.setUniform("uFontImage", 2);

            mPresentShader.drawTriangle();
        }
#if defined(USE_VIDEO)
        else {
            renderVideoBlit(io);
            {
            auto* r  = videoSource();
            auto* rB = videoSourceB();
            if (mShowImagePropWindow && mSupportComputeShader && mVideoRgbHistogramComputeReady
                    && r && r->isOpen() && r->texId() != 0) {
                const bool pipeBothSides = videoCompareActive() && inCompareMode();
                if (pipeBothSides && rB && rB->isOpen() && rB->texId() != 0) {
                    const bool dispSwap = mVideoSwapPresentationLR;
                    if (dispSwap) {
                        computeVideoTextureHistogram(rB->texId(), rB->width(), rB->height(), mTexHistogram, mHistogram);
                        computeVideoTextureHistogram(r->texId(),  r->width(),  r->height(),  mTexHistogramB, mHistogramB);
                    } else {
                        computeVideoTextureHistogram(r->texId(),  r->width(),  r->height(),  mTexHistogram, mHistogram);
                        computeVideoTextureHistogram(rB->texId(), rB->width(), rB->height(), mTexHistogramB, mHistogramB);
                    }
                } else {
                    computeVideoTextureHistogram(r->texId(), r->width(), r->height(), mTexHistogram, mHistogram);
                }
            }
        }
        }
#endif

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(mWindow);
    }

    mTexturePool.release();
}

void    App::gradingTexImage(StaticImageSource& image, int renderTexIdx)
{
    const Vec2i size = image.size();
    mRenderTextures[renderTexIdx].bindAsOutput(size, GL_RGBA16F);

    glViewport(0, 0, size.x, size.y);
    glClearColor(0.45f, 0.55f, 0.6f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);

    const int textureUnit = 0;
    glActiveTexture(GL_TEXTURE0);
    image.getTexture()->bind();
    mPointSampler.bind(textureUnit);

    mGradingShader.bind();
    mGradingShader.setUniform("uImage", textureUnit);
    mGradingShader.setUniform("uEV", mExposureValue);
    mGradingShader.setUniform("uInImageProp", Vec2i(
        static_cast<int>(image.getColorEncodingType()),
        static_cast<int>(image.getColorPrimaryType())));

    mGradingShader.drawTriangle();

    image.getTexture()->unbind();
    mPointSampler.unbind(textureUnit);
    mRenderTextures[renderTexIdx].unbind();
}

void App::computeImageStatistics(
    const RenderTexture& texture, float valueScale, GLuint histGpuTex, std::array<int, 768>& histCpu)
{
    if (histGpuTex == 0) {
        return;
    }
    ScopeMarker("Compute Image Statistics");

    std::fill(histCpu.begin(), histCpu.end(), 0);
    glBindTexture(GL_TEXTURE_2D, histGpuTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLint>(histCpu.size()), 1, GL_RED_INTEGER, GL_INT,
        (void*)histCpu.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    mStatisticsShader.bind();
    glBindImageTexture(0, texture.id(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
    glBindImageTexture(1, histGpuTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32I);

    Vec2i size = texture.size();
    mStatisticsShader.setUniform("uImageSize", size);
    mStatisticsShader.setUniform("uValueScale", valueScale);
    mStatisticsShader.compute(size.x / 16, size.y / 16);
}

#if defined(USE_VIDEO)
void App::computeVideoTextureHistogram(
    GLuint rgba8Tex, int width, int height, GLuint histGpuTex, std::array<int, 768>& histCpu)
{
    if (!mVideoRgbHistogramComputeReady || rgba8Tex == 0 || histGpuTex == 0 || width <= 0 || height <= 0) {
        return;
    }
    ScopeMarker("Compute Video Histogram");

    std::fill(histCpu.begin(), histCpu.end(), 0);
    glBindTexture(GL_TEXTURE_2D, histGpuTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLint>(histCpu.size()), 1, GL_RED_INTEGER, GL_INT,
        (void*)histCpu.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    mVideoStatisticsShader.bind();
    glBindImageTexture(0, rgba8Tex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
    glBindImageTexture(1, histGpuTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32I);

    const Vec2i size(width, height);
    mVideoStatisticsShader.setUniform("uImageSize", size);
    mVideoStatisticsShader.setUniform("uValueScale", 255.0f);
    mVideoStatisticsShader.compute(static_cast<GLuint>(size.x / 16), static_cast<GLuint>(size.y / 16));
}
#endif

void    App::onKeyPressed(const ImGuiIO& io)
{
    if (ImGui::IsKeyPressed(KeyCode::Tab)) {
        mShowImagePropWindow ^= true;
    } else if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(KeyCode::W)) {
        clearImages(true);
    } else if (io.KeyCtrl && ImGui::IsKeyPressed(KeyCode::Z)) {
        undoAction();
    } else if (io.KeyCtrl && ImGui::IsKeyPressed(KeyCode::O)) {
        showImportImageDlg();
    } else if (io.KeyCtrl && ImGui::IsKeyPressed(KeyCode::E)) {
        showExportSessionDlg();
    } else if (io.KeyAlt && ImGui::IsKeyPressed(KeyCode::C)) {
        syncSideBySideView(io);
    } else if (ImGui::IsKeyPressed(KeyCode::S)) {
        toggleSplitView();
    } else if (ImGui::IsKeyPressed(KeyCode::C)) {
        toggleSideBySideView();
    } else if (ImGui::IsKeyPressed(KeyCode::Space)) {
#if defined(USE_VIDEO)
        if (isVideoMode() && !io.WantTextInput) {
            mVideoPlaying ^= true;
            mVideoPlaybackTimeBank = 0.0;
            mVideoComparePlaybackBank = 0.0;
            mVideoCompareBankL = 0.0;
            mVideoCompareBankR = 0.0;
        }
#endif
    } else if (ImGui::IsKeyPressed(KeyCode::Q)) {
        mUseLinearFilter ^= true;
    } else if (ImGui::IsKeyPressed(KeyCode::W)) {
        mShowPixelMarker ^= true;
    } else if (ImGui::IsKeyPressed(KeyCode::F1)) {
        ImGui::OpenPopup(kInfoPopupName);
    } else if (ImGui::IsKeyPressed(KeyCode::F5)) {
        if (auto* image = getTopImage()) image->reload();
    } else if (ImGui::IsKeyPressed(KeyCode::Backspace) || ImGui::IsKeyPressed(KeyCode::Delete)) {
        if (mTopImageIndex > -1) ImGui::OpenPopup(kImageRemoveDlgTitle);
    } else {
        updateImagePairFromPressedKeys();
        // ps. There are few other key pressed cases are handled in updateImageTransform().
    }

    renderInfoPopup(kInfoPopupName);

    if (showRemoveImageDlg(kImageRemoveDlgTitle)) {
        removeTopImage(true);
    }
}

void    App::updateImageSplitterPos(ImGuiIO& io)
{
#if defined(USE_VIDEO)
    const bool splitForVideoCompare = isVideoMode() && videoCompareActive() && mCompositeFlags != CompositeFlags::Top;
#else
    const bool splitForVideoCompare = false;
#endif
    if (!shouldShowSplitter() && !splitForVideoCompare) {
        mIsMovingSplitter = false;
        return;
    }

    const float mouseXNorm = io.MousePos.x / io.DisplaySize.x;
    const bool isHoverSplitter = !ImGui::IsMouseDragging(0) && std::abs(mouseXNorm - mViewSplitPos) < 1e-2f;
    ImGui::SetMouseCursor(isHoverSplitter || mIsMovingSplitter ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_Arrow);

    // LMB anywhere on the image viewport (no ImGui window hovered) moves the compare split — click or drag.
    if (ImGui::IsMouseDown(0)) {
        mIsMovingSplitter = true;
        mViewSplitPos = glm::clamp(mouseXNorm, 0.02f, 0.98f);
    } else {
        mIsMovingSplitter = false;
    }
}

void    App::syncSideBySideView(const ImGuiIO& io)
{
    int columnIdx = static_cast<int>(io.MousePos.x > io.DisplaySize.x * mViewSplitPos);
    mColumnViews[columnIdx].setLocalOffset(mColumnViews[columnIdx ^ 1].getLocalOffset());
}

void    App::updateImageTransform(const ImGuiIO& io, bool useColumnView)
{
    auto* topImage = getTopImage();
#if defined(USE_VIDEO)
    const bool inVideo = isVideoMode();
#else
    constexpr bool inVideo = false;
#endif
    if (!topImage && !inVideo) {
        return;
    }

#if defined(USE_VIDEO)
    // Finish viewport drag scrub even if the cursor moved over ImGui (release would otherwise be missed).
    if (inVideo && ImGui::IsMouseReleased(0) && mViewportRmbScrubActive) {
        mViewportRmbScrubActive = false;
        if (videoCompareActive()) {
            clampVideoCompositionT();
            syncVideoDecodersToCompositionT();
            mVideoScrubValue = mVideoCompositionT;
        } else {
            auto* r = videoSource();
            if (r) {
                r->seek(mVideoScrubValue, true);
                r->decodeFrameThrough(mVideoScrubValue, 0);
            }
            uploadVideoTexture();
        }
        if (mVideoResumePlaybackAfterViewportScrub) {
            mVideoPlaying = true;
            mVideoPlaybackTimeBank = 0.0;
            mVideoComparePlaybackBank = 0.0;
            mVideoCompareBankL = 0.0;
            mVideoCompareBankR = 0.0;
        }
        mVideoResumePlaybackAfterViewportScrub = false;
    }
#endif

    Vec2f scalePivot(-1.0f);
    bool mouseAtRightColumn = false;
    auto fetchScalePivot = [](const ImGuiIO& io, bool useColumnView, float viewSplitPos, bool& mouseAtRightColumn) {
        mouseAtRightColumn = false;
        Vec2f scalePivot(io.MousePos.x, io.DisplaySize.y - io.MousePos.y);
        if (useColumnView) {
            const float leftColumnWidth = io.DisplaySize.x * viewSplitPos;
            if (scalePivot.x > leftColumnWidth) {
                scalePivot.x -= leftColumnWidth;
                mouseAtRightColumn = true;
            }
        }
        scalePivot = glm::round(scalePivot + Vec2f(0.5f)) - Vec2f(0.5f);
        return scalePivot;
    };

    mImageScale = useColumnView ? mColumnViews[0].getImageScale() : mView.getImageScale();

    const bool mouseActive = !io.WantCaptureMouse;
    float oldImageScale = mImageScale;

    if (mIsInSniperMode && ImGui::IsKeyReleased(KeyCode::Z)) {
        mImageScale = mPrevScale;
        mPrevScale = -1.0f;
        mIsInSniperMode = false;
    }

    if (mouseActive) {
        const bool doingCtrlMmbZoom = !mIsMovingSplitter && ImGui::IsMouseDown(2) && io.KeyCtrl;
        if (!doingCtrlMmbZoom) {
            mCtrlMmbZoomPivotLocked = false;
        }

        if (ImGui::IsMouseReleased(0) || ImGui::IsMouseReleased(1) || ImGui::IsMouseReleased(2)) {
            mIsScalingImage = false;
        }

#if defined(USE_VIDEO)
        if (inVideo) {
            Vec2f contentOrigin;
            Vec2f contentSize;
            getVideoBlitContentLayout(io, contentOrigin, contentSize);
            const float mx = io.MousePos.x;
            const float my = io.MousePos.y;
            const bool inContent = mx >= contentOrigin.x && mx < contentOrigin.x + contentSize.x
                && my >= contentOrigin.y && my < contentOrigin.y + contentSize.y;

            if (ImGui::IsMouseDoubleClicked(0) && inContent && !mViewportRmbScrubActive && !mIsMovingSplitter
                && !ImGui::IsMouseDown(1) && !ImGui::IsMouseDown(2)) {
                mVideoPlaying ^= true;
                mVideoPlaybackTimeBank = 0.0;
                mVideoComparePlaybackBank = 0.0;
                mVideoCompareBankL = 0.0;
                mVideoCompareBankR = 0.0;
            }

            const bool splitForVideoCompare = videoCompareActive() && mCompositeFlags != CompositeFlags::Top;
            const bool videoCompareSplitSlider = videoCompareActive() && mCompositeFlags == CompositeFlags::Split;

            if (!shouldShowSplitter() && !splitForVideoCompare && ImGui::IsMouseDown(0) && ImGui::IsMouseDown(1)) {
                mImageScale *= (1.0f - glm::roundEven(io.MouseDelta.y) * 0.0078125f);
                mIsScalingImage = true;
            } else if (!videoCompareSplitSlider && ImGui::IsMouseDown(0) && !ImGui::IsMouseDown(1)
                && !ImGui::IsMouseDown(2)) {
                if (ImGui::IsMouseClicked(0) && inContent) {
                    mViewportRmbScrubActive = true;
                    mVideoResumePlaybackAfterViewportScrub = mVideoPlaying;
                    mVideoPlaying = false;
                    mVideoPlaybackTimeBank = 0.0;
                    mVideoComparePlaybackBank = 0.0;
                    mVideoCompareBankL = 0.0;
                    mVideoCompareBankR = 0.0;
                    mVideoLastScrubDecodeTime = ImGui::GetTime();
                }
                if (mViewportRmbScrubActive && ImGui::IsMouseDown(0)) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

                    double tMin = 0.0;
                    double tMax = 1.0;
                    recomputeVideoScrubBounds(tMin, tMax);
                    const double span = (std::max)(tMax - tMin, 1e-9);
                    const double dT =
                        (static_cast<double>(io.MouseDelta.x) / static_cast<double>((std::max)(contentSize.x, 1.0f)))
                        * span;

                    const bool valueChanged  = std::abs(io.MouseDelta.x) > 0.0001f;
                    const bool itemActivated = ImGui::IsMouseClicked(0);
                    const bool itemClicked   = ImGui::IsMouseClicked(0);

                    struct ViewportScrubHint { bool run = false; int maxReadCapPerStream = 0; };
                    auto viewportScrubHint = [&](bool active, bool activated, bool clicked,
                        bool changed, bool deactivated, double timeSec) -> ViewportScrubHint {
                        ViewportScrubHint out;
                        constexpr double kMinIntervalSec = 1.0 / 20.0;
                        if (deactivated || activated || clicked) {
                            mVideoLastScrubDecodeTime = timeSec;
                            out.run = true;
                            return out;
                        }
                        if (active && changed && timeSec - mVideoLastScrubDecodeTime >= kMinIntervalSec) {
                            mVideoLastScrubDecodeTime = timeSec;
                            out.run = true;
                        }
                        return out;
                    };

                    const ViewportScrubHint scrubHint = viewportScrubHint(
                        true, itemActivated, itemClicked, valueChanged, false, ImGui::GetTime());

                    if (videoCompareActive()) {
                        mVideoCompositionT += dT;
                        clampVideoCompositionT();
                        mVideoScrubValue = mVideoCompositionT;
                    } else {
                        mVideoScrubValue = glm::clamp(mVideoScrubValue + dT, tMin, tMax);
                    }

                    if (scrubHint.run) {
                        if (videoCompareActive()) {
                            syncVideoDecodersToCompositionT(scrubHint.maxReadCapPerStream);
                        } else {
                            auto* r = videoSource();
                            if (r) {
                                r->seek(mVideoScrubValue, scrubHint.maxReadCapPerStream <= 0);
                                r->decodeFrameThrough(mVideoScrubValue, scrubHint.maxReadCapPerStream);
                            }
                            uploadVideoTexture();
                        }
                    }
                }
            } else if (!mIsMovingSplitter && ImGui::IsMouseDown(2)) {
                if (io.KeyCtrl) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
                    if (!mCtrlMmbZoomPivotLocked) {
                        bool atRightCol = false;
                        mCtrlMmbZoomPivot = fetchScalePivot(io, useColumnView, mViewSplitPos, atRightCol);
                        mCtrlMmbZoomRightColumn = atRightCol;
                        mCtrlMmbZoomPivotLocked = true;
                    }
                    scalePivot = mCtrlMmbZoomPivot;
                    mouseAtRightColumn = mCtrlMmbZoomRightColumn;
                    static constexpr float kMmbZoomSensitivity = 0.003f;
                    const float zoomDrag = (io.MouseDelta.x - io.MouseDelta.y) * kMmbZoomSensitivity;
                    mImageScale *= glm::clamp(1.0f + zoomDrag, 0.25f, 4.0f);
                    mIsScalingImage = true;
                } else {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                    Vec2f translate(io.MouseDelta.x, -io.MouseDelta.y);
                    if (!useColumnView) {
                        mView.translate(translate);
                    } else if (io.KeyAlt) {
                        translate += mPanResidual;
                        Vec2f roundedTranslate = glm::round(translate / mImageScale);
                        int columnIdx = static_cast<int>(io.MousePos.x > io.DisplaySize.x * mViewSplitPos);
                        mColumnViews[columnIdx].translate(roundedTranslate, true);
                        mPanResidual = translate - roundedTranslate * mImageScale;
                    } else {
                        mColumnViews[0].translate(translate);
                        mColumnViews[1].translate(translate);
                    }
                    return;
                }
            }
        } else {
#endif
            if (!shouldShowSplitter() && ImGui::IsMouseDown(0) && ImGui::IsMouseDown(1)) {
                // Scale when both buttons pressed (LMB is the split bar in compare mode).
                mImageScale *= (1.0f - glm::roundEven(io.MouseDelta.y) * 0.0078125f);
                mIsScalingImage = true;
            } else if (!mIsMovingSplitter && ImGui::IsMouseDown(2)) {
                if (io.KeyCtrl) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
                    if (!mCtrlMmbZoomPivotLocked) {
                        bool atRightCol = false;
                        mCtrlMmbZoomPivot = fetchScalePivot(io, useColumnView, mViewSplitPos, atRightCol);
                        mCtrlMmbZoomRightColumn = atRightCol;
                        mCtrlMmbZoomPivotLocked = true;
                    }
                    scalePivot = mCtrlMmbZoomPivot;
                    mouseAtRightColumn = mCtrlMmbZoomRightColumn;
                    static constexpr float kMmbZoomSensitivity = 0.003f;
                    const float zoomDrag = (io.MouseDelta.x - io.MouseDelta.y) * kMmbZoomSensitivity;
                    mImageScale *= glm::clamp(1.0f + zoomDrag, 0.25f, 4.0f);
                    mIsScalingImage = true;
                } else {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                    Vec2f translate(io.MouseDelta.x, -io.MouseDelta.y);
                    if (!useColumnView) {
                        mView.translate(translate);
                    } else if (io.KeyAlt) {
                        translate += mPanResidual;
                        Vec2f roundedTranslate = glm::round(translate / mImageScale);
                        int columnIdx = static_cast<int>(io.MousePos.x > io.DisplaySize.x * mViewSplitPos);
                        mColumnViews[columnIdx].translate(roundedTranslate, true);
                        mPanResidual = translate - roundedTranslate * mImageScale;
                    } else {
                        mColumnViews[0].translate(translate);
                        mColumnViews[1].translate(translate);
                    }
                    return;
                }
            }
#if defined(USE_VIDEO)
        }
#endif
    }

    // Zoom in/out from mouse scroll.
    static constexpr float kScrollZoomFactor = 1.12f;
    if (mouseActive && io.MouseWheel != 0.0f) {
        scalePivot = fetchScalePivot(io, useColumnView, mViewSplitPos, mouseAtRightColumn);
        if (io.MouseWheel > 0.0f) {
            mImageScale *= kScrollZoomFactor;
        } else {
            mImageScale /= kScrollZoomFactor;
        }
    }

    // Transform from keyboard
    const Vec2f activeSize = topImage ? topImage->size() : Vec2f(1.0f);
    if (ImGui::IsKeyPressed(KeyCode::KpSubtract) || ImGui::IsKeyPressed(KeyCode::Minus)) {
        mImageScale *= 0.5f;
    } else if (ImGui::IsKeyPressed(KeyCode::KpAdd) || ImGui::IsKeyPressed(KeyCode::Equal)) {
        mImageScale *= 2.0f;
    } else if (!io.KeyCtrl && ImGui::IsKeyPressed(KeyCode::Z) && mPrevScale < 0.0f) {
        mIsInSniperMode = true;
        scalePivot = fetchScalePivot(io, useColumnView, mViewSplitPos, mouseAtRightColumn);
        mPrevScale = mImageScale;
        mImageScale = 72.0f;
    } else if (io.KeyShift && ImGui::IsKeyPressed(KeyCode::F)) {
        resetImageTransform(activeSize, true);
        return;
    } else if (ImGui::IsKeyPressed(KeyCode::KpDivide) || ImGui::IsKeyPressed(KeyCode::Slash) || ImGui::IsKeyPressed(KeyCode::F)) {
        resetImageTransform(activeSize);
        return;
    }

    mImageScale = glm::clamp(mImageScale, 0.125f, 256.0f);
    const float relativeScale = mImageScale / oldImageScale;
    if (abs(relativeScale - 1.0f) < 1e-4f) {
        return;
    }

    if (!useColumnView) {
        mView.scale(relativeScale, scalePivot.x > 0.0f ? &scalePivot : nullptr);
    } else if (scalePivot.x < 0.0f) {
        mColumnViews[0].scale(relativeScale);
        mColumnViews[1].scale(relativeScale);
    } else {
        const int focusColumnIdx = mouseAtRightColumn ? 1 : 0;
        mColumnViews[focusColumnIdx].scale(relativeScale, &scalePivot);
        scalePivot = mColumnViews[focusColumnIdx].getImageScalePivot();

        Vec2f pixelCoords = mColumnViews[focusColumnIdx].getImageCoords(scalePivot);
        const int theOtherColumnIdx = focusColumnIdx ^ 1;
        Vec2f theOtherScalePivot = mColumnViews[theOtherColumnIdx].getViewportCoords(pixelCoords);

        theOtherScalePivot.y = scalePivot.y;
        mColumnViews[theOtherColumnIdx].scale(relativeScale, &theOtherScalePivot);
    }
}

void App::updateImagePairFromPressedKeys()
{
#if defined(USE_VIDEO)
    // Video mode:
    // - Left/Right arrow: jump ±5s (single or compare composition time), like many players
    // - ',' / '.': ±1 frame (YouTube-style)
    // - Ctrl+Shift+Left/Right arrow or Ctrl+Shift+L/R or A/D: cycle media list (compare), like image A/D
    // - Up / X: swap sides
    if (isVideoMode()) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.WantTextInput) {
            return;
        }

        const bool arrowLeft = ImGui::IsKeyPressed(KeyCode::Left);
        const bool arrowRight = ImGui::IsKeyPressed(KeyCode::Right);
        const bool keyComma = ImGui::IsKeyPressed(KeyCode::Comma);
        const bool keyPeriod = ImGui::IsKeyPressed(KeyCode::Period);
        const bool listMod = io.KeyCtrl && io.KeyShift;
        if (keyComma || keyPeriod) {
            stepVideoScrubOneFrame(keyPeriod ? 1 : -1);
            return;
        }
        if ((arrowLeft || arrowRight) && !listMod) {
            stepVideoScrubFiveSeconds(arrowRight ? 1 : -1);
            return;
        }

        const bool isSwap = ImGui::IsKeyPressed(KeyCode::Up) || ImGui::IsKeyPressed(KeyCode::X);
        const bool isNext = (arrowRight && listMod) || (listMod && ImGui::IsKeyPressed(KeyCode::R))
            || ImGui::IsKeyPressed(KeyCode::D);
        const bool isPrev =
            (arrowLeft && listMod) || (listMod && ImGui::IsKeyPressed(KeyCode::L)) || ImGui::IsKeyPressed(KeyCode::A);
        const int n = static_cast<int>(mMediaList.size());
        if (n >= 2 && (isSwap || isNext || isPrev)) {
            if (isSwap) {
                // If we're not currently in compare (only one decoder open), ensure the other side is
                // selected/opened first (especially important when exactly two media are loaded).
                if (!videoCompareActive()) {
                    if (mTopImageIndex < 0 || mTopImageIndex >= n) {
                        mTopImageIndex = 0;
                    }
                    if (mCmpImageIndex < 0 || mCmpImageIndex >= n || mCmpImageIndex == mTopImageIndex) {
                        mCmpImageIndex = (mTopImageIndex == 0) ? 1 : 0;
                    }
                    openVideosFromSelection();
                }
                swapVideoSides();
                return;
            } else if (isNext || isPrev) {
                if (!videoCompareActive()) {
                    // Solo playback: step only the active list row; keep a single decoder.
                    int l = mTopImageIndex;
                    if (l < 0 || l >= n) {
                        l = 0;
                    }
                    if (isNext) {
                        l = (l + 1) % n;
                    } else {
                        l = (l - 1 + n) % n;
                    }
                    mTopImageIndex = l;
                    mCmpImageIndex = -1;
                    mVideoSwapPresentationLR = false;
                    openVideosFromSelection();
                    return;
                }
                // With exactly 2 media, Left/Right should behave like images and swap instantly.
                // Avoid re-opening readers/textures (which is expensive) when we can just swap in memory.
                if (n == 2 && videoCompareActive()) {
                    swapVideoSides();
                    return;
                }
                // Match image behavior: advance DISPLAYED-left selection; keep DISPLAYED-right distinct by advancing it
                // only if it collides with the new left. This makes Left/Right swap when n==2.
                const bool dispSwap = mVideoSwapPresentationLR && videoCompareActive();
                int& idxDispL = dispSwap ? mCmpImageIndex : mTopImageIndex;
                int& idxDispR = dispSwap ? mTopImageIndex : mCmpImageIndex;

                int l = idxDispL;
                int r = idxDispR;
                if (l < 0 || l >= n) {
                    l = 0;
                }
                if (r < 0 || r >= n || r == l) {
                    r = (n >= 2) ? ((l + 1) % n) : -1;
                }

                if (isNext) {
                    l = (l + 1) % n;
                    if (r == l) {
                        r = (r + 1) % n;
                    }
                } else {
                    l = (l - 1 + n) % n;
                    if (r == l) {
                        r = (r - 1 + n) % n;
                    }
                }

                idxDispL = l;
                idxDispR = r;
            }
            openVideosFromSelection();
        }
        return;
    }
#endif

    const int imageNum = static_cast<int>(mMediaList.size());
    if (imageNum < 2) {
        return;
    }

    bool isSwap = ImGui::IsKeyPressed(KeyCode::Up) || ImGui::IsKeyPressed(KeyCode::X);
    bool isNext = ImGui::IsKeyPressed(KeyCode::Right) || ImGui::IsKeyPressed(KeyCode::D);
    bool isPrev = ImGui::IsKeyPressed(KeyCode::Left) || ImGui::IsKeyPressed(KeyCode::A);

    const bool enableCompareView = inCompareMode();

    if (enableCompareView && isSwap) {
        std::swap(mCmpImageIndex, mTopImageIndex);
        mTopImageRenderTexIdx ^= 1;
    } else if (isNext || isPrev) {
        // Same stepping in Top, Split, and Column: advance top and keep cmp distinct.
        // (Previously compare mode only moved cmp when n>2, so n==2 in split had no effect for arrows.)
        if (isNext) {
            mTopImageIndex = (mTopImageIndex + 1) % imageNum;
            if (mCmpImageIndex == mTopImageIndex) {
                mCmpImageIndex = (mCmpImageIndex + 1) % imageNum;
            }
        } else {
            mTopImageIndex = (mTopImageIndex - 1 + imageNum) % imageNum;
            if (mCmpImageIndex == mTopImageIndex) {
                mCmpImageIndex = (mCmpImageIndex - 1 + imageNum) % imageNum;
            }
        }
    }
}

// UI, image management, and video functions are in app_ui.cpp, app_image.cpp, app_video.cpp.

void    App::resetImageTransform(const Vec2f& /*imgSize*/, bool fitWindow)
{
    mImageScale = 1.0f;
    mPrevScale  = -1.0f;

    mView.reset(fitWindow);
    mColumnViews[0].reset(fitWindow);
    mColumnViews[1].reset(fitWindow);
}

bool App::getImageCoordinates(Vec2f viewportCoords, Vec2f& outImageCoords) const
{
    viewportCoords.y = ImGui::GetIO().DisplaySize.y - viewportCoords.y;

    bool isOutsideImage = false;
    if (!inSideBySideMode()) {
        outImageCoords = mView.getImageCoords(viewportCoords, &isOutsideImage);
    } else {
        auto& io = ImGui::GetIO();
        const float leftColumnWidth = glm::round(io.DisplaySize.x * mViewSplitPos);
        if (viewportCoords.x > leftColumnWidth) {
            viewportCoords.x -= leftColumnWidth;
            outImageCoords = mColumnViews[1].getImageCoords(viewportCoords, &isOutsideImage);
        } else {
            outImageCoords = mColumnViews[0].getImageCoords(viewportCoords, &isOutsideImage);
        }
    }

    if (!isOutsideImage && mTopImageIndex >= 0 && !inCompareMode() && !inSideBySideMode()) {
        const auto* im = getTopImage();
        if (im && refPxToPixel(outImageCoords, im->size(), im->size(),
                mFlipPresentMediaHorz[0], mFlipPresentMediaVert[0], outImageCoords)) {
            return true;
        }
    }

    outImageCoords = glm::floor(outImageCoords);

    return !isOutsideImage;
}

bool App::getCompareSideImagePixelAtMouse(Vec2f imguiMousePos, int side, Vec2f& outImagePx) const
{
    if (!inCompareMode() || mCmpImageIndex < 0 || mTopImageIndex < 0) {
        return false;
    }
    if (side != 0 && side != 1) {
        return false;
    }

    auto& io = ImGui::GetIO();
    Vec2f viewportCoords(imguiMousePos.x, io.DisplaySize.y - imguiMousePos.y);
    const float splitPx = glm::round(io.DisplaySize.x * mViewSplitPos);

    bool outsideRef = false;
    Vec2f refPx;
    if (!inSideBySideMode()) {
        if (side == 0 && viewportCoords.x >= splitPx) {
            return false;
        }
        if (side == 1 && viewportCoords.x < splitPx) {
            return false;
        }
        refPx = mView.getImageCoords(viewportCoords, &outsideRef);
    } else {
        if (side == 0) {
            if (viewportCoords.x > splitPx) {
                return false;
            }
            refPx = mColumnViews[0].getImageCoords(viewportCoords, &outsideRef);
        } else {
            if (viewportCoords.x <= splitPx) {
                return false;
            }
            viewportCoords.x -= splitPx;
            refPx = mColumnViews[1].getImageCoords(viewportCoords, &outsideRef);
        }
    }

    if (outsideRef) {
        return false;
    }

    const auto* topIm = getTopImage();
    const auto* cmpIm = getCmpImage();
    if (!topIm || !cmpIm) {
        return false;
    }

    const Vec2f topSz = topIm->size();
    const Vec2f cmpSz = cmpIm->size();
    const Vec2f refSz((std::max)(topSz.x, cmpSz.x), (std::max)(topSz.y, cmpSz.y));

    const auto* sideIm = (side == 0) ? topIm : cmpIm;
    const Vec2f actual = sideIm->size();
    return refPxToPixel(refPx, refSz, actual, mFlipPresentMediaHorz[side], mFlipPresentMediaVert[side], outImagePx);
}

float App::getPropWindowWidth() const
{
    ImGuiWindow* window = ImGui::FindWindowByName(kImagePropWindowName);
    return window == nullptr ? 250.0f : window->Size.x;
}

int App::getPixelMarkerFlags() const
{
    if (!mShowPixelMarker) {
        return 0;
    }

    PixelMarkerFlags flags = mPixelMarkerFlags;
    // Compare (two images or two videos): only diff/heatmap reach the shader, same as present.frag / video_blit.
    if (inCompareMode()) {
        flags &= PixelMarkerFlags::DiffMask;
    } else {
        flags &= ~PixelMarkerFlags::DiffMask;
    }

    return static_cast<int>(flags);
}

inline void    App::toggleSplitView()
{
    const bool canCompare = (mCmpImageIndex != -1)
#if defined(USE_VIDEO)
        || videoCompareActive()
        || (isVideoMode() && static_cast<int>(mMediaList.size()) >= 2)
#endif
        ;
    if (canCompare) {
#if defined(USE_VIDEO)
        const CompositeFlags prevComposite = mCompositeFlags;
#endif
        mCompositeFlags = toggleFlags(mCompositeFlags & ~CompositeFlags::SideBySide, CompositeFlags::Split);
#if defined(USE_VIDEO)
        const bool nowSplitOrSbs =
            (mCompositeFlags & (CompositeFlags::Split | CompositeFlags::SideBySide)) != CompositeFlags::Top;
        if (isVideoMode() && nowSplitOrSbs && !videoCompareActive()) {
            const int n = static_cast<int>(mMediaList.size());
            if (n >= 2 && mTopImageIndex >= 0 && mTopImageIndex < n
                && (mCmpImageIndex < 0 || mCmpImageIndex >= n || mCmpImageIndex == mTopImageIndex)) {
                for (int i = 0; i < n; ++i) {
                    if (i != mTopImageIndex) {
                        mCmpImageIndex = i;
                        break;
                    }
                }
                openVideosFromSelection();
            }
        }
        if (isVideoMode() && videoCompareActive()) {
            const bool wasLayoutCompare =
                (prevComposite & (CompositeFlags::Split | CompositeFlags::SideBySide)) != CompositeFlags::Top;
            const bool nowLayoutCompare =
                (mCompositeFlags & (CompositeFlags::Split | CompositeFlags::SideBySide)) != CompositeFlags::Top;
            if (!wasLayoutCompare && nowLayoutCompare) {
                resetVideoDecoderSyncCache();
            }
        }
#endif
    }
}

inline void    App::toggleSideBySideView()
{
    const bool canCompare = (mCmpImageIndex != -1)
#if defined(USE_VIDEO)
        || videoCompareActive()
        || (isVideoMode() && static_cast<int>(mMediaList.size()) >= 2)
#endif
        ;
    if (canCompare) {
#if defined(USE_VIDEO)
        const CompositeFlags prevComposite = mCompositeFlags;
#endif
        mCompositeFlags = toggleFlags(mCompositeFlags & ~CompositeFlags::Split, CompositeFlags::SideBySide);
#if defined(USE_VIDEO)
        const bool nowSplitOrSbs =
            (mCompositeFlags & (CompositeFlags::Split | CompositeFlags::SideBySide)) != CompositeFlags::Top;
        if (isVideoMode() && nowSplitOrSbs && !videoCompareActive()) {
            const int n = static_cast<int>(mMediaList.size());
            if (n >= 2 && mTopImageIndex >= 0 && mTopImageIndex < n
                && (mCmpImageIndex < 0 || mCmpImageIndex >= n || mCmpImageIndex == mTopImageIndex)) {
                for (int i = 0; i < n; ++i) {
                    if (i != mTopImageIndex) {
                        mCmpImageIndex = i;
                        break;
                    }
                }
                openVideosFromSelection();
            }
        }
        if (isVideoMode() && videoCompareActive()) {
            const bool wasLayoutCompare =
                (prevComposite & (CompositeFlags::Split | CompositeFlags::SideBySide)) != CompositeFlags::Top;
            const bool nowLayoutCompare =
                (mCompositeFlags & (CompositeFlags::Split | CompositeFlags::SideBySide)) != CompositeFlags::Top;
            if (!wasLayoutCompare && nowLayoutCompare) {
                resetVideoDecoderSyncCache();
            }
        }
#endif
    }
}

inline bool    App::inCompareMode() const
{
    const bool layoutIsCompare =
        (mCompositeFlags & (CompositeFlags::Split | CompositeFlags::SideBySide)) != CompositeFlags::Top;
#if defined(USE_VIDEO)
    if (isVideoMode()) {
        return videoCompareActive() && layoutIsCompare;
    }
#endif
    return mCmpImageIndex != -1 && layoutIsCompare;
}

inline bool    App::inSideBySideMode() const
{
    return (mCompositeFlags & CompositeFlags::SideBySide) != CompositeFlags::Top;
}

inline bool    App::shouldShowSplitter() const
{
    return inCompareMode() && (getPixelMarkerFlags() & static_cast<int>(PixelMarkerFlags::DiffMask)) == 0;
}


}  // namespace baktsiu