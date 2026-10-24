// What sits on the image area: overlays, the empty and error cards, loading progress, notices and panels.

#include "ui_internal.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <ctime>

namespace fs = std::filesystem;

namespace htv::ui {

// ---- shared with the other panels (ui_panels.cpp) ----

// 1 px dashed line, black dashes on white (4 on, 4 off) so it shows on any colour. Horizontal or vertical only.
void dashedLine(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    const float dash = S(4), t = line();
    const bool horizontal = a.y == b.y;
    const float len = horizontal ? b.x - a.x : b.y - a.y;
    auto rect = [&](float from, float to, ImU32 c) {
        if (horizontal) dl->AddRectFilled(ImVec2(a.x + from, a.y), ImVec2(a.x + to, a.y + t), c);
        else dl->AddRectFilled(ImVec2(a.x, a.y + from), ImVec2(a.x + t, a.y + to), c);
    };
    rect(0, len, IM_COL32(255, 255, 255, 255));
    for (float p = 0; p < len; p += 2 * dash) rect(p, std::min(p + dash, len), IM_COL32(0, 0, 0, 255));
}

void dashedRect(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    const float t = line();
    dashedLine(dl, a, ImVec2(b.x, a.y));
    dashedLine(dl, ImVec2(a.x, b.y - t), ImVec2(b.x, b.y - t));
    dashedLine(dl, a, ImVec2(a.x, b.y));
    dashedLine(dl, ImVec2(b.x - t, a.y), ImVec2(b.x - t, b.y));
}

// Dashed outline in a theme colour (the empty window's drop area).
void dashedBox(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col) {
    const float d = S(3), t = line();
    for (float x = a.x + S(4); x < b.x - S(4); x += 2 * d) {
        dl->AddRectFilled(ImVec2(x, a.y), ImVec2(std::min(x + d, b.x - S(4)), a.y + t), col);
        dl->AddRectFilled(ImVec2(x, b.y - t), ImVec2(std::min(x + d, b.x - S(4)), b.y), col);
    }
    for (float y = a.y + S(4); y < b.y - S(4); y += 2 * d) {
        dl->AddRectFilled(ImVec2(a.x, y), ImVec2(a.x + t, std::min(y + d, b.y - S(4))), col);
        dl->AddRectFilled(ImVec2(b.x - t, y), ImVec2(b.x, std::min(y + d, b.y - S(4))), col);
    }
    // Rounded corners as short arcs.
    const float r = S(4);
    dl->PathArcTo(ImVec2(a.x + r, a.y + r), r - t * 0.5f, IM_PI, IM_PI * 1.5f, 6);
    dl->PathStroke(col, 0, t);
    dl->PathArcTo(ImVec2(b.x - r, a.y + r), r - t * 0.5f, IM_PI * 1.5f, IM_PI * 2.0f, 6);
    dl->PathStroke(col, 0, t);
    dl->PathArcTo(ImVec2(a.x + r, b.y - r), r - t * 0.5f, IM_PI * 0.5f, IM_PI, 6);
    dl->PathStroke(col, 0, t);
    dl->PathArcTo(ImVec2(b.x - r, b.y - r), r - t * 0.5f, 0, IM_PI * 0.5f, 6);
    dl->PathStroke(col, 0, t);
}

// A small square button with a 12 px icon, inside a chip or panel header.
bool smallButton(ImDrawList* dl, const char* id, Icon icon, ImVec2 a, float size, float iconSize, const char* tip,
                 const char* key) {
    const Theme& t = T();
    ImGui::SetCursorScreenPos(a);
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(S(size), S(size)));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) dl->AddRectFilled(a, ImVec2(a.x + S(size), a.y + S(size)), t.ctlh, S(3));
    const float o = std::round((S(size) - S(iconSize)) * 0.5f);
    drawIcon(dl, icon, ImVec2(a.x + o, a.y + o), S(iconSize), hovered ? t.tx : t.tx2);
    requestTip(ImRect(a, ImVec2(a.x + S(size), a.y + S(size))), tip, key);
    return pressed;
}

// Panel window: translucent, 1 px border, radius 4, dragged anywhere, kept inside the image area and left where
// the user put it.
bool beginPanel(const char* id, ImVec2 defaultPos, float width, float height) {
    const Theme& t = T();
    ImGui::SetNextWindowPos(defaultPos, ImGuiCond_FirstUseEver);
    if (height > 0) ImGui::SetNextWindowSize(ImVec2(width, height));
    else ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(4));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, line());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, t.pnl);
    ImGui::PushStyleColor(ImGuiCol_Border, t.bd2);
    const bool visible = ImGui::Begin(id, nullptr,
                                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                          (height > 0 ? 0 : ImGuiWindowFlags_AlwaysAutoResize) |
                                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                          ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
    if (!visible) return false;
    // Stay inside the image area.
    const ImVec2 p = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
    const ImVec2 c(std::clamp(p.x, app.vpX, std::max(app.vpX, app.vpX + app.vpW - sz.x)),
                   std::clamp(p.y, app.vpY, std::max(app.vpY, app.vpY + app.vpH - sz.y)));
    if (c.x != p.x || c.y != p.y) ImGui::SetWindowPos(c);
    return true;
}

// Panel header (.ph): title, extra buttons, close.
void panelHeader(ImDrawList* dl, ImVec2 p, float w, float h, const char* title, float titleSize, float padL) {
    drawText(dl, fontSemibold, titleSize, p.x + S(padL), p.y + (h - line()) * 0.5f, T().tx, title);
    dl->AddRectFilled(ImVec2(p.x, p.y + h - line()), ImVec2(p.x + w, p.y + h), T().bd);
}

