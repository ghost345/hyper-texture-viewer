// The top bar (actions and modes) and the bottom bar (slider row and status), with their menus.

#include "ui_internal.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>

namespace htv::ui {

double cleanHintUntil = 0;

namespace {

// ---- pixel readout ----

struct Readout {
    bool valid = false;       // a pixel is under the cursor (or locked)
    bool readable = false;    // its values could be read
    int x = 0, y = 0;
    bool inTile = false;
    int tileX = 0, tileY = 0;  // copy of the 3x3 preview relative to the original: -1, 0 or 1
    float raw[4] = {0, 0, 0, 1};      // stored values of the pixel
    float display[4] = {0, 0, 0, 1};  // as an sRGB colour in 0..1, before view adjustments
    float avgRaw[4] = {0, 0, 0, 1};   // averaged over the Avg area
    float avgDisplay[4] = {0, 0, 0, 1};
    bool outOfRange = false;  // values outside 0..1
};

float srgbEncode(float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

// n x n stored values around (x, y), edge pixels repeated. Block-compressed formats go through the GPU.
bool readBlock(int x, int y, int n, float* out) {
    const htv::FormatInfo& info = htv::formatInfo(app.image.format);
    const int half = n / 2;
    if (!info.compressed) {
        for (int dy = -half; dy <= half; ++dy)
            for (int dx = -half; dx <= half; ++dx) {
                const int px = std::clamp(x + dx, 0, int(app.image.width) - 1);
                const int py = std::clamp(y + dy, 0, int(app.image.height) - 1);
                if (!htv::readPixel(app.image, uint32_t(px), uint32_t(py), out + ((dy + half) * n + dx + half) * 4))
                    return false;
            }
        return true;
    }
    if (!readPixelsGpu(x, y, n, out)) return false;
    for (int i = 0; i < n * n; ++i) {
        float* p = out + i * 4;
        if (info.channels == 1) p[1] = p[2] = p[0];
        if (info.channels < 4) p[3] = 1.0f;
        if (info.channels == 2) p[2] = 0.0f;
        if (isSrgbFormat(app.image.format))  // the GPU decoded them; the readout shows stored values
            for (int c = 0; c < 3; ++c) p[c] = srgbEncode(p[c]);
    }
    return true;
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
        if (r.valid && app.tiling != Tiling::Off) {
            const float w = float(app.rotation & 1 ? app.image.height : app.image.width) * app.zoom;
            const float h = float(app.rotation & 1 ? app.image.width : app.image.height) * app.zoom;
            r.inTile = true;
            r.tileX = std::clamp(int(std::floor((app.mouseX - app.originX) / w)), -1, 1);
            r.tileY = std::clamp(int(std::floor((app.mouseY - app.originY) / h)), -1, 1);
        }
    }
    if (!r.valid) return r;
    const int n = app.averageSize;
    float block[25 * 4];
    if (!readBlock(r.x, r.y, n, block)) return r;
    r.readable = true;
    const bool encode = isFloatFormat(htv::formatInfo(app.image.format)) && app.dataIsLinear;
    const float* center = block + (n * n / 2) * 4;
    for (int c = 0; c < 4; ++c) {
        float sum = 0;
        for (int i = 0; i < n * n; ++i) sum += block[i * 4 + c];
        r.raw[c] = center[c];
        r.avgRaw[c] = sum / float(n * n);
        if (r.raw[c] < 0.0f || r.raw[c] > 1.0f) r.outOfRange = true;
        r.display[c] = (c < 3 && encode) ? srgbEncode(r.raw[c]) : std::clamp(r.raw[c], 0.0f, 1.0f);
        r.avgDisplay[c] = (c < 3 && encode) ? srgbEncode(r.avgRaw[c]) : std::clamp(r.avgRaw[c], 0.0f, 1.0f);
    }
    return r;
}

const char* kColorFormatNames[int(ColorFormat::Count)] = {"HTML", "Hex", "Delphi Hex", "VB Hex", "RGB",
                                                          "RGB Float", "HSV", "HSL", "Long"};
// The longest text of each format, so the status row does not move as the value changes.
const char* kColorFormatWidest[int(ColorFormat::Count)] = {
    "#FFFFFF", "FFFFFF", "$00FFFFFF", "&H00FFFFFF&", "255, 255, 255", "0.000, 0.000, 0.000",
    "360\xC2\xB0, 100%, 100%", "360\xC2\xB0, 100%, 100%", "16777215"};
const char* kColorFormatTips[int(ColorFormat::Count)] = {
    "HTML color", "Hex RGB", "Delphi hex: $00BBGGRR", "Visual Basic hex: &H00BBGGRR&", "RGB, 0-255",
    "RGB as floats: stored values for float images, 0-1 for others", "Hue, saturation, value",
    "Hue, saturation, lightness", "Long: BGR packed into one integer (Windows COLORREF)"};

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

std::string formatColor(const float display[4], const float raw[4], ColorFormat f) {
    int c8[3];
    for (int i = 0; i < 3; ++i) c8[i] = int(std::lround(display[i] * 255.0f));
    const long bgr = long(c8[0]) | (long(c8[1]) << 8) | (long(c8[2]) << 16);
    char buf[96];
    switch (f) {
        case ColorFormat::Html: std::snprintf(buf, sizeof buf, "#%02X%02X%02X", c8[0], c8[1], c8[2]); break;
        case ColorFormat::Hex: std::snprintf(buf, sizeof buf, "%02X%02X%02X", c8[0], c8[1], c8[2]); break;
        case ColorFormat::DelphiHex: std::snprintf(buf, sizeof buf, "$00%06lX", bgr); break;
        case ColorFormat::VbHex: std::snprintf(buf, sizeof buf, "&H00%06lX&", bgr); break;
        case ColorFormat::Rgb: std::snprintf(buf, sizeof buf, "%d, %d, %d", c8[0], c8[1], c8[2]); break;
        case ColorFormat::RgbFloat:
            std::snprintf(buf, sizeof buf, "%.3f, %.3f, %.3f", double(raw[0]), double(raw[1]), double(raw[2]));
            break;
        case ColorFormat::Hsv:
        case ColorFormat::Hsl: {
            float h, sv, v, sl, l;
            hsvHsl(display, h, sv, v, sl, l);
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

Readout lastReadout;  // for Ctrl+C

void toggleLock(const Readout& r) {
    if (app.pixelLocked) app.pixelLocked = false;
    else if (r.valid) {
        app.pixelLocked = true;
        app.lockedX = r.x;
        app.lockedY = r.y;
    }
}

// Formats that round or clip the value get a warning next to it.
bool valueIsApproximate(const Readout& px) {
    return px.readable && app.colorFormat != ColorFormat::RgbFloat && (isHighPrecision(app.image) || px.outOfRange);
}

ImU32 swatchColor(const float display[4]) {
    return IM_COL32(int(display[0] * 255 + 0.5f), int(display[1] * 255 + 0.5f), int(display[2] * 255 + 0.5f), 255);
}

// ---- menus behind the dropdowns ----

const char* kCheckerNames[3] = {"small", "medium", "large"};

void zoomMenuItems() {
    if (menuItem({"Fit to window", keyText(Action::Fit), app.fit, true, true})) {
        app.fit = true;
        fitToWindow();
    }
    menuSeparator();
    static const float levels[] = {0.125f, 0.25f, 0.5f, 1, 2, 4, 8, 16, 32};
    static const char* keys[] = {nullptr, nullptr, nullptr, "1", nullptr, nullptr, nullptr, nullptr, nullptr};
    for (int i = 0; i < int(std::size(levels)); ++i) {
        const std::string label = formatZoom(levels[i]);
        if (menuItem({label.c_str(), keys[i], !app.fit && app.zoom == levels[i], true, true})) zoomCentered(levels[i]);
    }
}

void textureMenuItems() {
    menuHeading("Color data");
    if (menuItem({"sRGB", "Shift+L", !app.dataIsLinear, true, true})) app.dataIsLinear = false;
    if (menuItem({"Linear", "Shift+L", app.dataIsLinear, true, true})) app.dataIsLinear = true;
    menuSeparator();
    menuHeading("Mip level");
    const bool fileMips = app.hasImage && app.textureMips > 1;
    if (menuItem({"Auto", nullptr, app.forcedMip < 0, true, true})) app.forcedMip = -1;
    if (fileMips) {
        for (int i = 0; i < app.textureMips; ++i) {
            char label[64], size[32];
            std::snprintf(label, sizeof label, "Mip %d", i);
            std::snprintf(size, sizeof size, "%u\xC3\x97%u", std::max(1u, app.image.width >> i),
                          std::max(1u, app.image.height >> i));
            ImGui::PushID(i);
            MenuItem m{label, nullptr, app.forcedMip == i, true, true};
            m.value = size;
            if (menuItem(m)) app.forcedMip = i;
            ImGui::PopID();
        }
    }
    menuNote("[ and ] step through the mips");
    menuSeparator();
    menuItem({"Cube face", "soon", false, false, true});
    menuItem({"EXR layer", "soon", false, false, true});
}

void alphaMenuItems() {
    const bool checker = app.alphaBackground == AlphaBackground::Checker && !app.ignoreAlpha;
    MenuItem m{"Checkerboard", nullptr, checker, true, true};
    m.key = kCheckerNames[app.checkerSize];
    if (menuItem(m)) {
        app.alphaBackground = AlphaBackground::Checker;
        app.ignoreAlpha = false;
    }
    // Checker size: S, M and L pick the cell size directly.
    {
        const Theme& t = T();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = std::max(ImGui::GetContentRegionAvail().x, S(196) - 2 * S(4)), h = S(24);
        ImGui::Dummy(ImVec2(w, h));
        const float cy = p.y + h * 0.5f;
        drawText(dl, fontRegular, 12, p.x + S(8) + S(14) + S(8), cy, t.tx, "Checker size");
        // Drawn as the mono text "S · M · L", each letter clickable.
        static const char* letters[3] = {"S", "M", "L"};
        const float cw = textSize(fontMono, 11, "M").x;
        const float x0 = p.x + w - S(8) - 9 * cw;
        drawText(dl, fontMono, 11, x0 + 2 * cw, cy, t.tx3, "\xC2\xB7");
        drawText(dl, fontMono, 11, x0 + 6 * cw, cy, t.tx3, "\xC2\xB7");
        for (int i = 0; i < 3; ++i) {
            const float x = x0 + 4 * i * cw;
            const ImVec2 ha(x - S(3), p.y + S(3)), hb(x + cw + S(3), p.y + h - S(3));
            ImGui::SetCursorScreenPos(ha);
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("size", ImVec2(hb.x - ha.x, hb.y - ha.y))) {
                app.checkerSize = i;
                app.alphaBackground = AlphaBackground::Checker;
                app.ignoreAlpha = false;
            }
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            const bool on = app.checkerSize == i;
            if (hovered) dl->AddRectFilled(ha, hb, t.ctlh, S(3));
            drawText(dl, fontMono, 11, x, cy, on ? t.act : hovered ? t.tx : t.tx3, letters[i]);
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    }
    menuSeparator();
    static const char* solids[3] = {"Black", "Middle grey", "White"};
    for (int i = 0; i < 3; ++i)
        if (menuItem({solids[i], nullptr, int(app.alphaBackground) == i + 1 && !app.ignoreAlpha, true, true})) {
            app.alphaBackground = AlphaBackground(i + 1);
            app.ignoreAlpha = false;
        }
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (menuItem({"Custom color\xE2\x80\xA6", nullptr, app.alphaBackground == AlphaBackground::Custom && !app.ignoreAlpha,
                      true, true}))
            chooseCustomBackground();
        const float w = ImGui::GetItemRectSize().x;
        const float sw = S(12);
        const ImVec2 a(p.x + w - S(8) - sw, std::round(p.y + (S(24) - sw) * 0.5f));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(a, ImVec2(a.x + sw, a.y + sw),
                          IM_COL32(int(app.customBackground[0] * 255), int(app.customBackground[1] * 255),
                                   int(app.customBackground[2] * 255), 255));
        dl->AddRect(a, ImVec2(a.x + sw, a.y + sw), T().bd2, 0, 0, line());
    }
    menuSeparator();
    if (menuItem({"Ignore alpha", nullptr, app.ignoreAlpha, true, true})) app.ignoreAlpha = !app.ignoreAlpha;
}

void tilingMenuItems() {
    menuHeading("Tiling");
    if (menuItem({"Off", keyText(Action::Tiling), app.tiling == Tiling::Off, true, true})) setTiling(Tiling::Off);
    if (menuItem({"3\xC3\x97" "3 seamless", keyText(Action::Tiling), app.tiling == Tiling::Seamless, true, true})) setTiling(Tiling::Seamless);
    if (menuItem({"3\xC3\x97" "3 with borders", keyText(Action::Tiling), app.tiling == Tiling::Bordered, true, true}))
        setTiling(Tiling::Bordered);
    menuSeparator();
    menuNote("T cycles through the modes");
}

// ---- top bar ----

bool noImage() { return !app.hasImage; }

// Controls that move into the "..." menu one at a time as the window narrows, in bar order.
enum class Tool { ActualSize, Zoom, RotL, RotR, FlipH, FlipV, Channels, TexMode, AlphaBg, Tiling, Grid, Ruler,
                  Match, Histogram, Info, Layers, Compare, OnTop, Settings, Help, Count };

// Which goes first: the least used, keeping the tools that read the image and the channel switch for last.
const Tool kHideOrder[] = {Tool::Help,      Tool::Settings, Tool::Ruler, Tool::Grid,       Tool::FlipV,
                           Tool::FlipH,     Tool::RotR,  Tool::RotL,     Tool::Tiling,     Tool::AlphaBg,
                           Tool::OnTop,     Tool::Layers, Tool::Histogram, Tool::Info,     Tool::Match,
                           Tool::Compare,   Tool::ActualSize, Tool::Zoom, Tool::TexMode,   Tool::Channels};
constexpr int kHideable = int(sizeof kHideOrder / sizeof kHideOrder[0]);

// How the bar fits the window: the first `hidden` tools of kHideOrder sit in the "..." menu; once they all
// do, the file name gets shorter and at last the folder counter goes.
struct TopLayout {
    int hidden = 0;
    float nameMax = 240;
    bool counter = true;
    bool worstCase = false;  // measure the widest the bar can get at this layout, whatever the file

    bool shows(Tool tool) const {
        for (int i = 0; i < hidden; ++i)
            if (kHideOrder[i] == tool) return false;
        return true;
    }
    bool menu() const { return hidden > 0; }
};

void fileGroup(Row& r, const TopLayout& L) {
    const Theme& t = T();
    if (r.button("open", Icon::Open, nullptr, 0, "Open", keyText(Action::Open))) showOpenDialog();
    const int prevNext = app.folder.size() > 1 ? 0 : kDisabled;
    if (r.button("prev", Icon::Prev, nullptr, prevNext, "Previous file", "Left")) step(-1);
    if (r.button("next", Icon::Next, nullptr, prevNext, "Next file", "Right")) step(+1);
    const std::string name = fileName();
    const bool none = name.empty();
    const std::string shown = ellipsize(fontMedium, 13, none ? std::string("No image") : name, S(L.nameMax));
    const float nameW = L.worstCase ? S(L.nameMax) : textSize(fontMedium, 13, shown.c_str()).x;
    const bool loading = isLoadingVisible();
    const bool failed = (!app.error.empty() && !app.loading) || L.worstCase;
    if (!r.measure) {
        const ImVec2 a(r.x + S(6), r.cy - S(10)), b(a.x + nameW, r.cy + S(10));
        drawText(r.dl, fontMedium, 13, a.x, r.cy, none ? t.tx3 : loading ? t.tx2 : t.tx, shown.c_str());
        if (!none) {
            ImGui::SetCursorScreenPos(a);
            ImGui::InvisibleButton("filename", ImVec2(std::max(1.0f, nameW), b.y - a.y));
            r.hole(ImRect(a, b));
            const std::string full = app.fromClipboard && app.error.empty() ? "Pasted from the clipboard"
                                                                             : utf8(app.path.wstring());
            requestTip(ImRect(ImVec2(a.x, r.cy - S(12)), b), full.c_str(), keyText(Action::CopyPath));
            if (ImGui::IsItemClicked()) copyFilePath();
        }
    }
    r.x += S(6) + nameW + S(6) + r.gap;
    if (loading || failed) {
        if (!r.measure) {
            const ImVec2 c(r.x + S(7), std::round(r.cy));
            if (loading) spinner(r.dl, c, S(7) * 9.0f / 12.0f, t.ac);
            else drawIcon(r.dl, Icon::Alert, ImVec2(r.x, std::round(r.cy - S(7))), S(14), t.err);
        }
        r.x += S(14) + r.gap + S(6);
    }
    if (app.folderIndex >= 0 && L.counter) {
        char pos[32];
        std::snprintf(pos, sizeof pos, "%d / %d", app.folderIndex + 1, int(app.folder.size()));
        r.field("counter", pos, fontMono, 12, t.tx3, "Position in the folder", nullptr);
        r.x += S(2);
    }
}

void viewGroup(Row& r, const TopLayout& L) {
    const int off = noImage() ? kDisabled : 0;
    r.separator(16, 5);
    if (r.button("fit", Icon::Fit, nullptr, (app.fit && !off ? kOn : 0) | off, "Fit to window", keyText(Action::Fit))) {
        app.fit = true;
        fitToWindow();
    }
    if (L.shows(Tool::ActualSize) && r.button("actual", Icon::Count, "1:1", (!app.fit && app.zoom == 1.0f && !off ? kOn : 0) | off, "Actual size", keyText(Action::ActualSize)))
        zoomCentered(1.0f);
    if (L.shows(Tool::Zoom)) {
        const std::string zoom = off ? std::string("\xE2\x80\x94") : formatZoom(app.zoom);
        ImRect zr;
        toggleMenuOnPress(r.dropdown("zoom", zoom.c_str(), fontRegular, 12, 22, 7, 4, "Zoom", "Wheel", &zr, off, "3200%"),
                          "zoom menu");
        if (!r.measure && beginMenu("zoom menu", ImVec2(zr.Min.x, zr.Max.y + S(4)))) {
            zoomMenuItems();
            endMenu();
        }
    }
    if (L.shows(Tool::RotL) && r.button("rotl", Icon::RotateLeft, nullptr, off, "Rotate left", keyText(Action::RotateLeft))) rotate(-1);
    if (L.shows(Tool::RotR) && r.button("rotr", Icon::RotateRight, nullptr, off, "Rotate right", keyText(Action::RotateRight))) rotate(+1);
    if (L.shows(Tool::FlipH) &&
        r.button("fliph", Icon::FlipH, nullptr, (app.flipH ? kOn : 0) | off, "Flip horizontal", keyText(Action::FlipH)))
        app.flipH = !app.flipH;
    if (L.shows(Tool::FlipV) &&
        r.button("flipv", Icon::FlipV, nullptr, (app.flipV ? kOn : 0) | off, "Flip vertical", keyText(Action::FlipV)))
        app.flipV = !app.flipV;
}

// The channel switch (.seg): RGB R G B A L.
void channelSwitch(Row& row) {
    static const char* labels[] = {"RGB", "R", "G", "B", "A", "L"};
    static const char* tips[] = {"All channels", "Red channel", "Green channel", "Blue channel", "Alpha channel", "Luminance"};
    const char* keys[] = {keyText(Action::AllChannels), keyText(Action::Red),   keyText(Action::Green),
                          keyText(Action::Blue),        keyText(Action::Alpha), keyText(Action::Luminance)};
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
    const bool off = noImage();
    const float alpha = off ? 0.38f : 1.0f;
    const float h = S(22);
    const ImVec2 a(row.x, std::round(row.cy - h * 0.5f)), b(a.x + total, a.y + h);
    float x = a.x + line();
    for (int i = 0; i < 6; ++i) {
        const float bx = x + (i ? line() : 0.0f), bw = widths[i] - (i ? line() : 0.0f);
        if (i) row.dl->AddRectFilled(ImVec2(x, a.y), ImVec2(x + line(), b.y), fade(t.bd2, alpha));
        ImGui::SetCursorScreenPos(ImVec2(bx, a.y + line()));
        ImGui::PushID(i);
        const bool pressed = ImGui::InvisibleButton("ch", ImVec2(bw, h - 2 * line())) && !off;
        ImGui::PopID();
        const bool on = !off && int(app.channel) == i;
        const bool hovered = !off && ImGui::IsItemHovered();
        const ImVec2 ra(bx, a.y + line()), rb(bx + bw, b.y - line());
        ImDrawFlags corners = (i == 0 ? ImDrawFlags_RoundCornersLeft : 0) | (i == 5 ? ImDrawFlags_RoundCornersRight : 0);
        if (!corners) corners = ImDrawFlags_RoundCornersNone;
        if (on) row.dl->AddRectFilled(ra, rb, fade(t.acf, alpha), S(2), corners);
        else if (hovered) row.dl->AddRectFilled(ra, rb, t.ctlh, S(2), corners);
        const float tw = textSize(fontMedium, 12, labels[i]).x;
        drawText(row.dl, fontMedium, 12, bx + std::round((bw - tw) * 0.5f), row.cy,
                 fade(on ? kWhite : hovered ? t.tx : t.tx2, alpha), labels[i]);
        if (!off) requestTip(ImRect(ra, rb), tips[i], keys[i], !row.topBar);
        if (pressed) app.channel = (on && i != 0) ? Channel::RGB : Channel(i);
        x += widths[i];
    }
    row.dl->AddRect(a, b, fade(t.bd2, alpha), S(3), 0, line());
    row.hole(ImRect(a, b));
    row.advance(total);
}

std::string textureModeLabel() {
    std::string mode = app.dataIsLinear ? "Linear" : "sRGB";
    if (app.forcedMip >= 0) mode += " \xC2\xB7 Mip " + std::to_string(app.forcedMip);
    return mode;
}

void channelsGroup(Row& r, const TopLayout& L) {
    if (!L.shows(Tool::Channels) && !L.shows(Tool::TexMode)) return;
    r.separator(16, 5);
    if (L.shows(Tool::Channels)) channelSwitch(r);
    if (!L.shows(Tool::TexMode)) return;
    ImRect tr;
    const std::string mode = textureModeLabel();
    toggleMenuOnPress(r.dropdown("texmode", mode.c_str(), fontRegular, 12, 22, 7, 4, "Texture mode", nullptr, &tr, noImage(),
                                 "Linear \xC2\xB7 Mip 10"),
                      "texmode menu");
    if (!r.measure && beginMenu("texmode menu", ImVec2(tr.Min.x, tr.Max.y + S(4)))) {
        textureMenuItems();
        endMenu();
    }
}

void backgroundGroup(Row& r, const TopLayout& L) {
    if (!L.shows(Tool::AlphaBg) && !L.shows(Tool::Tiling)) return;
    const int off = noImage() ? kDisabled : 0;
    r.separator(16, 5);
    const float x0 = r.x;
    const bool checker = app.alphaBackground == AlphaBackground::Checker && !app.ignoreAlpha;
    const bool alphaOpen = ImGui::IsPopupOpen("alpha menu");
    if (L.shows(Tool::AlphaBg)) {
        toggleMenuOnPress(r.button("alphabg", Icon::AlphaBackground, nullptr,
                                   kChevron | (checker && !off ? kOn : 0) | (alphaOpen ? kOpen : 0) | off,
                                   "Alpha background", keyText(Action::AlphaBackground)),
                          "alpha menu");
        if (!r.measure && beginMenu("alpha menu", ImVec2(x0, r.cy + S(16)), ImVec2(0, 0), S(196))) {
            alphaMenuItems();
            endMenu();
        }
    }
    if (!L.shows(Tool::Tiling)) return;
    const float x1 = r.x;
    const bool tilingOpen = ImGui::IsPopupOpen("tiling menu");
    toggleMenuOnPress(r.button("tiling", Icon::Tiling, nullptr,
                               kChevron | (app.tiling != Tiling::Off && !off ? kOn : 0) | (tilingOpen ? kOpen : 0) | off,
                               "Tiling", keyText(Action::Tiling)),
                      "tiling menu");
    if (!r.measure && beginMenu("tiling menu", ImVec2(x1, r.cy + S(16)), ImVec2(0, 0), S(236))) {
        tilingMenuItems();
        endMenu();
    }
}

void overlaysGroup(Row& r, const TopLayout& L) {
    if (!L.shows(Tool::Grid) && !L.shows(Tool::Ruler)) return;
    const int off = noImage() ? kDisabled : 0;
    r.separator(16, 5);
    if (L.shows(Tool::Grid) &&
        r.button("grid", Icon::Grid, nullptr, (app.grid && !off ? kOn : 0) | off, "Pixel grid", keyText(Action::Grid)))
        app.grid = !app.grid;
    if (L.shows(Tool::Ruler) &&
        r.button("ruler", Icon::Ruler, nullptr, (app.ruler && !off ? kOn : 0) | off, "Ruler", keyText(Action::Ruler)))
        toggleRuler();
}

void analysisGroup(Row& r, const TopLayout& L) {
    const bool layers = isAnimation() || hasLayers();  // the button only exists for files with frames or layers
    if (!L.shows(Tool::Match) && !L.shows(Tool::Histogram) && !L.shows(Tool::Info) && !(layers && L.shows(Tool::Layers))) return;
    const int off = noImage() ? kDisabled : 0;
    r.separator(16, 5);
    if (L.shows(Tool::Match) && r.button("match", Icon::ColorMatch, nullptr, (app.colorMatch && !off ? kOn : 0) | off, "Color match",
                 keyText(Action::ColorMatch)))
        toggleColorMatch();
    if (L.shows(Tool::Histogram) && r.button("histogram", Icon::Histogram, nullptr, (app.showHistogram && !off ? kOn : 0) | off, "Histogram", keyText(Action::Histogram)))
        app.showHistogram = !app.showHistogram;
    const int infoOff = noImage() && app.error.empty() ? kDisabled : 0;
    if (L.shows(Tool::Info) && r.button("info", Icon::Info, nullptr, (app.showInfo && !infoOff ? kOn : 0) | infoOff, "Info", keyText(Action::Info)))
        app.showInfo = !app.showInfo;
    if (layers && L.shows(Tool::Layers) &&
        r.button("layers", Icon::Layers, nullptr, app.showFrames ? kOn : 0, isAnimation() ? "Frames" : "Layers",
                 keyText(Action::Layers)))
        toggleFramesOrLayers();
}

// The "..." menu: the tools the bar has no room for, in bar order, a separator between groups.
void moreMenuItems(const TopLayout& L) {
    const bool off = noImage();
    int group = -1;
    auto item = [&](Tool tool, int g) {
        if (L.shows(tool)) return false;
        if (group >= 0 && g != group) menuSeparator();
        group = g;
        return true;
    };
    if (item(Tool::ActualSize, 0) &&
        menuItem({"Actual size", keyText(Action::ActualSize), !app.fit && app.zoom == 1.0f && !off, !off, false, Icon::Search}))
        zoomCentered(1.0f);
    if (item(Tool::Zoom, 0) && submenu("more zoom", "Zoom", Icon::Search)) {
        zoomMenuItems();
        endSubmenu();
    }
    if (item(Tool::RotL, 0) && menuItem({"Rotate left", keyText(Action::RotateLeft), false, !off, false, Icon::RotateLeft})) rotate(-1);
    if (item(Tool::RotR, 0) && menuItem({"Rotate right", keyText(Action::RotateRight), false, !off, false, Icon::RotateRight})) rotate(+1);
    if (item(Tool::FlipH, 0) && menuItem({"Flip horizontal", keyText(Action::FlipH), app.flipH, !off, false, Icon::FlipH}))
        app.flipH = !app.flipH;
    if (item(Tool::FlipV, 0) && menuItem({"Flip vertical", keyText(Action::FlipV), app.flipV, !off, false, Icon::FlipV}))
        app.flipV = !app.flipV;
    if (item(Tool::Channels, 1) && submenu("more channels", "Channels", Icon::Opacity)) {
        static const char* names[] = {"All channels", "Red", "Green", "Blue", "Alpha", "Luminance"};
        static const Action keys[] = {Action::AllChannels, Action::Red, Action::Green, Action::Blue, Action::Alpha, Action::Luminance};
        for (int i = 0; i < 6; ++i)
            if (menuItem({names[i], keyText(keys[i]), int(app.channel) == i, !off, true})) app.channel = Channel(i);
        endSubmenu();
    }
    if (item(Tool::TexMode, 1) && submenu("more texmode", "Texture mode", Icon::Image)) {
        textureMenuItems();
        endSubmenu();
    }
    if (item(Tool::AlphaBg, 2) && submenu("more alpha", "Alpha background", Icon::AlphaBackground)) {
        alphaMenuItems();
        endSubmenu();
    }
    if (item(Tool::Tiling, 2) && submenu("more tiling", "Tiling", Icon::Tiling)) {
        tilingMenuItems();
        endSubmenu();
    }
    if (item(Tool::Grid, 3) && menuItem({"Pixel grid", keyText(Action::Grid), app.grid, !off, false, Icon::Grid})) app.grid = !app.grid;
    if (item(Tool::Ruler, 3) && menuItem({"Ruler", keyText(Action::Ruler), app.ruler, !off, false, Icon::Ruler})) toggleRuler();
    if (item(Tool::Match, 4) && menuItem({"Color match", keyText(Action::ColorMatch), app.colorMatch, !off, false, Icon::ColorMatch}))
        toggleColorMatch();
    if (item(Tool::Histogram, 4) &&
        menuItem({"Histogram", keyText(Action::Histogram), app.showHistogram, !off, false, Icon::Histogram}))
        app.showHistogram = !app.showHistogram;
    if (item(Tool::Info, 4) &&
        menuItem({"Info", keyText(Action::Info), app.showInfo, !(off && app.error.empty()), false, Icon::Info}))
        app.showInfo = !app.showInfo;
    if ((isAnimation() || hasLayers()) && item(Tool::Layers, 4) &&
        menuItem({isAnimation() ? "Frames" : "Layers", keyText(Action::Layers), app.showFrames, true, false, Icon::Layers}))
        toggleFramesOrLayers();
    if (item(Tool::Compare, 4) && menuItem({"Compare", keyText(Action::Compare), app.compare, !off, false, Icon::Compare}))
        toggleCompare();
    if (!kBrowser && item(Tool::OnTop, 5) && menuItem({"Always on top", keyText(Action::AlwaysOnTop), app.topmost, true, false, Icon::OnTop}))
        setTopmost(!app.topmost);
    if (item(Tool::Settings, 6) && menuItem({"Settings", keyText(Action::Settings), app.showSettings, true, false, Icon::Settings}))
        app.showSettings = !app.showSettings;
    if (item(Tool::Help, 6) && menuItem({"Keyboard shortcuts", keyText(Action::Help), app.showHelp, true, false, Icon::Help}))
        app.showHelp = !app.showHelp;
}

void compareGroup(Row& r, const TopLayout& L) {
    r.separator(16, 5);
    const int cmpOff = noImage() ? kDisabled : 0;
    if (L.shows(Tool::Compare) && r.button("compare", Icon::Compare, nullptr,
                 (app.compare && !cmpOff ? kOn : 0) | cmpOff, "Compare", keyText(Action::Compare)))
        toggleCompare();
    if (!L.menu()) return;
    const float x0 = r.x;
    const bool open = ImGui::IsPopupOpen("more menu");
    toggleMenuOnPress(r.button("more", Icon::More, nullptr, open ? kOpen : 0, "More tools", nullptr), "more menu");
    if (!r.measure && beginMenu("more menu", ImVec2(x0, r.cy + S(16)), ImVec2(0, 0), S(250))) {
        moreMenuItems(L);
        endMenu();
    }
}

// Always on top, Transparent to mouse and opacity: what the window does on the desktop.
void windowTools(Row& r, const TopLayout& L) {
    if (L.shows(Tool::OnTop) &&
        r.button("ontop", Icon::OnTop, nullptr, app.topmost ? kOn : 0, "Always on top", keyText(Action::AlwaysOnTop)))
        setTopmost(!app.topmost);
    // Transparent to mouse: only for a see-through or always-on-top window, and always labelled with its key,
    // since the button itself can't be clicked while it is on.
    if (app.opacity < 1.0f || app.topmost || app.clickThrough || L.worstCase) {
        const char* key = keyText(Action::ClickThrough);
        if (!key) key = "Ctrl+T";
        if (r.button("clickthrough", Icon::ClickThrough, key, kMonoLabel | (app.clickThrough ? kOn : 0),
                     "Transparent to mouse: clicks go to the window below. To turn it off, bring this window to the "
                     "front (Alt+Tab or the taskbar) and press the key again",
                     key))
            setClickThrough(!app.clickThrough);
    }
    char pct[16];
    std::snprintf(pct, sizeof pct, "%d%%", L.worstCase ? 100 : int(std::lround(app.opacity * 100)));
    const bool open = ImGui::IsPopupOpen("opacity menu");
    toggleMenuOnPress(r.button("opacity", Icon::Opacity, pct, kMonoLabel | (app.opacity < 1.0f ? kOn : 0) | (open ? kOpen : 0),
                               "Window opacity", "Alt+Wheel"),
                      "opacity menu");
    if (!r.measure) opacityPopup(ImVec2(r.x - r.gap, r.cy + S(16)));
}

void rightGroup(Row& r, const TopLayout& L) {
    if (!kBrowser) windowTools(r, L);
    if (L.shows(Tool::Settings) &&
        r.button("settings", Icon::Settings, nullptr, app.showSettings ? kOn : 0, "Settings", keyText(Action::Settings)))
        app.showSettings = !app.showSettings;
    if (L.shows(Tool::Help) &&
        r.button("help", Icon::Help, nullptr, app.showHelp ? kOn : 0, "Keyboard shortcuts", keyText(Action::Help)))
        app.showHelp = !app.showHelp;
    r.separator(16, 5);
    r.x -= S(5) + r.gap;  // the last separator has no right margin
}

// Minimise, maximise and close (.wc): 40 wide, the bar's height, no gap between them.
float windowButtons(Row& r) {
    if (kBrowser) return 0;  // the browser has its own
    const float w = S(40), h = topBarH - line();
    if (r.measure) return 3 * w;
    const Theme& t = T();
    const Icon icons[3] = {Icon::Minimize, windowMaximized() ? Icon::Restore : Icon::Maximize, Icon::Close};
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
            if (i == 0) minimizeWindow();
            else if (i == 1) toggleMaximize();
            else quitApp();
        }
    }
    return 3 * w;
}

void leftGroups(Row& r, const TopLayout& L) {
    fileGroup(r, L);
    viewGroup(r, L);
    channelsGroup(r, L);
    backgroundGroup(r, L);
    overlaysGroup(r, L);
    analysisGroup(r, L);
    compareGroup(r, L);
}

}  // namespace

void cycleAlphaBackground() {
    app.ignoreAlpha = false;
    app.alphaBackground = AlphaBackground((int(app.alphaBackground) + 1) % int(AlphaBackground::Count));
}

// The original stays where it is and the copies appear around it.
void setTiling(Tiling t) { app.tiling = t; }

void toggleLockPixel() { toggleLock(lastReadout); }

void copyColorValue() {
    if (!lastReadout.readable) return;
    ImGui::SetClipboardText(formatColor(lastReadout.display, lastReadout.raw, app.colorFormat).c_str());
    flashCopied(ImGui::GetID("##colorvalue"));
}

void copyAllColorFormats() {
    if (!lastReadout.readable) return;
    std::string all;
    for (int i = 0; i < int(ColorFormat::Count); ++i)
        all += std::string(kColorFormatNames[i]) + ": " + formatColor(lastReadout.display, lastReadout.raw, ColorFormat(i)) + "\r\n";
    ImGui::SetClipboardText(all.c_str());
    flashCopied(ImGui::GetID("##colorvalue"));
}

void opacityPopup(ImVec2 anchor) {
    if (!beginMenu("opacity menu", anchor, ImVec2(1, 0))) return;
    const Theme& t = T();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p(p0.x + S(6), p0.y + S(6));  // 10 px padding: 4 from the menu plus 6
    const float w = S(214);
    ImGui::Dummy(ImVec2(w + 2 * S(6), S(6) + S(16) + S(10) + S(16) + S(8) + S(18) + S(6)));
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d%%", int(std::lround(app.opacity * 100)));
    float y = p.y + S(8);
    drawText(dl, fontSemibold, 12, p.x, y, t.tx, "Window opacity");
    drawText(dl, fontMono, 12, p.x + w - textSize(fontMono, 12, buf).x, y, t.tx, buf);
    y += S(8) + S(10) + S(8);
    float v = (app.opacity - 0.1f) / 0.9f;
    bool reset = false;
    if (slider(dl, "opacity slider", p.x, y, 214, v, 0.0f, &reset)) setOpacity(0.1f + v * 0.9f);
    if (reset) setOpacity(1.0f);
    y += S(8) + S(8) + S(9);
    const float kw = kbdChip(dl, p.x, y, "Alt");
    drawText(dl, fontRegular, 11, p.x + kw + S(6), y, t.tx3, "+ wheel anywhere");
    if (!app.layerError.empty()) {
        // Windows refused the translucent window; say why instead of failing silently.
        ImGui::Dummy(ImVec2(w, S(18)));
        drawText(dl, fontRegular, 11, p.x, y + S(18), t.err, ellipsize(fontRegular, 11, app.layerError, w).c_str());
        requestTip(ImRect(ImVec2(p.x, y + S(10)), ImVec2(p.x + w, y + S(26))), app.layerError.c_str(), nullptr, true);
    }
    endMenu();
}

static float tightestWidth = 0;

float topBarMinWidth() { return tightestWidth; }

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

