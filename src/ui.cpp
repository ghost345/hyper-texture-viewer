#include "ui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "app.h"
#include "icons.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace fs = std::filesystem;

namespace htv {
namespace {

// ---- design tokens (from the main window mockup) ----

struct Theme {
    ImU32 vp, bar, bar2, pop, pnl, ctl, ctlh, bd, bd2, tx, tx2, tx3, ac, acf, act, acs, warn, err, ck1, ck2, kn, knb;
};

constexpr ImU32 hex(uint32_t rgb, float alpha = 1.0f) {
    return IM_COL32((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, int(alpha * 255.0f + 0.5f));
}

const Theme kDark = {hex(0x1b1b1b), hex(0x252525), hex(0x1e1e1e), hex(0x2c2c2c), hex(0x262626, 0.94f),
                     hex(0x323232), hex(0x3c3c3c), hex(0x363636), hex(0x474747), hex(0xe8e8e8),
                     hex(0xb4b4b4), hex(0x969696), hex(0x4a8cff), hex(0x2f6fdb), hex(0x8ab6ff),
                     hex(0x4a8cff, 0.16f), hex(0xe6a92e), hex(0xff7b72), hex(0x3a3a3a), hex(0x4b4b4b),
                     hex(0xdadada), hex(0x141414)};
const Theme kLight = {hex(0xc8c8c8), hex(0xf3f3f3), hex(0xffffff), hex(0xffffff), hex(0xfafafa, 0.96f),
                      hex(0xe4e4e4), hex(0xdadada), hex(0xd4d4d4), hex(0xbdbdbd), hex(0x1c1c1c),
                      hex(0x474747), hex(0x626262), hex(0x1f62d0), hex(0x1f62d0), hex(0x1a55b6),
                      hex(0x1f62d0, 0.13f), hex(0x9a6400), hex(0xc0322b), hex(0xd0d0d0), hex(0xeeeeee),
                      hex(0xffffff), hex(0x8a8a8a)};
const ImU32 kWhite = hex(0xffffff);
const ImU32 kCloseHover = hex(0xc42b1c);

const Theme& T() { return app.darkTheme ? kDark : kLight; }

ImU32 fade(ImU32 c, float alpha) {
    return (c & ~IM_COL32_A_MASK) | (ImU32(((c >> IM_COL32_A_SHIFT) & 0xff) * alpha) << IM_COL32_A_SHIFT);
}

ImFont* fontRegular = nullptr;
ImFont* fontMedium = nullptr;
ImFont* fontSemibold = nullptr;
ImFont* fontMono = nullptr;

// ImGui sizes fonts by line height (ascent - descent) where CSS sizes them by the em square; these factors
// make a 12 px font here match a 12 px font in the design.
float emRegular = 1.0f, emMono = 1.0f;
float em(ImFont* f) { return f == fontMono ? emMono : emRegular; }

float s = 1.0f;  // DPI scale
float S(float v) { return std::round(v * s); }  // design pixels to physical pixels
float line() { return std::max(1.0f, std::floor(s)); }  // 1 px borders

ImVec2 textSize(ImFont* f, float size, const char* t, const char* end = nullptr) {
    return f->CalcTextSizeA(size * s * em(f), FLT_MAX, 0.0f, t, end);
}

// Text whose line box is vertically centred on cy.
void drawText(ImDrawList* dl, ImFont* f, float size, float x, float cy, ImU32 col, const char* t, const char* end = nullptr) {
    const float px = size * s * em(f);
    dl->AddText(f, px, ImVec2(std::round(x), std::round(cy - px * 0.5f)), col, t, end);
}

// (ascent - descent) / units per em from a TrueType file's hhea and head tables.
float lineHeightInEm(const uint8_t* ttf, size_t size) {
    auto u16 = [&](size_t o) { return o + 2 <= size ? unsigned(ttf[o] << 8 | ttf[o + 1]) : 0u; };
    auto u32 = [&](size_t o) { return o + 4 <= size ? (uint32_t(u16(o)) << 16 | u16(o + 2)) : 0u; };
    size_t head = 0, hhea = 0;
    for (unsigned i = 0, n = u16(4); i < n; ++i) {
        const size_t rec = 12 + size_t(i) * 16;
        const uint32_t tag = u32(rec);
        if (tag == 0x68656164) head = u32(rec + 8);       // 'head'
        else if (tag == 0x68686561) hhea = u32(rec + 8);  // 'hhea'
    }
    const unsigned upem = head ? u16(head + 18) : 0;
    if (!upem || !hhea) return 1.0f;
    const int ascent = int16_t(u16(hhea + 4)), descent = int16_t(u16(hhea + 6));
    return float(ascent - descent) / float(upem);
}

// Shortens a UTF-8 string with an ellipsis to fit maxWidth.
std::string ellipsize(ImFont* f, float size, const std::string& t, float maxWidth) {
    if (textSize(f, size, t.c_str()).x <= maxWidth) return t;
    std::string cut = t;
    while (!cut.empty()) {
        do cut.pop_back();
        while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80);
        if (!cut.empty()) cut.pop_back();  // the lead byte of the removed character
        const std::string candidate = cut + "\xE2\x80\xA6";
        if (textSize(f, size, candidate.c_str()).x <= maxWidth) return candidate;
    }
    return "\xE2\x80\xA6";
}

std::string formatZoom(float zoom) {
    char buf[32];
    const float pct = zoom * 100.0f;
    if (pct >= 10.0f || std::fabs(pct - std::round(pct)) < 0.05f) std::snprintf(buf, sizeof buf, "%.0f%%", pct);
    else std::snprintf(buf, sizeof buf, "%.1f%%", pct);
    return buf;
}

std::string formatBytes(uint64_t n) {
    char buf[32];
    if (n < 1024) std::snprintf(buf, sizeof buf, "%u B", unsigned(n));
    else if (n < 1024 * 1024) std::snprintf(buf, sizeof buf, "%.0f KB", double(n) / 1024.0);
    else std::snprintf(buf, sizeof buf, "%.1f MB", double(n) / (1024.0 * 1024.0));
    return buf;
}

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// ---- interaction bookkeeping ----

std::vector<ImRect> holes, nextHoles;  // interactive top bar areas; the rest of the bar drags the window
float topBarH = 32, bottomBarH = 55;

struct PendingTip {
    ImGuiID id = 0;
    ImRect rect;
    std::string text, key;
    bool above = false;
};
PendingTip pendingTip;
ImGuiID tipId = 0;
double tipSince = 0, tipLastShown = -10;

void requestTip(ImRect r, const char* text, const char* key, bool above = false) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) || !text) return;
    pendingTip.id = ImGui::GetItemID();
    pendingTip.rect = r;
    pendingTip.text = text;
    pendingTip.key = key ? key : "";
    pendingTip.above = above;
}


// Shortcut key chip, as in tooltips and the help panel. Returns its width.
float kbdChip(ImDrawList* dl, float x, float cy, const char* key, bool draw = true) {
    const Theme& t = T();
    const float w = textSize(fontMono, 11, key).x + 2 * S(4) + 2 * line();
    if (draw) {
        const float h = S(16) + 2 * line();
        const ImVec2 a(std::round(x), std::round(cy - h * 0.5f));
        dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), t.bar2, S(3));
        dl->AddRect(a, ImVec2(a.x + w, a.y + h), t.bd2, S(3), 0, line());
        drawText(dl, fontMono, 11, a.x + line() + S(4), cy, t.tx2, key);
    }
    return w;
}

// Tooltips show below the hovered control after a short delay, then instantly while moving along the bar.
void drawTooltip() {
    const double now = ImGui::GetTime();
    if (!pendingTip.id || ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        tipId = 0;
        return;
    }
    if (pendingTip.id != tipId) {
        tipId = pendingTip.id;
        tipSince = now;
    }
    if (now - tipSince < 0.5 && now - tipLastShown > 0.3) return;
    tipLastShown = now;

    const Theme& t = T();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const char* text = pendingTip.text.c_str();
    const bool hasKey = !pendingTip.key.empty();
    const float textW = textSize(fontRegular, 12, text).x;
    const float keyW = hasKey ? kbdChip(dl, 0, 0, pendingTip.key.c_str(), false) : 0.0f;
    const float w = line() + S(8) + textW + (hasKey ? S(10) + keyW : 0.0f) + S(4) + line();
    const float h = S(24);
    float x = pendingTip.rect.Min.x;
    const float y = pendingTip.above ? pendingTip.rect.Min.y - h - S(6) : pendingTip.rect.Min.y + S(30);
    x = std::clamp(x, S(4), float(app.winW) - w - S(4));
    const ImVec2 a(std::round(x), std::round(y)), b(a.x + w, a.y + h);
    dl->AddRectFilled(a, b, t.pop, S(3));
    dl->AddRect(a, b, t.bd2, S(3), 0, line());
    const float cy = a.y + h * 0.5f;
    drawText(dl, fontRegular, 12, a.x + line() + S(8), cy, t.tx, text);
    if (hasKey) kbdChip(dl, a.x + line() + S(8) + textW + S(10), cy, pendingTip.key.c_str());
}