namespace {




// Splits text into lines no wider than `width`, breaking at spaces (or at `separator` when given).
std::vector<std::string> wrapLines(ImFont* f, float size, const std::string& text, float width,
                                   const std::string& separator = " ") {
    std::vector<std::string> lines;
    std::string current;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(separator, start);
        const std::string word = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const std::string candidate = current.empty() ? word : current + separator + word;
        if (!current.empty() && textSize(f, size, candidate.c_str()).x > width) {
            lines.push_back(current);
            current = word;
        } else {
            current = candidate;
        }
        if (end == std::string::npos) break;
        start = end + separator.size();
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}

// A chip in the style of .chip: panel background, 1 px border, radius 4.
void chipFrame(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    const Theme& t = T();
    dl->AddRectFilled(a, b, t.pnl, S(4));
    dl->AddRect(a, b, t.bd2, S(4), 0, line());
}


// Clickable areas over the viewport go into a small window of their own, so the rest of the image
// keeps receiving the mouse.
void beginArea(const char* id, ImVec2 a, ImVec2 b) {
    ImGui::SetNextWindowPos(a);
    ImGui::SetNextWindowSize(ImVec2(b.x - a.x, b.y - a.y));
    ImGui::Begin(id, nullptr, kBarFlags);
}

std::string relativeTime(int64_t when) {
    const int64_t now = int64_t(std::time(nullptr)), ago = now - when;
    char buf[32];
    if (ago < 60) return "Just now";
    if (ago < 3600) {
        std::snprintf(buf, sizeof buf, "%d min ago", int(ago / 60));
        return buf;
    }
    const time_t tw = time_t(when), tn = time_t(now);
    std::tm w = *std::localtime(&tw);
    const std::tm n = *std::localtime(&tn);
    const int days = (n.tm_year - w.tm_year) * 366 + n.tm_yday - w.tm_yday;
    if (days == 0) {
        std::snprintf(buf, sizeof buf, "%d h ago", int(ago / 3600));
        return buf;
    }
    if (days == 1) return "Yesterday";
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (w.tm_year == n.tm_year) std::snprintf(buf, sizeof buf, "%s %d", months[w.tm_mon], w.tm_mday);
    else std::snprintf(buf, sizeof buf, "%s %d, %d", months[w.tm_mon], w.tm_mday, 1900 + w.tm_year);
    return buf;
}

// ---- empty window ----

void emptyCard() {
    const Theme& t = T();
    const float W = S(480);
    const std::string formats =
        "EXR \xC2\xB7 HDR \xC2\xB7 DDS \xC2\xB7 KTX \xC2\xB7 KTX2 \xC2\xB7 TGA \xC2\xB7 TIFF \xC2\xB7 PSD \xC2\xB7 PNG "
        "\xC2\xB7 JPG \xC2\xB7 BMP \xC2\xB7 GIF \xC2\xB7 WebP \xC2\xB7 AVIF \xC2\xB7 JXL \xC2\xB7 QOI \xC2\xB7 HEIC";
    const std::vector<std::string> formatLines = wrapLines(fontMono, 11, formats, W, " \xC2\xB7 ");
    const float formatsH = S(17) * float(formatLines.size());

    // As many recent files as fit.
    const float fixedH = S(200) + S(28) + formatsH;
    int rows = std::min<int>(int(app.recent.size()), 6);
    while (rows > 0 && fixedH + S(28) + S(20) + rows * S(40) > app.vpH - S(32)) --rows;
    const float totalH = fixedH + (rows ? S(28) + S(20) + rows * S(40) : 0.0f);
    const float x = std::round(app.vpX + (app.vpW - W) * 0.5f);
    float y = std::round(app.vpY + std::max(S(16), (app.vpH - totalH) * 0.5f));

    beginArea("##empty", ImVec2(x, y), ImVec2(x + W, y + totalH));
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Drop area; a click opens the file dialog.
    const ImVec2 a(x, y), b(x + W, y + S(200));
    ImGui::SetCursorScreenPos(a);
    if (ImGui::InvisibleButton("drop", ImVec2(W, S(200)))) showOpenDialog();
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) dl->AddRectFilled(a, b, fade(t.ctlh, 0.35f), S(4));
    dashedBox(dl, a, b, hovered ? t.tx3 : t.bd2);
    const float contentH = S(32) + S(14) + S(18) + S(14) + S(16);
    float cy = a.y + std::round((S(200) - contentH) * 0.5f);
    drawIcon(dl, Icon::Image, ImVec2(std::round(x + (W - S(32)) * 0.5f), cy), S(32), t.tx3, 1.2f);
    cy += S(32) + S(14) + S(9);
    {
        const char* text = "Drop an image here or press";
        const float tw = textSize(fontRegular, 14, text).x, kw = kbdChip(dl, 0, 0, "Ctrl+O", false);
        const float lx = std::round(x + (W - tw - S(5) - kw) * 0.5f);
        drawText(dl, fontRegular, 14, lx, cy, t.tx, text);
        kbdChip(dl, lx + tw + S(5), cy, "Ctrl+O");
    }
    cy += S(9) + S(14) + S(8);
    {
        const char* text = "Paste from clipboard with";
        const float tw = textSize(fontRegular, 12, text).x, kw = kbdChip(dl, 0, 0, "Ctrl+V", false);
        const float lx = std::round(x + (W - tw - S(5) - kw) * 0.5f);
        drawText(dl, fontRegular, 12, lx, cy, t.tx3, text);
        kbdChip(dl, lx + tw + S(5), cy, "Ctrl+V");
    }
    y = b.y + S(28);

    for (const std::string& l : formatLines) {
        const float lw = textSize(fontMono, 11, l.c_str()).x;
        drawText(dl, fontMono, 11, std::round(x + (W - lw) * 0.5f), y + S(17) * 0.5f, t.tx3, l.c_str());
        y += S(17);
    }

    if (rows) {
        y += S(28);
        drawText(dl, fontSemibold, 11, x + S(8), y + S(7), t.tx3, "Recent");
        const char* keys = "Ctrl+1 \xE2\x80\xA6 9";
        drawText(dl, fontRegular, 11, x + W - S(8) - textSize(fontRegular, 11, keys).x, y + S(7), t.tx3, keys);
        y += S(20);
        for (int i = 0; i < rows; ++i) {
            const RecentFile& rf = app.recent[size_t(i)];
            const ImVec2 ra(x, y), rb(x + W, y + S(40));
            ImGui::SetCursorScreenPos(ra);
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("recent", ImVec2(W, S(40)))) openRecent(i);
            const bool rowHovered = ImGui::IsItemHovered();
            ImGui::PopID();
            const std::string full = utf8(rf.path.wstring());
            requestTip(ImRect(ra, rb), full.c_str(), i < 9 ? (std::string("Ctrl+") + char('1' + i)).c_str() : nullptr);
            if (rowHovered) dl->AddRectFilled(ra, rb, t.ctlh, S(3));
            const float rcy = y + S(20);
            const ImVec2 ta(x + S(8), std::round(rcy - S(14))), tb(ta.x + S(28), ta.y + S(28));
            if (ID3D11ShaderResourceView* thumb = recentThumbnail(i))
                dl->AddImage(ImTextureRef(ImTextureID(reinterpret_cast<intptr_t>(thumb))), ta, tb);
            else
                dl->AddRectFilled(ta, tb, t.ctl);
            dl->AddRect(ta, tb, t.bd2, 0, 0, line());
            const std::string when = relativeTime(rf.openedAt);
            const float whenW = textSize(fontRegular, 11, when.c_str()).x;
            const float textX = tb.x + S(10), textW = x + W - S(8) - whenW - S(10) - textX;
            drawText(dl, fontRegular, 13, textX, rcy - S(8), t.tx,
                     ellipsize(fontRegular, 13, utf8(rf.path.filename().wstring()), textW).c_str());
            drawText(dl, fontMono, 11, textX, rcy + S(8), t.tx3,
                     ellipsize(fontMono, 11, utf8(rf.path.parent_path().wstring()), textW).c_str());
            drawText(dl, fontRegular, 11, x + W - S(8) - whenW, rcy, t.tx3, when.c_str());
            y += S(40);
        }
    }
    ImGui::End();
}

// ---- read error ----

