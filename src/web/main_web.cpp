// Hyper Texture Viewer in the browser: a canvas filling the page, WebGL 2 (render_web.cpp), files the page hands
// over (files_web.cpp) and the same interface as on Windows. Input follows main.cpp's window procedure.

#include "viewer.h"

#include <emscripten.h>
#include <emscripten/html5.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "keys.h"
#include "ui.h"

EM_JS(void, js_set_cursor, (const char* cursor), { Module.canvas.style.cursor = UTF8ToString(cursor); });
EM_JS(void, js_copy_text, (const char* text), {
    const t = UTF8ToString(text);
    if (navigator.clipboard) navigator.clipboard.writeText(t).catch(() => {});
});
EM_JS(int, js_prefers_dark, (), { return window.matchMedia('(prefers-color-scheme: dark)').matches ? 1 : 0; });
EM_JS(void, js_toggle_fullscreen, (), {
    if (document.fullscreenElement) document.exitFullscreen();
    else document.documentElement.requestFullscreen().catch(() => {});
});
EM_JS(int, js_fullscreen, (), { return document.fullscreenElement ? 1 : 0; });

namespace htv {

App app;

// files_web.cpp
void decodeArrived();
bool decodePending();

double nowMs() { return emscripten_get_now(); }
double msSinceProcessStart() { return nowMs(); }

// wchar_t holds a whole code point here.
std::wstring widen(const std::string& s) {
    std::wstring w;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        uint32_t cp = n == 1 ? c : n == 2 ? c & 0x1F : n == 3 ? c & 0x0F : c & 0x07;
        for (int k = 1; k < n && i + k < s.size(); ++k) cp = cp << 6 | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        w += wchar_t(cp);
        i += size_t(n);
    }
    return w;
}

std::string narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t wc : w) {
        const uint32_t c = uint32_t(wc);
        if (c < 0x80) {
            s += char(c);
        } else if (c < 0x800) {
            s += char(0xC0 | c >> 6);
            s += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            s += char(0xE0 | c >> 12);
            s += char(0x80 | (c >> 6 & 0x3F));
            s += char(0x80 | (c & 0x3F));
        } else {
            s += char(0xF0 | c >> 18);
            s += char(0x80 | (c >> 12 & 0x3F));
            s += char(0x80 | (c >> 6 & 0x3F));
            s += char(0x80 | (c & 0x3F));
        }
    }
    return s;
}

void writeBench(bool) {}