    // Find the widest layout that fits, measuring without drawing.
    auto width = [&](const TopLayout& L) {
        r.measure = true;
        r.x = 0;
        leftGroups(r, L);
        float w = S(6) + r.x + S(8);
        r.x = 0;
        rightGroup(r, L);
        w += r.x + windowButtons(r);
        return w;
    };
    // Tools go into "..." one at a time, then the file name gives up width down to 60 px, then the counter goes.
    TopLayout L;
    while (L.hidden < kHideable && width(L) > W) ++L.hidden;
    while (L.nameMax > 60.0f && width(L) > W) L.nameMax = std::max(60.0f, L.nameMax - 4.0f);
    if (width(L) > W) L.counter = false;
    // The window can't get narrower than the bar at its tightest.
    TopLayout tightest;
    tightest.hidden = kHideable;
    tightest.nameMax = 60.0f;
    tightest.counter = false;
    tightest.worstCase = true;
    tightestWidth = std::ceil(width(tightest));
    TopLayout right = L;
    r.measure = true;
    r.x = 0;
    rightGroup(r, right);
    const float rightW = r.x + windowButtons(r);

    r.measure = false;
    r.x = S(6);
    leftGroups(r, L);
    r.x = W - rightW;
    rightGroup(r, L);
    windowButtons(r);
    ImGui::End();
}