// ---- menus ----

ImGuiID menuOpenLastFrame = 0, menuOpenThisFrame = 0;

// Opens the popup `id` on a click, unless that click is what just closed it.
void toggleMenuOnPress(bool pressed, const char* id) {
    if (pressed && menuOpenLastFrame != ImGui::GetID(id)) ImGui::OpenPopup(id);
}

// `pivot` (0, 1) opens the menu upwards from `pos`, for the status bar.
bool beginMenu(const char* id, ImVec2 pos, ImVec2 pivot = ImVec2(0, 0)) {
    const Theme& t = T();
    ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing, pivot);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(4), S(4)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, S(4));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, line());
    ImGui::PushStyleColor(ImGuiCol_PopupBg, t.pop);
    ImGui::PushStyleColor(ImGuiCol_Border, t.bd2);
    if (ImGui::BeginPopup(id, ImGuiWindowFlags_NoMove)) {
        menuOpenThisFrame = ImGui::GetCurrentWindow()->PopupId;
        return true;
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
    return false;
}

void endMenu() {
    ImGui::EndPopup();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

struct MenuItem {
    const char* label;
    const char* key = nullptr;  // shortcut or value shown on the right
    bool checked = false;
    bool enabled = true;
    bool checkColumn = false;  // leave room for a check mark
};

bool menuItem(const MenuItem& m) {
    const Theme& t = T();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float check = m.checkColumn ? S(16) + S(6) : 0.0f;
    const float textW = textSize(fontRegular, 13, m.label).x;
    const float keyW = m.key ? textSize(fontMono, 11, m.key).x : 0.0f;
    const float natural = S(8) + check + textW + (m.key ? S(24) + keyW : 0.0f) + S(8);
    const float w = std::max(natural, ImGui::GetContentRegionAvail().x);
    const float h = S(24);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(m.label, ImVec2(w, h));
    const bool hovered = m.enabled && ImGui::IsItemHovered();
    if (hovered) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), t.acf, S(3));
    const float cy = p.y + h * 0.5f;
    ImU32 col = hovered ? kWhite : m.enabled ? t.tx : fade(t.tx, 0.38f);
    if (m.checked) drawIcon(dl, Icon::Check, ImVec2(p.x + S(8), std::round(cy - S(8))), S(16), col, 1.8f);
    drawText(dl, fontRegular, 13, p.x + S(8) + check, cy, col, m.label);
    if (m.key) {
        const ImU32 kc = hovered ? fade(kWhite, 0.85f) : m.enabled ? t.tx3 : fade(t.tx3, 0.5f);
        drawText(dl, fontMono, 11, p.x + w - S(8) - keyW, cy, kc, m.key);
    }
    if (pressed && m.enabled) {
        ImGui::CloseCurrentPopup();
        return true;
    }
    return false;
}

void menuHeading(const char* label) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = S(6) + S(13) + S(3);
    ImGui::Dummy(ImVec2(textSize(fontSemibold, 11, label).x + 2 * S(8), h));
    drawText(dl, fontSemibold, 11, p.x + S(8), p.y + S(6) + S(13) * 0.5f, T().tx3, label);
}

void menuSeparator() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(1, S(4) * 2 + line()));
    dl->AddRectFilled(ImVec2(p.x + S(2), p.y + S(4)), ImVec2(p.x + w - S(2), p.y + S(4) + line()), T().bd);
}

// ---- controls ----

enum ButtonFlags { kOn = 1, kDisabled = 2, kChevron = 4, kMonoLabel = 8 };

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
        if (topBar) nextHoles.push_back(r);
    }

    // Toolbar button: icon, optional text and dropdown chevron. 24 px high, 4 px side padding (6 with text).
    bool button(const char* id, Icon icon, const char* label, int flags, const char* tip, const char* key,
                float iconSize = 16, float height = 24) {
        const bool hasIcon = icon != Icon::Count;
        const float pad = S(label ? 6 : 4);
        ImFont* lf = flags & kMonoLabel ? fontMono : fontRegular;
        const float labelW = label ? textSize(lf, 12, label).x : 0.0f;
        float w = pad + (hasIcon ? S(iconSize) : 0.0f) + (hasIcon && label ? S(3) : 0.0f) + labelW +
                  (flags & kChevron ? S(3) + S(10) : 0.0f) + pad;
        w = std::max(w, S(height));
        if (measure) {
            advance(w);
            return false;
        }
        const Theme& t = T();
        const float h = S(height);
        const ImVec2 a(x, std::round(cy - h * 0.5f)), b(a.x + w, a.y + h);
        ImGui::SetCursorScreenPos(a);
        const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
        const bool disabled = flags & kDisabled;
        const bool hovered = !disabled && ImGui::IsItemHovered();
        ImU32 fg = t.tx2;
        if (flags & kOn) {
            dl->AddRectFilled(a, b, t.acs, S(3));
            fg = t.act;
        } else if (hovered) {
            dl->AddRectFilled(a, b, t.ctlh, S(3));
            fg = t.tx;
        }
        if (disabled) fg = fade(fg, 0.38f);
        float cx = a.x + pad;
        if (!label && !(flags & kChevron)) cx = a.x + std::round((w - S(iconSize)) * 0.5f);
        if (hasIcon) {
            drawIcon(dl, icon, ImVec2(cx, std::round(cy - S(iconSize) * 0.5f)), S(iconSize), fg);
            cx += S(iconSize) + (label ? S(3) : 0.0f);
        }
        if (label) {
            drawText(dl, lf, 12, cx, cy, fg, label);
            cx += labelW;
        }
        if (flags & kChevron)
            drawIcon(dl, Icon::Chevron, ImVec2(cx + S(3), std::round(cy - S(5))), S(10), fade(fg, 0.75f));
        hole(ImRect(a, b));
        requestTip(ImRect(a, b), tip, key, !topBar);
        advance(w);
        return pressed && !disabled;
    }

    // Dropdown field: bordered box with a value and a chevron (.dd).
    bool dropdown(const char* id, const char* value, ImFont* f, float fontSize, float height, float padL, float padR,
                  const char* tip, const char* key, ImRect* outRect = nullptr) {
        const float w = line() + S(padL) + textSize(f, fontSize, value).x + S(5) + S(10) + S(padR) + line();
        if (measure) {
            advance(w);
            return false;
        }
        const Theme& t = T();
        const float h = S(height);
        const ImVec2 a(x, std::round(cy - h * 0.5f)), b(a.x + w, a.y + h);
        ImGui::SetCursorScreenPos(a);
        const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h), ImGuiButtonFlags_PressedOnClick);
        const bool hovered = ImGui::IsItemHovered();
        dl->AddRectFilled(a, b, hovered ? t.ctlh : t.ctl, S(3));
        dl->AddRect(a, b, t.bd2, S(3), 0, line());
        drawText(dl, f, fontSize, a.x + line() + S(padL), cy, t.tx, value);
        drawIcon(dl, Icon::Chevron, ImVec2(b.x - line() - S(padR) - S(10), std::round(cy - S(5))), S(10),
                 fade(t.tx, 0.75f));
        hole(ImRect(a, b));
        requestTip(ImRect(a, b), tip, key, !topBar);
        if (outRect) *outRect = ImRect(a, b);
        advance(w);
        return pressed;
    }

    // Vertical separator (.sep in the top bar, .vs in the status bar).
    void separator(float height, float margin) {
        x += S(margin);
        if (!measure) dl->AddRectFilled(ImVec2(x, std::round(cy - S(height) * 0.5f)),
                                        ImVec2(x + line(), std::round(cy - S(height) * 0.5f) + S(height)), T().bd2);
        x += line() + S(margin);
        x += gap;
    }

    void text(const char* t, ImFont* f, float size, ImU32 col, float marginL = 0, float marginR = 0) {
        x += S(marginL);
        if (!measure) drawText(dl, f, size, x, cy, col, t);
        x += textSize(f, size, t).x + S(marginR);
        x += gap;
    }
};