namespace {

double lastInputMs = 0;
bool redraw = true;
bool spaceDown = false;
double lastZebraMs = 0;
bool keyHeld[256] = {};

void input() { lastInputMs = nowMs(); }

// KeyboardEvent.code, which names the key's place whatever the layout, as a Windows virtual-key code.
int virtualKey(const char* code) {
    const std::string c = code;
    if (c.size() == 4 && c.compare(0, 3, "Key") == 0) return c[3];
    if (c.size() == 6 && c.compare(0, 5, "Digit") == 0) return c[5];
    if (c.size() == 7 && c.compare(0, 6, "Numpad") == 0 && c[6] >= '0' && c[6] <= '9') return VK_NUMPAD0 + (c[6] - '0');
    if (c.size() >= 2 && c[0] == 'F' && std::isdigit(static_cast<unsigned char>(c[1]))) {
        const int n = std::atoi(c.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    static const struct {
        const char* code;
        int vk;
    } kKeys[] = {{"Escape", VK_ESCAPE}, {"Space", VK_SPACE}, {"Enter", VK_RETURN}, {"NumpadEnter", VK_RETURN},
                 {"Tab", VK_TAB}, {"Backspace", VK_BACK}, {"Pause", VK_PAUSE}, {"PageUp", VK_PRIOR},
                 {"PageDown", VK_NEXT}, {"End", VK_END}, {"Home", VK_HOME}, {"ArrowLeft", VK_LEFT},
                 {"ArrowUp", VK_UP}, {"ArrowRight", VK_RIGHT}, {"ArrowDown", VK_DOWN}, {"Insert", VK_INSERT},
                 {"Delete", VK_DELETE}, {"ShiftLeft", VK_SHIFT}, {"ShiftRight", VK_SHIFT},
                 {"ControlLeft", VK_CONTROL}, {"ControlRight", VK_CONTROL}, {"AltLeft", VK_MENU},
                 {"AltRight", VK_MENU}, {"MetaLeft", VK_LWIN}, {"MetaRight", VK_RWIN},
                 {"NumpadMultiply", VK_MULTIPLY}, {"NumpadAdd", VK_ADD}, {"NumpadSubtract", VK_SUBTRACT},
                 {"NumpadDecimal", VK_DECIMAL}, {"NumpadDivide", VK_DIVIDE}, {"Semicolon", VK_OEM_1},
                 {"Equal", VK_OEM_PLUS}, {"Comma", VK_OEM_COMMA}, {"Minus", VK_OEM_MINUS},
                 {"Period", VK_OEM_PERIOD}, {"Slash", VK_OEM_2}, {"Backquote", VK_OEM_3},
                 {"BracketLeft", VK_OEM_4}, {"Backslash", VK_OEM_5}, {"BracketRight", VK_OEM_6},
                 {"Quote", VK_OEM_7}};
    for (const auto& k : kKeys)
        if (c == k.code) return k.vk;
    return 0;
}

ImGuiKey imguiKey(int vk) {
    if (vk >= 'A' && vk <= 'Z') return ImGuiKey(ImGuiKey_A + (vk - 'A'));
    if (vk >= '0' && vk <= '9') return ImGuiKey(ImGuiKey_0 + (vk - '0'));
    if (vk >= VK_F1 && vk < VK_F1 + 12) return ImGuiKey(ImGuiKey_F1 + (vk - VK_F1));
    switch (vk) {
        case VK_TAB: return ImGuiKey_Tab;
        case VK_LEFT: return ImGuiKey_LeftArrow;
        case VK_RIGHT: return ImGuiKey_RightArrow;
        case VK_UP: return ImGuiKey_UpArrow;
        case VK_DOWN: return ImGuiKey_DownArrow;
        case VK_PRIOR: return ImGuiKey_PageUp;
        case VK_NEXT: return ImGuiKey_PageDown;
        case VK_HOME: return ImGuiKey_Home;
        case VK_END: return ImGuiKey_End;
        case VK_INSERT: return ImGuiKey_Insert;
        case VK_DELETE: return ImGuiKey_Delete;
        case VK_BACK: return ImGuiKey_Backspace;
        case VK_SPACE: return ImGuiKey_Space;
        case VK_RETURN: return ImGuiKey_Enter;
        case VK_ESCAPE: return ImGuiKey_Escape;
        case VK_SHIFT: return ImGuiKey_LeftShift;
        case VK_CONTROL: return ImGuiKey_LeftCtrl;
        case VK_MENU: return ImGuiKey_LeftAlt;
        default: return ImGuiKey_None;
    }
}

void setModifiers(bool ctrl, bool shift, bool alt) {
    keyHeld[VK_CONTROL] = ctrl;
    keyHeld[VK_SHIFT] = shift;
    keyHeld[VK_MENU] = alt;
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
    io.AddKeyEvent(ImGuiMod_Shift, shift);
    io.AddKeyEvent(ImGuiMod_Alt, alt);
}

bool uiMouse() { return ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse; }

float dpr() { return float(emscripten_get_device_pixel_ratio()); }

// Mouse positions in canvas pixels; the canvas fills the page from its top-left corner.
void mousePos(const EmscriptenMouseEvent* e, int& x, int& y) {
    x = int(std::lround(double(e->clientX) * dpr()));
    y = int(std::lround(double(e->clientY) * dpr()));
}

void updateCursor() {
    static std::string current;
    std::string want = "default";
    if (app.colorMatch && app.hasImage && !uiMouse() && !app.dragging && !spaceDown) want = "crosshair";
    else if ((app.dragging || spaceDown) && app.hasImage && !uiMouse()) want = "move";
    if (want != current) {
        current = want;
        js_set_cursor(want.c_str());
    }
}

void resize() {
    double cssW = 0, cssH = 0;
    emscripten_get_element_css_size("#canvas", &cssW, &cssH);
    const float scale = dpr();
    app.winW = std::max(1, int(std::lround(cssW * scale)));
    app.winH = std::max(1, int(std::lround(cssH * scale)));
    emscripten_set_canvas_element_size("#canvas", app.winW, app.winH);
    ImGui::GetIO().DisplaySize = ImVec2(float(app.winW), float(app.winH));
    if (std::fabs(app.dpiScale - scale) > 0.01f) {
        app.dpiScale = scale;
        applyInterfaceScale();
    }
    layout();
    if (app.fit) fitToWindow();
    redraw = true;
}

EM_BOOL onResize(int, const EmscriptenUiEvent*, void*) {
    resize();
    input();
    return EM_FALSE;
}

EM_BOOL onMouseDown(int, const EmscriptenMouseEvent* e, void*) {
    input();
    int x, y;
    mousePos(e, x, y);
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(float(x), float(y));
    setModifiers(e->ctrlKey, e->shiftKey, e->altKey);
    if (e->button <= 2) io.AddMouseButtonEvent(e->button == 0 ? 0 : e->button == 1 ? 2 : 1, true);
    if (uiMouse() || !inViewport(x, y)) return EM_TRUE;
    if (e->button == 0 || e->button == 1) startPan(float(x), float(y), e->button == 0 && !spaceDown);
    return EM_TRUE;
}

EM_BOOL onMouseUp(int, const EmscriptenMouseEvent* e, void*) {
    input();
    if (e->button <= 2) ImGui::GetIO().AddMouseButtonEvent(e->button == 0 ? 0 : e->button == 1 ? 2 : 1, false);
    if (e->button == 0 || e->button == 1) endPan();
    return EM_FALSE;
}

EM_BOOL onMouseMove(int, const EmscriptenMouseEvent* e, void*) {
    input();
    int x, y;
    mousePos(e, x, y);
    ImGui::GetIO().AddMousePosEvent(float(x), float(y));
    const bool inside = x >= 0 && y >= 0 && x < app.winW && y < app.winH;
    app.mouseX = inside || app.dragging ? x : -1;
    app.mouseY = inside || app.dragging ? y : -1;
    movePan(float(x), float(y));
    return EM_FALSE;
}

EM_BOOL onMouseLeave(int, const EmscriptenMouseEvent*, void*) {
    if (!app.dragging) {
        app.mouseX = app.mouseY = -1;
        ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
    redraw = true;
    return EM_FALSE;
}

EM_BOOL onDoubleClick(int, const EmscriptenMouseEvent* e, void*) {
    input();
    int x, y;
    mousePos(e, x, y);
    if (uiMouse() || !inViewport(x, y) || e->button != 0) return EM_FALSE;
    imageDoubleClicked();
    return EM_TRUE;
}

// Wheel notches: 100 pixels a notch, as Windows browsers scroll; trackpads send fractions of one.
double wheelNotches(const EmscriptenWheelEvent* e) {
    double d = e->deltaY;
    if (e->deltaMode == DOM_DELTA_LINE) d *= 100.0 / 3.0;
    else if (e->deltaMode == DOM_DELTA_PAGE) d *= 100.0;
    return -d / 100.0;
}

EM_BOOL onWheel(int, const EmscriptenWheelEvent* e, void*) {
    input();
    int x, y;
    mousePos(&e->mouse, x, y);
    const double notches = wheelNotches(e);
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(float(x), float(y));
    io.AddMouseWheelEvent(0.0f, float(notches));
    if (uiMouse() || !inViewport(x, y)) return EM_TRUE;
    if (e->mouse.ctrlKey) {
        // Ctrl+wheel would zoom the page; here it is exposure, as on Windows.
        if (app.hasImage && isHighPrecision(app.image))
            app.exposure = std::clamp(app.exposure + 0.25f * float(notches), -10.0f, 10.0f);
        return EM_TRUE;
    }
    if (notches != 0) {
        const float z = app.zoom, next = z * float(std::pow(1.25, notches));
        zoomAt((z < 1.0f && next > 1.0f) || (z > 1.0f && next < 1.0f) ? 1.0f : next, float(x), float(y));  // stop at 100%
    }
    return EM_TRUE;
}

// Keys the browser keeps for itself whatever the page does, or must keep: leave them alone.
bool browserKey(int vk, bool ctrl) {
    if (ctrl && (vk == 'T' || vk == 'W' || vk == 'N' || vk == 'L' || vk == VK_TAB)) return true;
    if (ctrl && (vk == 'C' || vk == 'X' || vk == 'A')) return ImGui::GetIO().WantTextInput;
    if (vk == VK_F1 + 10) return true;  // F11, full screen
    return false;
}

EM_BOOL onKeyDown(int, const EmscriptenKeyboardEvent* e, void*) {
    input();
    const int vk = virtualKey(e->code);
    setModifiers(e->ctrlKey || e->metaKey, e->shiftKey, e->altKey);
    if (vk > 0 && vk < 256) keyHeld[vk] = true;
    ImGuiIO& io = ImGui::GetIO();
    if (const ImGuiKey k = imguiKey(vk)) io.AddKeyEvent(k, true);
    const bool ctrl = e->ctrlKey || e->metaKey;
    if (!ctrl && std::strlen(e->key) > 0 && static_cast<unsigned char>(e->key[0]) >= 0x20 &&
        (std::strlen(e->key) == 1 || static_cast<unsigned char>(e->key[0]) >= 0x80))
        io.AddInputCharactersUTF8(e->key);
    if (io.WantTextInput) return vk == VK_TAB || vk == VK_BACK;
    if (vk == 0 || browserKey(vk, ctrl)) return EM_FALSE;
    // Paste comes through the page's paste event, which only fires when the key is left to the browser.
    const KeyBinding pressed{uint16_t(vk), uint8_t((ctrl ? kModCtrl : 0) | (e->shiftKey ? kModShift : 0) |
                                                   (e->altKey ? kModAlt : 0))};
    if (actionFor(pressed) == Action::Paste) return EM_FALSE;
    if (vk == VK_SPACE) {
        if (app.showFrames && isAnimation()) {
            if (!e->repeat) {
                app.framePlaying = !app.framePlaying;
                app.frameShownMs = nowMs();
                redraw = true;
            }
            return EM_TRUE;
        }
        spaceDown = true;
        return EM_TRUE;
    }
    if (uiKeyDown(vk)) return EM_TRUE;
    // Alt alone would focus the browser's menu bar on Windows.
    return vk == VK_MENU || vk == VK_F1 + 4 || vk == VK_F1 + 6;  // F5 reloads the page, F7 caret browsing
}

EM_BOOL onKeyUp(int, const EmscriptenKeyboardEvent* e, void*) {
    input();
    const int vk = virtualKey(e->code);
    setModifiers(e->ctrlKey || e->metaKey, e->shiftKey, e->altKey);
    if (vk > 0 && vk < 256) keyHeld[vk] = false;
    if (const ImGuiKey k = imguiKey(vk)) ImGui::GetIO().AddKeyEvent(k, false);
    if (vk == VK_SPACE) spaceDown = false;
    return EM_FALSE;
}

EM_BOOL onBlur(int, const EmscriptenFocusEvent*, void*) {
    std::fill(std::begin(keyHeld), std::end(keyHeld), false);
    spaceDown = false;
    app.dragging = false;
    setModifiers(false, false, false);
    return EM_FALSE;
}

void setClipboardText(ImGuiContext*, const char* text) { js_copy_text(text); }

double lastFrameMs = 0;
bool decodeNext = false;  // a frame showed the loading state: decode the file that arrived

void frame() {
    const double now = nowMs();
    // Decoding blocks the page, so the loading badge goes up in a frame of its own first.
    if (decodeNext) {
        decodeNext = false;
        decodeArrived();
        redraw = true;
    }
    if (isAnimation() && app.framePlaying) advanceFrame();
    const bool zebra = app.showClipping && app.hasImage && isHighPrecision(app.image);
    if (zebra && now - lastZebraMs > 66) {
        lastZebraMs = now;
        redraw = true;
    }
    const bool active = now - lastInputMs < 1500.0 || app.dragging || app.loading || redraw;
    if (!active && !decodePending()) return;
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = lastFrameMs > 0 ? float(std::max(now - lastFrameMs, 1.0) / 1000.0) : 1.0f / 60.0f;
    lastFrameMs = now;
    redraw = false;
    render();
    updateCursor();
    if (decodePending()) decodeNext = true;
}

}  // namespace

void requestRedraw() { redraw = true; }

// Fonts are files embedded in the program (--embed-file), read once and kept.
bool fontResource(int id, const void*& data, size_t& size) {
    static const char* const names[] = {"/fonts/Inter-Regular.ttf", "/fonts/Inter-Medium.ttf",
                                        "/fonts/Inter-SemiBold.ttf", "/fonts/JetBrainsMono-Regular.ttf"};
    static std::vector<uint8_t> fonts[4];
    if (id < 101 || id > 104) return false;
    std::vector<uint8_t>& f = fonts[id - 101];
    if (f.empty()) {
        FILE* file = std::fopen(names[id - 101], "rb");
        if (!file) return false;
        std::fseek(file, 0, SEEK_END);
        f.resize(size_t(std::ftell(file)));
        std::fseek(file, 0, SEEK_SET);
        if (std::fread(f.data(), 1, f.size(), file) != f.size()) f.clear();
        std::fclose(file);
    }
    data = f.data();
    size = f.size();
    return !f.empty();
}

void applyTheme() {
    app.darkTheme = app.themeMode == 2 ? js_prefers_dark() != 0 : app.themeMode != 1;
    redraw = true;
}

// The window's own powers, which a page doesn't have.
void setTopmost(bool) {}
void setOpacity(float) {}
void setClickThrough(bool) {}
void minimizeWindow() {}
void quitApp() {}
void openDefaultApps() {}
void toggleMaximize() { js_toggle_fullscreen(); }
bool windowMaximized() { return js_fullscreen() != 0; }

}  // namespace htv

using namespace htv;

extern "C" EMSCRIPTEN_KEEPALIVE void htv_theme_changed() {
    if (app.themeMode == 2) applyTheme();
}

short GetKeyState(int virtualKey) {
    return virtualKey > 0 && virtualKey < 256 && keyHeld[virtualKey] ? short(0x8000) : 0;
}

int main() {
    app.tStart = nowMs();
    app.cacheMB = 512;  // of the 4 GB a page can have at most
    app.prefetch = 0;
    app.autoReload = false;
    app.singleInstance = false;
    loadSettings();
    applyTheme();

    EmscriptenWebGLContextAttributes attrs;
    emscripten_webgl_init_context_attributes(&attrs);
    attrs.majorVersion = 2;
    attrs.minorVersion = 0;
    attrs.alpha = EM_FALSE;
    attrs.depth = EM_FALSE;
    attrs.stencil = EM_FALSE;
    attrs.antialias = EM_FALSE;
    attrs.premultipliedAlpha = EM_FALSE;
    attrs.preserveDrawingBuffer = EM_TRUE;  // frames are drawn only when something changes
    const EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = emscripten_webgl_create_context("#canvas", &attrs);
    if (ctx <= 0) {
        EM_ASM({ Module.htvFail('This browser has no WebGL 2, which the viewer needs.'); });
        return 1;
    }
    emscripten_webgl_make_context_current(ctx);
    std::string error;
    if (!createPipeline(error)) {
        EM_ASM({ Module.htvFail('Cannot start WebGL: ' + UTF8ToString($0)); }, error.c_str());
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.BackendPlatformName = "htv_web";
    ImGui::GetPlatformIO().Platform_SetClipboardTextFn = setClipboardText;
    ImGui_ImplOpenGL3_Init("#version 300 es");
    uiInit();
    app.dpiScale = 0;  // resize() applies the page's scale
    app.scale = dpr();
    uiSetScale(app.scale);
    resize();

    emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_FALSE, onResize);
    emscripten_set_mousedown_callback("#canvas", nullptr, EM_FALSE, onMouseDown);
    emscripten_set_mouseup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_FALSE, onMouseUp);
    emscripten_set_mousemove_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_FALSE, onMouseMove);
    emscripten_set_mouseleave_callback("#canvas", nullptr, EM_FALSE, onMouseLeave);
    emscripten_set_dblclick_callback("#canvas", nullptr, EM_FALSE, onDoubleClick);
    emscripten_set_wheel_callback("#canvas", nullptr, EM_FALSE, onWheel);
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_FALSE, onKeyDown);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_FALSE, onKeyUp);
    emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_FALSE, onBlur);

    EM_ASM({ Module.htvReady(); });
    emscripten_set_main_loop(frame, 0, false);
    return 0;
}