void errorCard() {
    const Theme& t = T();
    const float W = S(460), padX = S(18), padY = S(16);
    const float colX = padX + S(20) + S(14), colW = W - colX - padX;
    const std::string title = ellipsize(fontSemibold, 14, "Can\xE2\x80\x99t read " + fileName(), colW);
    std::string message = app.error;
    if (!app.path.empty() && !app.fromClipboard) message += " It will reload automatically when the file changes.";
    const std::vector<std::string> lines = wrapLines(fontRegular, 12, message, colW);
    std::string meta = app.errorFormat;
    if (app.loadingSize) meta += (meta.empty() ? "" : " \xC2\xB7 ") + formatBytes(app.loadingSize);
    const float h = padY + S(18) + S(8) + S(18) * float(lines.size()) + S(8) + S(14) + S(8) + S(4) + S(24) + padY;
    const float x = std::round(app.vpX + (app.vpW - W) * 0.5f), y = std::round(app.vpY + (app.vpH - h) * 0.5f);

    beginArea("##error", ImVec2(x, y), ImVec2(x + W, y + h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    chipFrame(dl, ImVec2(x, y), ImVec2(x + W, y + h));
    drawIcon(dl, Icon::Alert, ImVec2(x + padX, y + padY + S(1)), S(20), t.err);
    float cy = y + padY + S(9);
    drawText(dl, fontSemibold, 14, x + colX, cy, t.tx, title.c_str());
    cy += S(9) + S(8);
    for (const std::string& l : lines) {
        drawText(dl, fontRegular, 12, x + colX, cy + S(9), t.tx2, l.c_str());
        cy += S(18);
    }
    cy += S(8);
    drawText(dl, fontMono, 11, x + colX, cy + S(7), t.tx3, meta.c_str());
    cy += S(14) + S(8) + S(4) + S(12);

    Row r;
    r.dl = dl;
    r.cy = cy;
    r.gap = S(6);
    r.x = x + colX;
    // Retry with its key chip inside the button.
    {
        const float kw = kbdChip(dl, 0, 0, "F5", false);
        const float w = 2 * line() + S(10) + textSize(fontRegular, 12, "Retry").x + S(4) + kw + S(10);
        const ImVec2 a(r.x, std::round(cy - S(12))), b(a.x + w, a.y + S(24));
        ImGui::SetCursorScreenPos(a);
        if (ImGui::InvisibleButton("retry", ImVec2(w, S(24)))) reloadFile();
        if (ImGui::IsItemHovered()) dl->AddRectFilled(a, b, t.ctlh, S(3));
        dl->AddRect(a, b, t.bd2, S(3), 0, line());
        drawText(dl, fontRegular, 12, a.x + line() + S(10), cy, t.tx, "Retry");
        kbdChip(dl, a.x + line() + S(10) + textSize(fontRegular, 12, "Retry").x + S(4), cy, "F5");
        r.x = b.x + r.gap;
    }
    const bool hasFile = !app.path.empty() && !app.fromClipboard;
    if (r.button("explorer", Icon::Count, "Show in Explorer", kBordered | (hasFile ? 0 : kDisabled), nullptr, nullptr, 16, 24, 10))
        showInExplorer();
    if (r.button("copyerror", Icon::Copy, nullptr, kBordered, "Copy error text", nullptr, 14, 24)) {
        const std::string text = "Can't read " + utf8(app.path.wstring()) + ": " + app.error;
        ImGui::SetClipboardText(text.c_str());
    }
    if (app.folder.size() > 1) {
        const char* hint = "\xE2\x86\x90 \xE2\x86\x92 keep browsing";
        drawText(dl, fontRegular, 11, x + W - padX - textSize(fontRegular, 11, hint).x, cy, t.tx3, hint);
    }
    ImGui::End();
}

// ---- loading ----

void loadingProgress() {
    const Theme& t = T();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float p = app.loadingProgress < 0 ? 1.0f : std::clamp(app.loadingProgress, 0.0f, 1.0f);
    if (app.showBars) dl->AddRectFilled(ImVec2(app.vpX, app.vpY), ImVec2(app.vpX + std::round(app.vpW * p), app.vpY + S(2)), t.ac);

    const std::string name = utf8(app.path.filename().wstring());
    char info[96];
    const double mb = double(app.loadingSize) / (1024.0 * 1024.0);
    if (app.loadingProgress < 0) std::snprintf(info, sizeof info, "decoding \xC2\xB7 %s", formatBytes(app.loadingSize).c_str());
    else
        std::snprintf(info, sizeof info, "%d%% \xC2\xB7 %.0f / %.0f MB", int(p * 100), mb * p, mb);
    const float nameW = textSize(fontMedium, 12, name.c_str()).x, infoW = textSize(fontMono, 12, info).x;
    const float loadingW = textSize(fontRegular, 12, "Loading ").x;
    const float w = line() + S(10) + S(14) + S(8) + loadingW + nameW + S(8) + infoW + S(8) + S(20) + S(6) + line();
    const float h = S(28);
    const ImVec2 a(std::round(app.vpX + app.vpW - S(16) - w), std::round(app.vpY + app.vpH - S(16) - h)), b(a.x + w, a.y + h);
    beginArea("##loading", a, b);
    ImDrawList* wl = ImGui::GetWindowDrawList();
    chipFrame(wl, a, b);
    const float cy = a.y + h * 0.5f;
    float x = a.x + line() + S(10);
    spinner(wl, ImVec2(x + S(7), cy), S(7) * 9.0f / 12.0f, t.ac);
    x += S(14) + S(8);
    drawText(wl, fontRegular, 12, x, cy, t.tx2, "Loading ");
    x += loadingW;
    drawText(wl, fontMedium, 12, x, cy, t.tx, name.c_str());
    x += nameW + S(8);
    drawText(wl, fontMono, 12, x, cy, t.tx3, info);
    x += infoW + S(8);
    if (smallButton(wl, "cancel", Icon::Dismiss, ImVec2(x, std::round(cy - S(10))), 20, 12, "Cancel loading", "Esc"))
        cancelLoading();
    ImGui::End();
}

}  // namespace

namespace {

const char* kRulerUnits[3] = {"px", "cm", "in"};

// The resolution the ruler converts with: the file's own, or the one set by hand.
float rulerDpi() { return app.image.dpi > 0 ? app.image.dpi : app.rulerDpi; }

// "12", "2.5", "0.25": as few decimals as the step needs.
std::string rulerLabel(double v, double step) {
    char buf[32];
    int decimals = 0;
    for (double s = step; decimals < 4 && std::fabs(s - std::round(s)) > 1e-6; s *= 10) ++decimals;
    std::snprintf(buf, sizeof buf, "%.*f", decimals, std::fabs(v) < step * 1e-6 ? 0.0 : v);
    return buf;
}

// One ruler. `along` is the screen coordinate where the image's shown edge starts, `from`..`to` the strip's
// extent on screen, `cross` the strip's other coordinate; ticks grow from the edge next to the image.
void rulerAxis(ImDrawList* dl, bool horizontal, float origin, float from, float to, float a, float b, float mouse) {
    const Theme& t = T();
    const double unitPx = app.rulerUnit == 0 ? 1.0 : rulerDpi() / (app.rulerUnit == 1 ? 2.54 : 1.0);
    const double ppu = double(app.zoom) * unitPx;  // screen pixels per unit
    // Major ticks at least 64 px apart, on 1, 2 or 5 times a power of ten (whole pixels at least).
    double step = 1;
    for (double p = std::pow(10.0, std::floor(std::log10(S(64) / ppu))); ; p *= 10) {
        if (p * ppu >= S(64)) { step = p; break; }
        if (2 * p * ppu >= S(64)) { step = 2 * p; break; }
        if (5 * p * ppu >= S(64)) { step = 5 * p; break; }
    }
    if (app.rulerUnit == 0) step = std::max(step, 1.0);
    int sub = 1;
    for (int n : {10, 5, 4, 2}) {
        const double minor = step / n;
        if (minor * ppu >= S(5) && (app.rulerUnit != 0 || std::fabs(minor - std::round(minor)) < 1e-9)) {
            sub = n;
            break;
        }
    }
    const double first = std::floor((from - origin) / ppu / step) * step;
    const double minorStep = step / sub;
    for (int i = 0;; ++i) {
        const double v = first + i * minorStep;
        const float p = std::round(float(origin + v * ppu));
        if (p > to) break;
        if (p < from) continue;
        const bool major = i % sub == 0;
        const float len = major ? (b - a) : (sub % 2 == 0 && i % (sub / 2) == 0 ? S(7) : S(4));
        if (horizontal) dl->AddRectFilled(ImVec2(p, b - len), ImVec2(p + line(), b), major ? t.tx3 : t.bd2);
        else dl->AddRectFilled(ImVec2(b - len, p), ImVec2(b, p + line()), major ? t.tx3 : t.bd2);
        if (!major) continue;
        const std::string label = rulerLabel(v, step);
        if (horizontal) {
            drawText(dl, fontMono, 10, p + S(3), a + S(6), t.tx3, label.c_str());
        } else {
            // Stacked characters read top to bottom in the narrow strip.
            float y = p + S(3);
            for (char c : label) {
                const char s[2] = {c, 0};
                drawText(dl, fontMono, 10, a + std::round((b - a - textSize(fontMono, 10, s).x) * 0.5f), y + S(5), t.tx3, s);
                y += S(9);
            }
        }
    }
    // Where the cursor is.
    if (mouse >= from && mouse <= to) {
        const float m = std::round(mouse);
        if (horizontal) dl->AddRectFilled(ImVec2(m, a), ImVec2(m + line(), b), t.ac);
        else dl->AddRectFilled(ImVec2(a, m), ImVec2(b, m + line()), t.ac);
    }
}

void rulerMenuItems() {
    for (int u = 0; u < 3; ++u) {
        static const char* names[3] = {"Pixels", "Centimetres", "Inches"};
        if (menuItem({names[u], kRulerUnits[u], app.rulerUnit == u, true, true}) && app.rulerUnit != u) {
            app.rulerUnit = u;
            saveSettings();
        }
    }
    menuSeparator();
    if (app.image.dpi > 0) {
        char buf[48];
        std::snprintf(buf, sizeof buf, "%g dpi", double(app.image.dpi));
        MenuItem m{"Resolution from the file", nullptr, false, false, true};
        m.value = buf;
        menuItem(m);
        return;
    }
    menuHeading("Resolution (the file states none)");
    for (float dpi : {72.0f, 96.0f, 150.0f, 300.0f, 600.0f}) {
        char label[32];
        std::snprintf(label, sizeof label, "%g dpi", double(dpi));
        if (menuItem({label, nullptr, app.rulerDpi == dpi, true, true}) && app.rulerDpi != dpi) {
            app.rulerDpi = dpi;
            saveSettings();
        }
    }
    // Any other value, typed.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = S(28);
    ImGui::Dummy(ImVec2(S(180), h));
    const float cy = p.y + h * 0.5f;
    drawText(dl, fontRegular, 12, p.x + S(8) + S(14) + S(8), cy, T().tx2, "Other");
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", double(app.rulerDpi));
    float v = app.rulerDpi;
    if (numberField(dl, "ruler dpi", p.x + S(90), cy, 70, buf, v, "Click to type a resolution in dpi") && v >= 1.0f &&
        v <= 100000.0f) {
        app.rulerDpi = v;
        saveSettings();
    }
}

}  // namespace

void rulers() {
    if (!app.ruler) return;
    const Theme& t = T();
    const float r = app.vpX;  // the strip width layout() set aside
    if (r <= 0) return;
    const float top = app.vpY - r, right = app.vpX + app.vpW, bottom = app.vpY + app.vpH;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilled(ImVec2(0, top), ImVec2(right, app.vpY), t.bar);
    dl->AddRectFilled(ImVec2(0, app.vpY), ImVec2(r, bottom), t.bar);
    dl->AddRectFilled(ImVec2(r, app.vpY - line()), ImVec2(right, app.vpY), t.bd);
    dl->AddRectFilled(ImVec2(r - line(), top), ImVec2(r, bottom), t.bd);
    const bool inside = app.mouseX >= 0 && app.mouseY >= top && app.mouseX < right && app.mouseY < bottom;
    if (app.hasImage) {
        dl->PushClipRect(ImVec2(r, top), ImVec2(right, app.vpY));
        rulerAxis(dl, true, app.originX, r, right, top, app.vpY - line(), inside ? float(app.mouseX) : -1e9f);
        dl->PopClipRect();
        dl->PushClipRect(ImVec2(0, app.vpY), ImVec2(r, bottom));
        rulerAxis(dl, false, app.originY, app.vpY, bottom, 0, r - line(), inside ? float(app.mouseY) : -1e9f);
        dl->PopClipRect();
    }
    // The corner names the unit and opens the unit and resolution menu.
    const ImVec2 a(0, top), b(r - line(), app.vpY - line());
    beginArea("##rulercorner", a, b);
    ImGui::SetCursorScreenPos(a);
    const bool open = ImGui::IsPopupOpen("ruler menu");
    toggleMenuOnPress(ImGui::InvisibleButton("unit", ImVec2(b.x - a.x, b.y - a.y)), "ruler menu");
    ImDrawList* cl = ImGui::GetWindowDrawList();
    if (ImGui::IsItemHovered() || open) cl->AddRectFilled(a, b, t.ctlh);
    const char* unit = kRulerUnits[app.rulerUnit];
    drawText(cl, fontMono, 10, std::round((a.x + b.x - textSize(fontMono, 10, unit).x) * 0.5f), (a.y + b.y) * 0.5f,
             open ? t.tx : t.tx2, unit);
    char tip[96];
    std::snprintf(tip, sizeof tip, "Ruler in %s \xC2\xB7 %g dpi%s", app.rulerUnit == 0 ? "pixels" : app.rulerUnit == 1 ? "centimetres" : "inches",
                  double(rulerDpi()), app.image.dpi > 0 ? " from the file" : "");
    requestTip(ImRect(a, b), tip, nullptr);
    if (beginMenu("ruler menu", ImVec2(a.x, b.y + S(2)), ImVec2(0, 0), S(220))) {
        rulerMenuItems();
        endMenu();
    }
    ImGui::End();
}

bool isLoadingVisible() { return app.loading && nowMs() - app.loadingSinceMs > 150.0; }

void viewportOverlays() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 va(app.vpX, app.vpY), vb(app.vpX + app.vpW, app.vpY + app.vpH);
    dl->PushClipRect(va, vb);
    if (app.hasImage) {
        float x0, y0, x1, y1;
        shownArea(x0, y0, x1, y1);
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
            for (int i = 1; i < 3; ++i) {
                dashedLine(dl, ImVec2(std::round(x0 + i * sw), y0), ImVec2(std::round(x0 + i * sw), y1));
                dashedLine(dl, ImVec2(x0, std::round(y0 + i * sh)), ImVec2(x1, std::round(y0 + i * sh)));
            }
        }
        // Outline around the pixels the readout reads: the one under the cursor or the locked one, with its
        // neighbours when Avg is 3x3 or 5x5.
        int px = 0, py = 0;
        bool show = app.pixelLocked;
        float shiftX = 0, shiftY = 0;  // the copy under the cursor when tiled
        if (app.pixelLocked) {
            px = app.lockedX;
            py = app.lockedY;
        } else if (!ImGui::GetIO().WantCaptureMouse && app.mouseX >= app.vpX && app.mouseY >= app.vpY &&
                   app.mouseX < app.vpX + app.vpW && app.mouseY < app.vpY + app.vpH &&
                   windowToPixel(float(app.mouseX), float(app.mouseY), px, py)) {
            show = true;
            if (app.tiling != Tiling::Off) {
                const float w = float(app.rotation & 1 ? app.image.height : app.image.width) * app.zoom;
                const float h = float(app.rotation & 1 ? app.image.width : app.image.height) * app.zoom;
                shiftX = float(std::clamp(int(std::floor((app.mouseX - app.originX) / w)), -1, 1)) * w;
                shiftY = float(std::clamp(int(std::floor((app.mouseY - app.originY) / h)), -1, 1)) * h;
            }
        }
        if (show) {
            const int half = app.averageSize / 2;
            const int bx0 = std::max(0, px - half), by0 = std::max(0, py - half);
            const int bx1 = std::min(int(app.image.width), px + half + 1), by1 = std::min(int(app.image.height), py + half + 1);
            float wx0, wy0, wx1, wy1;
            imageRectToWindow(float(bx0), float(by0), float(bx1), float(by1), wx0, wy0, wx1, wy1);
            wx0 = std::round(wx0 + shiftX);
            wy0 = std::round(wy0 + shiftY);
            wx1 = std::max(std::round(wx1 + shiftX), wx0 + 1);
            wy1 = std::max(std::round(wy1 + shiftY), wy0 + 1);
            dashedRect(dl, ImVec2(wx0 - line(), wy0 - line()), ImVec2(wx1 + line(), wy1 + line()));
        }
    }
    dl->PopClipRect();
    compareOverlay();

    // The image area's own states. While a file loads, the previous image (or nothing) stays.
    if (!app.hasImage && !app.loading) {
        if (!app.error.empty()) errorCard();
        else emptyCard();
    }
    if (isLoadingVisible()) loadingProgress();
}