// The channel switch (.seg): RGB R G B A L.
void channelSwitch(Row& row) {
    static const char* labels[] = {"RGB", "R", "G", "B", "A", "L"};
    static const char* tips[] = {"All channels", "Red channel", "Green channel", "Blue channel", "Alpha channel",
                                 "Luminance"};
    static const char* keys[] = {nullptr, "R", "G", "B", "A", nullptr};
    float widths[6], total = 2 * line();
    for (int i = 0; i < 6; ++i) {
        widths[i] = std::max(S(22), S(6) * 2 + textSize(fontMedium, 12, labels[i]).x) + (i ? line() : 0.0f);
        total += widths[i];
    }
    if (row.measure) {
        row.advance(total);
        return;
    }
    const Theme& t = T();
    const float h = S(22);
    const ImVec2 a(row.x, std::round(row.cy - h * 0.5f)), b(a.x + total, a.y + h);
    float x = a.x + line();
    for (int i = 0; i < 6; ++i) {
        const float bx = x + (i ? line() : 0.0f), bw = widths[i] - (i ? line() : 0.0f);
        if (i) row.dl->AddRectFilled(ImVec2(x, a.y), ImVec2(x + line(), b.y), t.bd2);
        ImGui::SetCursorScreenPos(ImVec2(bx, a.y + line()));
        ImGui::PushID(i);
        const bool pressed = ImGui::InvisibleButton("ch", ImVec2(bw, h - 2 * line()));
        ImGui::PopID();
        const bool on = int(app.channel) == i;
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 ra(bx, a.y + line()), rb(bx + bw, b.y - line());
        ImDrawFlags corners = (i == 0 ? ImDrawFlags_RoundCornersLeft : 0) | (i == 5 ? ImDrawFlags_RoundCornersRight : 0);
        if (!corners) corners = ImDrawFlags_RoundCornersNone;
        if (on) row.dl->AddRectFilled(ra, rb, t.acf, S(2), corners);
        else if (hovered) row.dl->AddRectFilled(ra, rb, t.ctlh, S(2), corners);
        const float tw = textSize(fontMedium, 12, labels[i]).x;
        drawText(row.dl, fontMedium, 12, bx + std::round((bw - tw) * 0.5f), row.cy, on ? kWhite : hovered ? t.tx : t.tx2,
                 labels[i]);
        requestTip(ImRect(ra, rb), tips[i], keys[i]);
        if (pressed) app.channel = (on && i != 0) ? Channel::RGB : Channel(i);
        x += widths[i];
    }
    row.dl->AddRect(a, b, t.bd2, S(3), 0, line());
    row.hole(ImRect(a, b));
    row.advance(total);
}

// ---- pixel readout ----

struct Readout {
    bool valid = false;       // a pixel is under the cursor (or locked)
    bool readable = false;    // its values could be read (not block-compressed)
    int x = 0, y = 0;
    float raw[4] = {0, 0, 0, 1};  // stored values, averaged over the sample area
    float display[4] = {0, 0, 0, 1};  // as an sRGB colour in 0..1, before view adjustments
    bool outOfRange = false;  // values outside 0..1
};

float srgbEncode(float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

Readout readPixelUnderCursor() {
    Readout r;
    if (!app.hasImage) return r;
    if (app.pixelLocked) {
        r.x = app.lockedX;
        r.y = app.lockedY;
        r.valid = true;
    } else if (app.mouseX >= app.vpX && app.mouseY >= app.vpY && app.mouseX < app.vpX + app.vpW &&
               app.mouseY < app.vpY + app.vpH && !ImGui::GetIO().WantCaptureMouse) {
        r.valid = windowToPixel(float(app.mouseX), float(app.mouseY), r.x, r.y);
    }
    if (!r.valid) return r;
    const int half = app.averageSize / 2;
    float sum[4] = {0, 0, 0, 0};
    int n = 0;
    for (int dy = -half; dy <= half; ++dy)
        for (int dx = -half; dx <= half; ++dx) {
            const int px = std::clamp(r.x + dx, 0, int(app.image.width) - 1);
            const int py = std::clamp(r.y + dy, 0, int(app.image.height) - 1);
            float v[4];
            if (!htv::readPixel(app.image, uint32_t(px), uint32_t(py), v)) return r;
            for (int i = 0; i < 4; ++i) sum[i] += v[i];
            ++n;
        }
    r.readable = true;
    const htv::FormatInfo& info = htv::formatInfo(app.image.format);
    const bool encode = isFloatFormat(info) && app.dataIsLinear;
    for (int i = 0; i < 4; ++i) {
        r.raw[i] = sum[i] / float(n);
        if (r.raw[i] < 0.0f || r.raw[i] > 1.0f) r.outOfRange = true;
        r.display[i] = (i < 3 && encode) ? srgbEncode(r.raw[i]) : std::clamp(r.raw[i], 0.0f, 1.0f);
    }
    return r;
}

const char* kColorFormatNames[int(ColorFormat::Count)] = {"HTML", "Hex", "Delphi Hex", "VB Hex", "RGB",
                                                          "RGB Float", "HSV", "HSL", "Long"};

void hsvHsl(const float c[3], float& h, float& sv, float& v, float& sl, float& l) {
    const float mx = std::max({c[0], c[1], c[2]}), mn = std::min({c[0], c[1], c[2]}), d = mx - mn;
    h = 0;
    if (d > 0) {
        if (mx == c[0]) h = std::fmod((c[1] - c[2]) / d + 6.0f, 6.0f);
        else if (mx == c[1]) h = (c[2] - c[0]) / d + 2.0f;
        else h = (c[0] - c[1]) / d + 4.0f;
        h *= 60.0f;
    }
    v = mx;
    sv = mx > 0 ? d / mx : 0;
    l = (mx + mn) * 0.5f;
    sl = (l > 0 && l < 1) ? d / (1 - std::fabs(2 * l - 1)) : 0;
}

std::string formatColor(const Readout& r, ColorFormat f) {
    int c8[3];
    for (int i = 0; i < 3; ++i) c8[i] = int(std::lround(r.display[i] * 255.0f));
    const long bgr = long(c8[0]) | (long(c8[1]) << 8) | (long(c8[2]) << 16);
    char buf[96];
    switch (f) {
        case ColorFormat::Html: std::snprintf(buf, sizeof buf, "#%02X%02X%02X", c8[0], c8[1], c8[2]); break;
        case ColorFormat::Hex: std::snprintf(buf, sizeof buf, "%02X%02X%02X", c8[0], c8[1], c8[2]); break;
        case ColorFormat::DelphiHex: std::snprintf(buf, sizeof buf, "$00%06lX", bgr); break;
        case ColorFormat::VbHex: std::snprintf(buf, sizeof buf, "&H00%06lX&", bgr); break;
        case ColorFormat::Rgb: std::snprintf(buf, sizeof buf, "%d, %d, %d", c8[0], c8[1], c8[2]); break;
        case ColorFormat::RgbFloat:
            std::snprintf(buf, sizeof buf, "%.4g, %.4g, %.4g", double(r.raw[0]), double(r.raw[1]), double(r.raw[2]));
            break;
        case ColorFormat::Hsv:
        case ColorFormat::Hsl: {
            float h, sv, v, sl, l;
            hsvHsl(r.display, h, sv, v, sl, l);
            if (f == ColorFormat::Hsv)
                std::snprintf(buf, sizeof buf, "%.0f\xC2\xB0, %.0f%%, %.0f%%", double(h), double(sv * 100), double(v * 100));
            else
                std::snprintf(buf, sizeof buf, "%.0f\xC2\xB0, %.0f%%, %.0f%%", double(h), double(sl * 100), double(l * 100));
            break;
        }
        default: std::snprintf(buf, sizeof buf, "%ld", bgr); break;
    }
    return buf;
}

std::string lastColorValue;  // for Ctrl+C

void copyColorValue() {
    if (!lastColorValue.empty()) ImGui::SetClipboardText(lastColorValue.c_str());
}

void toggleLock(const Readout& r) {
    if (app.pixelLocked) app.pixelLocked = false;
    else if (r.valid) {
        app.pixelLocked = true;
        app.lockedX = r.x;
        app.lockedY = r.y;
    }
}

// ---- menus behind the dropdowns ----

void cycleAlphaBackground() {
    app.alphaBackground = AlphaBackground((int(app.alphaBackground) + 1) % int(AlphaBackground::Count));
}

void setTiling(Tiling t) {
    app.tiling = t;
    if (app.fit) fitToWindow();
}

void zoomMenuItems() {
    if (menuItem({"Fit to window", "F", app.fit, true, true})) {
        app.fit = true;
        fitToWindow();
    }
    menuSeparator();
    static const float levels[] = {0.125f, 0.25f, 0.5f, 1, 2, 4, 8, 16, 32};
    static const char* keys[] = {nullptr, nullptr, nullptr, "1", "2", "4", "8", nullptr, nullptr};
    for (int i = 0; i < int(std::size(levels)); ++i) {
        const std::string label = formatZoom(levels[i]);
        if (menuItem({label.c_str(), keys[i], !app.fit && app.zoom == levels[i], true, true})) zoomCentered(levels[i]);
    }
}

void textureMenuItems() {
    menuHeading("Color data");
    if (menuItem({"sRGB", nullptr, !app.dataIsLinear, true, true})) app.dataIsLinear = false;
    if (menuItem({"Linear", nullptr, app.dataIsLinear, true, true})) app.dataIsLinear = true;
    menuSeparator();
    menuHeading("Normal map");
    if (menuItem({"Reconstruct Z", nullptr, app.normalZ, true, true})) app.normalZ = !app.normalZ;
    if (menuItem({"Flip green (DirectX / OpenGL)", nullptr, app.flipGreen, true, true})) app.flipGreen = !app.flipGreen;
    menuSeparator();
    menuHeading("Mip level");
    const bool fileMips = app.hasImage && app.image.mips.size() > 1;
    if (menuItem({"Auto", nullptr, app.forcedMip < 0, true, true})) app.forcedMip = -1;
    if (fileMips) {
        for (int i = 0; i < app.textureMips; ++i) {
            char label[64], size[32];
            std::snprintf(label, sizeof label, "Mip %d", i);
            std::snprintf(size, sizeof size, "%u\xC3\x97%u", std::max(1u, app.image.width >> i),
                          std::max(1u, app.image.height >> i));
            ImGui::PushID(i);
            if (menuItem({label, size, app.forcedMip == i, true, true})) app.forcedMip = i;
            ImGui::PopID();
        }
    } else {
        menuItem({"No mip chain in the file", nullptr, false, false, true});
    }
    menuSeparator();
    menuItem({"Cube face", "soon", false, false, true});
    menuItem({"EXR layer", "soon", false, false, true});
}

void alphaMenuItems() {
    menuHeading("Alpha background");
    static const char* names[int(AlphaBackground::Count)] = {"Checkerboard, small", "Checkerboard, medium",
                                                             "Checkerboard, large", "Black", "Gray", "White"};
    for (int i = 0; i < int(AlphaBackground::Count); ++i)
        if (menuItem({names[i], nullptr, int(app.alphaBackground) == i && !app.ignoreAlpha, true, true})) {
            app.alphaBackground = AlphaBackground(i);
            app.ignoreAlpha = false;
        }
    menuSeparator();
    if (menuItem({"Ignore alpha", nullptr, app.ignoreAlpha, true, true})) app.ignoreAlpha = !app.ignoreAlpha;
}

void tilingMenuItems() {
    if (menuItem({"Off", nullptr, app.tiling == Tiling::Off, true, true})) setTiling(Tiling::Off);
    if (menuItem({"3\xC3\x97" "3 seamless", nullptr, app.tiling == Tiling::Seamless, true, true}))
        setTiling(Tiling::Seamless);
    if (menuItem({"3\xC3\x97" "3 with borders", nullptr, app.tiling == Tiling::Bordered, true, true}))
        setTiling(Tiling::Bordered);
}

// A horizontal slider in the design's style (.trk/.fill/.kn). `t` is the knob position 0..1.
bool slider(ImDrawList* dl, const char* id, float x, float cy, float width, float& t, float fillFrom, bool* reset) {
    const Theme& th = T();
    const float w = S(width), knobW = S(8), knobH = S(16);
    ImGui::SetCursorScreenPos(ImVec2(x - knobW * 0.5f, std::round(cy - knobH * 0.5f)));
    ImGui::InvisibleButton(id, ImVec2(w + knobW, knobH));
    bool changed = false;
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        t = std::clamp((ImGui::GetIO().MousePos.x - x) / w, 0.0f, 1.0f);
        changed = true;
    } else if (ImGui::IsItemClicked()) {
        t = std::clamp((ImGui::GetIO().MousePos.x - x) / w, 0.0f, 1.0f);
        changed = true;
    }
    if (reset) *reset = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    const float ty = std::round(cy - S(2));
    dl->AddRectFilled(ImVec2(x, ty), ImVec2(x + w, ty + S(4)), th.bd2, S(2));
    const float kx = std::round(x + t * w), fx = std::round(x + fillFrom * w);
    if (kx != fx) dl->AddRectFilled(ImVec2(std::min(kx, fx), ty), ImVec2(std::max(kx, fx), ty + S(4)), th.ac, S(2));
    const ImVec2 ka(kx - std::round(knobW * 0.5f), std::round(cy - knobH * 0.5f)), kb(ka.x + knobW, ka.y + knobH);
    dl->AddRectFilled(ka, kb, th.kn, S(2));
    dl->AddRect(ka, kb, th.knb, S(2), 0, line());
    return changed;
}

