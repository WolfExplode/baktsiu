#define IMGUI_DEFINE_MATH_OPERATORS
#include <icons_font_awesome5.h>
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#pragma warning(push)
#pragma warning(disable: 4819 4566)
#include "portable_file_dialogs.h"      // File dialogs.

#include <GL/gl3w.h>    // Initialize with gl3wInit()
#include <GLFW/glfw3.h>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#pragma warning(pop)

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>

#include <fx/gltf.h>
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

namespace
{

bool endsWith(const std::string& str, const std::string& token)
{
    return str.rfind(token, str.size() - token.size()) != std::string::npos;
}

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

// See more implementations about toggle button https://github.com/ocornut/imgui/issues/1537
bool ToggleButton(const char* label, bool* value, const ImVec2 &size = ImVec2(0, 0), bool enable = true)
{
    bool valueChanged = false;
    ImGuiContext& g = *ImGui::GetCurrentContext();

    if (*value) {
        ImGui::PushStyleColor(ImGuiCol_Button, g.Style.Colors[ImGuiCol_ButtonActive]);
        
        if (ImGui::Button(label, size)) {
            *value ^= true;
            valueChanged = true;
        }

        ImGui::PopStyleColor(1);
        return valueChanged;
    }

    if (!enable) {
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g.Style.Colors[ImGuiCol_Button]);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, g.Style.Colors[ImGuiCol_Button]);
    }

    if (ImGui::Button(label, size) && enable) {
        *value = true;
        valueChanged = true;
    }

    if (!enable) {
        ImGui::PopStyleColor(2);
    }

    return valueChanged;
}

bool Splitter(bool split_vertically, float thickness, float* size1, float* size2, float min_size1, float min_size2, float splitter_long_axis_size = -1.0f)
{
    using namespace ImGui;
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = g.CurrentWindow;
    ImGuiID id = window->GetID("##Splitter");
    ImRect bb;
    bb.Min = window->DC.CursorPos + (split_vertically ? ImVec2(*size1, 0.0f) : ImVec2(0.0f, *size1));
    bb.Max = bb.Min + CalcItemSize(split_vertically ? ImVec2(thickness, splitter_long_axis_size) : ImVec2(splitter_long_axis_size, thickness), 0.0f, 0.0f);
    return SplitterBehavior(bb, id, split_vertically ? ImGuiAxis_X : ImGuiAxis_Y, size1, size2, min_size1, min_size2, 0.0f);
}

}  // namespace anonymous

namespace ImGui 
{

static ImU32 InvertColorU32(ImU32 in)
{
    ImVec4 in4 = ColorConvertU32ToFloat4(in);
    in4.x = 1.f - in4.x;
    in4.y = 1.f - in4.y;
    in4.z = 1.f - in4.z;
    return GetColorU32(in4);
}

// Draw advanced histogram https://github.com/ocornut/imgui/issues/632
void PlotMultiHistograms(
    const char* label,
    int num_hists,
    const char** names,
    const ImColor* colors,
    int pixel_counts[],
    int norm_pixel_count,
    int values_count,
    float scale_min,
    float scale_max,
    ImVec2 graph_size)
{
    const int values_offset = 0;

    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);

    const ImVec2 label_size = ImGui::CalcTextSize(label, NULL, true);
    if (graph_size.x == 0.0f)
        graph_size.x = CalcItemWidth();
    if (graph_size.y == 0.0f)
        graph_size.y = label_size.y + (style.FramePadding.y * 2);

    const ImRect frame_bb(window->DC.CursorPos, window->DC.CursorPos + ImVec2(graph_size.x, graph_size.y));
    const ImRect inner_bb(frame_bb.Min + style.FramePadding, frame_bb.Max - style.FramePadding);
    const ImRect total_bb(frame_bb.Min, frame_bb.Max + ImVec2(label_size.x > 0.0f ? style.ItemInnerSpacing.x + label_size.x : 0.0f, 0));
    ItemSize(total_bb, style.FramePadding.y);
    if (!ItemAdd(total_bb, 0)) {
        return;
    }

    RenderFrame(inner_bb.Min, inner_bb.Max, GetColorU32(ImGuiCol_FrameBg), true, 1.0f);

    int res_w = ImMin((int)graph_size.x, values_count);
    int item_count = values_count;

    // Tooltip on hover
    int v_hovered = -1;
    if (ItemHoverable(inner_bb, id)) {
        const float t = ImClamp((g.IO.MousePos.x - inner_bb.Min.x) / (inner_bb.Max.x - inner_bb.Min.x), 0.0f, 1.0f);
        const int v_idx = std::min((int)(t * item_count), item_count - 1);
        IM_ASSERT(v_idx >= 0 && v_idx < values_count);

        // std::string toolTip;
        ImGui::BeginTooltip();
        const int idx0 = (v_idx + values_offset) % values_count;
        TextColored(ImColor(255, 255, 255, 255), "Level: %d", v_idx);

        for (int dataIdx = 0; dataIdx < num_hists; ++dataIdx) {
            const int v0 = pixel_counts[dataIdx * values_count + idx0];
            TextColored(ImColor(255, 255, 255, 255), "%s: %d", names[dataIdx], v0);
        }
        
        ImGui::EndTooltip();
        v_hovered = v_idx;
    }

    for (int data_idx = 0; data_idx < num_hists; ++data_idx) {
        const float t_step = 1.0f / (float)res_w;

        float v0 = pixel_counts[data_idx * values_count] / static_cast<float>(norm_pixel_count);
        float t0 = 0.0f;
        ImVec2 tp0 = ImVec2(t0, 1.0f - ImSaturate((v0 - scale_min) / (scale_max - scale_min)));    // Point in the normalized space of our target rectangle

        const ImU32 col_base = colors[data_idx];
        const ImU32 col_hovered = InvertColorU32(colors[data_idx]);

        for (int n = 0; n < res_w; n++) {
            const float t1 = t0 + t_step;
            const int v1_idx = (int)(t0 * item_count + 0.5f);
            IM_ASSERT(v1_idx >= 0 && v1_idx < values_count);
            const float v1 = pixel_counts[data_idx * values_count + v1_idx] / static_cast<float>(norm_pixel_count);
            const ImVec2 tp1 = ImVec2(t1, 1.0f - ImSaturate((v1 - scale_min) / (scale_max - scale_min)));

            // NB: Draw calls are merged together by the DrawList system. Still, we should render our batch are lower level to save a bit of CPU.
            ImVec2 pos0 = ImLerp(inner_bb.Min, inner_bb.Max, tp0);
            ImVec2 pos1 = ImLerp(inner_bb.Min, inner_bb.Max, ImVec2(tp1.x, 1.0f));
            if (pos1.x >= pos0.x + 2.0f) {
                pos1.x -= 1.0f;
            }
            window->DrawList->AddRectFilled(pos0, pos1, v_hovered == v1_idx ? col_hovered : col_base);

            t0 = t1;
            tp0 = tp1;
        }
    }

    //RenderText(ImVec2(frame_bb.Max.x + style.ItemInnerSpacing.x, inner_bb.Min.y), label);
}

} // namespace ImGui


namespace
{

void formatVideoHms(double secIn, char* out, size_t outSize)
{
    if (outSize == 0) {
        return;
    }
    if (secIn < 0.0) {
        secIn = 0.0;
    }
    const double capped = std::min(secIn, static_cast<double>(LLONG_MAX / 16));
    const long long t = static_cast<long long>(std::floor(capped + 0.0005));
    const long long h = t / 3600LL;
    const long long m = (t % 3600LL) / 60LL;
    const long long s = t % 60LL;
    std::snprintf(out, outSize, "%lld:%02lld:%02lld", h, m, s);
}

}  // namespace

namespace baktsiu
{

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
        status = mStatisticsShader.initCompute("statistics", statistics_comp);
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
#endif

    // Cleanup
    mImageList.clear();
    mPointSampler.release();

    mPresentShader.release();
    mGradingShader.release();

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
            const int curImageNum = static_cast<int>(mImageList.size());
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
        mTopImageIndex = static_cast<int>(mImageList.size()) - 1;
        resetImageTransform(getTopImage()->size());

        if (mCmpImageIndex == -1 && mTopImageIndex >= 1) {
            mCmpImageIndex = 0;
        }

        mUpdateImageSelection = false;
    }
}

