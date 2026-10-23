// Interface building blocks: tokens, text, tooltips, menus and controls drawn in the design's style.

#include "ui_internal.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace htv::ui {
namespace {

// Tokens artboard, dark and light. ctlp is control-pressed, ok is success.
const Theme kDark = {hex(0x1b1b1b), hex(0x252525), hex(0x1e1e1e), hex(0x2c2c2c), hex(0x262626, 0.94f),
                     hex(0x323232), hex(0x3c3c3c), hex(0x464646), hex(0x363636), hex(0x474747),
                     hex(0xe8e8e8), hex(0xb4b4b4), hex(0x969696), hex(0x4a8cff), hex(0x2f6fdb),
                     hex(0x8ab6ff), hex(0x4a8cff, 0.16f), hex(0xe6a92e), hex(0xff7b72), hex(0x5cc87a),
                     hex(0x3a3a3a), hex(0x4b4b4b), hex(0xdadada), hex(0x141414)};
const Theme kLight = {hex(0xc8c8c8), hex(0xf3f3f3), hex(0xffffff), hex(0xffffff), hex(0xfafafa, 0.96f),
                      hex(0xe4e4e4), hex(0xdadada), hex(0xcecece), hex(0xd4d4d4), hex(0xbdbdbd),
                      hex(0x1c1c1c), hex(0x474747), hex(0x626262), hex(0x1f62d0), hex(0x1f62d0),
                      hex(0x1a55b6), hex(0x1f62d0, 0.13f), hex(0x9a6400), hex(0xc0322b), hex(0x1e7f3e),
                      hex(0xd0d0d0), hex(0xeeeeee), hex(0xffffff), hex(0x8a8a8a)};

std::vector<ImRect> holesNext;

struct PendingTip {
    ImGuiID id = 0;
    ImRect rect;
    std::string text, key;
    bool above = false;
};
PendingTip pendingTip;
ImGuiID tipId = 0;
double tipSince = 0, tipLastShown = -10;

ImGuiID copiedId = 0;
double copiedAt = -10;

ImGuiID menuOpenLastFrame = 0, menuOpenThisFrame = 0;

}  // namespace

std::vector<ImRect> holes;  // read by uiIsCaption

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
float s = 1.0f;
float topBarH = 32, bottomBarH = 55;

static float em(ImFont* f) { return f == fontMono ? emMono : emRegular; }

ImVec2 textSize(ImFont* f, float size, const char* t, const char* end) {
    return f->CalcTextSizeA(size * s * em(f), FLT_MAX, 0.0f, t, end);
}

void drawText(ImDrawList* dl, ImFont* f, float size, float x, float cy, ImU32 col, const char* t, const char* end) {
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
    else if (n < 10 * 1024) std::snprintf(buf, sizeof buf, "%.1f KB", double(n) / 1024.0);  // "3.6 KB"
    else if (n < 1024 * 1024) std::snprintf(buf, sizeof buf, "%.0f KB", double(n) / 1024.0);  // "324 KB"
    else std::snprintf(buf, sizeof buf, "%.1f MB", double(n) / (1024.0 * 1024.0));
    return buf;
}

std::string formatBytesExact(uint64_t n) {
    std::string digits = std::to_string(n), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out += ' ';
        out += digits[i];
    }
    return out + " B";
}

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string fileName() {
    if (app.fromClipboard && !app.loading && app.error.empty()) return "Clipboard";
    return app.path.empty() ? std::string() : utf8(app.path.filename().wstring());
}

std::string rgbaOrder(std::string text) {
    for (const char* abc : {"A,B,G,R", "B,G,R"}) {
        const size_t p = text.find(abc);
        if (p != std::string::npos) text.replace(p, std::strlen(abc), abc[0] == 'A' ? "RGBA" : "RGB");
    }
    return text;
}

void addHole(ImRect r) { holesNext.push_back(r); }

// ---- tooltips ----

void requestTip(ImRect r, const char* text, const char* key, bool above) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) || !text) return;
    pendingTip.id = ImGui::GetItemID();
    pendingTip.rect = r;
    pendingTip.text = text;
    pendingTip.key = key ? key : "";
    pendingTip.above = above;
}