void opacityMenuItems() {
    menuHeading("Window opacity");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float rowH = S(30);
    ImGui::Dummy(ImVec2(S(8) + S(140) + S(10) + textSize(fontMono, 12, "100%").x + S(8), rowH));
    const ImVec2 after = ImGui::GetCursorScreenPos();
    float t = (app.opacity - 0.2f) / 0.8f;
    bool reset = false;
    if (slider(dl, "opacity", p.x + S(8), p.y + rowH * 0.5f, 140, t, 0.0f, &reset)) setOpacity(0.2f + t * 0.8f);
    if (reset) setOpacity(1.0f);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d%%", int(std::lround(app.opacity * 100)));
    drawText(dl, fontMono, 12, p.x + S(8) + S(140) + S(10), p.y + rowH * 0.5f, T().tx, buf);
    ImGui::SetCursorScreenPos(after);
    menuItem({"Alt + mouse wheel", nullptr, false, false});
}

// ---- top bar ----

constexpr ImGuiWindowFlags kBarFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoScrollWithMouse;

// Groups of the left side in order. When the window is too narrow the last ones move into the "..." menu.
enum Group { kFile, kView, kChannels, kBackground, kOverlays, kAnalysis, kCompare, kGroupCount };

void fileGroup(Row& r) {
    if (r.button("open", Icon::Open, nullptr, 0, "Open", "Ctrl+O")) showOpenDialog();
    if (r.button("prev", Icon::Prev, nullptr, app.folder.size() > 1 ? 0 : kDisabled, "Previous file", "Left")) step(-1);
    if (r.button("next", Icon::Next, nullptr, app.folder.size() > 1 ? 0 : kDisabled, "Next file", "Right")) step(+1);
    const std::string name = app.path.empty() ? std::string("No file") : utf8(app.path.filename().wstring());
    const std::string shown = ellipsize(fontMedium, 13, name, S(240));
    const float nameW = textSize(fontMedium, 13, shown.c_str()).x;
    if (!r.measure) {
        const ImVec2 a(r.x + S(6), r.cy - S(10)), b(a.x + nameW, r.cy + S(10));
        drawText(r.dl, fontMedium, 13, a.x, r.cy, app.path.empty() ? T().tx3 : T().tx, shown.c_str());
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("filename", ImVec2(std::max(1.0f, nameW), b.y - a.y));
        if (!app.path.empty()) {
            r.hole(ImRect(a, b));
            const std::string full = utf8(app.path.wstring());
            requestTip(ImRect(ImVec2(a.x, r.cy - S(12)), b), full.c_str(), nullptr);
        }
    }
    r.x += S(6) + nameW + S(6) + r.gap;
    if (app.folderIndex >= 0) {
        char pos[32];
        std::snprintf(pos, sizeof pos, "%d / %d", app.folderIndex + 1, int(app.folder.size()));
        r.text(pos, fontMono, 12, T().tx3, 0, 2);
    }
}

void viewGroup(Row& r) {
    r.separator(16, 5);
    if (r.button("fit", Icon::Fit, nullptr, app.fit ? kOn : 0, "Fit to window", "F")) {
        app.fit = true;
        fitToWindow();
    }
    if (r.button("actual", Icon::Count, "1:1", (!app.fit && app.zoom == 1.0f ? kOn : 0), "Actual size", "1"))
        zoomCentered(1.0f);
    const std::string zoom = formatZoom(app.zoom);
    ImRect zr;
    toggleMenuOnPress(r.dropdown("zoom", zoom.c_str(), fontRegular, 12, 22, 7, 4, "Zoom", "Wheel", &zr), "zoom menu");
    if (!r.measure && beginMenu("zoom menu", ImVec2(zr.Min.x, zr.Max.y + S(4)))) {
        zoomMenuItems();
        endMenu();
    }
    if (r.button("rotl", Icon::RotateLeft, nullptr, 0, "Rotate left", "Ctrl+L")) rotate(-1);
    if (r.button("rotr", Icon::RotateRight, nullptr, 0, "Rotate right", "Ctrl+R")) rotate(+1);
    if (r.button("fliph", Icon::FlipH, nullptr, app.flipH ? kOn : 0, "Flip horizontal", "H")) app.flipH = !app.flipH;
    if (r.button("flipv", Icon::FlipV, nullptr, app.flipV ? kOn : 0, "Flip vertical", "V")) app.flipV = !app.flipV;
}