void notices() {
    const Theme& t = T();
    // "Reloaded" after the file changed on disk.
    if (app.toastUntilMs > nowMs() && app.hasImage) {
        const char* title = "Reloaded";
        const std::string detail = app.toastName + " changed on disk \xC2\xB7 zoom, position and exposure kept";
        const float w = line() + S(10) + S(14) + S(8) + textSize(fontRegular, 12, title).x + S(8) +
                        textSize(fontRegular, 12, detail.c_str()).x + S(8) + S(20) + S(6) + line();
        const float h = S(28);
        const ImVec2 a(std::round(app.vpX + (app.vpW - w) * 0.5f), app.vpY + S(14)), b(a.x + w, a.y + h);
        beginArea("##toast", a, b);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        chipFrame(dl, a, b);
        const float cy = a.y + h * 0.5f;
        float x = a.x + line() + S(10);
        drawIcon(dl, Icon::RotateRight, ImVec2(x, std::round(cy - S(7))), S(14), t.ok);
        x += S(14) + S(8);
        drawText(dl, fontRegular, 12, x, cy, t.tx, title);
        x += textSize(fontRegular, 12, title).x + S(8);
        drawText(dl, fontRegular, 12, x, cy, t.tx3, detail.c_str());
        x += textSize(fontRegular, 12, detail.c_str()).x + S(8);
        if (smallButton(dl, "dismiss", Icon::Dismiss, ImVec2(x, std::round(cy - S(10))), 20, 12, "Dismiss", nullptr))
            app.toastUntilMs = 0;
        ImGui::End();
    }
    // A normal map that left Z out: blue has to be rebuilt where it is used, so the colours here are not
    // what the engine shades with.
    if (!app.normalHint.empty() && !app.normalHintDismissed && app.hasImage && !isLoadingVisible()) {
        const char* title = "Normal map without Z?";
        const std::string& detail = app.normalHint;
        const float w = line() + S(10) + S(14) + S(8) + textSize(fontRegular, 12, title).x + S(8) +
                        textSize(fontRegular, 12, detail.c_str()).x + S(8) + S(20) + S(6) + line();
        const float h = S(28);
        if (w < app.vpW - S(28)) {
            const ImVec2 a(std::round(app.vpX + (app.vpW - w) * 0.5f), app.vpY + app.vpH - S(14) - h), b(a.x + w, a.y + h);
            beginArea("##normalhint", a, b);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            chipFrame(dl, a, b);
            const float cy = a.y + h * 0.5f;
            float x = a.x + line() + S(10);
            drawIcon(dl, Icon::Warning, ImVec2(x, std::round(cy - S(7))), S(14), t.warn);
            x += S(14) + S(8);
            drawText(dl, fontRegular, 12, x, cy, t.tx, title);
            x += textSize(fontRegular, 12, title).x + S(8);
            drawText(dl, fontRegular, 12, x, cy, t.tx3, detail.c_str());
            requestTip(ImRect(a, ImVec2(x + textSize(fontRegular, 12, detail.c_str()).x, b.y)),
                       "Only X and Y seem to be stored. Z is rebuilt where the map is used, as sqrt(1 - x\xC2\xB2 - y\xC2\xB2), "
                       "so the blue here is not what the engine shades with. A guess from the format, name and colours.",
                       nullptr, true);
            x += textSize(fontRegular, 12, detail.c_str()).x + S(8);
            if (smallButton(dl, "dismiss", Icon::Dismiss, ImVec2(x, std::round(cy - S(10))), 20, 12, "Dismiss", nullptr))
                app.normalHintDismissed = true;
            ImGui::End();
        }
    }
    // Clean mode: a short hint how to bring the panels back.
    if (!app.showBars && ImGui::GetTime() < cleanHintUntil) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const char* text = "Show panels";
        const float kw = textSize(fontMono, 11, "Tab").x + 2 * S(4) + 2 * line();
        const float w = line() + S(10) + kw + S(8) + textSize(fontRegular, 12, text).x + S(10) + line();
        const float h = S(26);
        const ImVec2 a(std::round(app.vpX + (app.vpW - w) * 0.5f), app.vpY + S(14)), b(a.x + w, a.y + h);
        dl->AddRectFilled(a, b, IM_COL32(30, 30, 30, 199), S(4));
        dl->AddRect(a, b, IM_COL32(255, 255, 255, 31), S(4), 0, line());
        const float cy = a.y + h * 0.5f;
        const ImVec2 ka(a.x + line() + S(10), std::round(cy - S(9))), kb(ka.x + kw, ka.y + S(18));
        dl->AddRect(ka, kb, IM_COL32(255, 255, 255, 64), S(3), 0, line());
        drawText(dl, fontMono, 11, ka.x + line() + S(4), cy, hex(0xe8e8e8), "Tab");
        drawText(dl, fontRegular, 12, kb.x + S(8), cy, hex(0xe8e8e8), text);
    }
}