void App::run(CompositeFlags initFlags)
{
    bool shouldChangeComposition = true;

    // Spawn worker threads to handle import images.
    const unsigned int workerNum = std::thread::hardware_concurrency();
    mTexturePool.initialize(workerNum);

    while (!glfwWindowShouldClose(mWindow)) {
        glfwPollEvents();

        processTextureUploadTasks();
        if (shouldChangeComposition && mImageList.size() >= 2) {
            mCompositeFlags = initFlags;
            shouldChangeComposition = false;
        }

#if defined(USE_VIDEO)
        const bool videoActive = mVideoMode && mVideoReader && mVideoReader->isOpen();
#else
        const bool videoActive = false;
#endif

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        initToolbar();
        initFooter();
        initImagePropWindow();
        
        ImGuiIO& io = ImGui::GetIO();

        // Use WantCaptureMouse + GLFW focus, not "!AnyWindowFocused". Toolbar items call
        // SetItemDefaultFocus(), so an ImGui window always has focus at startup and would
        // block pan/scroll until the user clicked away — even with the mouse over the image.
        if (glfwGetWindowAttrib(mWindow, GLFW_FOCUSED) && !io.WantCaptureMouse) {
            updateImageSplitterPos(io);
        }

        onKeyPressed(io);
        Image* topImage = getTopImage();
        Vec2f imageSize = topImage ? topImage->size() : Vec2f(1.0f);
        Vec2f refImageSize = imageSize;

        const bool useColumnView = inSideBySideMode();
        if (!videoActive) {
            const bool enableCompareView = inCompareMode();
            if (enableCompareView && mCmpImageIndex >= 0 && topImage) {
                Image* cmpImage = mImageList[mCmpImageIndex].get();
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
            tickAndUploadVideoFrame(io.DeltaTime);
            initVideoTransportBar(io);
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
                    computeImageStatistics(mRenderTextures[mTopImageRenderTexIdx], valueScale);
                }
            }

            const bool enableCompareView = inCompareMode();
            if (enableCompareView && mCmpImageIndex >= 0) {
                Image* cmpImage = mImageList[mCmpImageIndex].get();
                if (cmpImage->texId() != 0) {
                    gradingTexImage(*cmpImage, mTopImageRenderTexIdx ^ 1);
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
            Image* cmpForPresent = (enableCompareView && mCmpImageIndex >= 0)
                ? mImageList[mCmpImageIndex].get()
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
        }
#endif

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(mWindow);
    }

    mTexturePool.release();
}

void    App::gradingTexImage(Image& image, int renderTexIdx)
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

void    App::computeImageStatistics(const RenderTexture& texture, float valueScale)
{
    ScopeMarker("Compute Image Statistics");

    // Calculate image statistics.
    std::fill(mHistogram.begin(), mHistogram.end(), 0);
    glBindTexture(GL_TEXTURE_2D, mTexHistogram);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLint>(mHistogram.size()), 1, GL_RED_INTEGER, GL_INT, (void*)mHistogram.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    mStatisticsShader.bind();
    glBindImageTexture(0, texture.id(), 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
    glBindImageTexture(1, mTexHistogram, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32I);

    Vec2i size = texture.size();
    mStatisticsShader.setUniform("uImageSize", size);
    mStatisticsShader.setUniform("uValueScale", valueScale);
    mStatisticsShader.compute(size.x / 16, size.y / 16);

    /*glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
    glBindTexture(GL_TEXTURE_2D, mTexHistogram);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_INT, (void*)mHistogram.data());*/
}

void    App::onKeyPressed(const ImGuiIO& io)
{
    if (ImGui::IsKeyPressed(0x102)) { // tab
        mShowImagePropWindow ^= true;
    } else if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(0x57)) { // Ctrl+Shift+w
        clearImages(true);
    } else if (io.KeyCtrl && ImGui::IsKeyPressed(0x5A)) { // Ctrl+z
        undoAction();
    } else if (io.KeyCtrl && ImGui::IsKeyPressed(0x4F)) { // Ctrl+o
        showImportImageDlg();
    } else if (io.KeyCtrl && ImGui::IsKeyPressed(0x45)) { // Ctrl+e
        showExportSessionDlg();
    } else if (io.KeyAlt && ImGui::IsKeyPressed(0x43)) { // Alt+c
        syncSideBySideView(io);
    } else if (ImGui::IsKeyPressed(0x53)) { // s
        toggleSplitView();
    } else if (ImGui::IsKeyPressed(0x43)) { // c
        toggleSideBySideView();
    } else if (ImGui::IsKeyPressed(32)) { // Space
#if defined(USE_VIDEO)
        if (mVideoMode && mVideoReader && mVideoReader->isOpen() && !io.WantTextInput) {
            mVideoPlaying ^= true;
            mVideoPlaybackTimeBank = 0.0;
            mVideoComparePlaybackBank = 0.0;
        }
#endif
    } else if (ImGui::IsKeyPressed(0x51)) { // q
        mUseLinearFilter ^= true;
    } else if (ImGui::IsKeyPressed(0x57)) { // w
        mShowPixelMarker ^= true;
    } else if (ImGui::IsKeyPressed(0x122)) { // F1
        ImGui::OpenPopup("Home");
    } else if (ImGui::IsKeyPressed(0x126)) { // F5
        Image* image = getTopImage();
        if (image) image->reload();
    } else if (ImGui::IsKeyPressed(0x103) || ImGui::IsKeyPressed(0x105)) { // Backspace/Del
        if (mTopImageIndex > -1) ImGui::OpenPopup(kImageRemoveDlgTitle);
    } else {
        updateImagePairFromPressedKeys();
        // ps. There are few other key pressed cases are handled in updateImageTransform().
    }

    initHomeWindow("Home");

    if (showRemoveImageDlg(kImageRemoveDlgTitle)) {
        removeTopImage(true);
    }
}

void    App::updateImageSplitterPos(ImGuiIO& io)
{
#if defined(USE_VIDEO)
    const bool splitForVideoCompare = mVideoMode && videoCompareActive();
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
    Image* topImage = getTopImage();
    if (!topImage) {
        return;
    }

    Vec2f scalePivot(-1.0f);
    bool mouseAtRightColumn = false;
    auto fetchScalePivot = [](const ImGuiIO& io, bool useColumnView, float viewSplitPos, bool& mouseAtRightColumn) {
        mouseAtRightColumn = false;
        Vec2f scalePivot(io.MousePos.x, io.DisplaySize.y - io.MousePos.y);
        if (useColumnView) {
            float leftColumnWidth = io.DisplaySize.x * viewSplitPos;
            if (scalePivot.x > leftColumnWidth) {
                scalePivot.x -= leftColumnWidth;
                mouseAtRightColumn = true;
            }
        }

        scalePivot = glm::round(scalePivot + Vec2f(0.5f)) - Vec2f(0.5f);
        return scalePivot;
    };

    mImageScale = inSideBySideMode() ? mColumnViews[0].getImageScale() : mView.getImageScale();

    // Mouse on image: GLFW window focused and ImGui not consuming mouse this frame (see run()).
    const bool imageMouseActive = glfwGetWindowAttrib(mWindow, GLFW_FOCUSED) && !io.WantCaptureMouse;
    float oldImageScale = mImageScale;
    
    static bool isInSniperMode = false;

    if (isInSniperMode && ImGui::IsKeyReleased(0x5A)) {
        mImageScale = mPrevImageScale;
        mPrevImageScale = -1.0f;
        isInSniperMode = false;
    }

    if (imageMouseActive) {
        const bool doingCtrlMmbZoom = !mIsMovingSplitter && ImGui::IsMouseDown(2) && io.KeyCtrl;
        if (!doingCtrlMmbZoom) {
            mCtrlMmbZoomPivotLocked = false;
        }

        if (ImGui::IsMouseReleased(0) || ImGui::IsMouseReleased(1) || ImGui::IsMouseReleased(2)) {
            mIsScalingImage = false;
        }

        if (!shouldShowSplitter() && ImGui::IsMouseDown(0) && ImGui::IsMouseDown(1)) {
            // Scale the image when both left and right buttons are pressed (disabled while compare split is active — LMB is for the split bar).
            mImageScale *= (1.0f - glm::roundEven(io.MouseDelta.y) * 0.0078125f);
            mIsScalingImage = true;
        } else if (!mIsMovingSplitter && ImGui::IsMouseDown(2)) {
            if (io.KeyCtrl) {
                // Ctrl + MMB drag: zoom about the point under the cursor when the gesture started (pivot fixed until MMB/Ctrl released).
                // Right or up = in; left or down = out (ImGui MouseDelta.y is positive downward).
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
            } else { // MMB pan
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);

                Vec2f translate(io.MouseDelta.x, -io.MouseDelta.y);

                if (!useColumnView) {
                    mView.translate(translate);
                } else if (io.KeyAlt) {
                    static Vec2f residualTranslate = Vec2f(0.0f);
                    translate += residualTranslate;
                    
                    Vec2f roundedTranslate = glm::round(translate / mImageScale);
                    int columnIdx = static_cast<int>(io.MousePos.x > io.DisplaySize.x * mViewSplitPos);
                    mColumnViews[columnIdx].translate(roundedTranslate, true);
                    residualTranslate = translate - roundedTranslate * mImageScale;
                } else {
                    mColumnViews[0].translate(translate);
                    mColumnViews[1].translate(translate);
                }

                return;
            }
        }
    }

    // Zoom in/out from mouse scroll. Use sign(io.MouseWheel) only so Windows
    // "lines to scroll" (|wheel| > 1) does not apply multiple doublings at once;
    // use a modest factor per tick instead of 2^|wheel|.
    static constexpr float kScrollZoomFactor = 1.12f;
    if (imageMouseActive && io.MouseWheel != 0.0f) {
        scalePivot = fetchScalePivot(io, useColumnView, mViewSplitPos, mouseAtRightColumn);

        if (io.MouseWheel > 0.0f) {
            mImageScale *= kScrollZoomFactor;
        } else {
            mImageScale /= kScrollZoomFactor;
        }
    }

    // Transfrom from keyboard
    if (ImGui::IsKeyPressed(0x14D) || ImGui::IsKeyPressed(0x2D)) {
        mImageScale *= 0.5f;
    } else if (ImGui::IsKeyPressed(0x14E) || ImGui::IsKeyPressed(0x3D)) {
        mImageScale *= 2.0f;
    } 
    else if (!io.KeyCtrl && ImGui::IsKeyPressed(0x5A) && mPrevImageScale < 0.0f) {
        isInSniperMode = true;
        scalePivot = fetchScalePivot(io, useColumnView, mViewSplitPos, mouseAtRightColumn);
        mPrevImageScale = mImageScale;
        mImageScale = 72.0f;
    }
    else if (io.KeyShift && ImGui::IsKeyPressed(0x046)) { // shift+f
        resetImageTransform(topImage->size(), true);
        return;
    } else if (ImGui::IsKeyPressed(0x14B) || ImGui::IsKeyPressed(0x2F) || ImGui::IsKeyPressed(0x046)) { // '/' or 'f'
        resetImageTransform(topImage->size());
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
        // Use previous scale pivot.
        mColumnViews[0].scale(relativeScale);
        mColumnViews[1].scale(relativeScale);
    } else {
        const int focusColumnIdx = mouseAtRightColumn ? 1 : 0;
        mColumnViews[focusColumnIdx].scale(relativeScale, &scalePivot);
        scalePivot = mColumnViews[focusColumnIdx].getImageScalePivot();

        // Retrieve the scale pivot in the other column view, we first get the
        // pixel coordinates, then use it to get corresponding viewport coordinates.
        Vec2f pixelCoords = mColumnViews[focusColumnIdx].getImageCoords(scalePivot);
        const int theOtherColumnIdx = focusColumnIdx ^ 1;
        Vec2f theOtherScalePivot = mColumnViews[theOtherColumnIdx].getViewportCoords(pixelCoords);

        // Align pivot height.
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
    if (mVideoMode) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.WantTextInput) {
            return;
        }

        const bool arrowLeft = ImGui::IsKeyPressed(0x107);   // Left
        const bool arrowRight = ImGui::IsKeyPressed(0x106);  // Right
        const bool keyComma = ImGui::IsKeyPressed(0x2C);    // ','
        const bool keyPeriod = ImGui::IsKeyPressed(0x2E);  // '.'
        const bool listMod = io.KeyCtrl && io.KeyShift;
        if (mVideoReader && mVideoReader->isOpen() && (keyComma || keyPeriod)) {
            stepVideoScrubOneFrame(keyPeriod ? 1 : -1);
            return;
        }
        if (mVideoReader && mVideoReader->isOpen() && (arrowLeft || arrowRight) && !listMod) {
            stepVideoScrubFiveSeconds(arrowRight ? 1 : -1);
            return;
        }

        const bool isSwap = ImGui::IsKeyPressed(0x109) || ImGui::IsKeyPressed(0x58);  // Up arrow or 'x'
        const bool isNext = (arrowRight && listMod) || (listMod && ImGui::IsKeyPressed(0x52))  // R
            || ImGui::IsKeyPressed(0x44);                                                  // D
        const bool isPrev =
            (arrowLeft && listMod) || (listMod && ImGui::IsKeyPressed(0x4C)) || ImGui::IsKeyPressed(0x41);  // L, A
        const int n = static_cast<int>(mVideoPaths.size());
        if (n >= 2 && (isSwap || isNext || isPrev)) {
            if (isSwap) {
                // If we're not currently in compare (only one decoder open), ensure the other side is
                // selected/opened first (especially important when exactly two media are loaded).
                if (!videoCompareActive()) {
                    if (mVideoIndexL < 0 || mVideoIndexL >= n) {
                        mVideoIndexL = 0;
                    }
                    if (mVideoIndexR < 0 || mVideoIndexR >= n || mVideoIndexR == mVideoIndexL) {
                        mVideoIndexR = (mVideoIndexL == 0) ? 1 : 0;
                    }
                    openVideosFromSelection();
                }
                swapVideoSides();
                return;
            } else if (isNext || isPrev) {
                // With exactly 2 media, Left/Right should behave like images and swap instantly.
                // Avoid re-opening readers/textures (which is expensive) when we can just swap in memory.
                if (n == 2 && videoCompareActive()) {
                    swapVideoSides();
                    return;
                }
                // Match image behavior: advance DISPLAYED-left selection; keep DISPLAYED-right distinct by advancing it
                // only if it collides with the new left. This makes Left/Right swap when n==2.
                const bool dispSwap = mVideoSwapPresentationLR && videoCompareActive();
                int& idxDispL = dispSwap ? mVideoIndexR : mVideoIndexL;
                int& idxDispR = dispSwap ? mVideoIndexL : mVideoIndexR;

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

    const int imageNum = static_cast<int>(mImageList.size());
    if (imageNum < 2) {
        return;
    }

    bool isSwap = ImGui::IsKeyPressed(0x109) || ImGui::IsKeyPressed(0x58); // Up arrow or 'x'
    bool isNext = ImGui::IsKeyPressed(0x106) || ImGui::IsKeyPressed(0x44); // Right arrow or 'd'
    bool isPrev = ImGui::IsKeyPressed(0x107) || ImGui::IsKeyPressed(0x41); // Left arrow or 'a'

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

void    App::initToolbar()
{
    ImGuiContext& g = *ImGui::GetCurrentContext();

    ImGui::SetNextWindowPos(Vec2f(0.0f));
    ImGui::SetNextWindowSize(Vec2f(g.IO.DisplaySize.x, mToolbarHeight));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Vec2f(2.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, Vec2f(2.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, Vec2f(4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, Vec2f(0.5f, 0.75f));   // Make font icon align center.
    ImGui::PushStyleColor(ImGuiCol_Button, g.Style.Colors[ImGuiCol_MenuBarBg]);

    ImGui::Begin("toolbar", nullptr, ImGuiWindowFlags_NoDecoration);

    const Vec2f buttonSize(26.0f);
    const char* popupWindowName = "Home";

    if (ImGui::Button(ICON_FA_HOME, buttonSize)) {
        ImGui::OpenPopup(popupWindowName);
    }
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Home Window"); }


    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FILE_IMPORT, buttonSize)) {
        showImportImageDlg();
    }
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Import Images"); }

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FILE_EXPORT, buttonSize)) {
        showExportSessionDlg();
    }
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Export Session"); }

    const float comboMenuWidth = 100.0f;
    static float centeredToolItemWidth = 506.0f;
    ImGui::SameLine((g.IO.DisplaySize.x - centeredToolItemWidth) * 0.5f);
    float centeredToolBeginPos = g.CurrentWindow->DC.CursorPos.x;
    ToggleButton(ICON_FA_FEATHER, &mUseLinearFilter, buttonSize);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Smooth image");
    }

    ImGui::SameLine();
    const bool couldCompare = mImageList.size() > 1;
    bool inSplitView = (mCompositeFlags == CompositeFlags::Split);
    if (ToggleButton(ICON_FA_I_CURSOR, &inSplitView, buttonSize, couldCompare)) {
        toggleSplitView();
    }
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Split View"); }

    ImGui::SameLine();
    bool inSideBySideView = inSideBySideMode();
    if (ToggleButton(ICON_FA_COLUMNS, &inSideBySideView, buttonSize, couldCompare)) {
        toggleSideBySideView();
    }
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Side by Side View"); }

    ImGui::SameLine();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, Vec2f(5.0f));
    ImGui::PushItemWidth(comboMenuWidth);
    const char* outTransformTypes[] = { "sRGB", "P3 D65", "BT.2020" };
    if (ImGui::BeginCombo("##OutputColorSpace", outTransformTypes[mOutTransformType]))
    {
        for (int i = 0; i < IM_ARRAYSIZE(outTransformTypes); i++) {
            bool isSelected = (mOutTransformType == i);
            if (ImGui::Selectable(outTransformTypes[i], isSelected)) {
                mOutTransformType = i;
            }

            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Display Color Primaries");
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.0f);
    ImGui::SliderFloat("##EV", &mExposureValue, -8.0f, 8.0f, "EV: %.1f");
    if (ImGui::IsItemClicked(1)) {
        mExposureValue = 0.0f;
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::SliderFloat("##Gamma", &mDisplayGamma, 1.0f, 2.8f, "gamma: %.1f");
    if (ImGui::IsItemClicked(1)) {
        mDisplayGamma = 2.2f;
    }

    ImGui::SameLine();
    const char* presentModes[] = { "RGB", "R", "G", "B", "Luminance", "CIE L*", "CIE a*", "CIE b*" };
    if (ImGui::BeginCombo("##Channel", presentModes[mCurrentPresentMode]))
    {
        for (int i = 0; i < IM_ARRAYSIZE(presentModes); i++) {
            bool isSelected = (mCurrentPresentMode == i);
            if (ImGui::Selectable(presentModes[i], isSelected)) {
                mCurrentPresentMode = i;
            }

            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Channels");
    }

    ImGui::PopItemWidth();
    ImGui::PopStyleVar(1);

    ImGui::SameLine();
    ToggleButton(ICON_FA_FILM, &mEnableToneMapping, buttonSize, mCurrentPresentMode == 0);

    ImGui::SameLine();
    ToggleButton(ICON_FA_EXCLAMATION_TRIANGLE, &mShowPixelMarker, buttonSize);
    if (ImGui::IsItemClicked(1)) { ImGui::OpenPopup("PixelMarkMenu"); }
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Pixel Warning Markers"); }

    if (ImGui::BeginPopup("PixelMarkMenu")) {
        const bool enableCompareView = inCompareMode();

        bool itemValue = hasAllFlags(mPixelMarkerFlags, PixelMarkerFlags::Difference);
        if (ImGui::MenuItem("Difference", "", &itemValue, enableCompareView)) {
            auto newState = itemValue ? PixelMarkerFlags::Difference : PixelMarkerFlags::None;
            mPixelMarkerFlags = (mPixelMarkerFlags & ~PixelMarkerFlags::DiffMask) | newState;
        }
        itemValue = hasAllFlags(mPixelMarkerFlags, PixelMarkerFlags::DiffHeatMap);
        if (ImGui::MenuItem("Diff as Heatmap", "", &itemValue, enableCompareView)) {
            auto newState = itemValue ? PixelMarkerFlags::DiffHeatMap : PixelMarkerFlags::None;
            mPixelMarkerFlags = (mPixelMarkerFlags & ~PixelMarkerFlags::DiffMask) | newState;
        }

        const bool showDiffMarker = hasAnyFlags(mPixelMarkerFlags, PixelMarkerFlags::DiffMask);
        
        itemValue = hasAllFlags(mPixelMarkerFlags, PixelMarkerFlags::Overflow);
        if (ImGui::MenuItem("Overflow", "", &itemValue, !enableCompareView || !showDiffMarker)) {
            mPixelMarkerFlags = toggleFlags(mPixelMarkerFlags, PixelMarkerFlags::Overflow);
        }

        itemValue = hasAllFlags(mPixelMarkerFlags, PixelMarkerFlags::Underflow);
        if (ImGui::MenuItem("Underflow", "", &itemValue, !enableCompareView || !showDiffMarker)) {
            mPixelMarkerFlags = toggleFlags(mPixelMarkerFlags, PixelMarkerFlags::Underflow);
        }

        ImGui::EndPopup();
    }

    ImGui::SameLine();
    centeredToolItemWidth = g.CurrentWindow->DC.CursorPos.x - centeredToolBeginPos;

    // Show buttons at right hand side.
    ImGui::SameLine(g.IO.DisplaySize.x - (buttonSize.x + g.Style.ItemSpacing.x) * 2.0f);
    ToggleButton(ICON_FA_COMMENT_ALT, &mShowImageNameOverlay, buttonSize);
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Image Name Tags"); }

    ImGui::SameLine(g.IO.DisplaySize.x - buttonSize.x - g.Style.ItemSpacing.x);
    ToggleButton(ICON_FA_CHART_BAR, &mShowImagePropWindow, buttonSize);
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Property Window"); }

    ImGui::PopStyleColor(1);
    ImGui::PopStyleVar(7);

    initHomeWindow(popupWindowName);
    ImGui::End();
}

bool    App::initImagePropWindowHandle(float handleWidth, bool& showPropWindow)
{
    bool isHovered = false;

    ImGuiContext& g = *ImGui::GetCurrentContext();
    //const auto handleWidth = g.FontSize;
    ImGui::SetNextWindowPos(ImVec2(g.IO.DisplaySize.x - handleWidth, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(handleWidth, g.IO.DisplaySize.y));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Vec2f(0.0f));
    ImGui::Begin("ImagePropHandle", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::PushStyleColor(ImGuiCol_Button, g.Style.Colors[ImGuiCol_MenuBarBg]);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g.Style.Colors[ImGuiCol_ButtonHovered]);
    const char* label = showPropWindow ? ":\n}\n:" : ":\n{\n:";
    if (ImGui::Button(label, ImVec2(handleWidth, g.IO.DisplaySize.y))) {
        showPropWindow ^= true;
    }
    ImGui::PopStyleColor(2);

    if (ImGui::IsItemHovered()) {
        isHovered = true;
    }
    ImGui::End();
    ImGui::PopStyleVar(3);

    return isHovered;
}


// Ref: splitter https://github.com/ocornut/imgui/issues/319
void App::initImagePropWindow()
{
    bool checkPopupRect = false;

    ImGuiContext& g = *ImGui::GetCurrentContext();
    const float handleWidth = g.FontSize;

    // Create a window as a toggle handle to open/close property window.
    if (initImagePropWindowHandle(handleWidth, mShowImagePropWindow)) {
        mPopupImagePropWindow = true;
    } else {
        checkPopupRect = true;
    }

    if (!mShowImagePropWindow && !mPopupImagePropWindow) {
        return;
    }

    // Create a property window.
    const float propWindowWidth = getPropWindowWidth();
    ImGui::SetNextWindowPos(ImVec2(g.IO.DisplaySize.x - propWindowWidth - handleWidth, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(propWindowWidth, g.IO.DisplaySize.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Vec2f(4.0f, mToolbarHeight));

    ImGui::Begin(kImagePropWindowName, nullptr, ImGuiWindowFlags_NoTitleBar
        | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus
        | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::PopStyleVar(2);

    // Create horizontal splitter for up and down panels.
    float size1 = (g.IO.DisplaySize.y - mToolbarHeight - mFooterHeight) * mPropWindowHSplitRatio;
    float size2 = g.IO.DisplaySize.y - size1 - mToolbarHeight - mFooterHeight;
    Splitter(false, 2.5f, &size1, &size2, 60, 160, propWindowWidth);
    mPropWindowHSplitRatio = size1 / (g.IO.DisplaySize.y - mToolbarHeight - mFooterHeight);

    ImGui::BeginChild("ScrollingRegion1", ImVec2(propWindowWidth - g.Style.WindowPadding.x, size1));

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    
    auto* topImage = getTopImage();
    if (ImGui::CollapsingHeader("Histogram") && topImage && mSupportComputeShader) {
        ScopeMarker("Draw Histogram");
        glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
        glBindTexture(GL_TEXTURE_2D, mTexHistogram);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_INT, (void*)mHistogram.data());

        ImGui::SetNextItemWidth(propWindowWidth - ImGui::GetStyle().ItemSpacing.x);
        const char* names[3] = {"R", "G", "B"};
        static ImColor colors[3] = { ImColor(255, 0, 0, 150), ImColor(0, 255, 0, 150), ImColor(0, 0, 255, 150) };
        const int binSize = 256;
        static float arr[binSize * 3];

        // Get the first 25-th largest bin counts and use it as the normalize factor.
        std::vector<int> temp;
        temp.assign(mHistogram.begin(), mHistogram.end());
        std::nth_element(temp.begin(), temp.end() - 25, temp.end());
        int normPixelCount =*(temp.end() - 25);

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, Vec2f(3.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(69, 69, 69, 255));
        ImGui::PushFont(mSmallFont);
        ImGui::PlotMultiHistograms("##histogram", 3, names, colors, mHistogram.data(), normPixelCount, binSize, 0.0f, 1.0f, ImVec2(0, 100));
        ImGui::PopFont();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(1);
    }

    //if (ImGui::CollapsingHeader("Scope")) {
    //}

    //if (ImGui::CollapsingHeader("Wavefront")) {
    //}

    if (ImGui::CollapsingHeader("Image Properties", ImGuiTreeNodeFlags_DefaultOpen)) {
        showImageProperties();
    }

    ImGui::PopStyleVar(1);
    ImGui::EndChild();

    const float spacerHeight = 4.0f;
    Vec2f buttonSize(20.0f);
    const float toolbarHeight = buttonSize.y + g.Style.FramePadding.y * 2.0f + g.Style.ItemSpacing.y;
    const float titleHeight = ImGui::GetFrameHeightWithSpacing() + spacerHeight;
    ImGui::Dummy(Vec2f(0.0f, spacerHeight));
    ImGui::Text("  " ICON_FA_LAYER_GROUP "  %s", mVideoMode ? "Media" : "Images");

    ImGui::BeginChild("ScrollingRegion2", ImVec2(propWindowWidth - g.Style.WindowPadding.x, size2 - titleHeight - toolbarHeight), true);
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, Vec2f(0.0f, 0.5f));

    auto basename = [](const std::string& p) -> const char* {
        if (p.empty()) {
            return "";
        }
        size_t i = p.find_last_of("/\\");
        return (i == std::string::npos) ? p.c_str() : p.c_str() + i + 1;
    };

    const int bufSize = 64;
    const int maxFilenameLength = bufSize - 10; // Reserve 10 chars for icon and spaces
    char buf[bufSize];
    const int imageNum = static_cast<int>(mImageList.size());
    const float imageListWindowHeight = ImGui::GetWindowHeight() - g.Style.ItemInnerSpacing.y * 2.0f;
    const float imageItemHeight = 24.0f;
    const bool enableCompareView = inCompareMode();

    static const Vec4f activeBorderColor(1.0f, 1.0f, 1.0f, 1.0f);
    static const Vec4f borderColor(1.0f, 1.0f, 1.0f, 0.5f);
    static bool isDraggingImageItem = false;
    static std::unique_ptr<Action> moveAction;

    if (mVideoMode) {
        const ImVec4 tintL(0.25f, 0.80f, 0.75f, 1.0f);
        const ImVec4 tintR(0.95f, 0.55f, 0.20f, 1.0f);
        // Make video entries clickable. "L/R" tags reflect the PRESENTED left/right when
        // mVideoSwapPresentationLR is enabled (presentation-only swap for instant switching).
#if defined(USE_VIDEO)
        const int nMedia = static_cast<int>(mVideoPaths.size());
        const bool dispSwap = mVideoSwapPresentationLR && videoCompareActive();
        int& idxDispL = dispSwap ? mVideoIndexR : mVideoIndexL;
        int& idxDispR = dispSwap ? mVideoIndexL : mVideoIndexR;

        for (int i = 0; i < nMedia; ++i) {
            const bool isL = (i == idxDispL);
            const bool isR = (i == idxDispR);
            const ImVec4 tint = isL ? tintL : (isR ? tintR : ImGui::GetStyleColorVec4(ImGuiCol_Text));

            ImGui::PushID(i);
            char row[160];
            const char* tag = isL ? "L" : (isR ? "R" : " ");
            std::snprintf(row, sizeof row, "%s  %s", tag, basename(mVideoPaths[i]));
            ImGui::PushStyleColor(ImGuiCol_Text, tint);
            ImGui::Selectable(row, false, 0, Vec2f(propWindowWidth, imageItemHeight));
            const bool lmb = ImGui::IsItemClicked(0);
            const bool rmb = ImGui::IsItemClicked(1);
            ImGui::PopStyleColor(1);
            ImGui::PopID();

            if (lmb) {
                if (i == idxDispL) {
                    continue; // no-op
                }
                // LMB sets the PRESENTED-left selection.
                idxDispL = i;
                if (idxDispR == idxDispL) {
                    if (nMedia >= 2) {
                        idxDispR = (i == 0) ? 1 : 0;
                    } else {
                        idxDispR = -1;
                    }
                }
                openVideosFromSelection();
            } else if (rmb) {
                if (i == idxDispR) {
                    continue; // no-op
                }
                // RMB sets the PRESENTED-right selection.
                idxDispR = i;
                if (idxDispR == idxDispL) {
                    if (nMedia >= 2) {
                        idxDispL = (i == 0) ? 1 : 0;
                    } else {
                        idxDispL = -1;
                    }
                }
                openVideosFromSelection();
            }
        }
#else
        (void)tintL;
        (void)tintR;
        if (!mVideoPathL.empty()) { ImGui::TextUnformatted(basename(mVideoPathL)); }
        if (!mVideoPathR.empty()) { ImGui::TextUnformatted(basename(mVideoPathR)); }
#endif
    } else {
        for (int i = 0; i < imageNum; i++) {
            std::string filename = mImageList[i]->filename();
            void* addr = mImageList[i].get();
            const auto filenameLength = filename.size();
            if (filenameLength > maxFilenameLength) {
                filename = filename.substr(filenameLength - maxFilenameLength);
                snprintf(buf, bufSize, "        ...%s##%p", filename.c_str(), addr);
            } else {
                snprintf(buf, bufSize, "           %s##%p", filename.c_str(), addr);
            }

            if (ImGui::Selectable(buf, mTopImageIndex == i && !isDraggingImageItem, 0, Vec2f(propWindowWidth, imageItemHeight))) {
                if (isDraggingImageItem) {
                    isDraggingImageItem = false;
                    if (moveAction) {
                        if (moveAction->prevTopImageIdx != mTopImageIndex ||
                            moveAction->prevCmpImageIdx != mCmpImageIndex) {
                            appendAction(std::move(*moveAction));
                        }
                    }

                    moveAction.reset();
                } else {
                    mTopImageIndex = i;
                    if (mCmpImageIndex == i) {
                        mCmpImageIndex = (mCmpImageIndex + 1) % imageNum;
                    }
                }
            }

            // Right click mouse to set compared imaeg directly.
            if (ImGui::IsItemClicked(1)) {
                if (mTopImageIndex == i) {
                    mTopImageIndex = mCmpImageIndex;
                }
                mCmpImageIndex = i;
            }

            if (!isDraggingImageItem && ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            } else if (isDraggingImageItem) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            }

            if (ImGui::IsItemActive() && !ImGui::IsItemHovered()) {
                isDraggingImageItem = true;
                if (!moveAction) {
                    moveAction = std::make_unique<Action>();
                    moveAction->type = Action::Type::Move;
                    for (uint8_t i = 0; i < static_cast<uint8_t>(mImageList.size()); i++) {
                        moveAction->imageIdxArray.push_back(Action::composeImageIndex(mImageList[i]->id(), i));
                    }
                    moveAction->prevTopImageIdx = mTopImageIndex;
                    moveAction->prevCmpImageIdx = mCmpImageIndex;
                }

                const int nextItemIdx = i + (ImGui::GetMouseDragDelta(0).y < 0.0f ? -1 : 1);
                if (nextItemIdx >= 0 && nextItemIdx < imageNum) {
                    std::swap(mImageList[i], mImageList[nextItemIdx]);
                    ImGui::ResetMouseDragDelta();

                    if (nextItemIdx == mTopImageIndex) {
                        mTopImageIndex = i;
                        if (mCmpImageIndex == i) {
                            mCmpImageIndex = nextItemIdx;
                        }
                    } else if (nextItemIdx == mCmpImageIndex) {
                        mCmpImageIndex = i;
                        if (mTopImageIndex == i) {
                            mTopImageIndex = nextItemIdx;
                        }
                    } else {
                        if (mTopImageIndex == i) {
                            mTopImageIndex = nextItemIdx;
                        } else if (mCmpImageIndex == i) {
                            mCmpImageIndex = nextItemIdx;
                        }
                    }
                }
            }

            const GLuint texId = mImageList[i]->texId();
            if (texId != 0) {
                ImGui::SameLine(g.Style.ItemSpacing.x);
                ImGui::Image((void*)(intptr_t)texId, Vec2f(28.0f, 22.0f), Vec2f(0.0f, 0.0f), Vec2f(1.0f, 1.0f),
                    Vec4f(1.0f), mTopImageIndex == i ? activeBorderColor : borderColor);
            }

            if ((i == mCmpImageIndex || i == mTopImageIndex) && enableCompareView) {
                bool hasScrollBar = imageListWindowHeight < (imageNum * (imageItemHeight + g.Style.ItemSpacing.y) + g.Style.ItemSpacing.y);
                float scrollBarSpace = hasScrollBar ? g.Style.ScrollbarSize : 0.0f;
                
                ImGui::SameLine(propWindowWidth - g.FontSize - g.Style.ItemSpacing.x * 2.0f - scrollBarSpace);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(i == mCmpImageIndex ? ICON_FA_ANGLE_RIGHT : ICON_FA_ANGLE_LEFT);
            }
        }
    }
    ImGui::PopStyleVar(1);
    ImGui::EndChild();

    const int buttonNum = 4;
    ImGui::PushFont(mSmallIconFont);
    ImGui::Dummy(Vec2f(propWindowWidth - (buttonSize.x + g.Style.ItemSpacing.x) * buttonNum - handleWidth, buttonSize.y));
    ImGui::SameLine();
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, g.Style.Colors[ImGuiCol_MenuBarBg]);

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FILE_IMPORT "##ImportImage", buttonSize)) {
        showImportImageDlg();
    }
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Import Images"); }

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_SYNC_ALT "##ReloadImage", buttonSize) && mTopImageIndex > -1) {
        mImageList[mTopImageIndex]->reload();
    }
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Reload Selected Image"); }

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_MINUS_CIRCLE "##RemoveImage", buttonSize)) {
        if (ImGui::GetIO().KeyShift) {
            removeTopImage(true);
        } else {
            ImGui::OpenPopup(kImageRemoveDlgTitle);
        }
    }
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Close Selected Image"); }
    if (showRemoveImageDlg(kImageRemoveDlgTitle)) { removeTopImage(true); }

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_TRASH_ALT "##ClearImages", buttonSize)) {
        if (ImGui::GetIO().KeyShift) {
            clearImages(true);
        } else if (!mImageList.empty()) {
            ImGui::OpenPopup(kClearImagesDlgTitle);
        }
    }
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Close All Images"); }
    if (showClearImagesDlg(kClearImagesDlgTitle)) { clearImages(true); }

    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::PopFont();

    if (checkPopupRect) {
        mPopupImagePropWindow = ImGui::IsWindowHovered();
    }
    ImGui::End();
}


