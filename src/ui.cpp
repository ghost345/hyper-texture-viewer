// The interface's entry points: fonts, style, the frame and keyboard shortcuts.
// The parts of the window live in ui_bars.cpp (top and bottom bars) and ui_views.cpp (viewport states, panels).
#include "ui.h"

#include <algorithm>

#include "keys.h"
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
    if (app.customViewport)
        for (int i = 0; i < 3; ++i) c.viewport[i] = app.viewportColor[i];
    rgb(t.ck1, c.checker1);
    rgb(t.ck2, c.checker2);
    return c;
}

float uiTopBarHeight() { return topBarH; }
float uiBottomBarHeight() { return bottomBarH; }
float uiMinWidth() { return ui::topBarMinWidth(); }

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
    histogramPanel();
    colorMatchPanel();
    comparePanel();
    framesPanel();
    settingsPanel();
    helpPanel();
    notices();
    drawTooltip();
    finishMenuFrame();
}

namespace {

void runAction(Action a) {
    const bool hdr = app.hasImage && isHighPrecision(app.image);
    auto channel = [](Channel c) { app.channel = app.channel == c ? Channel::RGB : c; };
    auto exposure = [](float delta) { app.exposure = std::clamp(app.exposure + delta, -10.0f, 10.0f); };
    switch (a) {
        case Action::Open: showOpenDialog(); break;
        case Action::PrevFile: step(-1); break;
        case Action::NextFile: step(+1); break;
        case Action::FirstFile: stepTo(0); break;
        case Action::LastFile: stepTo(-1); break;
        case Action::Paste: pasteFromClipboard(); break;
        case Action::CopyPath: copyFilePath(); break;
        case Action::Reload: reloadFile(); break;

        case Action::Fit: app.fit = true; fitToWindow(); break;
        case Action::ActualSize: zoomCentered(1.0f); break;
        case Action::Zoom200: zoomCentered(2.0f); break;
        case Action::Zoom400: zoomCentered(4.0f); break;
        case Action::Zoom800: zoomCentered(8.0f); break;
        case Action::ZoomIn: zoomCentered(app.zoom * 1.25f); break;
        case Action::ZoomOut: zoomCentered(app.zoom / 1.25f); break;
        case Action::RotateLeft: rotate(-1); break;
        case Action::RotateRight: rotate(+1); break;
        case Action::FlipH: app.flipH = !app.flipH; break;
        case Action::FlipV: app.flipV = !app.flipV; break;
        case Action::HidePanels:
            toggleBars();
            if (!app.showBars) cleanHintUntil = ImGui::GetTime() + 1.4;
            break;

        case Action::AllChannels: app.channel = Channel::RGB; break;
        case Action::Red: channel(Channel::R); break;
        case Action::Green: channel(Channel::G); break;
        case Action::Blue: channel(Channel::B); break;
        case Action::Alpha: channel(Channel::A); break;
        case Action::Luminance: channel(Channel::Luminance); break;
        case Action::SrgbLinear: app.dataIsLinear = !app.dataIsLinear; break;
        case Action::NormalMode:  // off, reconstruct Z, reconstruct Z with green flipped
            if (!app.normalZ) app.normalZ = true;
            else if (!app.flipGreen) app.flipGreen = true;
            else app.normalZ = app.flipGreen = false;
            break;
        case Action::MipDown: app.forcedMip = app.forcedMip <= 0 ? -1 : app.forcedMip - 1; break;
        case Action::MipUp: app.forcedMip = std::min(app.forcedMip + 1, std::max(app.textureMips - 1, 0)); break;
        case Action::AlphaBackground: cycleAlphaBackground(); break;
        case Action::Tiling: setTiling(Tiling((int(app.tiling) + 1) % 3)); break;

        case Action::ExposureDown: if (hdr) exposure(-0.5f); break;
        case Action::ExposureUp: if (hdr) exposure(+0.5f); break;
        case Action::ResetExposure: app.exposure = 0.0f; break;
        case Action::AutoRange: if (hdr) autoRange(); break;
        case Action::Clipping: app.showClipping = !app.showClipping; break;

        case Action::Grid: app.grid = !app.grid; break;
        case Action::PixelOutline: app.pixelOutline = !app.pixelOutline; break;
        case Action::LockPixel: toggleLockPixel(); break;
        case Action::CopyValue: copyColorValue(); break;
        case Action::CopyAllFormats: copyAllColorFormats(); break;
        case Action::ColorMatch: toggleColorMatch(); break;
        case Action::MatchPrevRegion:
            if (app.showFrames && isAnimation()) { app.framePlaying = false; showFrame(app.frameIndex - 1); }
            else stepMatchRegion(-1);
            break;
        case Action::MatchNextRegion:
            if (app.showFrames && isAnimation()) { app.framePlaying = false; showFrame(app.frameIndex + 1); }
            else stepMatchRegion(+1);
            break;
        case Action::Histogram: app.showHistogram = !app.showHistogram; break;
        case Action::Info: app.showInfo = !app.showInfo; break;
        case Action::Layers: toggleFramesOrLayers(); break;
        case Action::Compare: toggleCompare(); break;

        case Action::AlwaysOnTop: setTopmost(!app.topmost); break;
        case Action::ClickThrough: setClickThrough(!app.clickThrough); break;
        case Action::Settings: app.showSettings = !app.showSettings; break;
        case Action::Help: app.showHelp = !app.showHelp; break;
        case Action::Quit: DestroyWindow(app.hwnd); break;
        case Action::Count: break;
    }
}

}  // namespace

bool uiKeyDown(int vk) {
    if (ImGui::GetIO().WantTextInput) return false;
    const bool ctrl = GetKeyState(VK_CONTROL) & 0x8000, shift = GetKeyState(VK_SHIFT) & 0x8000,
               alt = GetKeyState(VK_MENU) & 0x8000;
    if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN) return false;
    const KeyBinding pressed{uint16_t(vk), uint8_t((ctrl ? kModCtrl : 0) | (shift ? kModShift : 0) | (alt ? kModAlt : 0))};
    if (settingsKeyDown(pressed)) return true;  // Settings is waiting for a new key

    // Fixed keys: recent files and Esc.
    if (pressed.mods == kModCtrl && vk >= '1' && vk <= '9') {
        openRecent(vk - '1');
        return true;
    }
    if (vk == VK_ESCAPE) {
        if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return false;  // ImGui closes the menu
        if (app.loading) cancelLoading();
        else if (app.showSettings) app.showSettings = false;
        else if (app.showHelp) app.showHelp = false;
        else if (app.matchPicking) app.matchPicking = false;
        else if (app.colorMatch) app.colorMatch = false;
        else if (app.compare) app.compare = false;
        else if (app.showFrames) app.showFrames = false;
        else if (app.showInfo) app.showInfo = false;
        else if (app.showHistogram) app.showHistogram = false;
        else DestroyWindow(app.hwnd);
        return true;
    }
    const Action a = actionFor(pressed);
    if (a == Action::Count) return false;
    runAction(a);
    return true;
}

}  // namespace htv