// ---- floating panels ----

namespace {


// ---- Info ----

struct InfoRow {
    std::string key, value;
};
struct InfoSection {
    const char* title;
    std::vector<InfoRow> rows;
    const char* note = nullptr;  // shown on the right of a section without rows
};

std::string fileTime(const fs::path& path, bool created) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    SYSTEMTIME st;
    FILETIME local;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ||
        !FileTimeToLocalFileTime(created ? &data.ftCreationTime : &data.ftLastWriteTime, &local) ||
        !FileTimeToSystemTime(&local, &st))
        return "-";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                  st.wSecond);
    return buf;
}

std::string channelText() {
    using G = htv::GpuFormat;
    const htv::FormatInfo& info = htv::formatInfo(app.image.format);
    if (app.image.format == G::BGRX8 || app.image.format == G::BC1 || app.image.format == G::BC1Srgb ||
        app.image.format == G::BC6HUf16 || app.image.format == G::BC6HSf16 || app.image.format == G::RG11B10F)
        return "RGB";
    if (info.channels == 1) return "Grey (one channel)";
    if (info.channels == 2) return "RG";
    return app.imageAlpha ? "RGBA" : "RGBA (alpha unused)";
}

std::string bitsText() {
    using G = htv::GpuFormat;
    switch (app.image.format) {
        case G::RGBA32F: case G::RG32F: case G::R32F: return "32-bit float";
        case G::RGBA16F: case G::RG16F: case G::R16F: return "16-bit float (half)";
        case G::RGBA16Unorm: case G::RG16Unorm: case G::R16Unorm: return "16 per channel";
        case G::RGB10A2: return "10 per channel, 2 alpha";
        case G::RG11B10F: return "11/11/10-bit float";
        default: break;
    }
    return htv::formatInfo(app.image.format).compressed ? "block-compressed" : "8 per channel";
}