void channelsGroup(Row& r) {
    r.separator(16, 5);
    channelSwitch(r);
    ImRect tr;
    std::string mode = app.dataIsLinear ? "Linear" : "sRGB";
    if (app.normalZ || app.flipGreen) mode += " \xC2\xB7 Normal";
    if (app.forcedMip >= 0) mode += " \xC2\xB7 Mip " + std::to_string(app.forcedMip);
    toggleMenuOnPress(r.dropdown("texmode", mode.c_str(), fontRegular, 12, 22, 7, 4, "Texture mode", nullptr, &tr),
                      "texture menu");
    if (!r.measure && beginMenu("texture menu", ImVec2(tr.Min.x, tr.Max.y + S(4)))) {
        textureMenuItems();
        endMenu();
    }
}

void backgroundGroup(Row& r) {
    r.separator(16, 5);
    const float x0 = r.x;
    const bool checker = app.alphaBackground <= AlphaBackground::CheckerLarge && !app.ignoreAlpha;
    toggleMenuOnPress(r.button("alphabg", Icon::AlphaBackground, nullptr, kChevron | (checker ? kOn : 0),
                               "Alpha background", "Shift+A"),
                      "alpha menu");
    if (!r.measure && beginMenu("alpha menu", ImVec2(x0, r.cy + S(16)))) {
        alphaMenuItems();
        endMenu();
    }
    const float x1 = r.x;
    toggleMenuOnPress(r.button("tiling", Icon::Tiling, nullptr, kChevron | (app.tiling != Tiling::Off ? kOn : 0),
                               "Tiling", "T"),
                      "tiling menu");
    if (!r.measure && beginMenu("tiling menu", ImVec2(x1, r.cy + S(16)))) {
        tilingMenuItems();
        endMenu();
    }
}

void overlaysGroup(Row& r) {
    r.separator(16, 5);
    if (r.button("grid", Icon::Grid, nullptr, app.grid ? kOn : 0, "Pixel grid", "Ctrl+G")) app.grid = !app.grid;
    r.button("ruler", Icon::Ruler, nullptr, kDisabled, "Ruler (coming soon)", "Ctrl+U");
    if (r.button("outline", Icon::PixelOutline, nullptr, app.pixelOutline ? kOn : 0, "Pixel outline", "P"))
        app.pixelOutline = !app.pixelOutline;
}

void analysisGroup(Row& r) {
    r.separator(16, 5);
    r.button("match", Icon::ColorMatch, nullptr, kDisabled, "Color match (coming soon)", "M");
    r.button("histogram", Icon::Histogram, nullptr, kDisabled, "Histogram (coming soon)", "Ctrl+H");
    if (r.button("info", Icon::Info, nullptr, app.showInfo ? kOn : 0, "Info", "I")) app.showInfo = !app.showInfo;
    r.button("layers", Icon::Layers, nullptr, kDisabled, "Layers (coming soon)", "L");
}

void compareGroup(Row& r) {
    r.separator(16, 5);
    r.button("compare", Icon::Compare, "Compare", kDisabled, "Compare (coming soon)", "C");
}

void drawGroup(Row& r, int g) {
    switch (g) {
        case kFile: fileGroup(r); break;
        case kView: viewGroup(r); break;
        case kChannels: channelsGroup(r); break;
        case kBackground: backgroundGroup(r); break;
        case kOverlays: overlaysGroup(r); break;
        case kAnalysis: analysisGroup(r); break;
        default: compareGroup(r); break;
    }
}

// The "..." menu holds the groups that did not fit.
void overflowMenuItems(int firstHidden) {
    for (int g = firstHidden; g < kGroupCount; ++g) {
        if (g > firstHidden) menuSeparator();
        switch (g) {
            case kBackground:
                if (menuItem({"Next alpha background", "Shift+A"})) cycleAlphaBackground();
                if (menuItem({"Next tiling mode", "T"})) setTiling(Tiling((int(app.tiling) + 1) % 3));
                break;
            case kOverlays:
                if (menuItem({"Pixel grid", "Ctrl+G", app.grid, true, true})) app.grid = !app.grid;
                menuItem({"Ruler", "Ctrl+U", false, false, true});
                if (menuItem({"Pixel outline", "P", app.pixelOutline, true, true})) app.pixelOutline = !app.pixelOutline;
                break;
            case kAnalysis:
                menuItem({"Color match", "M", false, false, true});
                menuItem({"Histogram", "Ctrl+H", false, false, true});
                if (menuItem({"Info", "I", app.showInfo, true, true})) app.showInfo = !app.showInfo;
                menuItem({"Layers", "L", false, false, true});
                break;
            default: menuItem({"Compare", "C", false, false, true}); break;
        }
    }
}

void rightGroup(Row& r) {
    if (r.button("ontop", Icon::OnTop, nullptr, app.topmost ? kOn : 0, "Always on top", "Ctrl+T")) setTopmost(!app.topmost);
    char pct[16];
    std::snprintf(pct, sizeof pct, "%d%%", int(std::lround(app.opacity * 100)));
    const float x0 = r.x;
    toggleMenuOnPress(r.button("opacity", Icon::Opacity, pct, kMonoLabel | (app.opacity < 1.0f ? kOn : 0),
                               "Window opacity", "Alt+Wheel"),
                      "opacity menu");
    if (!r.measure && beginMenu("opacity menu", ImVec2(x0, r.cy + S(16)))) {
        opacityMenuItems();
        endMenu();
    }
    if (r.button("theme", Icon::Theme, nullptr, 0, app.darkTheme ? "Light theme" : "Dark theme", nullptr))
        app.darkTheme = !app.darkTheme;
    r.button("settings", Icon::Settings, nullptr, kDisabled, "Settings (coming soon)", "Ctrl+,");
    if (r.button("help", Icon::Help, nullptr, app.showHelp ? kOn : 0, "Keyboard shortcuts", "F1"))
        app.showHelp = !app.showHelp;
    r.separator(16, 5);
    r.x -= S(5) + r.gap;  // the last separator has no right margin
}

// Minimise, maximise and close (.wc): 40 x 31, no gap between them.
float windowButtons(Row& r) {
    const float w = S(40), h = topBarH - line();
    if (r.measure) return 3 * w;
    const Theme& t = T();
    const Icon icons[3] = {Icon::Minimize, IsZoomed(app.hwnd) ? Icon::Restore : Icon::Maximize, Icon::Close};
    const char* ids[3] = {"min", "max", "close"};
    for (int i = 0; i < 3; ++i) {
        const ImVec2 a(r.x + i * w, 0), b(a.x + w, h);
        ImGui::SetCursorScreenPos(a);
        const bool pressed = ImGui::InvisibleButton(ids[i], ImVec2(w, h));
        const bool hovered = ImGui::IsItemHovered();
        ImU32 fg = t.tx2;
        if (hovered) {
            r.dl->AddRectFilled(a, b, i == 2 ? kCloseHover : t.ctlh);
            fg = i == 2 ? kWhite : t.tx;
        }
        drawIcon(r.dl, icons[i], ImVec2(a.x + std::round((w - S(14)) * 0.5f), std::round((h - S(14)) * 0.5f)), S(14), fg, 1.2f);
        r.hole(ImRect(a, b));
        if (pressed) {
            if (i == 0) ShowWindow(app.hwnd, SW_MINIMIZE);
            else if (i == 1) toggleMaximize();
            else PostMessageW(app.hwnd, WM_CLOSE, 0, 0);
        }
    }
    return 3 * w;
}

void topBar() {
    const Theme& t = T();
    const float W = float(app.winW);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(W, topBarH));
    ImGui::Begin("##top", nullptr, kBarFlags);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, topBarH), t.bar);
    dl->AddRectFilled(ImVec2(0, topBarH - line()), ImVec2(W, topBarH), t.bd);

    Row r;
    r.dl = dl;
    r.cy = (topBarH - line()) * 0.5f;
    r.gap = S(2);
    r.topBar = true;

    // Measure: left groups, the "..." button and the right side.
    r.measure = true;
    float widths[kGroupCount];
    for (int g = 0; g < kGroupCount; ++g) {
        r.x = 0;
        drawGroup(r, g);
        widths[g] = r.x;
    }
    r.x = 0;
    r.separator(16, 5);
    r.button("more", Icon::More, nullptr, 0, nullptr, nullptr);
    const float moreW = r.x;
    r.x = 0;
    rightGroup(r);
    const float rightW = r.x + windowButtons(r);

    const float available = W - S(6) - rightW - S(8);
    int visible = kGroupCount;
    auto used = [&](int n) {
        float sum = 0;
        for (int g = 0; g < n; ++g) sum += widths[g];
        return sum + (n < kGroupCount ? moreW : 0.0f);
    };
    while (visible > kBackground && used(visible) > available) --visible;

    r.measure = false;
    r.x = S(6);
    for (int g = 0; g < visible; ++g) drawGroup(r, g);
    if (visible < kGroupCount) {
        r.separator(16, 5);
        const float x0 = r.x;
        toggleMenuOnPress(r.button("more", Icon::More, nullptr, 0, "More tools", nullptr), "more menu");
        if (beginMenu("more menu", ImVec2(x0, r.cy + S(16)))) {
            overflowMenuItems(visible);
            endMenu();
        }
    }
    r.x = W - rightW;
    rightGroup(r);
    windowButtons(r);
    ImGui::End();
}

