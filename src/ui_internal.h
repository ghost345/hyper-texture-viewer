// Building blocks shared by the interface files: design tokens, fonts, text, tooltips, menus and controls.
#pragma once

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "app.h"
#include "icons.h"
#include "keys.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace htv::ui {

// ---- design tokens (the Tokens artboard) ----

struct Theme {
    ImU32 vp, bar, bar2, pop, pnl, ctl, ctlh, ctlp, bd, bd2, tx, tx2, tx3, ac, acf, act, acs, warn, err, ok, ck1, ck2,
        kn, knb;
};

constexpr ImU32 hex(uint32_t rgb, float alpha = 1.0f) {
    return IM_COL32((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, int(alpha * 255.0f + 0.5f));
}

const Theme& T();
constexpr ImU32 kWhite = hex(0xffffff);
constexpr ImU32 kCloseHover = hex(0xc42b1c);
constexpr ImU32 kNan = hex(0xff00ff);
ImU32 fade(ImU32 c, float alpha);

// ---- fonts and text ----

extern ImFont* fontRegular;
extern ImFont* fontMedium;
extern ImFont* fontSemibold;
extern ImFont* fontMono;

extern float s;  // DPI scale
inline float S(float v) { return std::round(v * s); }               // design pixels to physical pixels
inline float line() { return std::max(1.0f, std::floor(s)); }      // 1 px borders

ImVec2 textSize(ImFont* f, float size, const char* t, const char* end = nullptr);
// Text whose line box is vertically centred on cy.
void drawText(ImDrawList* dl, ImFont* f, float size, float x, float cy, ImU32 col, const char* t,
              const char* end = nullptr);
float lineHeightInEm(const uint8_t* ttf, size_t size);
extern float emRegular, emMono;
std::string ellipsize(ImFont* f, float size, const std::string& t, float maxWidth);
std::string formatZoom(float zoom);
std::string formatBytes(uint64_t n);
std::string formatBytesExact(uint64_t n);  // "1 398 256 B"
std::string utf8(const std::wstring& w);
std::string fileName();  // the current file's name, or "Clipboard"
std::string rgbaOrder(std::string text);  // EXR lists channels alphabetically: "A,B,G,R" reads as "RGBA"

// ---- frame bookkeeping ----

extern float topBarH, bottomBarH;
extern std::vector<ImRect> holes;  // last frame's interactive top bar areas
void addHole(ImRect r);  // an interactive area of the top bar; the rest of the bar drags the window

// Tooltip below the hovered item after a short delay (above it with `above`).
void requestTip(ImRect r, const char* text, const char* key, bool above = false);
void drawTooltip();
// Shortcut key chip. Returns its width. `help` is the Help screen's taller variant.
float kbdChip(ImDrawList* dl, float x, float cy, const char* key, bool draw = true, bool help = false);
void spinner(ImDrawList* dl, ImVec2 center, float radius, ImU32 color);
// "Copied" flash next to a value that was clicked.
void flashCopied(ImGuiID id);
bool copiedRecently(ImGuiID id);

// ---- menus ----

void toggleMenuOnPress(bool pressed, const char* id);
bool beginMenu(const char* id, ImVec2 pos, ImVec2 pivot = ImVec2(0, 0), float minWidth = 0);
void endMenu();
void clearMenuState();
void finishMenuFrame();

struct MenuItem {
    const char* label;
    const char* key = nullptr;  // shortcut on the right, in muted mono
    bool checked = false;
    bool enabled = true;
    bool checkColumn = false;   // leave room for a check mark
    Icon icon = Icon::Count;    // 14 px icon in front of the label
    const char* value = nullptr;  // value on the right, in secondary mono (instead of `key`)
};
bool menuItem(const MenuItem& m);
// An item that opens a submenu to its right while hovered. Returns true while the submenu is open:
// then call endSubmenu() after its items.
bool submenu(const char* id, const char* label, Icon icon, bool checkColumn = false);
void endSubmenu();
void menuHeading(const char* label);
void menuNote(const char* text, const char* key = nullptr, const char* after = nullptr);
void menuSeparator();

// ---- controls ----

enum ButtonFlags { kOn = 1, kDisabled = 2, kChevron = 4, kMonoLabel = 8, kBordered = 16, kOpen = 32 };

// Lays out a row of controls left to right; with `measure` set it only adds up their widths.
struct Row {
    ImDrawList* dl = nullptr;
    float x = 0;
    float cy = 0;  // vertical centre of the row
    float gap = 0;
    bool measure = false;
    bool topBar = false;  // tooltips below, and controls cut holes in the draggable caption

    void advance(float w) { x += w + gap; }
    void hole(ImRect r) {
        if (topBar) addHole(r);
    }
    bool button(const char* id, Icon icon, const char* label, int flags, const char* tip, const char* key,
                float iconSize = 16, float height = 24, float padding = -1);
    // `widest` is the longest value the dropdown can show: its width does not change with the value.
    bool dropdown(const char* id, const char* value, ImFont* f, float fontSize, float height, float padL, float padR,
                  const char* tip, const char* key, ImRect* outRect = nullptr, bool disabled = false,
                  const char* widest = nullptr, float minWidth = 0);  // minWidth in physical pixels
    void separator(float height, float margin, ImU32 color = 0);
    void text(const char* t, ImFont* f, float size, ImU32 col, float marginL = 0, float marginR = 0);
    // Text with a tooltip that takes at least the width of `widest`, so changing values keep the row still.
    void field(const char* id, const char* t, ImFont* f, float size, ImU32 col, const char* tip,
               const char* widest = nullptr);
};

// A tooltip over a passive area such as a label or a swatch.
void tipArea(const char* id, ImRect r, const char* text, const char* key = nullptr, bool above = true);

// Dragging a control keeps working while the window changes under the mouse (opacity switches the
// window to a layered one, which can drop ImGui's mouse state): these read the mouse from Windows.
float mouseClientX();
bool leftButtonHeld();
// Snapping while dragging: `p` (0..1 along a control `widthPx` wide) moves to a target closer than 3 px.
// Alt drags freely.
float snapPosition(float p, std::initializer_list<float> targets, float widthPx);

// Horizontal slider (.trk/.fill/.kn). `t` is the knob position 0..1, the fill runs from fillFrom to the knob.
// `mark` (0..1, or < 0 for none) draws a tick at that position, e.g. the centre of the exposure slider.
// `snap` (0..1, or < 0 for none) is a default the knob sticks to; the mark snaps too.
bool slider(ImDrawList* dl, const char* id, float x, float cy, float width, float& t, float fillFrom, bool* reset,
            float mark = -1.0f, float snap = -1.0f);

// Segmented control (.seg); `compact` is the 20 px panel variant. Returns true when the selection changed.
float segmentedWidth(const char* const* labels, int n, bool compact);
bool segmented(ImDrawList* dl, const char* id, float x, float cy, const char* const* labels, int n, int& value,
               bool compact, const char* const* tips = nullptr, bool disabled = false);
// Checkbox with its label on the right. Returns true when clicked.
bool checkbox(ImDrawList* dl, const char* id, float x, float cy, const char* label, bool& value,
              const char* tip = nullptr);

// Read-only number box (.num) that turns into a text field when clicked. Returns true when a value was entered.
bool numberField(ImDrawList* dl, const char* id, float x, float cy, float width, const char* text, float& value,
                 const char* tip);

// ---- floating panels (ui_views.cpp) ----

// 1 px dashed line, black on white, horizontal or vertical; and a rectangle of them.
void dashedLine(ImDrawList* dl, ImVec2 a, ImVec2 b);
void dashedRect(ImDrawList* dl, ImVec2 a, ImVec2 b);
void dashedBox(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col);  // in a theme colour, rounded
// A small square button with an icon, inside a chip or panel header.
bool smallButton(ImDrawList* dl, const char* id, Icon icon, ImVec2 a, float size, float iconSize, const char* tip,
                 const char* key);
// Panel window: translucent, 1 px border, radius 4, kept inside the image area. Call ImGui::End() afterwards
// whatever it returns. `height` 0 sizes the panel to its content.
bool beginPanel(const char* id, ImVec2 defaultPos, float width, float height = 0);
// Panel header (.ph): title and the line under it.
void panelHeader(ImDrawList* dl, ImVec2 p, float w, float h, const char* title, float titleSize, float padL);

// ---- parts of the window (ui_bars.cpp, ui_views.cpp) ----

constexpr ImGuiWindowFlags kBarFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoScrollWithMouse;

float sliderRowHeight();  // 44 for HDR and 16-bit images (the HDR row), else 0
void topBar();
float topBarMinWidth();  // the bar at its tightest, as of the last frame; 0 before the first
void bottomBar();
void viewportOverlays();  // grid, tile borders, pixel outline, and the empty, error and loading states
void notices();           // "Reloaded", the clean-mode hint
void infoPanel();
void histogramPanel();  // ui_panels.cpp
void colorMatchPanel();
// M: opens Color match on the locked pixel or the one under the cursor (else waits for a click), or closes it.
void toggleColorMatch();
void stepMatchRegion(int delta);
void comparePanel();  // ui_compare.cpp
void compareOverlay();
void toggleCompare();
void framesPanel();  // ui_frames.cpp
void toggleFramesOrLayers();
void settingsPanel();   // ui_settings.cpp
// While Settings waits for a new key, takes the key press. False when it is not waiting.
bool settingsKeyDown(KeyBinding k);
void helpPanel();
void opacityPopup(ImVec2 anchorRightBottom);
bool isLoadingVisible();  // a load has run long enough to show its progress

void copyColorValue();
void toggleLockPixel();  // the pixel under the cursor
void copyAllColorFormats();
void cycleAlphaBackground();
void setTiling(Tiling t);
extern double cleanHintUntil;

}  // namespace htv::ui