float kbdChip(ImDrawList* dl, float x, float cy, const char* key, bool draw, bool help) {
    const Theme& t = T();
    const float pad = S(help ? 5 : 4);
    const float w = textSize(fontMono, 11, key).x + 2 * pad + 2 * line();
    if (draw) {
        const float h = help ? S(18) : S(16) + 2 * line();
        const ImVec2 a(std::round(x), std::round(cy - h * 0.5f)), b(a.x + w, a.y + h);
        dl->AddRectFilled(a, b, t.bar2, S(3));
        dl->AddRect(a, b, t.bd2, S(3), 0, line());
        if (help) dl->AddRectFilled(ImVec2(a.x + S(1), b.y - 2 * line()), ImVec2(b.x - S(1), b.y), t.bd2);  // 2 px bottom
        drawText(dl, fontMono, 11, a.x + line() + pad, cy - (help ? line() * 0.5f : 0.0f), help ? t.tx : t.tx2, key);
    }
    return w;
}

// Tooltips show below the hovered control after a short delay, then instantly while moving along the bar.
void drawTooltip() {
    const double now = ImGui::GetTime();
    if (!pendingTip.id || ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        tipId = 0;
        pendingTip = PendingTip();
        return;
    }
    if (pendingTip.id != tipId) {
        tipId = pendingTip.id;
        tipSince = now;
    }
    if (now - tipSince < 0.5 && now - tipLastShown > 0.3) {
        pendingTip = PendingTip();
        return;
    }
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
    pendingTip = PendingTip();
}

// The loading spinner: a three-quarter arc that turns.
void spinner(ImDrawList* dl, ImVec2 c, float r, ImU32 color) {
    const float a0 = float(ImGui::GetTime() * 6.0);
    dl->PathArcTo(c, r, a0, a0 + IM_PI * 1.5f, 24);
    dl->PathStroke(color, 0, std::max(1.0f, r * 2.4f / 9.0f));
}

void flashCopied(ImGuiID id) {
    copiedId = id;
    copiedAt = ImGui::GetTime();
}

bool copiedRecently(ImGuiID id) { return copiedId == id && ImGui::GetTime() - copiedAt < 0.6; }

// ---- menus ----

void toggleMenuOnPress(bool pressed, const char* id) {
    if (pressed && menuOpenLastFrame != ImGui::GetID(id)) ImGui::OpenPopup(id);
}

bool beginMenu(const char* id, ImVec2 pos, ImVec2 pivot, float minWidth) {
    const Theme& t = T();
    ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing, pivot);
    if (minWidth > 0) ImGui::SetNextWindowSizeConstraints(ImVec2(minWidth, 0), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(4), S(4)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, S(4));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, line());
    ImGui::PushStyleColor(ImGuiCol_PopupBg, t.pop);
    ImGui::PushStyleColor(ImGuiCol_Border, t.bd2);
    // A submenu must not take focus, or its parent menu stops reacting to the mouse.
    const bool nested = GImGui->BeginPopupStack.Size > 0;
    if (ImGui::BeginPopup(id, ImGuiWindowFlags_NoMove | (nested ? ImGuiWindowFlags_NoFocusOnAppearing : 0))) {
        if (!nested) menuOpenThisFrame = ImGui::GetCurrentWindow()->PopupId;
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

void clearMenuState() { menuOpenThisFrame = 0; }
void finishMenuFrame() {
    menuOpenLastFrame = menuOpenThisFrame;
    holes.swap(holesNext);
    holesNext.clear();
}

namespace {

// Closes a submenu of the current menu when another of its items is hovered.
void closeSubmenus() {
    ImGuiContext& g = *GImGui;
    if (g.OpenPopupStack.Size > g.BeginPopupStack.Size) ImGui::ClosePopupToLevel(g.BeginPopupStack.Size, false);
}

// Shared drawing of a menu row (.mi): 24 px, 8 px padding, 8 px gaps, 12 px text.
bool menuRow(const MenuItem& m, bool highlight, bool submenuArrow, bool& hoveredOut) {
    const Theme& t = T();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float lead = (m.checkColumn ? S(14) + S(8) : 0.0f) + (m.icon != Icon::Count ? S(14) + S(8) : 0.0f);
    const char* right = m.value ? m.value : m.key;
    const float rightW = submenuArrow ? S(10) : right ? textSize(fontMono, 11, right).x : 0.0f;
    const float textW = textSize(fontRegular, 12, m.label).x;
    const float natural = S(8) + lead + textW + (rightW > 0 ? S(24) + rightW : 0.0f) + S(8);
    const float w = std::max(natural, ImGui::GetContentRegionAvail().x);
    const float h = S(24);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(m.label, ImVec2(w, h));
    const bool hovered = m.enabled && ImGui::IsItemHovered();
    hoveredOut = hovered;
    const bool lit = hovered || highlight;
    if (lit) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), t.acf, S(3));
    const float cy = p.y + h * 0.5f;
    const ImU32 col = lit ? kWhite : m.enabled ? t.tx : fade(t.tx, 0.38f);
    float x = p.x + S(8);
    if (m.checkColumn) {
        if (m.checked) drawIcon(dl, Icon::Check, ImVec2(x, std::round(cy - S(7))), S(14), col);
        x += S(14) + S(8);
    }
    if (m.icon != Icon::Count) {
        drawIcon(dl, m.icon, ImVec2(x, std::round(cy - S(7))), S(14), lit ? kWhite : m.enabled ? t.tx2 : fade(t.tx2, 0.38f));
        x += S(14) + S(8);
    }
    drawText(dl, fontRegular, 12, x, cy, col, m.label);
    if (submenuArrow) {
        drawIcon(dl, Icon::Next, ImVec2(p.x + w - S(8) - S(10), std::round(cy - S(5))), S(10),
                 lit ? fade(kWhite, 0.85f) : t.tx3, 2.0f);
    } else if (right) {
        ImU32 kc = m.value ? t.tx2 : t.tx3;
        if (lit) kc = fade(kWhite, m.value ? 0.9f : 0.85f);
        else if (!m.enabled) kc = fade(kc, 0.5f);
        drawText(dl, fontMono, 11, p.x + w - S(8) - rightW, cy, kc, right);
    }
    return pressed && m.enabled;
}

}  // namespace