void App::initFooter()
{
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGui::PushFont(mSmallFont);

    //g.NextWindowData.MenuBarOffsetMinVal = ImVec2(g.Style.DisplaySafeAreaPadding.x, ImMax(g.Style.DisplaySafeAreaPadding.y - g.Style.FramePadding.y, 0.0f));
    ImGui::SetNextWindowPos(ImVec2(0.0f, g.IO.DisplaySize.y - mFooterHeight));
    ImGui::SetNextWindowSize(ImVec2(g.IO.DisplaySize.x, mFooterHeight));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, g.Style.FramePadding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("##MainFooter", NULL, window_flags);
    ImGui::PopStyleVar(4);

    if (mImageScale > 1.0f) {
        sprintf_s(mImageScaleInfo, IM_ARRAYSIZE(mImageScaleInfo), "%.1fx", mImageScale);
    } else {
        sprintf_s(mImageScaleInfo, IM_ARRAYSIZE(mImageScaleInfo), "%.2f%%\n", mImageScale * 100.0f);
    }

    ImGui::Text("%s", mImageScaleInfo);

    const bool compareUi = inCompareMode() && mTopImageIndex >= 0 && mCmpImageIndex >= 0;

    if (compareUi) {
        const Image* leftIm = mImageList[mTopImageIndex].get();
        const Image* rightIm = mImageList[mCmpImageIndex].get();
        const Vec2f leftSz = leftIm->size();
        const Vec2f rightSz = rightIm->size();

        ImGui::SameLine(g.Style.FramePadding.x + g.FontSize * 4.0f);
        ImGui::Text("L | %.0f x %.0f", leftSz.x, leftSz.y);
        Vec2f lcoords;
        if (getCompareSideImagePixelAtMouse(Vec2f(g.IO.MousePos.x, g.IO.MousePos.y), 0, lcoords)) {
            ImGui::SameLine();
            ImGui::Text("(%.0f, %.0f)", lcoords.x, leftSz.y - lcoords.y - 1.0f);
        }

        char rbuf[128];
        Vec2f rcoords;
        if (getCompareSideImagePixelAtMouse(Vec2f(g.IO.MousePos.x, g.IO.MousePos.y), 1, rcoords)) {
            sprintf_s(rbuf, "R | %.0f x %.0f (%.0f, %.0f)", rightSz.x, rightSz.y, rcoords.x, rightSz.y - rcoords.y - 1.0f);
        } else {
            sprintf_s(rbuf, "R | %.0f x %.0f", rightSz.x, rightSz.y);
        }
        const float rtextW = ImGui::CalcTextSize(rbuf).x;
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - rtextW);
        ImGui::TextUnformatted(rbuf);
    } else if (mTopImageIndex >= 0) {
        const Vec2f& imageSize = mImageList[mTopImageIndex]->size();
        ImGui::SameLine(g.Style.FramePadding.x + g.FontSize * 4.0f);
        ImGui::Text("| %.0f x %.0f", imageSize.x, imageSize.y);

        Vec2f imageCoords;
        if (getImageCoordinates(g.IO.MousePos, imageCoords)) {
            ImGui::SameLine();
            imageCoords.y = imageSize.y - imageCoords.y - 1;
            ImGui::Text("(%.0f, %.0f)", imageCoords.x, imageCoords.y);
        }
    }

    const auto* topImage = getTopImage();
    if (topImage && !inCompareMode()) {
        const std::string& filename = topImage->filename();
        ImGui::SameLine(g.IO.DisplaySize.x - ImGui::CalcTextSize(filename.c_str()).x - g.Style.FramePadding.x);
        ImGui::TextUnformatted(filename.c_str());
    }

    ImGui::End();
    ImGui::PopFont();
}