// ---- bottom bar ----

namespace {

// 4 significant digits as in the design: 0.0625, 4.000, 48236.
// Stored float values in the status row: four decimals, fewer as the integer part grows.
std::string formatFloat(float v) {
    char buf[32];
    const float a = std::fabs(v);
    if (!std::isfinite(v)) std::snprintf(buf, sizeof buf, "%s", std::isnan(v) ? "NaN" : v > 0 ? "+Inf" : "-Inf");
    else if (a < 10.0f) std::snprintf(buf, sizeof buf, "%.4f", double(v));
    else if (a < 100.0f) std::snprintf(buf, sizeof buf, "%.3f", double(v));
    else if (a < 1000.0f) std::snprintf(buf, sizeof buf, "%.2f", double(v));
    else if (a < 100000.0f) std::snprintf(buf, sizeof buf, "%.0f", double(v));
    else std::snprintf(buf, sizeof buf, "%.2e", double(v));
    return buf;
}

std::string formatValue(float v) {
    char buf[32];
    const float a = std::fabs(v);
    if (v == 0) return "0";
    if (a >= 1000) std::snprintf(buf, sizeof buf, "%.0f", double(v));
    else if (a >= 100) std::snprintf(buf, sizeof buf, "%.1f", double(v));
    else if (a >= 10) std::snprintf(buf, sizeof buf, "%.2f", double(v));
    else if (a >= 1) std::snprintf(buf, sizeof buf, "%.3f", double(v));
    else if (a >= 0.001f) std::snprintf(buf, sizeof buf, "%.4f", double(v));
    else std::snprintf(buf, sizeof buf, "%.2e", double(v));
    return buf;
}

void label(ImDrawList* dl, float& x, float cy, const char* text, float gap, const char* tip) {
    drawText(dl, fontMedium, 11, x, cy, T().tx3, text);
    const float w = textSize(fontMedium, 11, text).x;
    tipArea(text, ImRect(ImVec2(x, cy - S(8)), ImVec2(x + w, cy + S(8))), tip);
    x += w + gap;
}

void vs2(ImDrawList* dl, float& x, float cy, float gap) {
    x += S(4);
    dl->AddRectFilled(ImVec2(x, std::round(cy - S(14))), ImVec2(x + line(), std::round(cy - S(14)) + S(28)), T().bd);
    x += line() + S(4) + gap;
}

// Display gamma behind a small button in the HDR row: viewers for compositing (Nuke, tev, RV) offer it, but it is
// rarely needed and does not earn a slider of its own.
void gammaPopup(ImVec2 anchor) {
    if (!beginMenu("gamma menu", anchor, ImVec2(0, 1))) return;
    const Theme& t = T();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p(p0.x + S(6), p0.y + S(6));
    const float w = S(214);
    ImGui::Dummy(ImVec2(w + 2 * S(6), S(6) + S(16) + S(10) + S(16) + S(8) + S(10) + S(6)));
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.2f", double(app.gamma));
    float y = p.y + S(8);
    drawText(dl, fontSemibold, 12, p.x, y, t.tx, "Display gamma");
    drawText(dl, fontMono, 12, p.x + w - textSize(fontMono, 12, buf).x, y, t.tx, buf);
    y += S(8) + S(10) + S(8);
    float g = (std::log2(app.gamma) + 2.0f) / 4.0f;  // 0.25 .. 4 on a log scale, 1.0 in the middle
    bool reset = false;
    if (slider(dl, "gamma", p.x, y, 214, g, 0.5f, &reset, 0.5f, 0.5f))
        app.gamma = g == 0.5f ? 1.0f : std::exp2(g * 4.0f - 2.0f);
    if (reset) app.gamma = 1.0f;
    y += S(8) + S(8) + S(5);
    drawText(dl, fontMono, 10, p.x, y, t.tx3, "0.25");
    drawText(dl, fontMono, 10, p.x + std::round(w * 0.5f - textSize(fontMono, 10, "1.0").x * 0.5f), y, t.tx3, "1.0");
    drawText(dl, fontMono, 10, p.x + w - textSize(fontMono, 10, "4.0").x, y, t.tx3, "4.0");
    endMenu();
}

// The Range control: black and white points on a log2 scale over the image's values, with the luminance
// histogram above and marks for the image minimum, SDR white (1.0), the display peak and the maximum.
void rangeSlider(ImDrawList* dl, float x, float cy, float width) {
    const Theme& t = T();
    const htv::ImageStats& st = app.stats;
    const float w = S(width), top = std::round(cy - S(20));
    const float minStop = st.valid && st.minLuminance > 0 ? std::log2(st.minLuminance) : -8.0f;
    const float maxStop = st.valid && st.maxValue > 0 ? std::log2(st.maxValue) : 4.0f;
    float lo = std::min({minStop, std::log2(std::max(app.whitePoint, 1e-6f)) - 1.0f, 0.0f});
    if (app.blackPoint > 0) lo = std::min(lo, std::log2(app.blackPoint));
    float hi = std::max({maxStop, std::log2(std::max(app.whitePoint, 1e-6f)), 1.0f});
    if (app.displayPeak > 0) hi = std::max(hi, std::log2(app.displayPeak));
    const float span = std::max(hi - lo, 2.0f);
    lo -= span * 0.055f;
    hi += span * 0.02f;
    // While a point is dragged the scale stays put: it would otherwise grow with the white point under a
    // mouse held at the end and run away. It catches up on release.
    static int dragging = -1;
    static float dragLo = 0, dragHi = 0;
    if (dragging >= 0) {
        lo = dragLo;
        hi = dragHi;
    }
    auto pos = [&](float v) { return v <= 0 ? 0.0f : std::clamp((std::log2(v) - lo) / (hi - lo), 0.0f, 1.0f); };
    auto valueAt = [&](float p) { return p <= 0.005f ? 0.0f : std::exp2(lo + p * (hi - lo)); };

    // Mini histogram, 18 px high.
    if (st.valid) {
        const int bins = std::max(8, int(w / S(3)));
        const std::vector<float> h = st.histogramBetween(lo, hi, bins);
        const float peak = *std::max_element(h.begin(), h.end());
        if (peak > 0) {
            const ImU32 c = hex(0x8a8a8a, 0.55f);
            for (int i = 0; i < bins; ++i) {
                const float bh = std::round(h[size_t(i)] / peak * S(18));
                if (bh <= 0) continue;
                const float x0 = std::round(x + w * i / bins), x1 = std::round(x + w * (i + 1) / bins);
                dl->AddRectFilled(ImVec2(x0, top + S(2) + S(18) - bh), ImVec2(x1, top + S(2) + S(18)), c);
            }
        }
    }
    // Track and the fill between the points.
    const float ty = top + S(21);
    dl->AddRectFilled(ImVec2(x, ty), ImVec2(x + w, ty + S(4)), t.bd2, S(2));
    const float bx = std::round(x + pos(app.blackPoint) * w), wx = std::round(x + pos(app.whitePoint) * w);
    if (wx > bx) dl->AddRectFilled(ImVec2(bx, ty), ImVec2(wx, ty + S(4)), t.ac, S(2));

    // Marks and their labels; a label that would overlap one already drawn is left out.
    std::vector<std::pair<float, float>> taken;
    auto mark = [&](float v, ImU32 col, const char* text, int align, bool dashed) {
        if (v <= 0) return;
        const float mx = std::round(x + pos(v) * w);
        if (dashed) {
            for (float y = top + S(2); y < top + S(28); y += S(4))
                dl->AddRectFilled(ImVec2(mx, y), ImVec2(mx + line(), std::min(y + S(2), top + S(28))), col);
        } else {
            dl->AddRectFilled(ImVec2(mx, top + S(2)), ImVec2(mx + line(), top + S(28)), col);
        }
        const float lw = textSize(fontMono, 10, text).x;
        float lx = align < 0 ? mx - lw : align == 0 ? mx - std::round(lw * 0.5f) : mx + S(7);
        lx = std::clamp(lx, x - S(4), x + w - lw + S(4));
        for (auto [a, b] : taken)
            if (lx < b + S(4) && lx + lw > a - S(4)) return;
        taken.push_back({lx, lx + lw});
        drawText(dl, fontMono, 10, lx, top + S(35), col, text);
    };
    char buf[48];
    mark(1.0f, t.tx2, "1.0", 0, false);
    if (app.displayPeak > 0) {
        std::snprintf(buf, sizeof buf, "peak %.3g", double(app.displayPeak));
        mark(app.displayPeak, t.warn, buf, 1, true);
    }
    if (st.valid) {
        mark(st.minLuminance, t.tx3, "min", 0, false);
        std::snprintf(buf, sizeof buf, "max %s", formatValue(st.maxValue).c_str());
        mark(st.maxValue, t.tx3, buf, -1, false);
    }

    // Knobs: black point dark, white point light.
    auto knob = [&](float kx, ImU32 fill, ImU32 border) {
        const ImVec2 a(kx - S(4), top + S(15)), b(a.x + S(8), a.y + S(16));
        dl->AddRectFilled(a, b, fill, S(2));
        dl->AddRect(a, b, border, S(2), 0, line());
    };
    knob(bx, hex(0x111111), hex(0x9a9a9a));
    knob(wx, hex(0xf2f2f2), t.knb);

    // Dragging moves the nearer point; a double click resets to 0 .. 1.
    // The points stick to 0, 1.0 and the marks when dragged close to them (Alt drags freely).
    ImGui::SetCursorScreenPos(ImVec2(x - S(4), top + S(8)));
    ImGui::InvisibleButton("range", ImVec2(w + S(8), S(28)));
    const float mouse = mouseClientX();
    const float mp = std::clamp((mouse - x) / w, 0.0f, 1.0f);
    if (ImGui::IsItemActivated()) {
        dragging = std::fabs(mp * w - (bx - x)) <= std::fabs(mp * w - (wx - x)) ? 0 : 1;
        dragLo = lo;
        dragHi = hi;
    }
    if (dragging >= 0 && !leftButtonHeld()) dragging = -1;
    if (dragging >= 0) {
        const float minMark = st.valid ? pos(st.minLuminance) : -1.0f, maxMark = st.valid ? pos(st.maxValue) : -1.0f;
        const float peakMark = app.displayPeak > 0 ? pos(app.displayPeak) : -1.0f;
        if (dragging == 0) {
            const float p = snapPosition(mp, {0.0f, minMark, pos(1.0f)}, w);
            app.blackPoint = std::min(p == pos(1.0f) ? 1.0f : p == minMark ? st.minLuminance : valueAt(p),
                                      app.whitePoint * 0.99f);
        } else {
            const float p = snapPosition(mp, {pos(1.0f), peakMark, maxMark, minMark}, w);
            // Past the right end the white point keeps rising, slowly: a stop for every 60 px.
            const float beyond = std::max(0.0f, mouse - (x + w)) / S(60);
            const float v = beyond > 0       ? std::exp2(hi + beyond)
                            : p == pos(1.0f) ? 1.0f
                            : p == peakMark  ? app.displayPeak
                            : p == maxMark   ? st.maxValue
                            : p == minMark   ? st.minLuminance
                                             : valueAt(std::max(p, 0.006f));
            app.whitePoint = std::max(v, app.blackPoint * 1.01f + 1e-6f);
        }
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        app.blackPoint = 0.0f;
        app.whitePoint = 1.0f;
    }
    requestTip(ImRect(ImVec2(x, top), ImVec2(x + w, top + S(30))), "Black and white points, in stops; double-click resets",
               nullptr, true);
}

const char* kToneMapNames[int(ToneMap::Count)] = {"Linear clip", "Reinhard", "ACES", "AgX"};

// HDR and 16-bit images (Hdr artboard): Exposure | Range | Tone map | Gamma | Clip, NaN.
void hdrRow(ImDrawList* dl, float y0, float W) {
    const Theme& t = T();
    const float h = S(44);
    dl->AddRectFilled(ImVec2(0, y0 + h - line()), ImVec2(W, y0 + h), t.bd);
    const float cy = y0 + (h - line()) * 0.5f;
    const float gap = S(8);
    float x = S(12);
    char buf[32];

    // Everything but the range track has a fixed width; the track takes what is left, up to 520 px.
    const float nanW = textSize(fontMono, 11, "NaN 000000").x;
    const float fixed = S(12) + textSize(fontMedium, 11, "Exposure").x + gap + S(180) + gap + S(70) + gap +
                        (S(8) + line() + gap) * 4 + textSize(fontMedium, 11, "Range").x + gap + S(58) + gap + gap +
                        S(58) + gap + textSize(fontRegular, 12, "Auto").x + S(14) + gap +
                        textSize(fontMedium, 11, "Tone map").x + gap + textSize(fontRegular, 12, "Linear clip").x +
                        S(30) + gap + textSize(fontMono, 12, "\xCE\xB3 0.00").x + S(14) + gap + S(16) +
                        textSize(fontRegular, 12, "Clip").x + S(15) +
                        gap + S(12) + gap + nanW + S(12);
    const float rangeW = std::clamp((W - fixed) / s, 120.0f, 520.0f);

    label(dl, x, cy, "Exposure", gap, "Exposure in stops (EV): 2^EV multiplies the values before display");
    float e = (app.exposure + 10.0f) / 20.0f;
    bool reset = false;
    if (slider(dl, "exposure", x, cy, 180, e, 0.5f, &reset, 0.5f))
        app.exposure = std::round((e * 20.0f - 10.0f) * 20.0f) / 20.0f;
    if (reset) app.exposure = 0.0f;
    requestTip(ImRect(ImVec2(x, cy - S(8)), ImVec2(x + S(180), cy + S(8))), "Exposure in stops; double-click resets",
               "Ctrl+Wheel", true);
    x += S(180) + gap;
    std::snprintf(buf, sizeof buf, "%+.2f EV", double(app.exposure));
    float ev = app.exposure;
    if (numberField(dl, "exposure value", x, cy, 70, buf, ev, "Click to type a value")) app.exposure = std::clamp(ev, -10.0f, 10.0f);
    x += S(70) + gap;
    vs2(dl, x, cy, gap);

    label(dl, x, cy, "Range", gap, "Black and white points: values between them fill the display range");
    float black = app.blackPoint, white = app.whitePoint;
    if (numberField(dl, "black point", x, cy, 58, formatValue(app.blackPoint).c_str(), black, "Black point"))
        app.blackPoint = std::clamp(black, 0.0f, app.whitePoint * 0.99f);
    x += S(58) + gap;
    rangeSlider(dl, x, cy, rangeW);
    x += S(rangeW) + gap;
    if (numberField(dl, "white point", x, cy, 58, formatValue(app.whitePoint).c_str(), white, "White point"))
        app.whitePoint = std::max(white, app.blackPoint * 1.01f + 1e-6f);
    x += S(58) + gap;
    Row r;
    r.dl = dl;
    r.cy = cy;
    r.gap = gap;
    r.x = x;
    if (r.button("auto", Icon::Count, "Auto", kBordered | (app.stats.valid ? 0 : kDisabled), "Auto range from percentiles",
                 keyText(Action::AutoRange), 16, 22))
        autoRange();
    x = r.x;
    vs2(dl, x, cy, gap);

    label(dl, x, cy, "Tone map", gap, "How values above the white point are compressed for display");
    r.x = x;
    ImRect tr;
    toggleMenuOnPress(r.dropdown("tonemap", kToneMapNames[int(app.toneMap)], fontRegular, 12, 22, 7, 4,
                                 app.dataIsLinear ? "Tone mapping" : "Tone mapping applies to linear data", nullptr, &tr,
                                 false, "Linear clip"),
                      "tonemap menu");
    if (beginMenu("tonemap menu", ImVec2(tr.Min.x, tr.Min.y - S(4)), ImVec2(0, 1))) {
        for (int i = 0; i < int(ToneMap::Count); ++i)
            if (menuItem({kToneMapNames[i], nullptr, int(app.toneMap) == i, true, true})) app.toneMap = ToneMap(i);
        endMenu();
    }
    x = r.x;
    vs2(dl, x, cy, gap);

    r.x = x;
    std::snprintf(buf, sizeof buf, "\xCE\xB3 %.2f", double(app.gamma));
    const bool gammaOpen = ImGui::IsPopupOpen("gamma menu");
    toggleMenuOnPress(r.button("gamma", Icon::Count, buf,
                               kBordered | kMonoLabel | (app.gamma != 1.0f ? kOn : 0) | (gammaOpen ? kOpen : 0),
                               "Display gamma, applied after everything else; 1.00 shows the values as they are",
                               nullptr, 16, 22),
                      "gamma menu");
    gammaPopup(ImVec2(x, cy - S(16)));
    x = r.x;
    vs2(dl, x, cy, gap);

    r.x = x;
    if (r.button("clip", Icon::Clip, "Clip", app.showClipping ? kOn : 0, "Show clipping and NaN", keyText(Action::Clipping))) app.showClipping = !app.showClipping;
    x = r.x;
    const float sw = S(12);
    const float nanX = x;
    dl->AddRectFilled(ImVec2(x, std::round(cy - sw * 0.5f)), ImVec2(x + sw, std::round(cy - sw * 0.5f) + sw), kNan);
    x += sw + gap;
    if (app.stats.valid) std::snprintf(buf, sizeof buf, "NaN %llu", static_cast<unsigned long long>(app.stats.nonFinite));
    else std::snprintf(buf, sizeof buf, "NaN \xE2\x80\x93");
    drawText(dl, fontMono, 11, x, cy, t.tx3, buf);
    tipArea("nan", ImRect(ImVec2(nanX, cy - S(8)), ImVec2(x + textSize(fontMono, 11, buf).x, cy + S(8))),
            "Pixels that are NaN or infinite; with Clip on they show in magenta");
}

// Right side of the status row: texts separated by .vs, laid out from the right edge.
void statusRight(ImDrawList* dl, float cy, float W, float leftEnd, const std::vector<std::pair<std::string, ImU32>>& items,
                 const std::vector<const char*>& tips = {}) {
    const float vsW = S(6) * 2 + line(), gap = S(6);
    float x = W - S(4);
    for (size_t i = items.size(); i-- > 0;) {
        const auto& [text, col] = items[i];
        const float room = x - leftEnd - S(12);
        if (room < S(40)) return;
        const std::string shown = i == 0 ? ellipsize(fontMono, 12, text, room) : text;
        const float tw = textSize(fontMono, 12, shown.c_str()).x;
        x -= tw;
        drawText(dl, fontMono, 12, x, cy, col, shown.c_str());
        if (i < tips.size() && tips[i]) {
            ImGui::PushID(int(i));
            tipArea("status right", ImRect(ImVec2(x, cy - S(9)), ImVec2(x + tw, cy + S(9))), tips[i]);
            ImGui::PopID();
        }
        if (i > 0) {
            x -= gap + vsW;
            dl->AddRectFilled(ImVec2(x + S(6), std::round(cy - S(7))), ImVec2(x + S(6) + line(), std::round(cy - S(7)) + S(14)),
                              T().bd2);
            x -= gap;
        }
    }
}

std::string imageDescription() {
    std::string desc = std::to_string(app.image.width) + "\xC3\x97" + std::to_string(app.image.height);
    std::string d = app.image.description;
    // "PNG, 8 bit RGBA" reads as "PNG RGBA · 8-bit", as in the design.
    const size_t comma = d.find(", "), bit = d.find(" bit ");
    if (comma != std::string::npos && bit != std::string::npos && bit > comma &&
        d.find(", ", comma + 2) == std::string::npos)
        d = d.substr(0, comma) + " " + d.substr(bit + 5) + ", " + d.substr(comma + 2, bit - comma - 2) + "-bit";
    // "EXR, half A,B,G,R, ZIP" reads as "EXR half RGBA · ZIP": EXR lists its channels alphabetically.
    if (d.rfind("EXR, ", 0) == 0) {
        d.replace(3, 2, " ");
        d = rgbaOrder(d);
    }
    for (size_t p; (p = d.find(", ")) != std::string::npos;) d.replace(p, 2, " \xC2\xB7 ");
    return desc + " \xC2\xB7 " + d;
}

void statusRow(ImDrawList* dl, float y0, float W) {
    const Theme& t = T();
    const float h = S(24);
    Row r;
    r.dl = dl;
    r.cy = y0 + h * 0.5f;
    r.gap = S(6);
    r.x = S(12);

    // Empty window and read errors have their own short status.
    if (!app.hasImage) {
        if (app.loading) {
            r.text(isLoadingVisible() ? "Loading" : "", fontMono, 12, t.tx3);
        } else if (!app.error.empty()) {
            r.text("Read error", fontMono, 12, t.err);
            r.text(fileName().c_str(), fontMono, 12, t.tx3);
            statusRight(dl, r.cy, W, r.x, {{app.errorFormat.empty() ? std::string("?") : app.errorFormat, t.tx2},
                                           {formatBytes(app.loadingSize), t.tx2}});
        } else {
            r.text("Ready", fontMono, 12, t.tx2);
            statusRight(dl, r.cy, W, r.x, {{"v" HTV_VERSION, t.tx2}});
        }
        return;
    }
    if (isLoadingVisible()) {
        const std::string prev = "Showing previous: " + (app.fromClipboard ? std::string("Clipboard")
                                                                           : utf8(app.shownPath.filename().wstring()));
        r.text(prev.c_str(), fontMono, 12, t.tx3);
        statusRight(dl, r.cy, W, r.x,
                    {{imageDescription(), t.tx2}, {formatZoom(app.zoom), t.tx}, {formatBytes(app.fileSize), t.tx2}});
        return;
    }

    const Readout px = readPixelUnderCursor();
    lastReadout = px;
    const bool hdr = isHighPrecision(app.image);

    // Coordinates, padded to the image's digit count so the rest of the row stays put.
    const int digits = int(std::to_string(std::max(app.image.width, app.image.height)).size());
    char buf[96];
    const float xyStart = r.x;
    r.text("X", fontMono, 12, t.tx3);
    std::snprintf(buf, sizeof buf, "%*s", digits, px.valid ? std::to_string(px.x).c_str() : "-");
    r.text(buf, fontMono, 12, t.tx);
    r.text("Y", fontMono, 12, t.tx3);
    std::snprintf(buf, sizeof buf, "%*s", digits, px.valid ? std::to_string(px.y).c_str() : "-");
    r.text(buf, fontMono, 12, t.tx);
    tipArea("xy", ImRect(ImVec2(xyStart, r.cy - S(9)), ImVec2(r.x - r.gap, r.cy + S(9))),
            app.pixelLocked ? "Locked pixel, image coordinates" : "Pixel under the cursor, image coordinates", nullptr);
    if (app.tiling != Tiling::Off) {
        if (px.inTile) std::snprintf(buf, sizeof buf, "tile %d,%d", px.tileX, px.tileY);
        else std::snprintf(buf, sizeof buf, "tile -");
        r.field("tile", buf, fontMono, 12, t.tx3, "Copy of the tiling preview, relative to the original (0,0)", "tile -1,-1");
    }
    r.separator(14, 6);

    // Swatch and value; a click on the value copies it.
    const float sw = S(12);
    auto swatch = [&](const char* id, const float display[4], bool show, const char* tip) {
        const ImVec2 sa(r.x, std::round(r.cy - sw * 0.5f));
        if (show) dl->AddRectFilled(sa, ImVec2(sa.x + sw, sa.y + sw), swatchColor(display));
        dl->AddRect(sa, ImVec2(sa.x + sw, sa.y + sw), t.bd2, 0, 0, line());
        tipArea(id, ImRect(sa, ImVec2(sa.x + sw, sa.y + sw)), tip);
        r.advance(sw);
    };
    swatch("swatch", px.display, px.readable, "Color as displayed");
    const ImGuiID copyId = ImGui::GetID("##colorvalue");
    {
        const std::string value = px.readable ? formatColor(px.display, px.raw, app.colorFormat) : std::string("-");
        const float textW = textSize(fontMono, 12, value.c_str()).x;
        const float vw = std::max(textW, textSize(fontMono, 12, kColorFormatWidest[int(app.colorFormat)]).x);
        const ImVec2 va(r.x, r.cy - S(10));
        ImGui::SetCursorScreenPos(va);
        if (ImGui::InvisibleButton("value", ImVec2(vw, S(20))) && px.readable) copyColorValue();
        const bool hovered = ImGui::IsItemHovered() && px.readable;
        std::snprintf(buf, sizeof buf, "%s \xC2\xB7 click to copy", kColorFormatTips[int(app.colorFormat)]);
        requestTip(ImRect(va, ImVec2(r.x + vw, r.cy + S(10))), buf, keyText(Action::CopyValue), true);
        if (px.readable && copiedRecently(copyId)) {
            drawText(dl, fontMono, 12, r.x, r.cy, t.ok, "Copied");
        } else {
            drawText(dl, fontMono, 12, r.x, r.cy, px.readable ? t.tx : t.tx3, value.c_str());
            if (hovered) dl->AddRectFilled(ImVec2(r.x, r.cy + S(7)), ImVec2(r.x + textW, r.cy + S(7) + line()), t.tx3);
        }
        r.advance(vw);
        if (app.imageAlpha && !hdr) {
            if (px.readable)
                std::snprintf(buf, sizeof buf, "\xCE\xB1 %d", int(std::lround(std::clamp(px.raw[3], 0.0f, 1.0f) * 255)));
            else
                std::snprintf(buf, sizeof buf, "\xCE\xB1 -");
            r.field("alpha", buf, fontMono, 12, t.tx3, "Alpha, 0-255", "\xCE\xB1 255");
        }
    }
    // The warning slot is always kept so the row does not jump when it appears.
    {
        const ImVec2 wa(r.x, std::round(r.cy - S(7)));
        if (valueIsApproximate(px)) {
            drawIcon(dl, Icon::Warning, wa, S(14), t.warn);
            std::snprintf(buf, sizeof buf, "%s %s", kColorFormatNames[int(app.colorFormat)],
                          px.outOfRange ? "clips the value to 0-1 and rounds it to 8 bits" : "rounds the value to 8 bits");
            tipArea("warn", ImRect(wa, ImVec2(wa.x + S(14), wa.y + S(14))), buf);
        }
        r.advance(S(14));
    }

    ImRect fr;
    toggleMenuOnPress(r.dropdown("colorfmt", kColorFormatNames[int(app.colorFormat)], fontRegular, 11, 18, 6, 3,
                                 "Color value format", nullptr, &fr, false, "Delphi Hex"),
                      "colorfmt menu");
    if (beginMenu("colorfmt menu", ImVec2(fr.Min.x, fr.Min.y - S(4)), ImVec2(0, 1), S(300))) {
        menuHeading("Color format");
        for (int i = 0; i < int(ColorFormat::Count); ++i) {
            const std::string example = px.readable ? formatColor(px.display, px.raw, ColorFormat(i)) : std::string();
            MenuItem m{kColorFormatNames[i], nullptr, int(app.colorFormat) == i, true, true};
            if (px.readable) m.value = example.c_str();
            if (menuItem(m)) app.colorFormat = ColorFormat(i);
        }
        menuSeparator();
        menuNote("Click a value to copy", "Ctrl+Shift+C", "all");
        endMenu();
    }

    if (hdr) {
        // Exact values: float RGB in channel colours and luminance.
        r.separator(14, 6);
        r.field("float", "float", fontMono, 12, t.tx3, "Stored values, before exposure, range and tone mapping");
        const ImU32 channel[3] = {app.darkTheme ? hex(0xff8a80) : hex(0xc0322b), app.darkTheme ? hex(0x8fe39a) : hex(0x1e7f3e),
                                  app.darkTheme ? hex(0x8ab6ff) : hex(0x1f62d0)};
        static const char* ids[3] = {"red", "green", "blue"};
        static const char* tips[3] = {"Red, stored value", "Green, stored value", "Blue, stored value"};
        for (int c = 0; c < 3; ++c)
            r.field(ids[c], px.readable ? formatFloat(px.raw[c]).c_str() : "-", fontMono, 12, channel[c], tips[c], "-0.0000");
        const float lum = 0.2126f * px.raw[0] + 0.7152f * px.raw[1] + 0.0722f * px.raw[2];
        const float lumStart = r.x;
        r.text("L", fontMono, 12, t.tx3);
        r.field("luma", px.readable ? formatFloat(lum).c_str() : "-", fontMono, 12, t.tx, nullptr, "-0.0000");
        tipArea("luma tip", ImRect(ImVec2(lumStart, r.cy - S(9)), ImVec2(r.x - r.gap, r.cy + S(9))),
                "Luminance (Rec. 709): 0.2126 R + 0.7152 G + 0.0722 B");
        if (app.imageAlpha)
            r.field("float alpha", px.readable ? ("\xCE\xB1 " + formatFloat(px.raw[3])).c_str() : "\xCE\xB1 -", fontMono, 12,
                    t.tx3, "Alpha, stored value", "\xCE\xB1 0.0000");
    }
    if (r.button("copy", Icon::Copy, nullptr, px.readable ? 0 : kDisabled, "Copy value", keyText(Action::CopyValue), 14, 20)) copyColorValue();
    if (r.button("lock", Icon::Pin, nullptr, app.pixelLocked ? kOn : (px.valid ? 0 : kDisabled),
                 app.pixelLocked ? "Unlock pixel" : "Lock pixel", "Ctrl+Click", 14, 20))
        toggleLock(px);
    r.separator(14, 6);
    r.field("avg label", "Avg", fontMono, 12, t.tx3, "Average color over an area around the pixel");
    std::snprintf(buf, sizeof buf, "%d\xC3\x97%d", app.averageSize, app.averageSize);
    ImRect ar;
    toggleMenuOnPress(r.dropdown("avg", buf, fontRegular, 11, 18, 6, 3, "Average over an area", nullptr, &ar, false,
                                 "5\xC3\x97" "5"),
                      "avg menu");
    if (beginMenu("avg menu", ImVec2(ar.Min.x, ar.Min.y - S(4)), ImVec2(0, 1))) {
        for (int n : {1, 3, 5}) {
            char item[16];
            std::snprintf(item, sizeof item, "%d\xC3\x97%d", n, n);
            if (menuItem({item, nullptr, app.averageSize == n, true, true})) app.averageSize = n;
        }
        endMenu();
    }
    if (app.averageSize > 1) {
        std::snprintf(buf, sizeof buf, "Average of %d\xC3\x97%d pixels", app.averageSize, app.averageSize);
        swatch("avg swatch", px.avgDisplay, px.readable, buf);
        r.field("avg value", px.readable ? formatColor(px.avgDisplay, px.avgRaw, ColorFormat::Html).c_str() : "-", fontMono,
                12, t.tx2, buf, "#FFFFFF");
    }

    statusRight(dl, r.cy, W, r.x, {{imageDescription(), t.tx2}, {formatZoom(app.zoom), t.tx}, {formatBytes(app.fileSize), t.tx2}},
                {"Image size, format and bit depth", "Zoom", "File size"});
}

}  // namespace

float sliderRowHeight() { return app.hasImage && isHighPrecision(app.image) ? 44.0f : 0.0f; }

void bottomBar() {
    const Theme& t = T();
    const float W = float(app.winW), y0 = float(app.winH) - bottomBarH;
    ImGui::SetNextWindowPos(ImVec2(0, y0));
    ImGui::SetNextWindowSize(ImVec2(W, bottomBarH));
    ImGui::Begin("##bottom", nullptr, kBarFlags);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(0, y0), ImVec2(W, y0 + bottomBarH), t.bar);
    dl->AddRectFilled(ImVec2(0, y0), ImVec2(W, y0 + line()), t.bd);
    const float rowH = sliderRowHeight();
    if (rowH > 0) hdrRow(dl, y0 + line(), W);
    statusRow(dl, y0 + line() + S(rowH), W);
    ImGui::End();
}

}  // namespace htv::ui