bool menuItem(const MenuItem& m) {
    bool hovered = false;
    const bool pressed = menuRow(m, false, false, hovered);
    if (hovered) closeSubmenus();
    if (pressed) {
        ImGui::CloseCurrentPopup();
        return true;
    }
    return false;
}

bool submenu(const char* id, const char* label, Icon icon, bool checkColumn) {
    const bool open = ImGui::IsPopupOpen(id);
    MenuItem m{label};
    m.icon = icon;
    m.checkColumn = checkColumn;
    bool hovered = false;
    const bool pressed = menuRow(m, open, true, hovered);
    const ImRect r(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if ((hovered || pressed) && !open) {
        closeSubmenus();
        ImGui::OpenPopup(id);
    }
    // Opens to the right, its first item level with this one.
    return beginMenu(id, ImVec2(r.Max.x + S(2), r.Min.y - S(5)));
}

void endSubmenu() { endMenu(); }

void menuHeading(const char* label) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = S(4) + S(14) + S(4);
    ImGui::Dummy(ImVec2(textSize(fontSemibold, 11, label).x + 2 * S(8), h));
    drawText(dl, fontSemibold, 11, p.x + S(8), p.y + h * 0.5f, T().tx3, label);
}

// Footnote row (.mh, regular weight), optionally with a key chip and trailing text.
void menuNote(const char* text, const char* key, const char* after) {
    const Theme& t = T();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = S(4) + S(16) + S(4);
    const float textW = textSize(fontRegular, 11, text).x;
    const float keyW = key ? kbdChip(dl, 0, 0, key, false) : 0.0f;
    const float afterW = after ? textSize(fontRegular, 11, after).x : 0.0f;
    const float natural = S(8) + textW + (key ? S(12) + keyW : 0.0f) + (after ? S(6) + afterW : 0.0f) + S(8);
    const float w = std::max(natural, ImGui::GetContentRegionAvail().x);
    ImGui::Dummy(ImVec2(natural, h));
    const float cy = p.y + h * 0.5f;
    drawText(dl, fontRegular, 11, p.x + S(8), cy, t.tx3, text);
    if (key) {
        float x = p.x + w - S(8) - (after ? afterW + S(6) : 0.0f) - keyW;
        kbdChip(dl, x, cy, key);
        if (after) drawText(dl, fontRegular, 11, x + keyW + S(6), cy, t.tx3, after);
    }
}

void menuSeparator() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(1, S(4) * 2 + line()));
    dl->AddRectFilled(ImVec2(p.x + S(2), p.y + S(4)), ImVec2(p.x + w - S(2), p.y + S(4) + line()), T().bd);
}

// ---- controls ----