// ---- bottom bar ----

void sliderRow(ImDrawList* dl, float y0, float W) {
    const Theme& t = T();
    const float h = S(30);
    dl->AddRectFilled(ImVec2(0, y0 + h - line()), ImVec2(W, y0 + h), t.bd);
    const float cy = y0 + (h - line()) * 0.5f;
    float x = S(12);
    const float gap = S(10);
    const float valueW = textSize(fontMono, 12, "+10.00").x;
    char buf[32];

    auto label = [&](const char* text) {
        drawText(dl, fontMedium, 11, x, cy, t.tx3, text);
        x += textSize(fontMedium, 11, text).x + gap;
    };
    if (app.hasImage && isHighPrecision(app.image)) {
        label("Exposure");
        float e = (app.exposure + 10.0f) / 20.0f;
        bool reset = false;
        if (slider(dl, "exposure", x, cy, 160, e, 0.5f, &reset)) app.exposure = std::round((e * 20.0f - 10.0f) * 20.0f) / 20.0f;
        if (reset) app.exposure = 0.0f;
        requestTip(ImRect(ImVec2(x, cy - S(8)), ImVec2(x + S(160), cy + S(8))), "Exposure in stops; double-click resets",
                   "Ctrl+Wheel", true);
        x += S(160) + gap;
        std::snprintf(buf, sizeof buf, "%+.2f", double(app.exposure));
        drawText(dl, fontMono, 12, x, cy, t.tx, buf);
        x += valueW + gap + S(8);
    }
    label("Gamma");
    float g = (std::log2(app.gamma) + 2.0f) / 4.0f;
    bool reset = false;
    if (slider(dl, "gamma", x, cy, 160, g, 0.0f, &reset)) {
        app.gamma = std::exp2(g * 4.0f - 2.0f);
        if (std::fabs(app.gamma - 1.0f) < 0.03f) app.gamma = 1.0f;  // snaps to 1.00 at the centre
    }
    if (reset) app.gamma = 1.0f;
    requestTip(ImRect(ImVec2(x, cy - S(8)), ImVec2(x + S(160), cy + S(8))), "Display gamma; double-click resets", nullptr,
               true);
    x += S(160) + gap;
    std::snprintf(buf, sizeof buf, "%.2f", double(app.gamma));
    drawText(dl, fontMono, 12, x, cy, t.tx, buf);
    x += textSize(fontMono, 12, buf).x + gap;
    if (!app.hasImage || !isHighPrecision(app.image))
        drawText(dl, fontMedium, 11, x + S(8), cy, t.tx3, "Exposure and range appear for HDR and 16-bit images");
}

void statusRow(ImDrawList* dl, float y0, float W) {
    const Theme& t = T();
    const float h = S(24);
    Row r;
    r.dl = dl;
    r.cy = y0 + h * 0.5f;
    r.gap = S(6);
    r.x = S(12);
    const Readout px = readPixelUnderCursor();

    // Coordinates, padded to the image's digit count so the rest of the row stays put.
    const int digits = app.hasImage ? int(std::to_string(std::max(app.image.width, app.image.height)).size()) : 4;
    char buf[64];
    r.text("X", fontMono, 12, t.tx3);
    if (px.valid) std::snprintf(buf, sizeof buf, "%*d", digits, px.x);
    else std::snprintf(buf, sizeof buf, "%*s", digits, "-");
    r.text(buf, fontMono, 12, t.tx2);
    r.text("Y", fontMono, 12, t.tx3);
    if (px.valid) std::snprintf(buf, sizeof buf, "%*d", digits, px.y);
    else std::snprintf(buf, sizeof buf, "%*s", digits, "-");
    r.text(buf, fontMono, 12, t.tx2);
    r.separator(14, 6);

    // Swatch and value.
    const float sw = S(12);
    const ImVec2 sa(r.x, std::round(r.cy - sw * 0.5f));
    if (px.readable)
        dl->AddRectFilled(sa, ImVec2(sa.x + sw, sa.y + sw),
                          IM_COL32(int(px.display[0] * 255 + 0.5f), int(px.display[1] * 255 + 0.5f),
                                   int(px.display[2] * 255 + 0.5f), 255));
    dl->AddRect(sa, ImVec2(sa.x + sw, sa.y + sw), t.bd2, 0, 0, line());
    r.advance(sw);

    const htv::FormatInfo& info = htv::formatInfo(app.image.format);
    lastColorValue.clear();
    if (px.readable) {
        lastColorValue = formatColor(px, app.colorFormat);
        const float vw = textSize(fontMono, 12, lastColorValue.c_str()).x;
        ImGui::SetCursorScreenPos(ImVec2(r.x, r.cy - S(10)));
        if (ImGui::InvisibleButton("value", ImVec2(vw, S(20)))) copyColorValue();
        requestTip(ImRect(ImVec2(r.x, r.cy - S(10)), ImVec2(r.x + vw, r.cy + S(10))), "Click to copy", "Ctrl+C", true);
        r.text(lastColorValue.c_str(), fontMono, 12, t.tx);
        if (app.colorFormat == ColorFormat::RgbFloat || isFloatFormat(info))
            std::snprintf(buf, sizeof buf, "\xCE\xB1 %.3g", double(px.raw[3]));
        else
            std::snprintf(buf, sizeof buf, "\xCE\xB1 %d", int(std::lround(std::clamp(px.raw[3], 0.0f, 1.0f) * 255)));
        r.text(buf, fontMono, 12, t.tx3);
        // HDR and 16-bit values: the 8-bit formats round or clip them, so the exact numbers follow.
        if (app.colorFormat != ColorFormat::RgbFloat && (isHighPrecision(app.image) || px.outOfRange)) {
            std::snprintf(buf, sizeof buf, "%.4g %.4g %.4g", double(px.raw[0]), double(px.raw[1]), double(px.raw[2]));
            r.text(buf, fontMono, 12, t.tx3);
        }
    } else if (px.valid) {
        r.text("block-compressed: values not readable yet", fontMono, 12, t.tx3);
    } else {
        r.text("-", fontMono, 12, t.tx3);
    }

    ImRect fr;
    toggleMenuOnPress(r.dropdown("colorfmt", kColorFormatNames[int(app.colorFormat)], fontMono, 11, 18, 6, 3,
                                 "Color value format", nullptr, &fr),
                      "format menu");
    if (beginMenu("format menu", ImVec2(fr.Min.x, fr.Min.y - S(4)), ImVec2(0, 1))) {
        for (int i = 0; i < int(ColorFormat::Count); ++i) {
            const std::string example = px.readable ? formatColor(px, ColorFormat(i)) : std::string();
            if (menuItem({kColorFormatNames[i], px.readable ? example.c_str() : nullptr, int(app.colorFormat) == i, true, true}))
                app.colorFormat = ColorFormat(i);
        }
        endMenu();
    }
    if (px.readable && app.colorFormat != ColorFormat::RgbFloat && (isHighPrecision(app.image) || px.outOfRange)) {
        const ImVec2 wa(r.x, std::round(r.cy - S(7)));
        drawIcon(dl, Icon::Warning, wa, S(14), t.warn);
        ImGui::SetCursorScreenPos(wa);
        ImGui::InvisibleButton("warn", ImVec2(S(14), S(14)));
        requestTip(ImRect(wa, ImVec2(wa.x + S(14), wa.y + S(14))),
                   px.outOfRange ? "Clipped to 0-1 and rounded to 8 bits" : "Rounded to 8 bits", nullptr, true);
        r.advance(S(14));
    }
    if (r.button("copy", Icon::Copy, nullptr, px.readable ? 0 : kDisabled, "Copy value", "Ctrl+C", 14, 20))
        copyColorValue();
    if (r.button("lock", Icon::Pin, nullptr, app.pixelLocked ? kOn : (px.valid ? 0 : kDisabled), "Lock pixel", "Space", 14, 20))
        toggleLock(px);
    r.separator(14, 6);
    r.text("Avg", fontMono, 12, t.tx3);
    std::snprintf(buf, sizeof buf, "%d\xC3\x97%d", app.averageSize, app.averageSize);
    ImRect ar;
    toggleMenuOnPress(r.dropdown("avg", buf, fontMono, 11, 18, 6, 3, "Average over an area", nullptr, &ar), "avg menu");
    if (beginMenu("avg menu", ImVec2(ar.Min.x, ar.Min.y - S(4)), ImVec2(0, 1))) {
        for (int n : {1, 3, 5}) {
            char label[16];
            std::snprintf(label, sizeof label, "%d\xC3\x97%d", n, n);
            if (menuItem({label, nullptr, app.averageSize == n, true, true})) app.averageSize = n;
        }
        endMenu();
    }

    // Right side, laid out from the right edge: image size and format, zoom, file size.
    if (!app.hasImage) return;
    std::string desc = std::to_string(app.image.width) + "\xC3\x97" + std::to_string(app.image.height);
    {
        std::string d = app.image.description;
        for (size_t p; (p = d.find(", ")) != std::string::npos;) d.replace(p, 2, " \xC2\xB7 ");
        desc += " \xC2\xB7 " + d;
    }
    const std::string zoom = formatZoom(app.zoom), size = formatBytes(app.fileSize);
    const float vsW = S(6) * 2 + line();
    float x = W - S(4) - textSize(fontMono, 12, size.c_str()).x;
    drawText(dl, fontMono, 12, x, r.cy, t.tx2, size.c_str());
    x -= r.gap + vsW + r.gap;
    dl->AddRectFilled(ImVec2(x + S(6), std::round(r.cy - S(7))), ImVec2(x + S(6) + line(), std::round(r.cy - S(7)) + S(14)), t.bd2);
    x -= textSize(fontMono, 12, zoom.c_str()).x;
    drawText(dl, fontMono, 12, x, r.cy, t.tx, zoom.c_str());
    x -= r.gap + vsW + r.gap;
    dl->AddRectFilled(ImVec2(x + S(6), std::round(r.cy - S(7))), ImVec2(x + S(6) + line(), std::round(r.cy - S(7)) + S(14)), t.bd2);
    const float room = x - r.x - S(12);
    if (room > S(40)) {
        const std::string shown = ellipsize(fontMono, 12, desc, room);
        drawText(dl, fontMono, 12, x - textSize(fontMono, 12, shown.c_str()).x, r.cy, t.tx2, shown.c_str());
    }
}

