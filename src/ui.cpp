// The interface's entry points: fonts, style, the frame and keyboard shortcuts.
// The parts of the window live in ui_bars.cpp (top and bottom bars) and ui_views.cpp (viewport states, panels).
#include "ui.h"

#include <algorithm>

#include "ui_internal.h"
#include "viewer.h"

namespace htv {

using namespace ui;

void uiInit() {
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // nothing is written next to the exe
    io.LogFilename = nullptr;
    io.ConfigWindowsMoveFromTitleBarOnly = false;

    // Fonts are linked into the exe as resources (resources.rc).
    auto load = [&](int id, float* emFactor) -> ImFont* {
        HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));  // RT_RCDATA
        HGLOBAL mem = res ? LoadResource(nullptr, res) : nullptr;
        void* data = mem ? LockResource(mem) : nullptr;
        if (!data) return nullptr;
        const DWORD size = SizeofResource(nullptr, res);
        if (emFactor) *emFactor = lineHeightInEm(static_cast<const uint8_t*>(data), size);
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;
        return io.Fonts->AddFontFromMemoryTTF(data, int(size), 13.0f, &cfg);
    };
    fontRegular = load(101, &emRegular);
    fontMedium = load(102, nullptr);
    fontSemibold = load(103, nullptr);
    fontMono = load(104, &emMono);
    if (!fontRegular) fontRegular = io.Fonts->AddFontDefault();
    if (!fontMedium) fontMedium = fontRegular;
    if (!fontSemibold) fontSemibold = fontMedium;
    if (!fontMono) fontMono = fontRegular;
    io.FontDefault = fontRegular;
}

void uiSetScale(float dpiScale) {
    s = dpiScale;
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    st.WindowPadding = ImVec2(0, 0);
    st.ItemSpacing = ImVec2(0, 0);
    st.ItemInnerSpacing = ImVec2(0, 0);
    st.WindowBorderSize = 0;
    st.WindowRounding = 0;
    st.PopupRounding = 4;
    st.PopupBorderSize = 1;
    st.FrameRounding = 3;
    st.WindowMinSize = ImVec2(1, 1);
    st.ScaleAllSizes(s);
    st.FontSizeBase = 13.0f;
    st.FontScaleDpi = s;
    topBarH = S(32);
    bottomBarH = line() + S(sliderRowHeight()) + S(24);
}

UiColors uiColors() {
    const Theme& t = T();
    auto rgb = [](ImU32 c, float out[3]) {
        for (int i = 0; i < 3; ++i) out[i] = float((c >> (8 * i)) & 0xff) / 255.0f;  // IM_COL32 is ABGR
    };
    UiColors c;
    rgb(t.vp, c.viewport);
    rgb(t.ck1, c.checker1);
    rgb(t.ck2, c.checker2);
    return c;
}

float uiTopBarHeight() { return topBarH; }
float uiBottomBarHeight() { return bottomBarH; }

bool uiIsCaption(float x, float y) {
    if (!app.showBars || y < 0 || y >= topBarH) return false;
    for (const ImRect& r : holes)
        if (r.Contains(ImVec2(x, y))) return false;
    return true;
}

