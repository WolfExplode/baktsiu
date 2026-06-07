#define IMGUI_DEFINE_MATH_OPERATORS
#include <icons_font_awesome5.h>
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#pragma warning(push)
#pragma warning(disable: 4819 4566)
#include <GL/gl3w.h>
#pragma warning(pop)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "app.h"
#include "colour.h"

#ifndef _MSC_VER
#define sprintf_s snprintf
#endif

#ifndef _WIN32
#include <sys/stat.h>
#else
#include <io.h>
#endif

namespace
{

const char* const kInfoPopupName = "Info";

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

std::int64_t fileSizeBytesUtf8(const std::string& path)
{
    if (path.empty()) {
        return -1;
    }
#ifdef _WIN32
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (wlen <= 0) {
        return -1;
    }
    std::wstring wpath(static_cast<size_t>(wlen), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], wlen) <= 0) {
        return -1;
    }
    WIN32_FILE_ATTRIBUTE_DATA fad {};
    if (!GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &fad)) {
        return -1;
    }
    ULARGE_INTEGER uli;
    uli.LowPart = fad.nFileSizeLow;
    uli.HighPart = fad.nFileSizeHigh;
    return static_cast<std::int64_t>(uli.QuadPart);
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return -1;
    }
    return static_cast<std::int64_t>(st.st_size);
#endif
}

void formatFileSizeHuman(std::int64_t bytes, char* out, size_t outSize)
{
    if (outSize == 0) {
        return;
    }
    if (bytes < 0) {
        std::snprintf(out, outSize, "?");
        return;
    }
    if (bytes < 1024) {
        std::snprintf(out, outSize, "%lld B", static_cast<long long>(bytes));
        return;
    }
    static const char* const suf[] = {"KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int i = -1;
    do {
        v /= 1024.0;
        ++i;
    } while (v >= 1024.0 && i < 3);
    std::snprintf(out, outSize, "%.2f %s", v, suf[i]);
}

void footerFileSizeHuman(
    const std::string& path, std::string& cachePath, std::int64_t& cacheBytes, char* out, size_t outSize)
{
    if (path != cachePath) {
        cachePath = path;
        cacheBytes = fileSizeBytesUtf8(path);
    }
    formatFileSizeHuman(cacheBytes, out, outSize);
}

}  // namespace

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

    int v_hovered = -1;
    if (ItemHoverable(inner_bb, id)) {
        const float t = ImClamp((g.IO.MousePos.x - inner_bb.Min.x) / (inner_bb.Max.x - inner_bb.Min.x), 0.0f, 1.0f);
        const int v_idx = std::min((int)(t * item_count), item_count - 1);
        IM_ASSERT(v_idx >= 0 && v_idx < values_count);

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
        ImVec2 tp0 = ImVec2(t0, 1.0f - ImSaturate((v0 - scale_min) / (scale_max - scale_min)));

        const ImU32 col_base = colors[data_idx];
        const ImU32 col_hovered = InvertColorU32(colors[data_idx]);

        for (int n = 0; n < res_w; n++) {
            const float t1 = t0 + t_step;
            const int v1_idx = (int)(t0 * item_count + 0.5f);
            IM_ASSERT(v1_idx >= 0 && v1_idx < values_count);
            const float v1 = pixel_counts[data_idx * values_count + v1_idx] / static_cast<float>(norm_pixel_count);
            const ImVec2 tp1 = ImVec2(t1, 1.0f - ImSaturate((v1 - scale_min) / (scale_max - scale_min)));

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
}

} // namespace ImGui


namespace baktsiu
{

void    App::renderToolbar()
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
    const float toolbarTextBtnH = 26.0f;

    if (ImGui::Button(ICON_FA_INFO "##toolbarInfo", ImVec2(0.f, toolbarTextBtnH))) {
        ImGui::OpenPopup(kInfoPopupName);
    }
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Info"); }

    ImGui::SameLine();
    if (ImGui::Button((std::string(ICON_FA_FILE_IMPORT) + " Import##toolbarImport").c_str(), ImVec2(0.f, toolbarTextBtnH))) {
        showImportImageDlg();
    }
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Import Images"); }

    ImGui::SameLine();
    if (ImGui::Button((std::string(ICON_FA_FILE_EXPORT) + " Export##toolbarExport").c_str(), ImVec2(0.f, toolbarTextBtnH))) {
        showExportSessionDlg();
    }
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Export Session"); }