std::vector<InfoSection> infoSections() {
    std::vector<InfoSection> out;
    const fs::path& path = app.hasImage ? app.shownPath : app.path;
    InfoSection file{"File", {}, nullptr};
    if (app.hasImage && app.fromClipboard) {
        file.rows.push_back({"Source", "Clipboard"});
    } else if (!path.empty()) {
        const uint64_t size = app.hasImage ? app.fileSize : app.loadingSize;
        file.rows.push_back({"Path", utf8(path.wstring())});
        file.rows.push_back({"Size", formatBytes(size) + " \xC2\xB7 " + formatBytesExact(size)});
        file.rows.push_back({"Modified", fileTime(path, false)});
        file.rows.push_back({"Created", fileTime(path, true)});
    }
    out.push_back(file);
    if (!app.hasImage) {
        out.push_back({"Image", {}, "n/a"});
        return out;
    }
    const htv::FormatInfo& info = htv::formatInfo(app.image.format);
    out.push_back({"Image",
                   {{"Dimensions", std::to_string(app.image.width) + " \xC3\x97 " + std::to_string(app.image.height)},
                    {"Channels", channelText()},
                    {"Bits", bitsText()},
                    {"Color space", app.dataIsLinear ? "Linear" : "sRGB"}}});
    InfoSection format{"Compression / Format", {}, nullptr};
    format.rows.push_back({"Format", info.name});
    format.rows.push_back({"Header", rgbaOrder(app.image.description)});
    char mips[64];
    if (app.image.mips.size() > 1)
        std::snprintf(mips, sizeof mips, "%d \xC2\xB7 %u \xE2\x86\x92 %u", int(app.image.mips.size()),
                      std::max(app.image.width, app.image.height),
                      std::max(1u, std::max(app.image.width, app.image.height) >> (app.image.mips.size() - 1)));
    else std::snprintf(mips, sizeof mips, "1 (%d made for display)", app.textureMips);
    format.rows.push_back({"Mip levels", mips});
    const htv::Image& img = app.image;
    if (hasLayers()) {
        std::string layers = std::string(htv::layerKindName(img.layerKind)) + " \xC2\xB7 " + std::to_string(img.layers.size());
        if (img.layerKind != htv::LayerKind::PsdLayers) layers += " (showing " + img.layers[size_t(img.shownLayer)].name + ")";
        format.rows.push_back({"Layers", layers});
    } else {
        format.rows.push_back({"Layers", "no \xC2\xB7 1"});
    }
    if (!img.note.empty()) format.rows.push_back({"Note", img.note});
    out.push_back(format);
    if (!img.exr.empty()) {
        InfoSection exr{"EXR", {}, nullptr};
        for (const htv::Property& p : img.exr) exr.rows.push_back({p.key, p.value});
        out.push_back(exr);
    } else {
        out.push_back({"EXR", {}, "n/a"});
    }
    if (app.stats.valid) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.4g \xE2\x80\xA6 %.4g", double(app.stats.minValue), double(app.stats.maxValue));
        out.push_back({"Values", {{"Range", buf}, {"NaN / Inf", std::to_string(app.stats.nonFinite) + " pixels"}}});
    }
    InfoSection meta{"Metadata", {}, nullptr};
    if (img.dpi > 0) {
        char dpi[32];
        std::snprintf(dpi, sizeof dpi, "%g dpi", double(std::round(img.dpi * 100.0f) / 100.0f));
        meta.rows.push_back({"Resolution", dpi});
    }
    for (const htv::Property& p : img.metadata) meta.rows.push_back({p.key, p.value});
    if (meta.rows.empty()) meta.note = "none in the file";
    out.push_back(meta);
    return out;
}

std::string infoText(const std::vector<InfoSection>& sections) {
    std::string text;
    for (const InfoSection& sec : sections) {
        if (sec.rows.empty()) continue;
        text += std::string(sec.title) + "\r\n";
        for (const InfoRow& r : sec.rows) text += "  " + r.key + ": " + r.value + "\r\n";
    }
    return text;
}


}  // namespace