// Toolbar button (.b): icon, optional text and dropdown chevron; 4 px side padding, 6 with text.
bool Row::button(const char* id, Icon icon, const char* label, int flags, const char* tip, const char* key,
                 float iconSize, float height, float padding) {
    const bool hasIcon = icon != Icon::Count;
    const float pad = padding >= 0 ? S(padding) : S(label ? 6 : 4);
    ImFont* lf = flags & kMonoLabel ? fontMono : fontRegular;
    const float labelW = label ? textSize(lf, 12, label).x : 0.0f;
    float w = pad + (hasIcon ? S(iconSize) : 0.0f) + (hasIcon && label ? S(3) : 0.0f) + labelW +
              (flags & kChevron ? S(3) + S(10) : 0.0f) + pad;
    if (flags & kBordered) w += 2 * line();
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
    const bool held = hovered && ImGui::IsItemActive();
    ImU32 fg = flags & kBordered ? t.tx : t.tx2;
    if (flags & kOn) {
        dl->AddRectFilled(a, b, t.acs, S(3));
        fg = t.act;
    } else if (held) {
        dl->AddRectFilled(a, b, t.ctlp, S(3));
        fg = t.tx;
    } else if (hovered || (flags & kOpen)) {
        dl->AddRectFilled(a, b, t.ctlh, S(3));
        fg = t.tx;
    }
    if (flags & kBordered) dl->AddRect(a, b, t.bd2, S(3), 0, line());
    if (disabled) fg = fade(fg, 0.38f);
    float cx = a.x + pad + (flags & kBordered ? line() : 0.0f);
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

// Dropdown field (.dd): bordered box with a value and a chevron; accent border while its menu is open.
bool Row::dropdown(const char* id, const char* value, ImFont* f, float fontSize, float height, float padL, float padR,
                   const char* tip, const char* key, ImRect* outRect, bool disabled, const char* widest) {
    const float textW = std::max(textSize(f, fontSize, value).x, widest ? textSize(f, fontSize, widest).x : 0.0f);
    const float w = line() + S(padL) + textW + S(5) + S(10) + S(padR) + line();
    if (measure) {
        advance(w);
        return false;
    }
    const Theme& t = T();
    const float h = S(height);
    const ImVec2 a(x, std::round(cy - h * 0.5f)), b(a.x + w, a.y + h);
    ImGui::SetCursorScreenPos(a);
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h), ImGuiButtonFlags_PressedOnClick);
    const bool hovered = !disabled && ImGui::IsItemHovered();
    const bool open = ImGui::IsPopupOpen((std::string(id) + " menu").c_str());
    const float alpha = disabled ? 0.38f : 1.0f;
    dl->AddRectFilled(a, b, fade(hovered || open ? t.ctlh : t.ctl, alpha), S(3));
    dl->AddRect(a, b, fade(open ? t.ac : t.bd2, alpha), S(3), 0, line());
    const ImU32 fg = disabled ? fade(t.tx, 0.38f) : t.tx;
    drawText(dl, f, fontSize, a.x + line() + S(padL), cy, fg, value);
    drawIcon(dl, Icon::Chevron, ImVec2(b.x - line() - S(padR) - S(10), std::round(cy - S(5))), S(10), fade(fg, 0.75f));
    hole(ImRect(a, b));
    requestTip(ImRect(a, b), tip, key, !topBar);
    if (outRect) *outRect = ImRect(a, b);
    advance(w);
    return pressed && !disabled;
}

// Vertical separator (.sep in the top bar, .vs / .vs2 in the bottom bar).
void Row::separator(float height, float margin, ImU32 color) {
    x += S(margin);
    if (!measure)
        dl->AddRectFilled(ImVec2(x, std::round(cy - S(height) * 0.5f)),
                          ImVec2(x + line(), std::round(cy - S(height) * 0.5f) + S(height)), color ? color : T().bd2);
    x += line() + S(margin);
    x += gap;
}

void Row::text(const char* t, ImFont* f, float size, ImU32 col, float marginL, float marginR) {
    x += S(marginL);
    if (!measure) drawText(dl, f, size, x, cy, col, t);
    x += textSize(f, size, t).x + S(marginR);
    x += gap;
}

void Row::field(const char* id, const char* t, ImFont* f, float size, ImU32 col, const char* tip, const char* widest) {
    const float w = std::max(textSize(f, size, t).x, widest ? textSize(f, size, widest).x : 0.0f);
    if (!measure) {
        drawText(dl, f, size, x, cy, col, t);
        if (tip) {
            const ImRect r(ImVec2(x, cy - S(9)), ImVec2(x + w, cy + S(9)));
            tipArea(id, r, tip, nullptr, !topBar);
            hole(r);
        }
    }
    advance(w);
}

void tipArea(const char* id, ImRect r, const char* text, const char* key, bool above) {
    ImGui::SetCursorScreenPos(r.Min);
    ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, r.GetWidth()), std::max(1.0f, r.GetHeight())));
    requestTip(r, text, key, above);
}

float mouseClientX() {
    POINT p;
    if (!GetCursorPos(&p) || !ScreenToClient(app.hwnd, &p)) return ImGui::GetIO().MousePos.x;
    return float(p.x);
}

bool leftButtonHeld() {
    return GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000;
}