void uiFrame() {
    const Theme& t = T();
    ImGuiStyle& st = ImGui::GetStyle();
    st.Colors[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(t.tx);
    st.Colors[ImGuiCol_WindowBg] = ImGui::ColorConvertU32ToFloat4(t.pnl);
    st.Colors[ImGuiCol_PopupBg] = ImGui::ColorConvertU32ToFloat4(t.pop);
    st.Colors[ImGuiCol_Border] = ImGui::ColorConvertU32ToFloat4(t.bd2);
    st.Colors[ImGuiCol_FrameBg] = ImGui::ColorConvertU32ToFloat4(t.ctl);
    st.Colors[ImGuiCol_TextSelectedBg] = ImGui::ColorConvertU32ToFloat4(t.acs);

    // The bottom bar grows with the slider row: none without an image, taller for HDR.
    const float bottom = line() + S(sliderRowHeight()) + S(24);
    if (bottom != bottomBarH) {
        bottomBarH = bottom;
        relayout();
        requestRedraw();
    }

    clearMenuState();
    viewportOverlays();
    if (app.showBars) {
        topBar();
        bottomBar();
    }
    infoPanel();
    helpPanel();
    notices();
    drawTooltip();
    finishMenuFrame();
}

bool uiKeyDown(int vk) {
    if (ImGui::GetIO().WantTextInput) return false;
    const bool ctrl = GetKeyState(VK_CONTROL) & 0x8000, shift = GetKeyState(VK_SHIFT) & 0x8000,
               alt = GetKeyState(VK_MENU) & 0x8000;
    const bool image = app.hasImage, hdr = image && isHighPrecision(app.image);
    auto channel = [](Channel c) { app.channel = app.channel == c ? Channel::RGB : c; };
    auto exposure = [](float delta) { app.exposure = std::clamp(app.exposure + delta, -10.0f, 10.0f); };

    if (ctrl && vk >= '1' && vk <= '9') {
        openRecent(vk - '1');
        return true;
    }
    switch (vk) {
        // Files
        case 'O': if (ctrl) showOpenDialog(); else return false; break;
        case VK_LEFT:
        case VK_PRIOR:
        case VK_BACK: step(-1); break;
        case VK_RIGHT:
        case VK_NEXT: step(+1); break;
        case VK_HOME: stepTo(0); break;
        case VK_END: stepTo(-1); break;
        case VK_F5: reloadFile(); break;

        // View
        case 'F': app.fit = true; fitToWindow(); break;
        case '1': zoomCentered(1.0f); break;
        case '2': zoomCentered(2.0f); break;
        case '4': zoomCentered(4.0f); break;
        case '8': zoomCentered(8.0f); break;
        case VK_ADD: zoomCentered(app.zoom * 1.25f); break;
        case VK_SUBTRACT: zoomCentered(app.zoom / 1.25f); break;
        case 'L':
            if (ctrl) rotate(-1);
            else if (shift) app.dataIsLinear = !app.dataIsLinear;
            else return false;
            break;
        case 'R': if (ctrl) rotate(+1); else channel(Channel::R); break;
        case 'H': if (!ctrl) app.flipH = !app.flipH; else return false; break;
        case 'V':
            if (ctrl) pasteFromClipboard();
            else app.flipV = !app.flipV;
            break;
        case VK_TAB:
            toggleBars();
            if (!app.showBars) cleanHintUntil = ImGui::GetTime() + 1.4;
            break;

        // Channels and modes
        case 'G': if (ctrl) app.grid = !app.grid; else channel(Channel::G); break;
        case 'B': channel(Channel::B); break;
        case 'A':
            if (ctrl) { if (hdr) autoRange(); }
            else if (shift) cycleAlphaBackground();
            else channel(Channel::A);
            break;
        case 'Y': channel(Channel::Luminance); break;
        case 'N':  // off, reconstruct Z, reconstruct Z with green flipped
            if (!app.normalZ) app.normalZ = true;
            else if (!app.flipGreen) app.flipGreen = true;
            else app.normalZ = app.flipGreen = false;
            break;
        case VK_OEM_4:  // [
            app.forcedMip = app.forcedMip <= 0 ? -1 : app.forcedMip - 1;
            break;
        case VK_OEM_6:  // ]
            app.forcedMip = std::min(app.forcedMip + 1, std::max(app.textureMips - 1, 0));
            break;
        case 'T':
            if (ctrl) setTopmost(!app.topmost);
            else setTiling(Tiling((int(app.tiling) + 1) % 3));
            break;

        // HDR
        case VK_OEM_MINUS: if (hdr) exposure(-0.5f); break;
        case VK_OEM_PLUS: if (hdr) exposure(+0.5f); break;
        case '0': app.exposure = 0.0f; break;
        case 'Z': app.showClipping = !app.showClipping; break;

        // Analysis
        case 'P': app.pixelOutline = !app.pixelOutline; break;
        case 'C':
            if (ctrl && alt) copyFilePath();
            else if (ctrl && shift) copyAllColorFormats();
            else if (ctrl) copyColorValue();
            else app.channel = Channel::RGB;
            break;
        case 'I': app.showInfo = !app.showInfo; break;

        // Window
        case VK_F1: app.showHelp = !app.showHelp; break;
        case 'W': if (ctrl) DestroyWindow(app.hwnd); else return false; break;
        case VK_ESCAPE:
            if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return false;  // ImGui closes the menu
            if (app.loading) cancelLoading();
            else if (app.showHelp) app.showHelp = false;
            else if (app.showInfo) app.showInfo = false;
            else DestroyWindow(app.hwnd);
            break;
        default: return false;
    }
    return true;
}

}  // namespace htv