void infoPanel() {
    if (!app.showInfo || (!app.hasImage && app.path.empty())) return;
    const Theme& t = T();
    const float width = S(328);
    if (!beginPanel("##info", ImVec2(app.vpX + app.vpW - width - S(16), app.vpY + S(16)), width)) {
        ImGui::End();
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetWindowPos();
    const std::vector<InfoSection> sections = infoSections();
    const float hh = S(26);
    panelHeader(dl, p, width, hh, "Info", 12, 9);
    const float hcy = std::round(p.y + (hh - line() - S(20)) * 0.5f);
    if (smallButton(dl, "copyall", Icon::Copy, ImVec2(p.x + width - S(3) - S(20) - S(6) - S(20), hcy), 20, 14,
                    "Copy all as text", nullptr))
        ImGui::SetClipboardText(infoText(sections).c_str());
    if (smallButton(dl, "close", Icon::Dismiss, ImVec2(p.x + width - S(3) - S(20), hcy), 20, 14, "Close", keyText(Action::Info)))
        app.showInfo = false;

    static bool open[8] = {true, true, true, true, true, true, true, true};
    float y = p.y + hh;
    const ImU32 sectionBg = app.darkTheme ? IM_COL32(255, 255, 255, 9) : IM_COL32(0, 0, 0, 9);
    for (size_t i = 0; i < sections.size(); ++i) {
        const InfoSection& sec = sections[i];
        const bool empty = sec.rows.empty();
        ImGui::PushID(int(i));
        // Section header (.sh): 24 px, chevron, title; sections without data are muted.
        const ImVec2 a(p.x, y), b(p.x + width, y + S(24));
        ImGui::SetCursorScreenPos(a);
        if (ImGui::InvisibleButton("section", ImVec2(width, S(24))) && !empty && i < 8) open[i] = !open[i];
        dl->AddRectFilled(a, b, sectionBg);
        if (i) dl->AddRectFilled(a, ImVec2(b.x, a.y + line()), t.bd);
        const bool isOpen = !empty && i < 8 && open[i];
        const float cy = y + S(12);
        drawIcon(dl, isOpen ? Icon::Chevron : Icon::Next, ImVec2(a.x + S(8), std::round(cy - S(6))), S(12),
                 empty ? t.tx3 : t.tx2);
        drawText(dl, empty ? fontMedium : fontSemibold, 11, a.x + S(8) + S(12) + S(6), cy, empty ? t.tx3 : t.tx2, sec.title);
        if (sec.note) drawText(dl, fontRegular, 11, b.x - S(8) - textSize(fontRegular, 11, sec.note).x, cy, t.tx3, sec.note);
        y += S(24);
        if (isOpen) {
            for (size_t k = 0; k < sec.rows.size(); ++k) {
                const InfoRow& row = sec.rows[k];
                const ImVec2 ra(p.x, y), rb(p.x + width, y + S(22));
                ImGui::SetCursorScreenPos(ra);
                ImGui::PushID(int(k));
                const bool clicked = ImGui::InvisibleButton("row", ImVec2(width, S(22)));
                const bool hovered = ImGui::IsItemHovered();
                const ImGuiID id = ImGui::GetItemID();
                ImGui::PopID();
                if (clicked) {
                    ImGui::SetClipboardText(row.value.c_str());
                    flashCopied(id);
                }
                if (hovered) dl->AddRectFilled(ra, rb, t.ctlh);
                const float rcy = y + S(11);
                drawText(dl, fontRegular, 12, ra.x + S(24), rcy, t.tx3, ellipsize(fontRegular, 12, row.key, S(92)).c_str());
                const float vx = ra.x + S(24) + S(92) + S(8);
                const float vw = rb.x - S(8) - vx - (hovered ? S(13) + S(8) : 0.0f);
                if (copiedRecently(id)) drawText(dl, fontMono, 12, vx, rcy, t.ok, "Copied");
                else drawText(dl, fontMono, 12, vx, rcy, t.tx, ellipsize(fontMono, 12, row.value, vw).c_str());
                if (hovered) {
                    drawIcon(dl, Icon::Copy, ImVec2(rb.x - S(8) - S(13), std::round(rcy - S(6.5f))), S(13), t.tx2);
                    if (textSize(fontMono, 12, row.value.c_str()).x > vw)
                        requestTip(ImRect(ra, rb), row.value.c_str(), nullptr);
                }
                y += S(22);
            }
        }
        ImGui::PopID();
    }
    y += S(6);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y));
    ImGui::Dummy(ImVec2(width, y - p.y));
    ImGui::End();
}

// ---- Help (F1) ----

namespace {

// A row lists fixed keys (mouse input, Esc), or actions whose current keys it shows.
struct HelpRow {
    const char* label;
    std::vector<const char*> keys;
    std::vector<Action> actions;
};
struct HelpGroup {
    const char* title;
    std::vector<HelpRow> rows;
};

const std::vector<HelpGroup>& helpColumn(int c) {
    using A = Action;
    static const std::vector<HelpGroup> columns[3] = {
        {{"Files",
          {{"Open", {}, {A::Open}},
           {"Previous / next file", {}, {A::PrevFile, A::NextFile}},
           {"First / last in folder", {}, {A::FirstFile, A::LastFile}},
           {"Paste image from clipboard", {}, {A::Paste}},
           {"Copy file path", {}, {A::CopyPath}},
           {"Reload", {}, {A::Reload}},
           {"Recent files", {"Ctrl+1\xE2\x80\xA6" "9"}, {}}}},
         {"View",
          {{"Fit", {"Dbl-click"}, {A::Fit}},
           {"Actual size 100%", {}, {A::ActualSize}},
           {"Zoom to cursor", {"Wheel"}, {}},
           {"Pan", {"Space+drag", "MMB"}, {}},
           {"Rotate left / right", {}, {A::RotateLeft, A::RotateRight}},
           {"Flip horizontal / vertical", {}, {A::FlipH, A::FlipV}},
           {"Hide panels", {}, {A::HidePanels}}}}},
        {{"Channels and modes",
          {{"Red / green / blue / alpha", {}, {A::Red, A::Green, A::Blue, A::Alpha}},
           {"All channels", {}, {A::AllChannels}},
           {"Luminance", {}, {A::Luminance}},
           {"sRGB / Linear", {}, {A::SrgbLinear}},
           {"Mip level \xE2\x88\x92 / +", {}, {A::MipDown, A::MipUp}},
           {"Alpha background", {}, {A::AlphaBackground}},
           {"Tiling", {}, {A::Tiling}}}},
         {"HDR",
          {{"Exposure", {"Ctrl+Wheel"}, {}},
           {"Exposure \xE2\x88\x92 / + 0.5 EV", {}, {A::ExposureDown, A::ExposureUp}},
           {"Reset exposure", {}, {A::ResetExposure}},
           {"Auto range", {}, {A::AutoRange}},
           {"Show clipping", {}, {A::Clipping}}}}},
        {{"Analysis",
          {{"Pixel grid", {}, {A::Grid}},
           {"Ruler", {}, {A::Ruler}},
           {"Lock pixel", {"Ctrl+Click"}, {A::LockPixel}},
           {"Copy pixel value", {}, {A::CopyValue}},
           {"Color match", {}, {A::ColorMatch}},
           {"Previous / next match region", {}, {A::MatchPrevRegion, A::MatchNextRegion}},
           {"Histogram / Info / Layers", {}, {A::Histogram, A::Info, A::Layers}},
           {"Compare", {}, {A::Compare}}}},
         {"Window",
          {{"Always on top", {}, {A::AlwaysOnTop}},
           {"Opacity", {"Alt+Wheel"}, {}},
           {"Transparent to mouse", {}, {A::ClickThrough}},
           {"Move window", {"Right drag", "Alt+drag"}, {}},
           {"Settings", {}, {A::Settings}},
           {"Keyboard shortcuts", {}, {A::Help}},
           {"Close", {"Esc"}, {A::Quit}}}}},
    };
    return columns[c];
}

// The keys a row shows: each action's primary key, the secondary keys while there is room, then fixed keys.
std::vector<std::string> rowKeys(const HelpRow& r) {
    std::vector<std::string> out;
    for (int slot = 0; slot < 2; ++slot)
        for (Action a : r.actions) {
            const std::string k = keyName(binding(a, slot));
            if (!k.empty() && (slot == 0 || out.size() + r.keys.size() < 4)) out.push_back(k);
        }
    for (const char* k : r.keys) out.push_back(k);
    if (out.size() > 4) out.resize(4);
    return out;
}

bool matches(const HelpRow& r, const std::string& filter) {
    if (filter.empty()) return true;
    auto lower = [](std::string v) {
        for (char& ch : v) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        return v;
    };
    const std::string f = lower(filter);
    if (lower(r.label).find(f) != std::string::npos) return true;
    for (const std::string& k : rowKeys(r))
        if (lower(k).find(f) != std::string::npos) return true;
    return false;
}

}  // namespace