void    App::showImageProperties()
{
    auto* topImage = getTopImage();
    if (!topImage) {
        return;
    }

    ImGui::Columns(2, nullptr, false);
    ImGui::SetColumnWidth(0, 100.0f);

    ImGui::Text("Attrbute");
    ImGui::NextColumn();

    ImGui::Text("Value");
    ImGui::NextColumn();

    ImGui::Separator();
    ImGui::Text("Color Primaries");
    ImGui::Text("Color Encoding");
    ImGui::Text("Location");
    ImGui::Text("Resolution");
    //ImGui::Text("Bit Depth");
    ImGui::NextColumn();

    static const ColorPrimaryType colorPrimaryTypes[] = {
        ColorPrimaryType::BT_709,
        ColorPrimaryType::DCI_P3_D65,
        ColorPrimaryType::BT_2020,
        ColorPrimaryType::ACES_AP1,
    };

    ImGui::TextUnformatted(getPropertyLabel(topImage->getColorPrimaryType()));
    if (ImGui::BeginPopupContextItem("ColorPrimaryMenu")) {
        for (auto type : colorPrimaryTypes) {
            const char* label = getPropertyLabel(type);
            if (ImGui::Selectable(label)) {
                topImage->setColorPrimaryType(type);
            }
        }
        ImGui::EndPopup();
    }

    static const ColorEncodingType colorEncodingTypes[] = {
        ColorEncodingType::Linear,
        //ColorEncodingType::BT_2020,
        //ColorEncodingType::BT_2100_HLG,
        ColorEncodingType::BT_2100_PQ,
        ColorEncodingType::BT_709,
        ColorEncodingType::sRGB
    };

    ImGui::TextUnformatted(getPropertyLabel(topImage->getColorEncodingType()));
    if (ImGui::BeginPopupContextItem("ColorEncodingMenu")) {
        for (auto type : colorEncodingTypes) {
            const char* label = getPropertyLabel(type);
            if (ImGui::Selectable(label)) {
                topImage->setColorEncodingType(type);
            }
        }
        ImGui::EndPopup();
    }

    ImGui::TextUnformatted(topImage->filepath().c_str());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", topImage->filepath().c_str());
    }


    auto imageSize = topImage->size();
    ImGui::Text("%.0fx%.0f", imageSize.x, imageSize.y);
    //ImGui::Text("8 bit");
    ImGui::NextColumn();
}