float snapPosition(float p, std::initializer_list<float> targets, float widthPx) {
    if (GetKeyState(VK_MENU) & 0x8000) return p;
    float best = p, bestDist = S(3);  // a mouse lands within a few pixels; more feels sticky
    for (float target : targets) {
        const float d = std::fabs(p - target) * widthPx;
        if (target >= 0 && d < bestDist) {
            best = target;
            bestDist = d;
        }
    }
    return best;
}

bool slider(ImDrawList* dl, const char* id, float x, float cy, float width, float& t, float fillFrom, bool* reset,
            float mark, float snap) {
    const Theme& th = T();
    const float w = S(width), knobW = S(8), knobH = S(16);
    ImGui::SetCursorScreenPos(ImVec2(x - knobW * 0.5f, std::round(cy - knobH * 0.5f)));
    ImGui::InvisibleButton(id, ImVec2(w + knobW, knobH));
    // The drag follows the mouse button itself, not ImGui's active item (see mouseClientX).
    static ImGuiID dragging = 0;
    const ImGuiID self = ImGui::GetItemID();
    if (ImGui::IsItemActivated()) dragging = self;
    if (dragging == self && !leftButtonHeld()) dragging = 0;
    bool changed = false;
    if (dragging == self) {
        const float p = std::clamp((mouseClientX() - x) / w, 0.0f, 1.0f);
        t = snapPosition(p, {mark, snap}, w);
        changed = true;
    }
    if (reset) *reset = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    const float ty = std::round(cy - S(2));
    dl->AddRectFilled(ImVec2(x, ty), ImVec2(x + w, ty + S(4)), th.bd2, S(2));
    const float kx = std::round(x + t * w), fx = std::round(x + fillFrom * w);
    if (kx != fx) dl->AddRectFilled(ImVec2(std::min(kx, fx), ty), ImVec2(std::max(kx, fx), ty + S(4)), th.ac, S(2));
    if (mark >= 0) {
        const float mx = std::round(x + mark * w);
        dl->AddRectFilled(ImVec2(mx, std::round(cy - S(6))), ImVec2(mx + line(), std::round(cy - S(6)) + S(12)), th.tx3);
    }
    const ImVec2 ka(kx - std::round(knobW * 0.5f), std::round(cy - knobH * 0.5f)), kb(ka.x + knobW, ka.y + knobH);
    dl->AddRectFilled(ka, kb, th.kn, S(2));
    dl->AddRect(ka, kb, th.knb, S(2), 0, line());
    return changed;
}

bool numberField(ImDrawList* dl, const char* id, float x, float cy, float width, const char* text, float& value,
                 const char* tip) {
    const Theme& t = T();
    const float w = S(width), h = S(22);
    const ImVec2 a(x, std::round(cy - h * 0.5f)), b(a.x + w, a.y + h);
    static ImGuiID editing = 0;
    static char buf[32];
    const ImGuiID gid = ImGui::GetID(id);
    if (editing == gid) {
        ImGui::SetCursorScreenPos(a);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(6) - line(), (h - textSize(fontMono, 12, "0").y) * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(3));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, line());
        ImGui::PushStyleColor(ImGuiCol_FrameBg, t.bar2);
        ImGui::PushStyleColor(ImGuiCol_Border, t.ac);
        ImGui::PushStyleColor(ImGuiCol_Text, t.tx);
        ImGui::PushFont(fontMono, 12 * emMono);
        ImGui::SetNextItemWidth(w);
        if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
        const bool done = ImGui::InputText(id, buf, sizeof buf,
                                           ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll |
                                               ImGuiInputTextFlags_CharsScientific);
        const bool left = ImGui::IsItemDeactivated();
        ImGui::PopFont();
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(3);
        if (done || left) {
            editing = 0;
            char* end = nullptr;
            const float v = std::strtof(buf, &end);
            if (done && end != buf && std::isfinite(v)) {
                value = v;
                return true;
            }
        }
        return false;
    }
    ImGui::SetCursorScreenPos(a);
    if (ImGui::InvisibleButton(id, ImVec2(w, h))) {
        editing = gid;
        std::snprintf(buf, sizeof buf, "%g", double(value));
    }
    const bool hovered = ImGui::IsItemHovered();
    dl->AddRectFilled(a, b, t.bar2, S(3));
    dl->AddRect(a, b, hovered ? t.tx3 : t.bd2, S(3), 0, line());
    drawText(dl, fontMono, 12, a.x + line() + S(6), cy, t.tx, ellipsize(fontMono, 12, text, w - 2 * S(6)).c_str());
    requestTip(ImRect(a, b), tip, nullptr, true);
    return false;
}

}  // namespace htv::ui