void helpPanel() {
    if (!app.showHelp) return;
    const Theme& t = T();
    static char filter[64] = "";
    const std::string f = filter;

    // Column heights decide the panel height; the width follows the design, within the image area.
    const float width = std::min(S(1080), app.vpW - S(32));
    float bodyH = 0;
    for (int c = 0; c < 3; ++c) {
        float h = 0;
        for (const HelpGroup& g : helpColumn(c)) {
            int n = 0;
            for (const HelpRow& r : g.rows) n += matches(r, f);
            if (n) h += (h > 0 ? S(18) : 0.0f) + S(11) + S(5) + line() + S(6) + n * S(24);
        }
        bodyH = std::max(bodyH, h);
    }
    const float headerH = S(30), footerH = S(36);
    const float height = std::min(headerH + S(18) + std::max(bodyH, S(120)) + S(18) + footerH, app.vpH - S(32));
    const ImVec2 def(std::round(app.vpX + (app.vpW - width) * 0.5f), std::round(app.vpY + (app.vpH - height) * 0.5f));
    if (!beginPanel("##help", def, width, height)) {
        ImGui::End();
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetWindowPos();
    panelHeader(dl, p, width, headerH, "Keyboard shortcuts", 13, 12);
    const float hcy = p.y + (headerH - line()) * 0.5f;

    // Filter field.
    const float fx = p.x + S(12) + textSize(fontSemibold, 13, "Keyboard shortcuts").x + S(8) + S(16);
    {
        const float fw = S(240), fh = S(24);
        const ImVec2 a(fx, std::round(hcy - fh * 0.5f));
        ImGui::SetCursorScreenPos(a);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(8) + S(14) + S(6), std::round((fh - S(12) * emRegular) * 0.5f)));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(3));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, line());
        ImGui::PushStyleColor(ImGuiCol_FrameBg, t.bar2);
        ImGui::PushStyleColor(ImGuiCol_Border, t.bd2);
        ImGui::PushStyleColor(ImGuiCol_Text, t.tx);
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, t.tx3);
        ImGui::PushFont(fontRegular, 12 * emRegular);
        ImGui::SetNextItemWidth(fw);
        ImGui::InputTextWithHint("##filter", "Filter shortcuts", filter, sizeof filter);
        ImGui::PopFont();
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(3);
        drawIcon(dl, Icon::Search, ImVec2(a.x + S(8), std::round(hcy - S(7))), S(14), t.tx3);
    }
    const char* hint = "Esc or F1 to close";
    drawText(dl, fontRegular, 11, p.x + width - S(4) - S(20) - S(8) - textSize(fontRegular, 11, hint).x, hcy, t.tx3, hint);
    if (smallButton(dl, "close", Icon::Dismiss, ImVec2(p.x + width - S(4) - S(20), std::round(hcy - S(10))), 20, 14, "Close",
                    keyText(Action::Help)))
        app.showHelp = false;

    // Three columns of groups.
    const float colGap = S(36), padX = S(22);
    const float colW = (width - 2 * padX - 2 * colGap) / 3;
    bool any = false;
    for (int c = 0; c < 3; ++c) {
        const float cx = p.x + padX + c * (colW + colGap);
        float y = p.y + headerH + S(18);
        bool first = true;
        for (const HelpGroup& g : helpColumn(c)) {
            int n = 0;
            for (const HelpRow& r : g.rows) n += matches(r, f);
            if (!n) continue;
            any = true;
            if (!first) y += S(18);
            first = false;
            drawText(dl, fontSemibold, 11, cx, y + S(7), t.tx3, g.title);
            y += S(11) + S(5);
            dl->AddRectFilled(ImVec2(cx, y), ImVec2(cx + colW, y + line()), t.bd);
            y += line() + S(6);
            for (const HelpRow& r : g.rows) {
                if (!matches(r, f)) continue;
                const float cy = y + S(12);
                float kx = cx + colW;
                const std::vector<std::string> keys = rowKeys(r);
                for (int k = int(keys.size()) - 1; k >= 0; --k) {
                    kx -= kbdChip(dl, 0, 0, keys[size_t(k)].c_str(), false, true);
                    kbdChip(dl, kx, cy, keys[size_t(k)].c_str(), true, true);
                    kx -= S(4);
                }
                drawText(dl, fontRegular, 12, cx, cy, t.tx2,
                         ellipsize(fontRegular, 12, r.label, kx - cx - S(8)).c_str());
                y += S(24);
            }
        }
    }
    if (!any) drawText(dl, fontRegular, 12, p.x + padX, p.y + headerH + S(30), t.tx3, "No shortcut matches the filter");

    // Footer.
    const float fy = p.y + height - footerH;
    dl->AddRectFilled(ImVec2(p.x, fy), ImVec2(p.x + width, fy + line()), t.bd);
    const float fcy = fy + footerH * 0.5f;
    drawText(dl, fontRegular, 12, p.x + padX, fcy, t.tx3, "All keys can be reassigned in Settings \xE2\x80\xBA Keys");
    Row r;
    r.dl = dl;
    r.cy = fcy;
    r.measure = true;
    r.button("keys", Icon::Count, "Open Keys settings", kBordered, nullptr, nullptr);
    const float bw = r.x;
    r.measure = false;
    r.x = p.x + width - padX - bw;
    if (r.button("keys", Icon::Count, "Open Keys settings", kBordered, "Reassign keys in Settings", keyText(Action::Settings))) {
        app.showHelp = false;
        app.showSettings = true;
    }
    ImGui::End();
}

}  // namespace htv::ui