void    App::initHomeWindow(const char* name)
{
    bool open = true;
    
    ImGui::SetNextWindowContentWidth(500.0f);
    if (ImGui::BeginPopupModal(name, &open, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        //ImGui::Dummy(Vec2f(550.0f, ImGui::GetFrameHeight()));
        if (ImGui::BeginTabBar("ControlsBar", ImGuiTabBarFlags_None)) {
            if (ImGui::BeginTabItem("Controls")) {
                ImGui::Columns(2, nullptr, false);

                ImGui::Text("Show Home Panel");
                ImGui::Text("Toggle Property Window");
                ImGui::Text("Toggle Split View");
                ImGui::Text("Toggle Side-by-Side Column View");
                ImGui::Text("Toggle Pixel Warning Markers");
                ImGui::Text("Toggle Linear Image Filter");
                ImGui::NextColumn();

                ImGui::Text("F1");
                ImGui::Text("Tab");
                ImGui::Text("S");
                ImGui::Text("C");
                ImGui::Text("W");
                ImGui::Text("Q");
                ImGui::NextColumn();
                ImGui::Separator();

                ImGui::Text("Import Images");
                ImGui::Text("Export Session");
                ImGui::Text("Reload Selected Image");
                ImGui::Text("Close Selected Image");
                ImGui::Text("Close All Images");
                ImGui::NextColumn();

                ImGui::Text("Ctrl+O");
                ImGui::Text("Ctrl+E");
                ImGui::Text("F5");
                ImGui::Text("Backspace/Del");
                ImGui::Text("Ctrl+Shift+W");
                ImGui::NextColumn();

                ImGui::Separator();
                ImGui::Text("Pan");
                ImGui::Text("Offset Column View");
                ImGui::Text("Move compare split (Split / Column)");
                ImGui::Text("Zoom In/Out\n ");
                ImGui::Text("Zoom In/Out in Power-of-Two");
                ImGui::Text("Zoom to Actual Size");
                ImGui::Text("Fit to Viewport");
                ImGui::Text("Sync Column Views");
                ImGui::Text("Pixel Sniper");
                ImGui::NextColumn();

                ImGui::Text("Drag Middle Mouse Button");
                ImGui::Text("Drag Middle Mouse Button + Alt");
                ImGui::Text("Left click / drag on image");
                ImGui::Text("Mouse Scroll or\nDrag L+R Buttons Vertically\n(not in Split/Column compare)");
                ImGui::Text("+/-");
                ImGui::Text("/ or F");
                ImGui::Text("Shift+F");
                ImGui::Text("Alt+C");
                ImGui::Text("Holding Z");
                ImGui::NextColumn();

                ImGui::Separator();
                ImGui::Text("Next Compared Image");
                ImGui::Text("Previous Compared Image");
                ImGui::Text("Switch Compared Images");
                ImGui::NextColumn();

                ImGui::Text("D or Right Arrow");
                ImGui::Text("A or Left Arrow");
                ImGui::Text("X or Up Arrow");
#if defined(USE_VIDEO)
                ImGui::NextColumn();
                ImGui::Separator();
                ImGui::Text("Video: jump ±5 seconds");
                ImGui::NextColumn();
                ImGui::Text("Left / Right Arrow");
                ImGui::NextColumn();
                ImGui::Text("Video: frame step (, / .)");
                ImGui::NextColumn();
                ImGui::Text(",  .");
                ImGui::NextColumn();
                ImGui::Text("Video: next/prev in media list");
                ImGui::NextColumn();
                ImGui::Text("Ctrl+Shift+Left/Right\nor Ctrl+Shift+L/R, or A/D");
#endif
                //ImGui::NextColumn();

                ImGui::Columns(1);
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::Dummy(Vec2f(ImGui::GetFrameHeight()));
        
        if (ImGui::BeginTabBar("AboutBar", ImGuiTabBarFlags_None)) {
            if (ImGui::BeginTabItem("About")) {
                ImGui::TextWrapped("Bak-Tsiu %s, an image viewer designed for comparing images and examining pixel differences.", VERSION);

                ImGui::Text(u8"\nCopyright© Shih-Chin Weng. https://github.com/shihchinw/baktsiu");
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::EndPopup();
    }
}

bool    App::showRemoveImageDlg(const char* title)
{
    bool result = false;

    ImGui::SetNextWindowSizeConstraints(Vec2f(350.0f, 100.0f), Vec2f(400.0f, 250.0f));
    if (ImGui::BeginPopupModal(title, NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Close image %s?\n", getTopImage()->filepath().c_str());
        ImGui::Dummy(Vec2f(0.0f, ImGui::GetCurrentContext()->FontSize));
        ImGui::Separator();

        auto& style = ImGui::GetStyle();
        float buttonWidth = (ImGui::GetWindowWidth() - style.ItemSpacing.x) * 0.5f - style.FramePadding.x * 2.0f;
        if (ImGui::Button("OK", ImVec2(buttonWidth, 0))) { ImGui::CloseCurrentPopup(); result = true; }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    return result;
}

bool    App::showClearImagesDlg(const char* title)
{
    bool result = false;
    
    ImGui::SetNextWindowSizeConstraints(Vec2f(300.0f, 80.0f), Vec2f(300.0f, 120.0f));
    if (ImGui::BeginPopupModal(title, NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Close all images?\n");
        ImGui::Dummy(Vec2f(0.0f, ImGui::GetCurrentContext()->FontSize));
        ImGui::Separator();
        
        auto& style = ImGui::GetStyle();
        float buttonWidth = (ImGui::GetWindowWidth() - style.ItemSpacing.x) * 0.5f - style.FramePadding.x * 2.0f;
        if (ImGui::Button("OK", ImVec2(buttonWidth, 0))) { ImGui::CloseCurrentPopup(); result = true; }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0))) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    return result;
}

void    App::showImageNameOverlays()
{
    ImGuiIO& io = ImGui::GetIO();

    const float padding = 8.0f;
    auto basename = [](const std::string& p) -> const char* {
        if (p.empty()) {
            return "";
        }
        size_t i = p.find_last_of("/\\");
        return (i == std::string::npos) ? p.c_str() : p.c_str() + i + 1;
    };

    Vec2f windowPos(padding, mToolbarHeight + padding);
    ImGui::SetNextWindowPos(windowPos, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.5f);
    const ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##ImageOverlay1", nullptr, windowFlags)) {
        if (mVideoMode && !mVideoPathL.empty()) {
            const bool dispSwap = mVideoSwapPresentationLR && videoCompareActive();
            ImGui::TextUnformatted(basename(dispSwap ? mVideoPathR : mVideoPathL));
        } else if (mTopImageIndex >= 0) {
            ImGui::TextUnformatted(mImageList[mTopImageIndex]->filename().c_str());
        }
    }
    ImGui::End();

    if (mVideoMode) {
        const bool dispSwap = mVideoSwapPresentationLR && videoCompareActive();
        if ((dispSwap ? mVideoPathL : mVideoPathR).empty()) {
            return;
        }
    } else {
        if (mCmpImageIndex < 0) {
            return;
        }
    }

    ImGuiContext& g = *ImGui::GetCurrentContext();
    const float imagePropHandleWidth = g.FontSize + g.Style.FramePadding.x * 4.0f;
    const std::string& cmpImageName = mVideoMode ? mVideoPathR : mImageList[mCmpImageIndex]->filename();
    const bool dispSwap = mVideoSwapPresentationLR && videoCompareActive();
    const std::string& shownPath = mVideoMode ? (dispSwap ? mVideoPathL : mVideoPathR) : cmpImageName;
    const char* shownName = mVideoMode ? basename(shownPath) : shownPath.c_str();
    windowPos.x = io.DisplaySize.x - padding - ImGui::CalcTextSize(shownName).x - imagePropHandleWidth;
    if (mShowImagePropWindow || mPopupImagePropWindow) {
        windowPos.x -= getPropWindowWidth();
    }

    ImGui::SetNextWindowPos(windowPos, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.5f);
    if (ImGui::Begin("##ImageOverlay2", nullptr, windowFlags)) {
        ImGui::TextUnformatted(shownName);
    }
    ImGui::End();
}

void App::showHeatRangeOverlay(const Vec2f& pos, float width)
{
    ImGuiContext& g = *ImGui::GetCurrentContext();

    const ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDecoration 
        | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings 
        | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

    Vec2f barSize(width, 12.0f);

    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(Vec2f(0, 20.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Vec2f(8.0f, 4.0f));
    ImGui::PushFont(mSmallFont);
    //ImGui::SetNextWindowBgAlpha(0.5f);
    if (ImGui::Begin("##ImageOverlayBar", nullptr, windowFlags)) {
        ImGui::Text("low");
        ImGui::SameLine();
        ImGui::Dummy(barSize);

        ImU32 hues[16 + 1];
        for (size_t i = 0; i <= 16; i++) {
            float r = i / 16.0f;
            float g = glm::sin(180.0f * glm::radians(r));
            float b = glm::cos(60.0f * glm::radians(r));
            hues[i] = IM_COL32(r * 255, g * 255, b * 255, 255);
        }

        Vec2f rampBarPadding = g.Style.WindowPadding + pos;
        rampBarPadding.x += ImGui::CalcTextSize("low").x + g.Style.ItemSpacing.x;

        ImDrawList* drawList = ImGui::GetCurrentWindow()->DrawList;
        for (int i = 0; i < 16; ++i) {
            const auto& startHue = hues[i];
            const auto& endHue = hues[i + 1];
            drawList->AddRectFilledMultiColor(
                Vec2f(barSize.x * i / 16.0, 0.0f) + rampBarPadding, 
                Vec2f(barSize.x * (i + 1) / 16.0, barSize.y) + rampBarPadding, 
                startHue, endHue, endHue, startHue);
        }

        ImGui::SameLine();
        ImGui::Text("high");
    }
    ImGui::End();
    ImGui::PopFont();
    ImGui::PopStyleVar(1);
}

//---------------------------------------------------------------------------------------
//---------------------------------------------------------------------------------------

void App::appendAction(Action&& action)
{
    mActionStack.push_back(action);
    if (mActionStack.size() > 16) {
        // Only keep the latest 16 actions.
        mActionStack.pop_front();
    }
}

void App::undoAction()
{
    if (mActionStack.empty()) {
        return;
    } else if (mCurAction.type != Action::Type::Unknown) {
        // Skip instruction if there is an undo action running currently.
        return;
    }

    Action action = mActionStack.back();
    mActionStack.pop_back();

    if (action.type == Action::Type::Remove) {
        mCurAction = action;
        importImageFiles(action.filepathArray, false, &action.imageIdxArray);
    } else if (action.type == Action::Type::Add) {
        const int imageCount = static_cast<int>(mImageList.size());
        for (int i = imageCount - 1; i >= 0; --i) {
            for (auto index : action.imageIdxArray) {
                uint8_t layerIndex = Action::extractLayerIndex(index);
                if (layerIndex == i) {
                    mImageList.erase(mImageList.begin() + layerIndex);
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
        std::vector<ImageUPtr> newImageList;
        newImageList.reserve(mImageList.size());

        for (auto index : action.imageIdxArray) {
            uint8_t id = Action::extractImageId(index);
            for (auto& image : mImageList) {
                if (image && image->id() == id) {
                    newImageList.push_back(std::move(image));
                    break;
                }
            }
        }

        mImageList.swap(newImageList);
        mTopImageIndex = action.prevTopImageIdx;
        mCmpImageIndex = action.prevCmpImageIdx;
    }
}

void    App::showImportImageDlg()
{
    static std::vector<std::string> filters = {
        "Supported Image Files", "*.bmp; *.exr; *.gif; *.jpg; *.hdr; *.png; *.bts; *.mp4; *.mov; *.wmv; *.avi; *.mkv; *.webm; *.m4v" ,
        "BMP (*.BMP)", "*.bmp",
        "OpenEXR (*.EXR)", "*.exr",
        "GIF (*.GIF)", "*.gif",
        "JPEG (*.JPG, *.JPEG)", "*.jpg; *.jpeg",
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
        // Add all selected videos to the media list, then open current selection.
        for (const std::string& p : videoPaths) {
            const int idx = addVideoPath(p);
            if (idx >= 0) {
                if (mVideoIndexL < 0) {
                    mVideoIndexL = idx;
                } else if (mVideoIndexR < 0 && idx != mVideoIndexL) {
                    mVideoIndexR = idx;
                }
            }
        }
        openVideosFromSelection();
#else
        LOGW("Video playback is disabled in this build (USE_VIDEO).");
#endif
    } else if (!imagePaths.empty()) {
        importImageFiles(imagePaths, true);
    }
}

void    App::showExportSessionDlg()
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

// This function is executed in main thread.
void    App::importImageFiles(const std::vector<std::string>& filepathArray, 
                              bool recordAction, std::vector<uint16_t>* imageIdxArray)
{
    const size_t imageNum = filepathArray.size();

    if (imageNum == 0) {
        return;
    }

    Action action(Action::Type::Add, mTopImageIndex, mCmpImageIndex);

    for (size_t i = 0; i < imageNum; ++i) {
        std::string path = filepathArray[i];
        std::replace(path.begin(), path.end(), '\\', '/');

        if (!Texture::isSupported(path)) {
            LOGW("Unsupported image type for \"{}\"", path);
            continue;
        }
        
        uint8_t insertIdx = static_cast<uint8_t>(mImageList.size());
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
        auto newImage = std::make_unique<Image>(newTexture, id);
        const ImageType imageType = Texture::getImageType(path);
        if (imageType == ImageType::HDR || imageType == ImageType::OPENEXR) {
            newImage->setColorEncodingType(ColorEncodingType::Linear);
        }

        mImageList.insert(mImageList.begin() + insertIdx, std::move(newImage));

        action.filepathArray.push_back(path);
        action.imageIdxArray.push_back(Action::composeImageIndex(id, insertIdx));
        mUpdateImageSelection = true;
    }
    
    if (recordAction) {
        appendAction(std::move(action));
    }
}

Image* App::getTopImage()
{
    return mTopImageIndex >= 0 ? mImageList[mTopImageIndex].get() : nullptr;
}

void    App::removeTopImage(bool recordAction)
{
    ImageUPtr image = std::move(mImageList[mTopImageIndex]);
    mImageList.erase(mImageList.begin() + mTopImageIndex);

    if (recordAction) {
        Action action(Action::Type::Remove, mTopImageIndex, mCmpImageIndex);
        action.filepathArray.push_back(image->filepath());
        action.imageIdxArray.push_back(Action::composeImageIndex(image->id(), mTopImageIndex));
        appendAction(std::move(action));
    }

    image.reset();
    mTexturePool.cleanUnusedTextures();
    
    const int imageNum = static_cast<int>(mImageList.size());
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

void    App::clearImages(bool recordAction)
{
    if (recordAction) {
        Action action(Action::Type::Remove, mTopImageIndex, mCmpImageIndex);

        for (int index = 0; index < mImageList.size(); ++index) {
            action.filepathArray.push_back(mImageList[index]->filepath());
            action.imageIdxArray.push_back(Action::composeImageIndex(mImageList[index]->id(), index));
        }

        mActionStack.push_back(action);
    }

    mImageList.clear();
    mTopImageIndex = mCmpImageIndex = -1;
    mCompositeFlags = CompositeFlags::Top;
}

void    App::resetImageTransform(const Vec2f &imgSize, bool fitWindow)
{
    mImageScale = 1.0f;

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

    const Image* topIm = mImageList[mTopImageIndex].get();
    const Image* cmpIm = mImageList[mCmpImageIndex].get();
    if (!topIm || !cmpIm) {
        return false;
    }

    const Vec2f topSz = topIm->size();
    const Vec2f cmpSz = cmpIm->size();
    const Vec2f refSz((std::max)(topSz.x, cmpSz.x), (std::max)(topSz.y, cmpSz.y));

    const Image* sideIm = (side == 0) ? topIm : cmpIm;
    const Vec2f actual = sideIm->size();
    const float sc = (std::min)(refSz.x / (std::max)(actual.x, 1e-6f), refSz.y / (std::max)(actual.y, 1e-6f));
    const Vec2f disp(actual.x * sc, actual.y * sc);
    const Vec2f ox = (refSz - disp) * 0.5f;
    const Vec2f uv((refPx.x - ox.x) / (std::max)(disp.x, 1e-6f), (refPx.y - ox.y) / (std::max)(disp.y, 1e-6f));

    if (uv.x < -1e-4f || uv.y < -1e-4f || uv.x > 1.0f + 1e-4f || uv.y > 1.0f + 1e-4f) {
        return false;
    }

    outImagePx = glm::floor(uv * actual);
    outImagePx.x = glm::clamp(outImagePx.x, 0.0f, glm::max(actual.x - 1.0f, 0.0f));
    outImagePx.y = glm::clamp(outImagePx.y, 0.0f, glm::max(actual.y - 1.0f, 0.0f));

    return true;
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
    if (inCompareMode()) {
        flags &= PixelMarkerFlags::DiffMask;  // Only keeps pixel differnce marker.
    } else {
        flags &= ~PixelMarkerFlags::DiffMask; // Clear any flags for difference modes.
    }

    return static_cast<int>(flags);
}

inline void    App::toggleSplitView()
{
    if (mCmpImageIndex != -1) {
        mCompositeFlags = toggleFlags(mCompositeFlags & ~CompositeFlags::SideBySide, CompositeFlags::Split);
    }
}

inline void    App::toggleSideBySideView()
{
    if (mCmpImageIndex != -1) {
        mCompositeFlags = toggleFlags(mCompositeFlags & ~CompositeFlags::Split, CompositeFlags::SideBySide);
    }
}

inline bool    App::inCompareMode() const
{
    return mCmpImageIndex != -1 && (mCompositeFlags & (CompositeFlags::Split | CompositeFlags::SideBySide)) != CompositeFlags::Top;
}

inline bool    App::inSideBySideMode() const
{
    return (mCompositeFlags & CompositeFlags::SideBySide) != CompositeFlags::Top;
}

inline bool    App::shouldShowSplitter() const
{
    return inCompareMode() && (getPixelMarkerFlags() & static_cast<int>(PixelMarkerFlags::DiffMask)) == 0;
}

void    App::onFileDrop(int count, const char* filepaths[])
{
    std::vector<std::string> filepathArray;
    filepathArray.reserve(count);
    std::vector<std::string> videoPaths;

    for (int i = 0; i < count; ++i) {
        const std::string &filepath = filepaths[i];

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
        for (const std::string& p : videoPaths) {
            const int idx = addVideoPath(p);
            if (idx >= 0) {
                if (mVideoIndexL < 0) {
                    mVideoIndexL = idx;
                } else if (mVideoIndexR < 0 && idx != mVideoIndexL) {
                    mVideoIndexR = idx;
                }
            }
        }
        openVideosFromSelection();
#else
        LOGW("Video playback is disabled in this build (USE_VIDEO).");
#endif
    } else if (!filepathArray.empty()) {
        importImageFiles(filepathArray, true);
    }
}

void    App::openSession(const std::string& filepath)
{
    fx::gltf::Document sessionFile = fx::gltf::LoadFromText(filepath);
    if ("baktsiu" != sessionFile.asset.generator) {
        LOGW("Input is not a valid Bak-Tsiu session file");
        return;
    }

    std::vector<std::string> filepathArray;
    filepathArray.reserve(sessionFile.images.size());
    
    for (auto &imageProp : sessionFile.images) {
        filepathArray.push_back(imageProp.uri);
    }

    importImageFiles(filepathArray, true);
}

void    App::saveSession(const std::string& filepath)
{
    fx::gltf::Document sessionFile;
    sessionFile.asset.generator = "baktsiu";

    for (auto &image : mImageList) {
        fx::gltf::Image imageProp;
        imageProp.uri = image->filepath();
        sessionFile.images.push_back(imageProp);
    }

    fx::gltf::Save(sessionFile, filepath, false);
}

namespace
{
constexpr double kVideoScrubUnknownMaxSec = 3600.0 * 24.0;  // 24h upper composition bound when duration unknown
// Small forward frame steps: prefer keyframe-style seek below this delta (see seek(..., false)).
constexpr double kVideoForwardKeyframeSeekMaxSec = 1.35;
// Cap real-time delta fed into playback clocks so a blocking open / hitch does not schedule many decodes in one UI frame.
constexpr double kVideoPlaybackWallDtCapSec = 0.25;
constexpr int kVideoMaxCompareStepsPerTick = 8;

#if defined(BAKTSIU_DEBUG_BUILD)
inline bool videoPipelineDbgEnabled(bool userFlag)
{
    return userFlag;
}
#else
inline bool videoPipelineDbgEnabled(bool)
{
    return false;
}
#endif

double videoScrubSpanSec(const MpvGlPlayer* r)
{
    if (!r || !r->isOpen()) {
        return 0.0;
    }
    const double d = r->durationSec();
    if (r->hasReliableDuration() && d > 0.001) {
        return d;
    }
    return kVideoScrubUnknownMaxSec;
}

double mediaTimeFromComposition(double T, double startS, double durationSec)
{
    double t = T - startS;
    if (durationSec > 1e-6) {
        if (t < 0.0) {
            t = 0.0;
        } else if (t > durationSec) {
            t = durationSec;
        }
    } else {
        t = 0.0;
    }
    return t;
}

// Skip redundant seek/decode when scrubbing if target media time barely changed (compare mode: one
// stream often stays clamped while the other moves).
double mediaSyncEpsilonSec(const MpvGlPlayer* r)
{
    if (!r || !r->isOpen()) {
        return 1e-4;
    }
    double fd = r->frameDurationSec();
    if (fd < 1e-6 || fd > 1.0) {
        fd = 1.0 / 30.0;
    }
    return (std::max)(1e-4, fd * 0.25);
}

double saneFrameDurSec(const MpvGlPlayer* r)
{
    double fd = (r && r->isOpen()) ? r->frameDurationSec() : 0.0;
    if (fd < 1e-6 || fd > 1.0) {
        fd = 1.0 / 30.0;
    }
    return fd;
}

struct VideoScrubDecodeHint {
    bool run = false;
    // 0 = exact seek + full render; >0 = keyframe-style seek (scrub preview only).
    int maxReadCapPerStream = 0;
};

// While dragging: throttle + use keyframe seeks between previews; on grab, release, or click: exact seek.
VideoScrubDecodeHint videoScrubDecodeHint(bool itemActive, bool itemActivated, bool itemClicked, bool valueChanged,
    bool itemDeactivated, double imguiTimeSec, double& lastDecodeTimeSec)
{
    VideoScrubDecodeHint out;
    constexpr double kMinIntervalSec = 1.0 / 20.0;
    constexpr int kPreviewReadCap = 96;
    if (itemDeactivated || itemActivated || itemClicked) {
        lastDecodeTimeSec = imguiTimeSec;
        out.run = true;
        out.maxReadCapPerStream = 0;
        return out;
    }
    if (itemActive && valueChanged) {
        if (imguiTimeSec - lastDecodeTimeSec >= kMinIntervalSec) {
            lastDecodeTimeSec = imguiTimeSec;
            out.run = true;
            out.maxReadCapPerStream = kPreviewReadCap;
            return out;
        }
    }
    return out;
}
}  // namespace

bool App::videoCompareActive() const
{
#if !defined(USE_VIDEO)
    return false;
#else
    return mVideoReaderB != nullptr && mVideoReaderB->isOpen();
#endif
}

void App::recomputeVideoScrubBounds(double& outMin, double& outMax) const
{
#if !defined(USE_VIDEO)
    outMin = 0.0;
    outMax = 1.0;
#else
    if (videoCompareActive() && mVideoReader && mVideoReader->isOpen()) {
        const double spanL = videoScrubSpanSec(mVideoReader.get());
        const double spanR = videoScrubSpanSec(mVideoReaderB.get());
        outMin = (std::min)(mVideoStartL, mVideoStartR);
        outMax = (std::max)(mVideoStartL + spanL, mVideoStartR + spanR);
        if (outMax < outMin + 1e-3) {
            outMax = outMin + 1.0;
        }
    } else if (mVideoReader && mVideoReader->isOpen()) {
        outMin = 0.0;
        outMax = videoScrubSpanSec(mVideoReader.get());
    } else {
        outMin = 0.0;
        outMax = 1.0;
    }
#endif
}

void App::clampVideoCompositionT()
{
#if defined(USE_VIDEO)
    double tMin = 0.0;
    double tMax = 1.0;
    recomputeVideoScrubBounds(tMin, tMax);
    if (mVideoCompositionT < tMin) {
        mVideoCompositionT = tMin;
    }
    if (mVideoCompositionT > tMax) {
        mVideoCompositionT = tMax;
    }
#endif
}

void App::syncVideoDecodersToCompositionT(int maxDecodeReadCapPerStream)
{
#if !defined(USE_VIDEO)
    (void)maxDecodeReadCapPerStream;
    return;
#else
#if defined(BAKTSIU_DEBUG_BUILD)
    namespace chrono = std::chrono;
    const auto tSync0 = chrono::steady_clock::now();
#endif
    if (!mVideoReader || !mVideoReader->isOpen()) {
        return;
    }
    if (videoPipelineDbgEnabled(mVideoLogPipelineTiming)) {
        mVideoDbgLastUploadLMs = 0.f;
        mVideoDbgLastUploadRMs = 0.f;
    }
    bool ranDecodeL = false;
    bool ranDecodeR = false;
    const double dL = videoScrubSpanSec(mVideoReader.get());
    const double tL = mediaTimeFromComposition(mVideoCompositionT, mVideoStartL, dL);
    double tR = 0.0;
    bool hasR = false;
    const double epsL = mediaSyncEpsilonSec(mVideoReader.get());
    const bool needL = (mVideoLastSyncedTargetMediaL < 0.0)
        || (std::fabs(tL - mVideoLastSyncedTargetMediaL) > epsL);
    if (needL) {
        mVideoReader->seek(tL, maxDecodeReadCapPerStream <= 0);
        mVideoReader->decodeFrameThrough(tL, maxDecodeReadCapPerStream);
        uploadVideoTexture();
        ranDecodeL = true;
        // Preview decodes use a read cap and may not reach target; keep cache for skip-optimization
        // aligned with full-quality sync only.
        if (maxDecodeReadCapPerStream <= 0) {
            mVideoLastSyncedTargetMediaL = tL;
        }
    }
    if (videoCompareActive()) {
        const double dR = videoScrubSpanSec(mVideoReaderB.get());
        tR = mediaTimeFromComposition(mVideoCompositionT, mVideoStartR, dR);
        hasR = true;
        const double epsR = mediaSyncEpsilonSec(mVideoReaderB.get());
        const bool needR = (mVideoLastSyncedTargetMediaR < 0.0)
            || (std::fabs(tR - mVideoLastSyncedTargetMediaR) > epsR);
        if (needR) {
            mVideoReaderB->seek(tR, maxDecodeReadCapPerStream <= 0);
            mVideoReaderB->decodeFrameThrough(tR, maxDecodeReadCapPerStream);
            uploadVideoTextureB();
            ranDecodeR = true;
            if (maxDecodeReadCapPerStream <= 0) {
                mVideoLastSyncedTargetMediaR = tR;
            }
        }
    } else {
        mVideoLastSyncedTargetMediaR = -1.0;
    }
    if (videoPipelineDbgEnabled(mVideoLogPipelineTiming) && (ranDecodeL || ranDecodeR)) {
        if (ranDecodeL) {
            LOGI(
                "[video-pipe] readCap={} L: seek={:.2f}ms (flush={:.2f} setPos={:.2f}) decode+buf={:.2f}ms "
                "reads={} hitIterCap={} upload={:.2f}ms nv12Out={}",
                maxDecodeReadCapPerStream, static_cast<double>(mVideoReader->lastSeekMs()),
                static_cast<double>(mVideoReader->lastSeekFlushMs()),
                static_cast<double>(mVideoReader->lastSeekSetPosMs()),
                static_cast<double>(mVideoReader->lastDecodeThroughMs()),
                mVideoReader->lastDecodeThroughReads(), mVideoReader->lastDecodeThroughHitCap() ? 1 : 0,
                static_cast<double>(mVideoDbgLastUploadLMs), 0);
        }
        if (ranDecodeR && mVideoReaderB && mVideoReaderB->isOpen()) {
            LOGI(
                "[video-pipe] readCap={} R: seek={:.2f}ms (flush={:.2f} setPos={:.2f}) decode+buf={:.2f}ms "
                "reads={} hitIterCap={} upload={:.2f}ms nv12Out={}",
                maxDecodeReadCapPerStream, static_cast<double>(mVideoReaderB->lastSeekMs()),
                static_cast<double>(mVideoReaderB->lastSeekFlushMs()),
                static_cast<double>(mVideoReaderB->lastSeekSetPosMs()),
                static_cast<double>(mVideoReaderB->lastDecodeThroughMs()),
                mVideoReaderB->lastDecodeThroughReads(), mVideoReaderB->lastDecodeThroughHitCap() ? 1 : 0,
                static_cast<double>(mVideoDbgLastUploadRMs), 0);
        }
    }

#if defined(BAKTSIU_DEBUG_BUILD)
    // Post-sync debug: how far mpv's reported time-pos is from our target (ms).
    {
        const double posL = (mVideoReader && mVideoReader->isOpen()) ? mVideoReader->positionSec() : 0.0;
        mVideoDbgLastDriftLMs = static_cast<float>((posL - tL) * 1000.0);
        if (hasR && mVideoReaderB && mVideoReaderB->isOpen()) {
            const double posR = mVideoReaderB->positionSec();
            mVideoDbgLastDriftRMs = static_cast<float>((posR - tR) * 1000.0);
        } else {
            mVideoDbgLastDriftRMs = 0.f;
        }
    }
    mVideoDbgLastSyncMs =
        chrono::duration<float, std::milli>(chrono::steady_clock::now() - tSync0).count();
#endif
#endif
}

void App::resetVideoDecoderSyncCache()
{
    mVideoLastSyncedTargetMediaL = -1.0;
    mVideoLastSyncedTargetMediaR = -1.0;
    mVideoLastScrubDecodeTime = -1.0e9;
    mVideoComparePlaybackBank = 0.0;
}

void App::stepVideoScrubFiveSeconds(int direction)
{
#if !defined(USE_VIDEO)
    (void)direction;
    return;
#else
    if (!mVideoReader || !mVideoReader->isOpen() || direction == 0) {
        return;
    }
    constexpr double kJumpSec = 5.0;
    mVideoPlaying = false;
    mVideoPlaybackTimeBank = 0.0;
    mVideoComparePlaybackBank = 0.0;
    if (videoCompareActive()) {
        mVideoCompositionT += static_cast<double>(direction) * kJumpSec;
        clampVideoCompositionT();
        syncVideoDecodersToCompositionT();
        mVideoScrubValue = mVideoCompositionT;
        return;
    }
    const double pos = mVideoReader->positionSec();
    const double metaDur = mVideoReader->durationSec();
    double frameDur = mVideoReader->frameDurationSec();
    if (frameDur < 1e-6 || frameDur > 1.0) {
        frameDur = 1.0 / 30.0;
    }
    double tMax = videoScrubSpanSec(mVideoReader.get());
    if (mVideoReader->hasReliableDuration() && metaDur > 0.001) {
        const double endSlack = std::max(frameDur * 3.0, 0.1);
        tMax = std::max(0.0, metaDur - endSlack);
    }
    double newPos = pos + static_cast<double>(direction) * kJumpSec;
    if (newPos < 0.0) {
        newPos = 0.0;
    } else if (newPos > tMax) {
        newPos = tMax;
    }
    if (videoPipelineDbgEnabled(mVideoLogPipelineTiming)) {
        mVideoDbgLastUploadLMs = 0.f;
    }
    mVideoReader->seek(newPos);
    mVideoReader->decodeFrameThrough(newPos);
    uploadVideoTexture();
    if (videoPipelineDbgEnabled(mVideoLogPipelineTiming)) {
        LOGI(
            "[video-pipe] readCap={} L: seek={:.2f}ms (flush={:.2f} setPos={:.2f}) decode+buf={:.2f}ms "
            "reads={} hitIterCap={} upload={:.2f}ms nv12Out={}",
            0, static_cast<double>(mVideoReader->lastSeekMs()),
            static_cast<double>(mVideoReader->lastSeekFlushMs()),
            static_cast<double>(mVideoReader->lastSeekSetPosMs()),
            static_cast<double>(mVideoReader->lastDecodeThroughMs()),
            mVideoReader->lastDecodeThroughReads(), mVideoReader->lastDecodeThroughHitCap() ? 1 : 0,
            static_cast<double>(mVideoDbgLastUploadLMs), 0);
    }
    mVideoScrubValue = mVideoReader->positionSec();
#endif
}

void App::stepVideoScrubOneFrame(int direction)
{
#if !defined(USE_VIDEO)
    (void)direction;
    return;
#else
    if (!mVideoReader || !mVideoReader->isOpen() || direction == 0) {
        return;
    }
    mVideoPlaying = false;
    mVideoPlaybackTimeBank = 0.0;
    mVideoComparePlaybackBank = 0.0;
    if (videoCompareActive()) {
        double stepA = mVideoReader->frameDurationSec();
        double stepB =
            (mVideoReaderB && mVideoReaderB->isOpen()) ? mVideoReaderB->frameDurationSec() : stepA;
        if (stepA < 1e-6 || stepA > 1.0) {
            stepA = 1.0 / 30.0;
        }
        if (stepB < 1e-6 || stepB > 1.0) {
            stepB = 1.0 / 30.0;
        }
        const double step = (std::min)(stepA, stepB);
        mVideoCompositionT += static_cast<double>(direction) * step;
        clampVideoCompositionT();
        syncVideoDecodersToCompositionT();
        mVideoScrubValue = mVideoCompositionT;
        return;
    }
    double step = mVideoReader->frameDurationSec();
    if (step < 1e-6 || step > 1.0) {
        step = 1.0 / 30.0;
    }
    const double pos = mVideoReader->positionSec();
    const double metaDur = mVideoReader->durationSec();
    double tMax = videoScrubSpanSec(mVideoReader.get());
    if (mVideoReader->hasReliableDuration() && metaDur > 0.001) {
        const double endSlack = std::max(step * 3.0, 0.1);
        tMax = std::max(0.0, metaDur - endSlack);
    }
    double newPos = pos + static_cast<double>(direction) * step;
    if (newPos < 0.0) {
        newPos = 0.0;
    } else if (newPos > tMax) {
        newPos = tMax;
    }
    if (videoPipelineDbgEnabled(mVideoLogPipelineTiming)) {
        mVideoDbgLastUploadLMs = 0.f;
    }
    // Small forward steps: seek(..., false) uses keyframe-style seek in mpv; large/backward use exact seek.
    const double deltaSec = newPos - pos;
    const bool forwardCatchup =
        direction > 0 && deltaSec >= -1e-9 && deltaSec <= kVideoForwardKeyframeSeekMaxSec;
    mVideoReader->seek(newPos, !forwardCatchup);
    mVideoReader->decodeFrameThrough(newPos);
    uploadVideoTexture();
    if (videoPipelineDbgEnabled(mVideoLogPipelineTiming)) {
        LOGI(
            "[video-pipe] readCap={} L: seek={:.2f}ms (flush={:.2f} setPos={:.2f}) decode+buf={:.2f}ms "
            "reads={} hitIterCap={} upload={:.2f}ms nv12Out={}",
            0, static_cast<double>(mVideoReader->lastSeekMs()),
            static_cast<double>(mVideoReader->lastSeekFlushMs()),
            static_cast<double>(mVideoReader->lastSeekSetPosMs()),
            static_cast<double>(mVideoReader->lastDecodeThroughMs()),
            mVideoReader->lastDecodeThroughReads(), mVideoReader->lastDecodeThroughHitCap() ? 1 : 0,
            static_cast<double>(mVideoDbgLastUploadLMs), 0);
    }
    mVideoScrubValue = mVideoReader->positionSec();
#endif
}

namespace {
std::string normalizeVideoPath(std::string p)
{
    std::replace(p.begin(), p.end(), '\\', '/');
    return p;
}
}  // namespace

void App::swapVideoSides()
{
#if !defined(USE_VIDEO)
    return;
#else
    // Presentation-only swap for instant switching while playing.
    // IMPORTANT:
    // - We DO NOT swap decoders/readers/textures here.
    // - We only flip the interpretation of "Left/Right" at the UI + renderer level.
    // This avoids a costly seek/decode/upload hitch at the swap moment.
    if (!videoCompareActive()) {
        return;
    }
    mVideoSwapPresentationLR = !mVideoSwapPresentationLR;
#endif
}

int App::addVideoPath(const std::string& filepath)
{
#if !defined(USE_VIDEO)
    (void)filepath;
    return -1;
#else
    const std::string path = normalizeVideoPath(filepath);
    for (int i = 0; i < static_cast<int>(mVideoPaths.size()); ++i) {
        if (mVideoPaths[i] == path) {
            return i;
        }
    }
    mVideoPaths.push_back(path);
    return static_cast<int>(mVideoPaths.size()) - 1;
#endif
}

void App::openVideosFromSelection()
{
#if !defined(USE_VIDEO)
    return;
#else
    const int n = static_cast<int>(mVideoPaths.size());
    if (n <= 0 || mVideoIndexL < 0 || mVideoIndexL >= n) {
        return;
    }

    const int idxL = mVideoIndexL;
    int idxR = (mVideoIndexR >= 0 && mVideoIndexR < n && mVideoIndexR != idxL) ? mVideoIndexR : -1;
    // If exactly two media are loaded, auto-pick the other as right so compare/swap works
    // without requiring an explicit RMB selection.
    if (idxR < 0 && n == 2) {
        idxR = (idxL == 0) ? 1 : 0;
        mVideoIndexR = idxR;
    }
    const std::string pathL = mVideoPaths[idxL];
    const std::string pathR = (idxR >= 0) ? mVideoPaths[idxR] : std::string();

    // If the selected media are already open, don't reopen/recreate textures/reset offsets.
    // Just resync decoding to the current composition time.
    // Do not clear mVideoSwapPresentationLR here — storage indices match decoder paths but the
    // user may still want L/R flipped on screen.
    if (mVideoMode && mVideoReader && mVideoReader->isOpen()
        && mVideoPathL == pathL
        && ((idxR < 0 && mVideoPathR.empty()) || (idxR >= 0 && mVideoPathR == pathR))) {
        // Ensure compare decoder is actually open when we expect it.
        if (idxR < 0 || videoCompareActive()) {
            syncVideoDecodersToCompositionT();
            mVideoLazyDurationGraceFrames = 0;
            return;
        }
    }

    // Property-window clicks write mVideoIndexL/R in "storage" order, while L/R *labels* follow
    // presentation (mVideoSwapPresentationLR). Infer which path the user wants on screen-left vs
    // screen-right; if that pair is exactly the two files already open, only adjust presentation +
    // canonical list indices — no reopen.
    if (mVideoMode && videoCompareActive() && idxR >= 0
        && mVideoReader && mVideoReader->isOpen()
        && mVideoReaderB && mVideoReaderB->isOpen()) {
        const int pL = mVideoSwapPresentationLR ? mVideoIndexR : mVideoIndexL;
        const int pR = mVideoSwapPresentationLR ? mVideoIndexL : mVideoIndexR;
        if (pL >= 0 && pL < n && pR >= 0 && pR < n && pL != pR) {
            const std::string& wantLeft = mVideoPaths[pL];
            const std::string& wantRight = mVideoPaths[pR];
            const std::string& openA = mVideoPathL;
            const std::string& openB = mVideoPathR;
            const bool samePair =
                (wantLeft == openA && wantRight == openB) || (wantLeft == openB && wantRight == openA);
            if (samePair) {
                // Keep list indices in sync with what the user actually picked (pL / pR), not
                // findIdx(openA) which always returns the first matching row (wrong if the same path
                // appears twice). Decoder slot 0 is always openA, slot 1 is openB — map rows to slots:
                // natural screen L=A,R=B => storage (pL,pR); swapped L=B,R=A => storage (pR,pL).
                if (wantLeft == openA && wantRight == openB) {
                    mVideoIndexL = pL;
                    mVideoIndexR = pR;
                    mVideoSwapPresentationLR = false;
                } else {
                    mVideoIndexL = pR;
                    mVideoIndexR = pL;
                    mVideoSwapPresentationLR = true;
                }
                syncVideoDecodersToCompositionT();
                mVideoLazyDurationGraceFrames = 0;
                return;
            }
        }
    }

    if (idxR >= 0) {
        LOGI("[video-open] selection n={} idxL={} idxR={} (dual/compare) L=\"{}\" R=\"{}\"", n, idxL, idxR, pathL, pathR);
    } else {
        LOGI("[video-open] selection n={} idxL={} idxR=-1 (single) L=\"{}\"", n, idxL, pathL);
    }

    auto readerL = std::make_unique<MpvGlPlayer>();
    std::unique_ptr<MpvGlPlayer> readerR;
    bool openedL = false;
    bool openedR = false;

    if (idxR >= 0) {
        readerR = std::make_unique<MpvGlPlayer>();
        openedL = readerL->open(mWindow, pathL);
        openedR = readerR->open(mWindow, pathR);
        if (!openedL) {
            LOGW("Failed to open video \"{}\"", pathL);
            if (openedR) {
                readerR->close();
            }
            return;
        }
        if (!openedR) {
            LOGW("Failed to open video \"{}\"", pathR);
            readerL->close();
            return;
        }
    } else {
        openedL = readerL->open(mWindow, pathL);
        if (!openedL) {
            LOGW("Failed to open video \"{}\"", pathL);
            return;
        }
    }

    // Tear down previous decode state/textures.
    if (mVideoReader) {
        mVideoReader->close();
        mVideoReader.reset();
    }
    if (mVideoReaderB) {
        mVideoReaderB->close();
        mVideoReaderB.reset();
    }
    if (mVideoTexture != 0) {
        glDeleteTextures(1, &mVideoTexture);
        mVideoTexture = 0;
    }
    if (mVideoTextureB != 0) {
        glDeleteTextures(1, &mVideoTextureB);
        mVideoTextureB = 0;
    }

    clearImages(false);
    mTexturePool.cleanUnusedTextures();

    mVideoReader = std::move(readerL);
    mVideoReaderB = std::move(readerR);
    if (mVideoReader) {
        mVideoReader->setPipelineTimingLog(videoPipelineDbgEnabled(mVideoLogPipelineTiming));
    }
    if (mVideoReaderB) {
        mVideoReaderB->setPipelineTimingLog(videoPipelineDbgEnabled(mVideoLogPipelineTiming));
    }
    mVideoMode = true;
    mVideoPlaying = true;
    mVideoPlaybackTimeBank = 0.0;

    mVideoPathL = pathL;
    mVideoPathR = pathR;
    mVideoSwapPresentationLR = false;

    // Reset offsets when changing active media.
    mVideoStartL = 0.0;
    mVideoStartR = 0.0;

    double tMin = 0.0;
    double tMax = 1.0;
    recomputeVideoScrubBounds(tMin, tMax);
    mVideoCompositionT = tMin;
    mVideoScrubValue = mVideoCompositionT;

    {
        namespace chrono = std::chrono;
        const auto tTexSync0 = chrono::steady_clock::now();
        recreateVideoTexture();
        recreateVideoTextureB();
        resetVideoDecoderSyncCache();
        syncVideoDecodersToCompositionT();
        LOGI("[video-open] recreate textures + first syncVideoDecodersToCompositionT: {:.3f}s", //
            chrono::duration<double>(chrono::steady_clock::now() - tTexSync0).count());
    }

    mVideoLazyDurationGraceFrames = 0;
    if (mVideoReader && mVideoReader->isOpen() && mVideoReader->durationSec() <= 0.001) {
        mVideoLazyDurationGraceFrames = 1;
    }
    if (videoCompareActive() && mVideoReaderB && mVideoReaderB->isOpen()
        && mVideoReaderB->durationSec() <= 0.001) {
        mVideoLazyDurationGraceFrames = 1;
    }
#endif
}

void App::openVideoFile(const std::string& filepath)
{
#if !defined(USE_VIDEO)
    (void)filepath;
    LOGW("Video playback is disabled in this build (USE_VIDEO).");
#else
    const int idx = addVideoPath(filepath);
    if (idx < 0) {
        return;
    }
    mVideoIndexL = idx;
    // Keep existing right selection if valid & distinct; otherwise clear.
    if (mVideoIndexR == mVideoIndexL) {
        mVideoIndexR = -1;
    }
    openVideosFromSelection();
#endif
}

void App::openVideoCompare(const std::string& leftPath, const std::string& rightPath)
{
#if !defined(USE_VIDEO)
    (void)leftPath;
    (void)rightPath;
    LOGW("Video playback is disabled in this build (USE_VIDEO).");
#else
    const int idxL = addVideoPath(leftPath);
    const int idxR = addVideoPath(rightPath);
    if (idxL < 0 || idxR < 0) {
        return;
    }
    if (idxL == idxR) {
        // Same list entry (duplicate path or identical paths after normalization). Opening compare
        // with one file used to bail out entirely and leave video mode empty — that looked like a
        // failed import and could strand the app on retry.
        LOGW("Video compare: left and right resolve to the same list row; opening as single view.");
        mVideoIndexL = idxL;
        mVideoIndexR = -1;
        openVideosFromSelection();
        return;
    }
    mVideoIndexL = idxL;
    mVideoIndexR = idxR;
    openVideosFromSelection();
    LOGI("Video compare: left \"{}\", right \"{}\"", mVideoPaths[idxL], mVideoPaths[idxR]);
#endif
}

void App::openVideoPaths(const std::vector<std::string>& paths)
{
#if !defined(USE_VIDEO)
    (void)paths;
    LOGW("Video playback is disabled in this build (USE_VIDEO).");
#else
    if (paths.empty()) {
        return;
    }
    for (const std::string& p : paths) {
        const int idx = addVideoPath(p);
        if (idx >= 0) {
            if (mVideoIndexL < 0) {
                mVideoIndexL = idx;
            } else if (mVideoIndexR < 0 && idx != mVideoIndexL) {
                mVideoIndexR = idx;
            }
        }
    }
    openVideosFromSelection();
#endif
}

void App::exitVideoMode()
{
#if !defined(USE_VIDEO)
    return;
#else
    mVideoMode = false;
    mVideoPlaying = false;
    mVideoLazyDurationGraceFrames = 0;
    mVideoPlaybackTimeBank = 0.0;
    resetVideoDecoderSyncCache();
    mVideoStartL = mVideoStartR = 0.0;
    mVideoCompositionT = 0.0;
    mVideoSwapPresentationLR = false;
    mVideoPathL.clear();
    mVideoPathR.clear();
    mVideoPaths.clear();
    mVideoIndexL = -1;
    mVideoIndexR = -1;
    if (mVideoReader) {
        mVideoReader->close();
        mVideoReader.reset();
    }
    if (mVideoReaderB) {
        mVideoReaderB->close();
        mVideoReaderB.reset();
    }
    if (mVideoTexture != 0) {
        glDeleteTextures(1, &mVideoTexture);
        mVideoTexture = 0;
    }
    if (mVideoTextureB != 0) {
        glDeleteTextures(1, &mVideoTextureB);
        mVideoTextureB = 0;
    }
#endif
}

void App::recreateVideoTexture()
{
#if !defined(USE_VIDEO)
    return;
#else
    if (mVideoTexture != 0) {
        glDeleteTextures(1, &mVideoTexture);
        mVideoTexture = 0;
    }
    if (!mVideoReader || !mVideoReader->isOpen()) {
        return;
    }
    glGenTextures(1, &mVideoTexture);
    glBindTexture(GL_TEXTURE_2D, mVideoTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const int w = mVideoReader->width();
    const int h = mVideoReader->height();
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
    LOGI("OpenGL video texture (L): {}x{} display {:.0f}x{:.0f}", w, h,
        static_cast<double>(mVideoReader->displayWidthForAspect()),
        static_cast<double>(mVideoReader->displayHeightForAspect()));
#endif
}

void App::recreateVideoTextureB()
{
#if !defined(USE_VIDEO)
    return;
#else
    if (mVideoTextureB != 0) {
        glDeleteTextures(1, &mVideoTextureB);
        mVideoTextureB = 0;
    }
    if (!mVideoReaderB || !mVideoReaderB->isOpen()) {
        return;
    }
    glGenTextures(1, &mVideoTextureB);
    glBindTexture(GL_TEXTURE_2D, mVideoTextureB);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const int w = mVideoReaderB->width();
    const int h = mVideoReaderB->height();
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
    LOGI("OpenGL video texture (R): {}x{} display {:.0f}x{:.0f}", w, h,
        static_cast<double>(mVideoReaderB->displayWidthForAspect()),
        static_cast<double>(mVideoReaderB->displayHeightForAspect()));
#endif
}

void App::uploadVideoTexture()
{
#if !defined(USE_VIDEO)
    return;
#else
    if (mVideoTexture == 0 || !mVideoReader || !mVideoReader->isOpen()) {
        return;
    }
    namespace chrono = std::chrono;
    const bool timeUpload = videoPipelineDbgEnabled(mVideoLogPipelineTiming);
    const auto tUpload0 = timeUpload ? chrono::steady_clock::now() : chrono::steady_clock::time_point {};
    mVideoReader->renderToTexture(mVideoTexture);
    if (timeUpload) {
        mVideoDbgLastUploadLMs = chrono::duration<float, std::milli>(chrono::steady_clock::now() - tUpload0).count();
    }
#endif
}

void App::uploadVideoTextureB()
{
#if !defined(USE_VIDEO)
    return;
#else
    if (mVideoTextureB == 0 || !mVideoReaderB || !mVideoReaderB->isOpen()) {
        return;
    }
    namespace chrono = std::chrono;
    const bool timeUpload = videoPipelineDbgEnabled(mVideoLogPipelineTiming);
    const auto tUpload0 = timeUpload ? chrono::steady_clock::now() : chrono::steady_clock::time_point {};
    mVideoReaderB->renderToTexture(mVideoTextureB);
    if (timeUpload) {
        mVideoDbgLastUploadRMs = chrono::duration<float, std::milli>(chrono::steady_clock::now() - tUpload0).count();
    }
#endif
}

void App::tickAndUploadVideoFrame(float deltaTime)
{
#if !defined(USE_VIDEO)
    (void)deltaTime;
    return;
#else
    if (!mVideoPlaying || !mVideoReader || !mVideoReader->isOpen()) {
        return;
    }
#if defined(BAKTSIU_DEBUG_BUILD)
    mVideoDbgLastTickDtMs = static_cast<float>(static_cast<double>(deltaTime) * 1000.0);
#endif
    if (videoCompareActive()) {
        double tMin = 0.0;
        double tMax = 1.0;
        recomputeVideoScrubBounds(tMin, tMax);
        // Compare playback: do NOT seek every frame. Frame-step both players according to a shared
        // playback bank (like single-video mode), then render once per tick.
        const double stepA = saneFrameDurSec(mVideoReader.get());
        const double stepB = saneFrameDurSec(mVideoReaderB.get());
        const double step = (std::min)(stepA, stepB);

        const double dt = (std::min)(static_cast<double>(deltaTime), kVideoPlaybackWallDtCapSec);
        mVideoComparePlaybackBank += dt;
        int n = static_cast<int>(std::floor((mVideoComparePlaybackBank / step) + 1e-9));
        if (n > kVideoMaxCompareStepsPerTick) {
            n = kVideoMaxCompareStepsPerTick;
        }
        if (n > 0) {
            mVideoComparePlaybackBank -= static_cast<double>(n) * step;
        }

        // Our composition clock follows the number of steps we actually issued.
        mVideoCompositionT += static_cast<double>(n) * step;
        constexpr double kEndEps = 1.0 / 240.0;
        if (mVideoCompositionT >= tMax - kEndEps) {
            mVideoCompositionT = tMax;
            mVideoPlaying = false;
            mVideoComparePlaybackBank = 0.0;
        }
        clampVideoCompositionT();
        if (n > 0) {
            // Step both streams; keep them in lockstep in "displayed frames", not absolute time.
            for (int i = 0; i < n; ++i) {
                (void)mVideoReader->decodeFrame(true);
                if (mVideoReaderB && mVideoReaderB->isOpen()) {
                    (void)mVideoReaderB->decodeFrame(true);
                }
            }
            uploadVideoTexture();
            uploadVideoTextureB();
        }

#if defined(BAKTSIU_DEBUG_BUILD)
        // Console-friendly summary (copy/paste) for diagnosing compare playback lag.
        if (videoPipelineDbgEnabled(mVideoLogPipelineTiming)) {
            const double now = ImGui::GetTime();
            if (now - mVideoDbgLastConsoleBeatSec >= 1.0) {
                mVideoDbgLastConsoleBeatSec = now;
                const double posL = (mVideoReader && mVideoReader->isOpen()) ? mVideoReader->positionSec() : 0.0;
                const double posR =
                    (mVideoReaderB && mVideoReaderB->isOpen()) ? mVideoReaderB->positionSec() : 0.0;
                LOGI("[video-compare] dt={:.1f}ms sync={:.1f}ms driftL={:+.1f}ms driftR={:+.1f}ms "
                     "T={:.3f}s posL={:.3f}s posR={:.3f}s readCap={}",
                    static_cast<double>(mVideoDbgLastTickDtMs),
                    static_cast<double>(mVideoDbgLastSyncMs),
                    static_cast<double>(mVideoDbgLastDriftLMs),
                    static_cast<double>(mVideoDbgLastDriftRMs),
                    mVideoCompositionT, posL, posR, n);
            }
        }
#endif
        return;
    }
    double frameDur = mVideoReader->frameDurationSec();
    if (frameDur < 1e-6 || frameDur > 1.0) {
        frameDur = 1.0 / 30.0;
    }
    const double dt = (std::min)(static_cast<double>(deltaTime), kVideoPlaybackWallDtCapSec);
    mVideoPlaybackTimeBank += dt;
    // Never cap the bank: capping drops real elapsed time and playback lags wall clock (cannot catch up).
    // How many whole video frames the accumulated bank can pay for (fixes FP remainder vs while-loop).
    constexpr int kMaxDecodeStepsPerFrame = 64;
    const double framesOwed = mVideoPlaybackTimeBank / frameDur;
    int n = static_cast<int>(std::floor(framesOwed + 1e-9));
    if (n > kMaxDecodeStepsPerFrame) {
        n = kMaxDecodeStepsPerFrame;
    }
    if (n > 0) {
        mVideoPlaybackTimeBank -= static_cast<double>(n) * frameDur;
        for (int i = 0; i < n; ++i) {
            if (!mVideoReader->decodeFrame(true)) {
                mVideoPlaybackTimeBank = 0.0;
                mVideoPlaying = false;
                break;
            }
        }
        uploadVideoTexture();
    }
#endif
}

void App::renderVideoBlit(const ImGuiIO& io)
{
#if !defined(USE_VIDEO)
    (void)io;
    return;
#else
    if (!mVideoShaderReady || mVideoTexture == 0 || !mVideoReader || !mVideoReader->isOpen()) {
        return;
    }
    const Vec2f viewportSize = io.DisplaySize * io.DisplayFramebufferScale;
    glViewport(0, 0, static_cast<GLsizei>(viewportSize.x), static_cast<GLsizei>(viewportSize.y));
    glClearColor(0.45f, 0.55f, 0.6f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);

    mVideoBlitShader.bind();
    const bool cmp = videoCompareActive();
#if defined(BAKTSIU_DEBUG_BUILD)
    constexpr float kVideoTransportPipelineDbgRow = 26.0f;
#else
    constexpr float kVideoTransportPipelineDbgRow = 0.0f;
#endif
    const float transportBarH = (cmp ? 80.0f : 36.0f) + kVideoTransportPipelineDbgRow;
    const float contentH =
        io.DisplaySize.y - mToolbarHeight - mFooterHeight - transportBarH;
    const Vec2f contentOrigin(0.0f, mToolbarHeight);
    const Vec2f contentSize(io.DisplaySize.x, std::max(1.0f, contentH));

    mVideoBlitShader.setUniform("uWindowSize", Vec2f(io.DisplaySize.x, io.DisplaySize.y));
    mVideoBlitShader.setUniform("uContentOrigin", contentOrigin);
    mVideoBlitShader.setUniform("uContentSize", contentSize);

    if (cmp && mVideoTextureB != 0) {
        const bool dispSwap = mVideoSwapPresentationLR && videoCompareActive();
        const GLuint t0 = dispSwap ? mVideoTextureB : mVideoTexture;
        const GLuint t1 = dispSwap ? mVideoTexture : mVideoTextureB;
        const MpvGlPlayer* r0 = dispSwap ? mVideoReaderB.get() : mVideoReader.get();
        const MpvGlPlayer* r1 = dispSwap ? mVideoReader.get() : mVideoReaderB.get();

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, t0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, t1);

        mVideoBlitShader.setUniform("uCompareMode", 1);
        mVideoBlitShader.setUniform("uVideo", 0);
        mVideoBlitShader.setUniform("uVideoR", 1);
        mVideoBlitShader.setUniform("uVideoSize",
            Vec2f(r0 ? r0->displayWidthForAspect() : 1.0f, r0 ? r0->displayHeightForAspect() : 1.0f));
        mVideoBlitShader.setUniform("uVideoSizeR",
            Vec2f(r1 ? r1->displayWidthForAspect() : 1.0f, r1 ? r1->displayHeightForAspect() : 1.0f));
        mVideoBlitShader.setUniform("uSplitPos", mViewSplitPos);
    } else {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, mVideoTexture);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, mVideoTexture);

        mVideoBlitShader.setUniform("uCompareMode", 0);
        mVideoBlitShader.setUniform("uVideo", 0);
        mVideoBlitShader.setUniform("uVideoR", 1);
        mVideoBlitShader.setUniform("uVideoSize",
            Vec2f(mVideoReader->displayWidthForAspect(), mVideoReader->displayHeightForAspect()));
        mVideoBlitShader.setUniform("uVideoSizeR",
            Vec2f(mVideoReader->displayWidthForAspect(), mVideoReader->displayHeightForAspect()));
    }
    mVideoBlitShader.drawTriangle();
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
#endif
}

void App::initVideoTransportBar(const ImGuiIO& io)
{
#if !defined(USE_VIDEO)
    (void)io;
    return;
#else
    if (!mVideoReader || !mVideoReader->isOpen()) {
        return;
    }
    const bool cmp = videoCompareActive();
#if defined(BAKTSIU_DEBUG_BUILD)
    constexpr float kVideoTransportPipelineDbgRow = 26.0f;
#else
    constexpr float kVideoTransportPipelineDbgRow = 0.0f;
#endif
    const float barH = (cmp ? 80.0f : 36.0f) + kVideoTransportPipelineDbgRow;
    ImGui::SetNextWindowPos(ImVec2(0.0f, io.DisplaySize.y - mFooterHeight - barH));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, barH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("VideoTransport", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
        | ImGuiWindowFlags_NoSavedSettings);

    if (ImGui::Button(mVideoPlaying ? "Pause" : "Play")) {
        mVideoPlaying ^= true;
        mVideoPlaybackTimeBank = 0.0;
        mVideoComparePlaybackBank = 0.0;
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
        exitVideoMode();
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }
    ImGui::SameLine();

    if (cmp) {
        const ImVec4 tintL(0.25f, 0.80f, 0.75f, 1.0f);  // teal
        const ImVec4 tintR(0.95f, 0.55f, 0.20f, 1.0f);  // orange

        double tMin = 0.0;
        double tMax = 1.0;
        recomputeVideoScrubBounds(tMin, tMax);

        ImGui::SetNextItemWidth(std::max(120.0f, io.DisplaySize.x - 380.0f));
        ImGui::PushID("videoscrubcmp");
        const bool valueChanged =
            ImGui::SliderScalar("##t", ImGuiDataType_Double, &mVideoScrubValue, &tMin, &tMax, "");

        // Overlay each video's bounds onto the scrub bar so their relative alignment is visible.
        // In composition time: media spans [mVideoStartX, mVideoStartX + durationX].
        if (tMax > tMin) {
            const ImVec2 rmin = ImGui::GetItemRectMin();
            const ImVec2 rmax = ImGui::GetItemRectMax();
            const float padY = 2.0f;
            const float y0 = rmin.y + padY;
            const float y1 = rmax.y - padY;
            auto drawRange = [&](double a, double b, const ImVec4& tint) {
                const double lo = (std::min)(a, b);
                const double hi = (std::max)(a, b);
                const double c0 = (std::max)(lo, tMin);
                const double c1 = (std::min)(hi, tMax);
                if (c1 <= c0) {
                    return;
                }
                const float u0 = static_cast<float>((c0 - tMin) / (tMax - tMin));
                const float u1 = static_cast<float>((c1 - tMin) / (tMax - tMin));
                const float x0 = rmin.x + (rmax.x - rmin.x) * glm::clamp(u0, 0.0f, 1.0f);
                const float x1 = rmin.x + (rmax.x - rmin.x) * glm::clamp(u1, 0.0f, 1.0f);
                const ImU32 fill = ImGui::ColorConvertFloat4ToU32(ImVec4(tint.x, tint.y, tint.z, 0.22f));
                const ImU32 edge = ImGui::ColorConvertFloat4ToU32(ImVec4(tint.x, tint.y, tint.z, 0.65f));
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), fill, 2.0f);
                ImGui::GetWindowDrawList()->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), edge, 2.0f);
            };

            const bool dispSwap = mVideoSwapPresentationLR && videoCompareActive();
            const double dA = mVideoReader ? videoScrubSpanSec(mVideoReader.get()) : 0.0;
            const double dB = mVideoReaderB ? videoScrubSpanSec(mVideoReaderB.get()) : 0.0;
            // "Left/Right" colors follow what is visible on screen.
            const double startLeft = dispSwap ? mVideoStartR : mVideoStartL;
            const double durLeft = dispSwap ? dB : dA;
            const double startRight = dispSwap ? mVideoStartL : mVideoStartR;
            const double durRight = dispSwap ? dA : dB;
            drawRange(startLeft, startLeft + durLeft, tintL);
            drawRange(startRight, startRight + durRight, tintR);
        }

        const bool itemActive = ImGui::IsItemActive();
        const bool itemActivated = ImGui::IsItemActivated();
        const bool itemClicked = ImGui::IsItemClicked();
        const bool itemDeactivated = ImGui::IsItemDeactivated();
        if (itemActivated) {
            mVideoPlayingBeforeScrub = mVideoPlaying;
        }
        if (itemActive || itemActivated || valueChanged || itemClicked || itemDeactivated) {
            mVideoCompositionT = mVideoScrubValue;
            clampVideoCompositionT();
            mVideoPlaybackTimeBank = 0.0;
            mVideoPlaying = false;
            const VideoScrubDecodeHint scrubHint = videoScrubDecodeHint(itemActive, itemActivated, itemClicked,
                valueChanged, itemDeactivated, ImGui::GetTime(), mVideoLastScrubDecodeTime);
            if (scrubHint.run) {
                syncVideoDecodersToCompositionT(scrubHint.maxReadCapPerStream);
            }
        }
        if (itemDeactivated && mVideoPlayingBeforeScrub) {
            mVideoPlaying = true;
            mVideoPlaybackTimeBank = 0.0;
            mVideoComparePlaybackBank = 0.0;
        }
        if (!itemActive) {
            mVideoScrubValue = mVideoCompositionT;
        }
        ImGui::PopID();

        ImGui::SameLine();
        const double tShown = itemActive ? mVideoScrubValue : mVideoCompositionT;
        char tBuf[32];
        char maxBuf[32];
        formatVideoHms(tShown, tBuf, sizeof tBuf);
        formatVideoHms(tMax, maxBuf, sizeof maxBuf);
        const bool durRelL = mVideoReader && mVideoReader->isOpen() && mVideoReader->hasReliableDuration()
            && mVideoReader->durationSec() > 0.001;
        const bool durRelR = mVideoReaderB && mVideoReaderB->isOpen() && mVideoReaderB->hasReliableDuration()
            && mVideoReaderB->durationSec() > 0.001;
        const bool durRelCmp = durRelL && durRelR;
        if (durRelCmp) {
            ImGui::Text("T %s / %s", tBuf, maxBuf);
        } else {
            ImGui::Text("T %s / ?", tBuf);
        }
        if (!durRelCmp) {
            ImGui::SameLine();
            ImGui::TextDisabled("(duration unknown)");
        }

        ImGui::TextUnformatted("Offsets (composition time at media 0):");

        static char sOffBufL[64] = "";
        static char sOffBufR[64] = "";

        auto offsetControl = [&](const char* label, double& v, double stepSec, const ImVec4& tint, char* buf,
                                 size_t bufSize) -> bool {
            bool changed = false;
            ImGui::PushID(label);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(tint, "%s", label);
            ImGui::SameLine();

            // Tint controls by side (button + input background).
            const ImVec4 btn = ImVec4(tint.x * 0.85f, tint.y * 0.85f, tint.z * 0.85f, 1.0f);
            const ImVec4 btnHover = ImVec4(tint.x, tint.y, tint.z, 1.0f);
            const ImVec4 btnActive = ImVec4(tint.x * 1.05f, tint.y * 1.05f, tint.z * 1.05f, 1.0f);
            const ImVec4 frame = ImVec4(tint.x * 0.20f, tint.y * 0.20f, tint.z * 0.20f, 0.85f);
            ImGui::PushStyleColor(ImGuiCol_Button, btn);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, btnHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, btnActive);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, frame);
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, frame);
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, frame);

            auto stripUnitsInPlace = [](char* s) {
                if (!s) {
                    return;
                }
                // Truncate at first non-number-ish char (keeps sign, decimal, exponent).
                for (char* p = s; *p; ++p) {
                    const char c = *p;
                    const bool ok = (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E';
                    if (!ok) {
                        *p = '\0';
                        break;
                    }
                }
            };

            if (ImGui::SmallButton("<")) {
                v -= stepSec;
                changed = true;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(92.0f);
            {
                const ImGuiID vId = ImGui::GetID("##v");
                const bool wasActive = (ImGui::GetActiveID() == vId);
                if (!wasActive) {
                    std::snprintf(buf, bufSize, "%.3f", v);
                }

                ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll;
                const bool enter = ImGui::InputText("##v", buf, bufSize, flags);
                const bool nowActive = ImGui::IsItemActive();
                const bool activated = ImGui::IsItemActivated();
                const bool deactivatedAfterEdit = ImGui::IsItemDeactivatedAfterEdit();

                if (activated) {
                    stripUnitsInPlace(buf);
                }

                if (enter || deactivatedAfterEdit) {
                    char* endp = nullptr;
                    const double parsed = std::strtod(buf, &endp);
                    if (endp != buf) {
                        v = parsed;
                        changed = true;
                    }
                }

                if (!nowActive) {
                    std::snprintf(buf, bufSize, "%.3f", v);

                    // Visual-only unit suffix, not part of the input buffer.
                    const ImVec2 rmin = ImGui::GetItemRectMin();
                    const ImVec2 rmax = ImGui::GetItemRectMax();
                    const char* unit = "s";
                    const ImVec2 unitSz = ImGui::CalcTextSize(unit);
                    const float padX = 6.0f;
                    const float x = rmax.x - unitSz.x - padX;
                    const float y = rmin.y + (rmax.y - rmin.y - unitSz.y) * 0.5f;
                    const ImU32 col = ImGui::ColorConvertFloat4ToU32(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    ImGui::GetWindowDrawList()->AddText(ImVec2(x, y), col, unit);
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(">")) {
                v += stepSec;
                changed = true;
            }

            ImGui::PopStyleColor(6);
            ImGui::PopID();
            return changed;
        };

        const bool dispSwap = mVideoSwapPresentationLR && videoCompareActive();
        const double stepA = (mVideoReader && mVideoReader->frameDurationSec() > 0.0) ? mVideoReader->frameDurationSec() : 0.05;
        const double stepB =
            (mVideoReaderB && mVideoReaderB->frameDurationSec() > 0.0) ? mVideoReaderB->frameDurationSec() : 0.05;
        double& startLeft = dispSwap ? mVideoStartR : mVideoStartL;
        double& startRight = dispSwap ? mVideoStartL : mVideoStartR;
        const double stepLeft = dispSwap ? stepB : stepA;
        const double stepRight = dispSwap ? stepA : stepB;

        const bool changedL = offsetControl("L", startLeft, stepLeft, tintL, sOffBufL, sizeof sOffBufL);
        ImGui::SameLine();
        const bool changedR = offsetControl("R", startRight, stepRight, tintR, sOffBufR, sizeof sOffBufR);
        if (changedL || changedR) {
            clampVideoCompositionT();
            syncVideoDecodersToCompositionT();
            mVideoPlaying = false;
        }
    } else {
        const double pos = mVideoReader->positionSec();
        const double metaDur = mVideoReader->durationSec();
        const bool durRel = mVideoReader->hasReliableDuration() && metaDur > 0.001;
        double sliderMaxSec = std::max({videoScrubSpanSec(mVideoReader.get()), pos + 0.01, 0.25});
        if (durRel) {
            const double frameDur = mVideoReader->frameDurationSec();
            const double endSlack = std::max(frameDur * 3.0, 0.1);
            const double seekableEnd = std::max(0.0, metaDur - endSlack);
            sliderMaxSec = std::max({seekableEnd, pos + 0.01, 0.25});
        }
        const double sliderMinSec = 0.0;

        ImGui::SetNextItemWidth(std::max(120.0f, io.DisplaySize.x - 280.0f));
        ImGui::PushID("videoscrub");
        const bool valueChanged =
            ImGui::SliderScalar("##t", ImGuiDataType_Double, &mVideoScrubValue, &sliderMinSec, &sliderMaxSec, "");
        const bool itemActive = ImGui::IsItemActive();
        const bool itemActivated = ImGui::IsItemActivated();
        const bool itemClicked = ImGui::IsItemClicked();
        const bool itemDeactivated = ImGui::IsItemDeactivated();
        if (itemActivated) {
            mVideoPlayingBeforeScrub = mVideoPlaying;
        }
        if (itemActive || itemActivated || valueChanged || itemClicked || itemDeactivated) {
            mVideoPlaybackTimeBank = 0.0;
            mVideoPlaying = false;
            const VideoScrubDecodeHint scrubHint = videoScrubDecodeHint(itemActive, itemActivated, itemClicked,
                valueChanged, itemDeactivated, ImGui::GetTime(), mVideoLastScrubDecodeTime);
            if (scrubHint.run) {
                mVideoReader->seek(mVideoScrubValue, scrubHint.maxReadCapPerStream <= 0);
                mVideoReader->decodeFrameThrough(mVideoScrubValue, scrubHint.maxReadCapPerStream);
                uploadVideoTexture();
                if (videoPipelineDbgEnabled(mVideoLogPipelineTiming)) {
                    LOGI(
                        "[video-pipe] readCap={} L: seek={:.2f}ms (flush={:.2f} setPos={:.2f}) "
                        "decode+buf={:.2f}ms reads={} hitIterCap={} upload={:.2f}ms nv12Out={}",
                        scrubHint.maxReadCapPerStream, static_cast<double>(mVideoReader->lastSeekMs()),
                        static_cast<double>(mVideoReader->lastSeekFlushMs()),
                        static_cast<double>(mVideoReader->lastSeekSetPosMs()),
                        static_cast<double>(mVideoReader->lastDecodeThroughMs()),
                        mVideoReader->lastDecodeThroughReads(), mVideoReader->lastDecodeThroughHitCap() ? 1 : 0,
                        static_cast<double>(mVideoDbgLastUploadLMs), 0);
                }
            }
        }
        if (itemDeactivated && mVideoPlayingBeforeScrub) {
            mVideoPlaying = true;
            mVideoPlaybackTimeBank = 0.0;
            mVideoComparePlaybackBank = 0.0;
        }
        if (!itemActive) {
            mVideoScrubValue = mVideoReader->positionSec();
        }
        ImGui::PopID();

        ImGui::SameLine();
        const double tDisplayed = itemActive ? mVideoScrubValue : mVideoReader->positionSec();
        char posBuf[32];
        char durBuf[32];
        formatVideoHms(tDisplayed, posBuf, sizeof posBuf);
        formatVideoHms(metaDur, durBuf, sizeof durBuf);
        if (durRel) {
            ImGui::Text("%s / %s", posBuf, durBuf);
        } else {
            ImGui::Text("%s / ?", posBuf);
            ImGui::SameLine();
            ImGui::TextDisabled("(duration unknown)");
        }
    }

#if defined(BAKTSIU_DEBUG_BUILD)
    ImGui::Separator();
    if (ImGui::Checkbox("Log pipeline (console)##videopipe", &mVideoLogPipelineTiming)) {
        if (mVideoReader) {
            mVideoReader->setPipelineTimingLog(videoPipelineDbgEnabled(mVideoLogPipelineTiming));
        }
        if (mVideoReaderB) {
            mVideoReaderB->setPipelineTimingLog(videoPipelineDbgEnabled(mVideoLogPipelineTiming));
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("seek / decode+buf / upload ms");
    ImGui::SameLine();
    if (videoCompareActive()) {
        ImGui::TextDisabled("dt %.1fms sync %.1fms drift L %+0.1fms R %+0.1fms",
            static_cast<double>(mVideoDbgLastTickDtMs),
            static_cast<double>(mVideoDbgLastSyncMs),
            static_cast<double>(mVideoDbgLastDriftLMs),
            static_cast<double>(mVideoDbgLastDriftRMs));
    } else {
        ImGui::TextDisabled("dt %.1fms", static_cast<double>(mVideoDbgLastTickDtMs));
    }
#endif

    ImGui::End();
    ImGui::PopStyleVar();
#endif
}

}  // namespace baktsiu