void bottomBar() {
    const Theme& t = T();
    const float W = float(app.winW), y0 = float(app.winH) - bottomBarH;
    ImGui::SetNextWindowPos(ImVec2(0, y0));
    ImGui::SetNextWindowSize(ImVec2(W, bottomBarH));
    ImGui::Begin("##bottom", nullptr, kBarFlags);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(0, y0), ImVec2(W, y0 + bottomBarH), t.bar);
    dl->AddRectFilled(ImVec2(0, y0), ImVec2(W, y0 + line()), t.bd);
    sliderRow(dl, y0 + line(), W);
    statusRow(dl, y0 + line() + S(30), W);
    ImGui::End();
}

// ---- viewport overlays ----

// 1 px dashed line, black and white so it shows on any colour. Horizontal or vertical only.
void dashedLine(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    const float dash = S(3), t = line();
    const bool horizontal = a.y == b.y;
    const float len = horizontal ? b.x - a.x : b.y - a.y;
    auto rect = [&](float from, float to, ImU32 c) {
        if (horizontal) dl->AddRectFilled(ImVec2(a.x + from, a.y), ImVec2(a.x + to, a.y + t), c);
        else dl->AddRectFilled(ImVec2(a.x, a.y + from), ImVec2(a.x + t, a.y + to), c);
    };
    rect(0, len, IM_COL32(0, 0, 0, 200));
    for (float p = 0; p < len; p += 2 * dash) rect(p, std::min(p + dash, len), IM_COL32(255, 255, 255, 230));
}

void dashedRect(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    const float t = line();
    dashedLine(dl, a, ImVec2(b.x, a.y));
    dashedLine(dl, ImVec2(a.x, b.y - t), ImVec2(b.x, b.y - t));
    dashedLine(dl, a, ImVec2(a.x, b.y));
    dashedLine(dl, ImVec2(b.x - t, a.y), ImVec2(b.x - t, b.y));
}

void centeredLines(ImDrawList* dl, float cy, const std::vector<std::pair<std::string, ImU32>>& lines, float size) {
    float y = cy - lines.size() * S(size + 6) * 0.5f;
    for (const auto& [text, col] : lines) {
        const float w = textSize(fontRegular, size, text.c_str()).x;
        drawText(dl, fontRegular, size, std::round(app.vpX + (app.vpW - w) * 0.5f), y + S(size) * 0.5f, col, text.c_str());
        y += S(size + 6);
    }
}

void viewportOverlays() {
    const Theme& t = T();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 va(app.vpX, app.vpY), vb(app.vpX + app.vpW, app.vpY + app.vpH);
    dl->PushClipRect(va, vb);

    if (app.hasImage) {
        float tw, th;
        imageSizeOnScreen(tw, th);
        const float x0 = app.originX, y0 = app.originY, x1 = x0 + tw * app.zoom, y1 = y0 + th * app.zoom;
        // Pixel grid every `gridStep` image pixels, once the cells are big enough to see.
        if (app.grid && app.gridStep * app.zoom >= S(4)) {
            const float stepPx = app.gridStep * app.zoom;
            const ImU32 c = app.darkTheme ? IM_COL32(0, 0, 0, 90) : IM_COL32(255, 255, 255, 110);
            const float fx = std::max(x0, va.x - std::fmod(va.x - x0, stepPx)), fy = std::max(y0, va.y - std::fmod(va.y - y0, stepPx));
            for (float x = fx; x <= std::min(x1, vb.x); x += stepPx)
                dl->AddRectFilled(ImVec2(std::round(x), std::max(y0, va.y)), ImVec2(std::round(x) + line(), std::min(y1, vb.y)), c);
            for (float y = fy; y <= std::min(y1, vb.y); y += stepPx)
                dl->AddRectFilled(ImVec2(std::max(x0, va.x), std::round(y)), ImVec2(std::min(x1, vb.x), std::round(y) + line()), c);
        }
        // Tile borders for the 3x3 preview.
        if (app.tiling == Tiling::Bordered) {
            const float sw = (x1 - x0) / 3, sh = (y1 - y0) / 3;
            for (int i = 0; i <= 3; ++i) {
                const float x = std::round(x0 + i * sw) - (i == 3 ? line() : 0.0f);
                const float y = std::round(y0 + i * sh) - (i == 3 ? line() : 0.0f);
                dashedLine(dl, ImVec2(x, y0), ImVec2(x, y1));
                dashedLine(dl, ImVec2(x0, y), ImVec2(x1, y));
            }
        }
        // Outline around the pixel under the cursor.
        int px, py;
        if (app.pixelOutline && !ImGui::GetIO().WantCaptureMouse &&
            windowToPixel(float(app.mouseX), float(app.mouseY), px, py)) {
            const float cx = x0 + std::floor((app.mouseX - x0) / app.zoom) * app.zoom;
            const float cy = y0 + std::floor((app.mouseY - y0) / app.zoom) * app.zoom;
            const float z = std::max(app.zoom, 1.0f);
            dashedRect(dl, ImVec2(std::round(cx) - line(), std::round(cy) - line()),
                       ImVec2(std::round(cx + z) + line(), std::round(cy + z) + line()));
        }
    }

    // Empty window, or a file that failed to open.
    const float cy = app.vpY + app.vpH * 0.5f;
    if (!app.error.empty()) {
        const std::string name = app.path.empty() ? std::string() : utf8(app.path.filename().wstring());
        const std::string msg = "Cannot open " + name + ": " + app.error;
        if (!app.hasImage) {
            centeredLines(dl, cy, {{msg, t.err}}, 13);
        } else {
            // The previous image stays; the message sits at the top of the viewport.
            const float w = textSize(fontRegular, 13, msg.c_str()).x + S(14) + S(8) + 2 * S(10);
            const ImVec2 a(std::round(app.vpX + (app.vpW - w) * 0.5f), app.vpY + S(12)), b(a.x + w, a.y + S(28));
            dl->AddRectFilled(a, b, t.pop, S(4));
            dl->AddRect(a, b, t.bd2, S(4), 0, line());
            drawIcon(dl, Icon::Warning, ImVec2(a.x + S(10), a.y + S(7)), S(14), t.err);
            drawText(dl, fontRegular, 13, a.x + S(10) + S(14) + S(8), a.y + S(14), t.tx, msg.c_str());
        }
    } else if (!app.hasImage) {
        centeredLines(dl, cy,
                      {{"Drop an image here or press Ctrl+O", t.tx2},
                       {"EXR \xC2\xB7 HDR \xC2\xB7 DDS \xC2\xB7 KTX \xC2\xB7 PNG \xC2\xB7 JPEG \xC2\xB7 WebP \xC2\xB7 AVIF \xC2\xB7 "
                        "JPEG XL \xC2\xB7 TGA \xC2\xB7 PSD \xC2\xB7 BMP \xC2\xB7 GIF \xC2\xB7 QOI \xC2\xB7 TIFF \xC2\xB7 HEIC",
                        t.tx3}},
                      13);
    }
    dl->PopClipRect();
}