    const float comboMenuWidth = 100.0f;
    ImGui::SameLine((g.IO.DisplaySize.x - mCenteredToolbarWidth) * 0.5f);
    float centeredToolBeginPos = g.CurrentWindow->DC.CursorPos.x;
#if defined(USE_VIDEO)
    if (!isVideoMode()) {
#endif
        ToggleButton(ICON_FA_FEATHER, &mUseLinearFilter, buttonSize);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Smooth image");
        }
        ImGui::SameLine();
#if defined(USE_VIDEO)
    }
    const bool couldCompare = (mMediaList.size() > 1)
        || (isVideoMode() && (videoCompareActive() || static_cast<int>(mMediaList.size()) >= 2));
#else
    const bool couldCompare = mMediaList.size() > 1;
#endif
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
#if defined(USE_VIDEO)
    const bool canFlipMedia = isVideoMode() ? true : (mTopImageIndex >= 0);
#else
    const bool canFlipMedia = mTopImageIndex >= 0;
#endif
    if (canFlipMedia) {
        auto drawFlipAxisButton = [&](const char* label, bool* leftState, bool* rightState, const char* tooltip) {
            const bool flipActive = *leftState || *rightState;
            if (flipActive) {
                ImGui::PushStyleColor(ImGuiCol_Button, g.Style.Colors[ImGuiCol_ButtonActive]);
            }
            ImGui::Button(label, buttonSize);
            const bool flipLeft = ImGui::IsItemClicked(0);
            const bool flipRight = ImGui::IsItemClicked(1);
            if (flipActive) {
                ImGui::PopStyleColor(1);
            }
            if (flipLeft) {
                *leftState ^= true;
            }
            if (flipRight) {
                *rightState ^= true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s\nLMB: left media\nRMB: right media", tooltip);
            }
        };

        drawFlipAxisButton(ICON_FA_ARROWS_ALT_V "##flipVert", &mFlipPresentMediaVert[0],
            &mFlipPresentMediaVert[1], "Vertical flip");
        ImGui::SameLine();
        drawFlipAxisButton(ICON_FA_ARROWS_ALT_H "##flipHorz", &mFlipPresentMediaHorz[0],
            &mFlipPresentMediaHorz[1], "Horizontal flip");
        ImGui::SameLine();
    } else {
        ImGui::SameLine();
    }

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
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("ACES tone mapping (only when Channels is RGB)");
    }

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
        const bool overflowMenuEnabled = !enableCompareView || !showDiffMarker;

        itemValue = hasAllFlags(mPixelMarkerFlags, PixelMarkerFlags::Overflow);
        if (ImGui::MenuItem("Overflow", "", &itemValue, overflowMenuEnabled)) {
            mPixelMarkerFlags = toggleFlags(mPixelMarkerFlags, PixelMarkerFlags::Overflow);
        }

        itemValue = hasAllFlags(mPixelMarkerFlags, PixelMarkerFlags::Underflow);
        if (ImGui::MenuItem("Underflow", "", &itemValue, overflowMenuEnabled)) {
            mPixelMarkerFlags = toggleFlags(mPixelMarkerFlags, PixelMarkerFlags::Underflow);
        }

        ImGui::EndPopup();
    }

    ImGui::SameLine();
    mCenteredToolbarWidth = g.CurrentWindow->DC.CursorPos.x - centeredToolBeginPos;

    // Show buttons at right hand side.
    ImGui::SameLine(g.IO.DisplaySize.x - (buttonSize.x + g.Style.ItemSpacing.x) * 2.0f);
    ToggleButton(ICON_FA_COMMENT_ALT, &mShowImageNameOverlay, buttonSize);
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Image Name Tags"); }

    ImGui::SameLine(g.IO.DisplaySize.x - buttonSize.x - g.Style.ItemSpacing.x);
    ToggleButton(ICON_FA_CHART_BAR, &mShowImagePropWindow, buttonSize);
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Property Window"); }

    ImGui::PopStyleColor(1);
    ImGui::PopStyleVar(7);

    renderInfoPopup(kInfoPopupName);
    ImGui::End();
}

bool    App::renderPropWindowHandle(float handleWidth, bool& showPropWindow)
{
    bool isHovered = false;

    ImGuiContext& g = *ImGui::GetCurrentContext();
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
void App::renderPropertiesPanel()
{
    bool checkPopupRect = false;

    ImGuiContext& g = *ImGui::GetCurrentContext();
    const float handleWidth = g.FontSize;

    // Create a window as a toggle handle to open/close property window.
    if (renderPropWindowHandle(handleWidth, mShowImagePropWindow)) {
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
#if defined(USE_VIDEO)
    auto* _vsHist = videoSource();
    const bool histVideo = _vsHist && _vsHist->isOpen() && _vsHist->texId() != 0 && mVideoRgbHistogramComputeReady;
#else
    const bool histVideo = false;
#endif
    const bool histImage = !isVideoMode() && topImage;
    if (ImGui::CollapsingHeader("Histogram") && mSupportComputeShader && (histImage || histVideo)) {
        ScopeMarker("Draw Histogram");
        glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);

        const bool dualHistImage =
            !isVideoMode() && inCompareMode() && topImage && mCmpImageIndex >= 0
            && static_cast<size_t>(mCmpImageIndex) < mMediaList.size()
            && mMediaList[static_cast<size_t>(mCmpImageIndex)]->texId() != 0;
#if defined(USE_VIDEO)
        auto* _rB = videoSourceB();
        const bool dualHistVideo = isVideoMode() && inCompareMode() && videoCompareActive()
            && _rB && _rB->isOpen() && _rB->texId() != 0;
#else
        const bool dualHistVideo = false;
#endif
        const bool dualHist = dualHistImage || dualHistVideo;

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, Vec2f(3.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(69, 69, 69, 255));
        ImGui::PushFont(mSmallFont);

        auto plotRgbHistogram = [&](const char* plotId, int* hist768) {
            ImGui::SetNextItemWidth(propWindowWidth - ImGui::GetStyle().ItemSpacing.x);
            const char* names[3] = {"R", "G", "B"};
            static ImColor colors[3] = { ImColor(255, 0, 0, 150), ImColor(0, 255, 0, 150), ImColor(0, 0, 255, 150) };
            constexpr int binSize = 256;
            std::vector<int> temp;
            temp.assign(hist768, hist768 + 768);
            std::nth_element(temp.begin(), temp.end() - 25, temp.end());
            const int normPixelCount = *(temp.end() - 25);
            ImGui::PlotMultiHistograms(
                plotId, 3, names, colors, hist768, normPixelCount, binSize, 0.0f, 1.0f, ImVec2(0, 100));
        };

        if (dualHist) {
            const ImVec4 tintL(0.25f, 0.80f, 0.75f, 1.0f);
            const ImVec4 tintR(0.95f, 0.55f, 0.20f, 1.0f);
            glBindTexture(GL_TEXTURE_2D, mTexHistogram);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_INT, (void*)mHistogram.data());
            ImGui::TextColored(tintL, "Left");
            plotRgbHistogram("##histogramL", mHistogram.data());
            ImGui::Spacing();
            glBindTexture(GL_TEXTURE_2D, mTexHistogramB);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_INT, (void*)mHistogramB.data());
            ImGui::TextColored(tintR, "Right");
            plotRgbHistogram("##histogramR", mHistogramB.data());
            glBindTexture(GL_TEXTURE_2D, 0);
        } else {
            glBindTexture(GL_TEXTURE_2D, mTexHistogram);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_INT, (void*)mHistogram.data());
            plotRgbHistogram("##histogram", mHistogram.data());
            glBindTexture(GL_TEXTURE_2D, 0);
        }

        ImGui::PopFont();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(1);
    }

    if (isVideoMode()) {
#if defined(USE_VIDEO)
        if (ImGui::CollapsingHeader("Video Properties", ImGuiTreeNodeFlags_DefaultOpen)) {
            showVideoProperties();
        }
#else
        if (ImGui::CollapsingHeader("Video Properties", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextUnformatted("Video is not available in this build.");
        }
#endif
    } else if (ImGui::CollapsingHeader("Image Properties", ImGuiTreeNodeFlags_DefaultOpen)) {
        showImageProperties();
    }

    ImGui::PopStyleVar(1);
    ImGui::EndChild();

    const float spacerHeight = 4.0f;
    Vec2f buttonSize(20.0f);
    const float toolbarHeight = buttonSize.y + g.Style.FramePadding.y * 2.0f + g.Style.ItemSpacing.y;
    const float titleHeight = ImGui::GetFrameHeightWithSpacing() + spacerHeight;
    ImGui::Dummy(Vec2f(0.0f, spacerHeight));
    ImGui::Text("  " ICON_FA_LAYER_GROUP "  %s", isVideoMode() ? "Media" : "Images");

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
    const int imageNum = static_cast<int>(mMediaList.size());
    const float imageListWindowHeight = ImGui::GetWindowHeight() - g.Style.ItemInnerSpacing.y * 2.0f;
    const float imageItemHeight = 24.0f;
    const bool enableCompareView = inCompareMode();

    static const Vec4f activeBorderColor(1.0f, 1.0f, 1.0f, 1.0f);
    static const Vec4f borderColor(1.0f, 1.0f, 1.0f, 0.5f);

    if (isVideoMode()) {
        const ImVec4 tintL(0.25f, 0.80f, 0.75f, 1.0f);
        const ImVec4 tintR(0.95f, 0.55f, 0.20f, 1.0f);
#if defined(USE_VIDEO)
        const int nMedia = static_cast<int>(mMediaList.size());
        if (videoCompareActive()) {
            const bool dispSwap = mVideoSwapPresentationLR && videoCompareActive();
            int& idxDispL = dispSwap ? mCmpImageIndex : mTopImageIndex;
            int& idxDispR = dispSwap ? mTopImageIndex : mCmpImageIndex;

            for (int i = 0; i < nMedia; ++i) {
                const bool isL = (i == idxDispL);
                const bool isR = (i == idxDispR);
                const ImVec4 tint = isL ? tintL : (isR ? tintR : ImGui::GetStyleColorVec4(ImGuiCol_Text));

                ImGui::PushID(i);
                char row[160];
                const char* tag = isL ? "L" : (isR ? "R" : " ");
                std::snprintf(row, sizeof row, "%s  %s", tag, basename(mMediaList[i]->filepath()));
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
        } else {
            for (int i = 0; i < nMedia; ++i) {
                const bool sel = (i == mTopImageIndex);
                ImGui::PushID(i);
                char row[160];
                std::snprintf(row, sizeof row, "   %s", basename(mMediaList[i]->filepath()));
                ImGui::PushStyleColor(ImGuiCol_Text,
                    sel ? tintL : ImGui::GetStyleColorVec4(ImGuiCol_Text));
                ImGui::Selectable(row, sel, 0, Vec2f(propWindowWidth, imageItemHeight));
                const bool lmb = ImGui::IsItemClicked(0);
                const bool rmb = ImGui::IsItemClicked(1);
                ImGui::PopStyleColor(1);
                ImGui::PopID();
                if (lmb && !sel) {
                    mTopImageIndex = i;
                    mCmpImageIndex = -1;
                    mVideoSwapPresentationLR = false;
                    openVideosFromSelection();
                } else if (rmb && i != mTopImageIndex) {
                    mCmpImageIndex = i;
                    mVideoSwapPresentationLR = false;
                    openVideosFromSelection();
                }
            }
        }
#else
        (void)tintL;
        (void)tintR;
        {
            auto* _r = videoSource();
            auto* _rB = videoSourceB();
            if (_r) { ImGui::TextUnformatted(basename(_r->filepath())); }
            if (_rB) { ImGui::TextUnformatted(basename(_rB->filepath())); }
        }
#endif
    } else {
        for (int i = 0; i < imageNum; i++) {
            std::string filename = mMediaList[i]->filename();
            void* addr = mMediaList[i].get();
            const auto filenameLength = filename.size();
            if (filenameLength > maxFilenameLength) {
                filename = filename.substr(filenameLength - maxFilenameLength);
                snprintf(buf, bufSize, "        ...%s##%p", filename.c_str(), addr);
            } else {
                snprintf(buf, bufSize, "           %s##%p", filename.c_str(), addr);
            }

            if (ImGui::Selectable(buf, mTopImageIndex == i && !mIsDraggingImageItem, 0, Vec2f(propWindowWidth, imageItemHeight))) {
                if (mIsDraggingImageItem) {
                    mIsDraggingImageItem = false;
                    if (mMoveAction) {
                        if (mMoveAction->prevTopImageIdx != mTopImageIndex ||
                            mMoveAction->prevCmpImageIdx != mCmpImageIndex) {
                            appendAction(std::move(*mMoveAction));
                        }
                    }

                    mMoveAction.reset();
                } else {
                    mTopImageIndex = i;
                    if (mCmpImageIndex == i) {
                        mCmpImageIndex = (mCmpImageIndex + 1) % imageNum;
                    }
                }
            }

            // Right click to set the compared image directly.
            if (ImGui::IsItemClicked(1)) {
                if (mTopImageIndex == i) {
                    mTopImageIndex = mCmpImageIndex;
                }
                mCmpImageIndex = i;
            }

            if (!mIsDraggingImageItem && ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            } else if (mIsDraggingImageItem) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            }

            if (ImGui::IsItemActive() && !ImGui::IsItemHovered()) {
                mIsDraggingImageItem = true;
                if (!mMoveAction) {
                    mMoveAction = std::make_unique<Action>();
                    mMoveAction->type = Action::Type::Move;
                    for (uint8_t i = 0; i < static_cast<uint8_t>(mMediaList.size()); i++) {
                        mMoveAction->imageIdxArray.push_back(Action::composeImageIndex(mMediaList[i]->id(), i));
                    }
                    mMoveAction->prevTopImageIdx = mTopImageIndex;
                    mMoveAction->prevCmpImageIdx = mCmpImageIndex;
                }

                const int nextItemIdx = i + (ImGui::GetMouseDragDelta(0).y < 0.0f ? -1 : 1);
                if (nextItemIdx >= 0 && nextItemIdx < imageNum) {
                    std::swap(mMediaList[i], mMediaList[nextItemIdx]);
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

            const GLuint texId = mMediaList[i]->texId();
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

    ImGui::PushFont(mSmallIconFont);
    const std::string importPropLbl = std::string(ICON_FA_FILE_IMPORT) + " Import##propImport";
    const float importPropBtnW = ImGui::CalcTextSize(importPropLbl.c_str()).x + g.Style.FramePadding.x * 2.f;
    const float propRowBtnsW = importPropBtnW + g.Style.ItemSpacing.x + (buttonSize.x + g.Style.ItemSpacing.x) * 3;
    ImGui::Dummy(Vec2f(propWindowWidth - propRowBtnsW - handleWidth, buttonSize.y));
    ImGui::SameLine();
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, g.Style.Colors[ImGuiCol_MenuBarBg]);

    ImGui::SameLine();
    if (ImGui::Button(importPropLbl.c_str(), ImVec2(0.f, buttonSize.y))) {
        showImportImageDlg();
    }
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Import Images"); }

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_SYNC_ALT "##ReloadImage", buttonSize) && mTopImageIndex > -1) {
        if (auto* si = getTopImage()) si->reload();
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
        } else if (!mMediaList.empty()) {
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


void App::renderFooter()
{
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGui::PushFont(mSmallFont);

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

#if defined(USE_VIDEO)
    auto* _vsFooter = videoSource();
    auto* _vsBFooter = videoSourceB();
    const bool videoFooterDims = _vsFooter && _vsFooter->isOpen();
    if (videoFooterDims) {
        auto footerVideoDurationStr = [](const VideoSource* p, char* out, size_t outSz) {
            if (!p || !p->isOpen() || !p->hasReliableDuration() || p->durationSec() <= 0.001) {
                std::snprintf(out, outSz, "?");
            } else {
                formatVideoHms(p->durationSec(), out, outSz);
            }
        };
        if (videoCompareActive() && _vsBFooter && _vsBFooter->isOpen()) {
            const bool dispSwap = mVideoSwapPresentationLR;
            const VideoSource* leftP = dispSwap ? _vsBFooter : _vsFooter;
            const VideoSource* rightP = dispSwap ? _vsFooter : _vsBFooter;
            const Vec2f leftSz(leftP->displayWidthForAspect(), leftP->displayHeightForAspect());
            const Vec2f rightSz(rightP->displayWidthForAspect(), rightP->displayHeightForAspect());

            const std::string pathLeft = dispSwap ? _vsBFooter->filepath() : _vsFooter->filepath();
            const std::string pathRight = dispSwap ? _vsFooter->filepath() : _vsBFooter->filepath();
            char szLeft[40];
            char szRight[40];
            footerFileSizeHuman(pathLeft, mFooterCachePathVL, mFooterCacheBytesVL, szLeft, sizeof szLeft);
            footerFileSizeHuman(pathRight, mFooterCachePathVR, mFooterCacheBytesVR, szRight, sizeof szRight);
            char durLeft[40];
            char durRight[40];
            footerVideoDurationStr(leftP, durLeft, sizeof durLeft);
            footerVideoDurationStr(rightP, durRight, sizeof durRight);

            ImGui::SameLine(g.Style.FramePadding.x + g.FontSize * 4.0f);
            ImGui::Text("%.0f x %.0f · %s · %s", leftSz.x, leftSz.y, durLeft, szLeft);
            Vec2f lcoords;
            if (getCompareSideVideoPixelAtMouse(g.IO, 0, lcoords)) {
                ImGui::SameLine();
                ImGui::Text("(%.0f, %.0f)", lcoords.x, leftSz.y - lcoords.y - 1.0f);
            }

            char rbuf[256];
            Vec2f rcoords;
            if (getCompareSideVideoPixelAtMouse(g.IO, 1, rcoords)) {
                sprintf_s(rbuf, sizeof rbuf, "(%.0f, %.0f) · %.0f x %.0f · %s · %s", rcoords.x,
                    rightSz.y - rcoords.y - 1.0f, rightSz.x, rightSz.y, durRight, szRight);
            } else {
                sprintf_s(rbuf, sizeof rbuf, "%.0f x %.0f · %s · %s", rightSz.x, rightSz.y, durRight, szRight);
            }
            const float rtextW = ImGui::CalcTextSize(rbuf).x;
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - rtextW);
            ImGui::TextUnformatted(rbuf);
        } else {
            char szV[40];
            footerFileSizeHuman(_vsFooter->filepath(), mFooterCachePathVSingle, mFooterCacheBytesVSingle, szV, sizeof szV);
            char durV[40];
            footerVideoDurationStr(_vsFooter, durV, sizeof durV);
            const Vec2f imageSize(_vsFooter->displayWidthForAspect(), _vsFooter->displayHeightForAspect());
            ImGui::SameLine(g.Style.FramePadding.x + g.FontSize * 4.0f);
            ImGui::Text("%.0f x %.0f · %s · %s", imageSize.x, imageSize.y, durV, szV);
            Vec2f imageCoords;
            if (getSingleVideoPixelAtMouse(g.IO, imageCoords)) {
                ImGui::SameLine();
                imageCoords.y = imageSize.y - imageCoords.y - 1;
                ImGui::Text("(%.0f, %.0f)", imageCoords.x, imageCoords.y);
            }
        }
    } else
#endif
    {
    const bool compareUi = inCompareMode() && mTopImageIndex >= 0 && mCmpImageIndex >= 0;

    if (compareUi) {
        const auto* leftIm  = getTopImage();
        const auto* rightIm = getCmpImage();
        const Vec2f leftSz = leftIm->size();
        const Vec2f rightSz = rightIm->size();

        const std::string& pathLeft = leftIm->filepath();
        const std::string& pathRight = rightIm->filepath();
        char szLeft[40];
        char szRight[40];
        footerFileSizeHuman(pathLeft, mFooterCachePathIL, mFooterCacheBytesIL, szLeft, sizeof szLeft);
        footerFileSizeHuman(pathRight, mFooterCachePathIR, mFooterCacheBytesIR, szRight, sizeof szRight);

        ImGui::SameLine(g.Style.FramePadding.x + g.FontSize * 4.0f);
        ImGui::Text("%.0f x %.0f · %s", leftSz.x, leftSz.y, szLeft);
        Vec2f lcoords;
        if (getCompareSideImagePixelAtMouse(Vec2f(g.IO.MousePos.x, g.IO.MousePos.y), 0, lcoords)) {
            ImGui::SameLine();
            ImGui::Text("(%.0f, %.0f)", lcoords.x, leftSz.y - lcoords.y - 1.0f);
        }

        char rbuf[192];
        Vec2f rcoords;
        if (getCompareSideImagePixelAtMouse(Vec2f(g.IO.MousePos.x, g.IO.MousePos.y), 1, rcoords)) {
            sprintf_s(rbuf, sizeof rbuf, "(%.0f, %.0f) · %.0f x %.0f · %s", rcoords.x,
                rightSz.y - rcoords.y - 1.0f, rightSz.x, rightSz.y, szRight);
        } else {
            sprintf_s(rbuf, sizeof rbuf, "%.0f x %.0f · %s", rightSz.x, rightSz.y, szRight);
        }
        const float rtextW = ImGui::CalcTextSize(rbuf).x;
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - rtextW);
        ImGui::TextUnformatted(rbuf);
    } else if (mTopImageIndex >= 0) {
        const Vec2f& imageSize = mMediaList[mTopImageIndex]->size();
        ImGui::SameLine(g.Style.FramePadding.x + g.FontSize * 4.0f);
        ImGui::Text("%.0f x %.0f", imageSize.x, imageSize.y);

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

    ImGui::Text("Attribute");
    ImGui::NextColumn();

    ImGui::Text("Value");
    ImGui::NextColumn();

    ImGui::Separator();
    ImGui::Text("Color Primaries");
    ImGui::Text("Color Encoding");
    ImGui::Text("Location");
    ImGui::Text("Resolution");
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
    ImGui::NextColumn();
}

void App::showVideoProperties()
{
#if !defined(USE_VIDEO)
    ImGui::TextUnformatted("Video is not available in this build.");
    return;
#else
    auto* _rProp = videoSource();
    if (!_rProp || !_rProp->isOpen()) {
        ImGui::TextUnformatted("No video loaded.");
        return;
    }

    ImGui::Columns(2, nullptr, false);
    ImGui::SetColumnWidth(0, 108.0f);

    ImGui::TextUnformatted("Property");
    ImGui::NextColumn();
    ImGui::TextUnformatted("Value");
    ImGui::NextColumn();
    ImGui::Separator();

    const bool cmp = videoCompareActive();
    if (cmp) {
        double tMin = 0.0;
        double tMax = 1.0;
        recomputeVideoScrubBounds(tMin, tMax);
        char tComp[32];
        char tEnd[32];
        formatVideoHms(mVideoCompositionT, tComp, sizeof tComp);
        formatVideoHms(tMax, tEnd, sizeof tEnd);
        ImGui::TextUnformatted("Composition T");
        ImGui::NextColumn();
        {
            char line[80];
            std::snprintf(line, sizeof line, "%s / %s", tComp, tEnd);
            ImGui::TextUnformatted(line);
        }
        ImGui::NextColumn();

        const bool dispSwap = mVideoSwapPresentationLR;
        char offL[48];
        char offR[48];
        std::snprintf(offL, sizeof offL, "%.3f s", dispSwap ? mVideoStartR : mVideoStartL);
        std::snprintf(offR, sizeof offR, "%.3f s", dispSwap ? mVideoStartL : mVideoStartR);
        ImGui::TextUnformatted("Left offset");
        ImGui::NextColumn();
        ImGui::TextUnformatted(offL);
        ImGui::NextColumn();
        ImGui::TextUnformatted("Right offset");
        ImGui::NextColumn();
        ImGui::TextUnformatted(offR);
        ImGui::NextColumn();
    }

    auto drawStream = [&](const char* sideTitle, const VideoSource* p, const std::string& path) {
        ImGui::Columns(1);
        ImGui::Spacing();
        ImGui::TextUnformatted(sideTitle);
        ImGui::Separator();
        ImGui::Columns(2, nullptr, false);
        ImGui::SetColumnWidth(0, 108.0f);

        if (!p || !p->isOpen()) {
            ImGui::TextUnformatted("Status");
            ImGui::NextColumn();
            ImGui::TextUnformatted("-");
            ImGui::NextColumn();
            return;
        }

        ImGui::TextUnformatted("File");
        ImGui::NextColumn();
        if (path.empty()) {
            ImGui::TextUnformatted("-");
        } else {
            ImGui::TextUnformatted(path.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", path.c_str());
            }
        }
        ImGui::NextColumn();

        ImGui::TextUnformatted("Coded size");
        ImGui::NextColumn();
        {
            char sz[64];
            std::snprintf(sz, sizeof sz, "%d x %d", p->width(), p->height());
            ImGui::TextUnformatted(sz);
        }
        ImGui::NextColumn();

        ImGui::TextUnformatted("Display size");
        ImGui::NextColumn();
        {
            char sz[64];
            std::snprintf(sz, sizeof sz, "%.0f x %.0f", static_cast<double>(p->displayWidthForAspect()),
                static_cast<double>(p->displayHeightForAspect()));
            ImGui::TextUnformatted(sz);
        }
        ImGui::NextColumn();

        ImGui::TextUnformatted("Duration");
        ImGui::NextColumn();
        if (p->hasReliableDuration() && p->durationSec() > 0.001) {
            char dur[32];
            formatVideoHms(p->durationSec(), dur, sizeof dur);
            ImGui::TextUnformatted(dur);
        } else {
            ImGui::TextUnformatted("Unknown");
        }
        ImGui::NextColumn();

        ImGui::TextUnformatted("Position");
        ImGui::NextColumn();
        {
            char pos[32];
            formatVideoHms(p->positionSec(), pos, sizeof pos);
            ImGui::TextUnformatted(pos);
        }
        ImGui::NextColumn();

        ImGui::TextUnformatted("Frame rate");
        ImGui::NextColumn();
        {
            const double fd = p->frameDurationSec();
            char fr[48];
            if (fd > 1e-6 && fd < 1.0) {
                std::snprintf(fr, sizeof fr, "%.2f fps", 1.0 / fd);
                ImGui::TextUnformatted(fr);
            } else {
                ImGui::TextUnformatted("-");
            }
        }
        ImGui::NextColumn();
    };

    if (cmp) {
        const bool dispSwap = mVideoSwapPresentationLR;
        auto* _rPropB = videoSourceB();
        const VideoSource* leftP = dispSwap ? _rPropB : _rProp;
        const VideoSource* rightP = dispSwap ? _rProp : _rPropB;
        const std::string leftPath = leftP ? leftP->filepath() : std::string();
        const std::string rightPath = rightP ? rightP->filepath() : std::string();
        drawStream("Left", leftP, leftPath);
        drawStream("Right", rightP, rightPath);
    } else {
        drawStream("Video", _rProp, _rProp->filepath());
    }

    ImGui::Columns(1);
#endif
}

void    App::renderInfoPopup(const char* name)
{
    ImGui::SetNextWindowContentWidth(500.0f);
    if (ImGui::BeginPopupModal(name, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Escape))) {
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::BeginTabBar("ControlsBar", ImGuiTabBarFlags_None)) {
            if (ImGui::BeginTabItem("Controls")) {
                ImGui::Columns(2, nullptr, false);

                ImGui::Text("Show Info Panel");
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
        auto* _rOvL = videoSource();
        if (isVideoMode() && _rOvL) {
            const bool dispSwap = mVideoSwapPresentationLR && videoCompareActive();
            auto* _rOvR = videoSourceB();
            const std::string& shownFp = (dispSwap && _rOvR) ? _rOvR->filepath() : _rOvL->filepath();
            ImGui::TextUnformatted(basename(shownFp));
        } else if (mTopImageIndex >= 0) {
            ImGui::TextUnformatted(mMediaList[mTopImageIndex]->filename().c_str());
        }
    }
    ImGui::End();

    if (isVideoMode()) {
        const bool dispSwap = mVideoSwapPresentationLR && videoCompareActive();
        auto* _rOvR = videoSourceB();
        if (!_rOvR) {
            return;
        }
        (void)dispSwap;
    } else {
        if (mCmpImageIndex < 0) {
            return;
        }
    }

    ImGuiContext& g = *ImGui::GetCurrentContext();
    const float imagePropHandleWidth = g.FontSize + g.Style.FramePadding.x * 4.0f;
    auto* _rOvR2 = videoSourceB();
    auto* _rOvL2 = videoSource();
    const std::string cmpImageName = isVideoMode()
        ? (_rOvR2 ? _rOvR2->filepath() : std::string())
        : mMediaList[mCmpImageIndex]->filename();
    const bool dispSwap = mVideoSwapPresentationLR && videoCompareActive();
    const std::string shownPath = isVideoMode()
        ? (dispSwap && _rOvL2 ? _rOvL2->filepath() : cmpImageName)
        : cmpImageName;
    const char* shownName = isVideoMode() ? basename(shownPath) : shownPath.c_str();
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
    if (ImGui::Begin("##ImageOverlayBar", nullptr, windowFlags)) {
        ImGui::Text("low");
        ImGui::SameLine();
        ImGui::Dummy(barSize);

        ImU32 hues[16 + 1];
        for (size_t i = 0; i <= 16; i++) {
            float r = i / 16.0f;
            float gg = glm::sin(180.0f * glm::radians(r));
            float b = glm::cos(60.0f * glm::radians(r));
            hues[i] = IM_COL32(r * 255, gg * 255, b * 255, 255);
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

}  // namespace baktsiu