// ---- floating panels ----

// Panel frame (.pnl/.ph): 26 px header with title and close button, translucent body; drags anywhere.
bool beginPanel(const char* title, bool* open, ImVec2 defaultPos, float width) {
    const Theme& t = T();
    ImGui::SetNextWindowPos(defaultPos, ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(S(width), 0), ImVec2(S(width), FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(4));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, line());
    ImGui::PushStyleColor(ImGuiCol_WindowBg, t.pnl);
    ImGui::PushStyleColor(ImGuiCol_Border, t.bd2);
    const bool visible = ImGui::Begin(title, open,
                                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                          ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
    if (!visible) return false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetWindowPos();
    const float w = S(width), h = S(26);
    dl->AddRectFilled(ImVec2(p.x, p.y + h - line()), ImVec2(p.x + w, p.y + h), t.bd);
    drawText(dl, fontSemibold, 12, p.x + S(9), p.y + (h - line()) * 0.5f, t.tx, title);
    Row r;
    r.dl = dl;
    r.cy = p.y + (h - line()) * 0.5f;
    r.x = p.x + w - S(3) - S(20);
    if (r.button("close", Icon::Close, nullptr, 0, nullptr, nullptr, 14, 20)) *open = false;
    ImGui::SetCursorScreenPos(ImVec2(p.x + S(9), p.y + h + S(8)));
    ImGui::BeginGroup();
    return true;
}

void endPanel() {
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(1, S(8)));
    ImGui::End();
}

void panelRow(const char* key, const std::string& value, bool mono = false, float keyWidth = 84) {
    const Theme& t = T();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = S(20), avail = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - S(9) - (p.x + S(keyWidth));
    ImGui::Dummy(ImVec2(1, h));
    drawText(dl, fontRegular, 12, p.x, p.y + h * 0.5f, t.tx3, key);
    ImFont* f = mono ? fontMono : fontRegular;
    const std::string shown = ellipsize(f, 12, value, avail);
    drawText(dl, f, 12, p.x + S(keyWidth), p.y + h * 0.5f, t.tx, shown.c_str());
}

void panelHeading(const char* text) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(1, S(22)));
    drawText(dl, fontSemibold, 11, p.x, p.y + S(13), T().tx3, text);
}

std::string fileTime(const fs::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    SYSTEMTIME st;
    FILETIME local;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ||
        !FileTimeToLocalFileTime(&data.ftLastWriteTime, &local) || !FileTimeToSystemTime(&local, &st))
        return "-";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

void infoPanel() {
    if (!app.showInfo) return;
    const float width = 300;
    if (!beginPanel("Info", &app.showInfo, ImVec2(app.vpX + app.vpW - S(width) - S(12), app.vpY + S(12)), width)) return;
    if (!app.hasImage) {
        panelRow("", "No image");
    } else {
        const htv::FormatInfo& info = htv::formatInfo(app.image.format);
        panelHeading("FILE");
        panelRow("Name", utf8(app.shownPath.filename().wstring()));
        panelRow("Folder", utf8(app.shownPath.parent_path().wstring()));
        panelRow("Size", formatBytes(app.fileSize), true);
        panelRow("Modified", fileTime(app.shownPath), true);
        panelHeading("IMAGE");
        panelRow("Dimensions", std::to_string(app.image.width) + " \xC3\x97 " + std::to_string(app.image.height), true);
        panelRow("Format", app.image.description);
        panelRow("GPU format", info.name, true);
        panelRow("Mips", app.image.mips.size() > 1 ? std::to_string(app.image.mips.size()) + " in file"
                                                   : std::to_string(app.textureMips) + " generated for display");
        if (app.image.maxValue > 0) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.4g", double(app.image.maxValue));
            panelRow("Max value", buf, true);
        }
        if (!app.image.note.empty()) panelRow("Note", app.image.note);
    }
    endPanel();
}

void helpPanel() {
    if (!app.showHelp) return;
    const float width = 420;
    if (!beginPanel("Keyboard shortcuts", &app.showHelp,
                    ImVec2(std::round(app.vpX + (app.vpW - S(width)) * 0.5f), app.vpY + S(40)), width))
        return;
    static const char* rows[][2] = {
        {"Ctrl+O", "Open"}, {"Left / Right", "Previous / next file"}, {"F", "Fit to window"},
        {"1 2 4 8", "Zoom 100% to 800%"}, {"Wheel", "Zoom at the cursor"}, {"Drag", "Pan (left or middle button)"},
        {"Ctrl+Wheel", "Exposure"}, {"+ / - / 0", "Exposure up, down, reset"}, {"R G B A", "Channel (again for RGB)"},
        {"Ctrl+L / Ctrl+R", "Rotate left / right"}, {"H / V", "Flip horizontal / vertical"},
        {"Shift+A", "Next alpha background"}, {"T", "Next tiling mode"}, {"Ctrl+G", "Pixel grid"},
        {"P", "Pixel outline"}, {"I", "Info"}, {"Space", "Lock the pixel readout"}, {"Ctrl+C", "Copy the color value"},
        {"Tab", "Hide or show the bars"}, {"Ctrl+T", "Always on top"}, {"Alt+Wheel", "Window opacity"},
        {"Alt+Drag", "Move the window"}, {"F1", "This list"}, {"Esc", "Close panel, then quit"},
    };
    const Theme& t = T();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (const auto& row : rows) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = S(24);
        ImGui::Dummy(ImVec2(1, h));
        kbdChip(dl, p.x, p.y + h * 0.5f, row[0]);
        drawText(dl, fontRegular, 12, p.x + S(130), p.y + h * 0.5f, t.tx, row[1]);
    }
    endPanel();
}

}  // namespace

// ---- public ----

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
    bottomBarH = line() + S(30) + S(24);
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

    pendingTip = PendingTip();
    nextHoles.clear();
    menuOpenThisFrame = 0;
    viewportOverlays();
    if (app.showBars) {
        topBar();
        bottomBar();
    }
    infoPanel();
    helpPanel();
    drawTooltip();
    holes.swap(nextHoles);
    menuOpenLastFrame = menuOpenThisFrame;
}

bool uiKeyDown(int vk) {
    if (ImGui::GetIO().WantTextInput) return false;
    const bool ctrl = GetKeyState(VK_CONTROL) & 0x8000, shift = GetKeyState(VK_SHIFT) & 0x8000;
    auto channel = [](Channel c) { app.channel = app.channel == c ? Channel::RGB : c; };
    switch (vk) {
        case 'O': if (ctrl) showOpenDialog(); else return false; break;
        case VK_LEFT:
        case VK_PRIOR:
        case VK_BACK: step(-1); break;
        case VK_RIGHT:
        case VK_NEXT: step(+1); break;
        case 'F': app.fit = true; fitToWindow(); break;
        case '1': zoomCentered(1.0f); break;
        case '2': zoomCentered(2.0f); break;
        case '4': zoomCentered(4.0f); break;
        case '8': zoomCentered(8.0f); break;
        case VK_OEM_PLUS:
        case VK_ADD: app.exposure = std::min(app.exposure + 0.5f, 10.0f); break;
        case VK_OEM_MINUS:
        case VK_SUBTRACT: app.exposure = std::max(app.exposure - 0.5f, -10.0f); break;
        case '0': app.exposure = 0.0f; break;
        case 'R': if (ctrl) rotate(+1); else channel(Channel::R); break;
        case 'L': if (ctrl) rotate(-1); else return false; break;
        case 'G': if (ctrl) app.grid = !app.grid; else channel(Channel::G); break;
        case 'B': channel(Channel::B); break;
        case 'A': if (shift) { cycleAlphaBackground(); app.ignoreAlpha = false; } else channel(Channel::A); break;
        case 'H': if (!ctrl) app.flipH = !app.flipH; else return false; break;
        case 'V': app.flipV = !app.flipV; break;
        case 'T': if (ctrl) setTopmost(!app.topmost); else setTiling(Tiling((int(app.tiling) + 1) % 3)); break;
        case 'P': app.pixelOutline = !app.pixelOutline; break;
        case 'I': app.showInfo = !app.showInfo; break;
        case 'C': if (ctrl) copyColorValue(); else return false; break;
        case VK_SPACE: toggleLock(readPixelUnderCursor()); break;
        case VK_TAB: toggleBars(); break;
        case VK_F1: app.showHelp = !app.showHelp; break;
        case VK_ESCAPE:
            if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return false;  // ImGui closes the menu
            if (app.showHelp) app.showHelp = false;
            else if (app.showInfo) app.showInfo = false;
            else DestroyWindow(app.hwnd);
            break;
        default: return false;
    }
    return true;
}

}  // namespace htv
