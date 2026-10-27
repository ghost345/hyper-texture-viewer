// Frames (L on an animation): step or play through the frames of a GIF, APNG, or animated WebP, AVIF or
// JPEG XL.
// Layers (L on a file with layers): cube faces, array and volume slices, TIFF pages and EXR layers, one at a
// time; or PSD layers, each with its own eye. Files with neither have no panel.

#include "ui.h"
#include "ui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace htv::ui {
namespace {

// A square icon button inside the panel; `glyph` draws into a 24-unit box at the given origin and size.
template <class F>
bool glyphButton(ImDrawList* dl, const char* id, ImVec2 a, float size, bool on, const char* tip, const char* key, F glyph) {
    const Theme& t = T();
    ImGui::SetCursorScreenPos(a);
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(S(size), S(size)));
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 b(a.x + S(size), a.y + S(size));
    if (on) dl->AddRectFilled(a, b, t.acs, S(3));
    else if (hovered) dl->AddRectFilled(a, b, t.ctlh, S(3));
    glyph(dl, a, S(size), on ? t.act : hovered ? t.tx : t.tx2);
    requestTip(ImRect(a, b), tip, key, true);
    return pressed;
}

void playGlyph(ImDrawList* dl, ImVec2 a, float s, ImU32 c) {
    const float cx = a.x + s * 0.42f, cy = a.y + s * 0.5f, r = s * 0.2f;
    dl->AddTriangleFilled(ImVec2(cx - r * 0.7f, cy - r), ImVec2(cx - r * 0.7f, cy + r), ImVec2(cx + r, cy), c);
}

void pauseGlyph(ImDrawList* dl, ImVec2 a, float s, ImU32 c) {
    const float cx = a.x + s * 0.5f, cy = a.y + s * 0.5f, w = s * 0.08f, h = s * 0.22f, g = s * 0.1f;
    dl->AddRectFilled(ImVec2(cx - g - w, cy - h), ImVec2(cx - g + w, cy + h), c, S(1));
    dl->AddRectFilled(ImVec2(cx + g - w, cy - h), ImVec2(cx + g + w, cy + h), c, S(1));
}

void framesBody(ImDrawList* dl, ImVec2 p, float width) {
    const Theme& t = T();
    const int n = int(app.image.frames.size());
    const float x0 = p.x + S(10), x1 = p.x + width - S(10);
    float y = p.y + S(26) + S(10);

    // Previous, play/pause, next, and the frame number.
    const float cy = y + S(12);
    float x = x0;
    if (glyphButton(dl, "prev frame", ImVec2(x, cy - S(12)), 24, false, "Previous frame", keyText(Action::MatchPrevRegion),
                    [](ImDrawList* d, ImVec2 a, float s, ImU32 c) {
                        drawIcon(d, Icon::Prev, ImVec2(a.x + s * 0.3f, a.y + s * 0.3f), s * 0.42f, c);
                    })) {
        app.framePlaying = false;
        showFrame(app.frameIndex - 1);
    }
    x += S(24) + S(4);
    if (glyphButton(dl, "play", ImVec2(x, cy - S(12)), 24, app.framePlaying, app.framePlaying ? "Pause" : "Play", "Space",
                    app.framePlaying ? pauseGlyph : playGlyph)) {
        app.framePlaying = !app.framePlaying;
        app.frameShownMs = nowMs();
    }
    x += S(24) + S(4);
    if (glyphButton(dl, "next frame", ImVec2(x, cy - S(12)), 24, false, "Next frame", keyText(Action::MatchNextRegion),
                    [](ImDrawList* d, ImVec2 a, float s, ImU32 c) {
                        drawIcon(d, Icon::Next, ImVec2(a.x + s * 0.3f, a.y + s * 0.3f), s * 0.42f, c);
                    })) {
        app.framePlaying = false;
        showFrame(app.frameIndex + 1);
    }
    x += S(24) + S(8);
    char text[48];
    std::snprintf(text, sizeof text, "Frame %d / %d", app.frameIndex + 1, n);
    drawText(dl, fontMono, 12, x, cy, t.tx, text);
    y += S(24) + S(10);

    // Scrub bar across all frames.
    float tpos = n > 1 ? float(app.frameIndex) / float(n - 1) : 0.0f;
    if (slider(dl, "frame", x0 + S(4), y + S(8), (width - S(20)) / S(1) - S(8), tpos, 0.0f, nullptr)) {
        app.framePlaying = false;
        showFrame(int(std::round(tpos * float(n - 1))));
    }
    requestTip(ImRect(ImVec2(x0, y), ImVec2(x1, y + S(16))), "Drag to any frame", nullptr, true);
    y += S(16) + S(8);

    // Delay of the current frame and the loop count.
    char delay[32], loop[32];
    // 0 and 10 ms play at 100 ms, as in browsers.
    const int stored = app.image.frames[size_t(app.frameIndex)].delayMs;
    if (stored <= 10) std::snprintf(delay, sizeof delay, "delay 100 ms (file: %d)", stored);
    else std::snprintf(delay, sizeof delay, "delay %d ms", stored);
    if (app.image.loopCount <= 0) std::snprintf(loop, sizeof loop, "loop \xE2\x88\x9E");
    else std::snprintf(loop, sizeof loop, "loop %d\xC3\x97", app.image.loopCount);
    drawText(dl, fontMono, 11, x0, y + S(6), t.tx3, delay);
    drawText(dl, fontMono, 11, x1 - textSize(fontMono, 11, loop).x, y + S(6), t.tx3, loop);
    y += S(12) + S(10);
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(width, y - p.y));
}

constexpr float kRowH = 32;
constexpr int kMaxRows = 10;

// What tells a layer apart besides its name.
std::string layerDetail(const htv::Layer& l, bool psd) {
    if (psd) {
        if (l.group || l.opacity >= 1.0f) return l.detail;
        char t[16];
        std::snprintf(t, sizeof t, "%d%%", int(std::lround(l.opacity * 100.0f)));
        return t;
    }
    if (!l.detail.empty() || app.image.layerKind == htv::LayerKind::IconSizes) return l.detail;
    if (l.width != app.image.layers[0].width || l.height != app.image.layers[0].height) {
        char t[32];
        std::snprintf(t, sizeof t, "%u \xC3\x97 %u", l.width, l.height);
        return t;
    }
    return {};
}

void layerRow(ImDrawList* dl, int i, float x0, float y, float width, bool psd) {
    const Theme& t = T();
    const htv::Image& img = app.image;
    const htv::Layer& l = img.layers[size_t(i)];
    const float h = S(kRowH), cy = y + h * 0.5f;
    const ImVec2 a(x0, y), b(x0 + width, y + h);
    const bool shown = psd ? htv::layerShows(img, i) : i == img.shownLayer;
    ImGui::PushID(i);

    // The eye: PSD layers toggle, the one-at-a-time kinds mark the layer on screen.
    const float indent = psd ? S(12) * float(std::min(l.depth, 6)) : 0.0f;
    const ImVec2 ea(x0 + S(6), std::round(cy - S(10)));
    bool eyeHovered = false;
    if (psd) {
        ImGui::SetCursorScreenPos(ea);
        if (ImGui::InvisibleButton("eye", ImVec2(S(20), S(20)))) setLayerVisible(i, !l.visible, (GetKeyState(VK_MENU) & 0x8000) != 0);
        eyeHovered = ImGui::IsItemHovered();
        requestTip(ImRect(ea, ImVec2(ea.x + S(20), ea.y + S(20))), l.visible ? "Hide" : "Show", "Alt+click: only this", true);
    }
    // The rest of the row picks the layer (one-at-a-time kinds).
    ImGui::SetCursorScreenPos(ImVec2(psd ? ea.x + S(22) : a.x, a.y));
    const bool rowPressed = ImGui::InvisibleButton("row", ImVec2(b.x - (psd ? ea.x + S(22) : a.x), h));
    const bool rowHovered = ImGui::IsItemHovered();
    if (!psd && rowPressed) selectLayer(i);
    if (!psd && shown) dl->AddRectFilled(a, b, t.acs);
    else if (!psd && rowHovered) dl->AddRectFilled(a, b, t.ctlh);
    if (eyeHovered) dl->AddRectFilled(ea, ImVec2(ea.x + S(20), ea.y + S(20)), t.ctlh, S(3));
    const ImU32 eyeColor = psd ? (l.visible ? (shown ? t.tx : t.tx3) : 0) : (shown ? t.act : rowHovered ? t.tx3 : 0);
    if (eyeColor) drawIcon(dl, Icon::Eye, ImVec2(ea.x + S(3), ea.y + S(3)), S(14), eyeColor);

    // Thumbnail (or a folder for a group) and the name.
    const ImVec2 ta(ea.x + S(20) + S(6) + indent, std::round(cy - S(13)));
    if (l.group) {
        drawIcon(dl, Icon::Folder, ImVec2(ta.x + S(5), ta.y + S(5)), S(16), shown ? t.tx2 : t.tx3);
    } else {
        if (GpuView* thumb = layerThumbnail(i))
            dl->AddImage(textureRef(thumb), ta, ImVec2(ta.x + S(26), ta.y + S(26)));
        dl->AddRect(ta, ImVec2(ta.x + S(26), ta.y + S(26)), t.bd2, 0, 0, line());
    }
    const std::string detail = layerDetail(l, psd);
    const float detailW = detail.empty() ? 0.0f : textSize(fontMono, 11, detail.c_str()).x + S(8);
    const float nameX = ta.x + S(26) + S(8);
    const std::string name = l.name.empty() ? std::string("Layer ") + std::to_string(i + 1) : l.name;
    drawText(dl, l.group ? fontMedium : fontRegular, 12, nameX, cy, shown ? t.tx : t.tx3,
             ellipsize(l.group ? fontMedium : fontRegular, 12, name, b.x - S(8) - detailW - nameX).c_str());
    if (!detail.empty()) drawText(dl, fontMono, 11, b.x - S(8) - (detailW - S(8)), cy, t.tx3, detail.c_str());
    ImGui::PopID();
}

void layersBody(ImDrawList* dl, ImVec2 p, float width) {
    const Theme& t = T();
    const htv::Image& img = app.image;
    const bool psd = img.layerKind == htv::LayerKind::PsdLayers;
    const int n = int(img.layers.size());
    const float x0 = p.x + S(10);
    float y = p.y + S(26);

    // The rows, scrolling past ten.
    const int visible = std::min(n, kMaxRows);
    const float listH = S(kRowH) * float(visible);
    ImGui::SetCursorScreenPos(ImVec2(p.x, y));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, S(6));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, 0);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, t.bd2);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, t.tx3);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, t.tx3);
    ImGui::BeginChild("rows", ImVec2(width - line(), listH), ImGuiChildFlags_None,
                      n > kMaxRows ? 0 : ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* rows = ImGui::GetWindowDrawList();
    // Stepping with , and . keeps the layer on screen in view.
    static int lastShown = -1;
    const float scroll = ImGui::GetScrollY();
    if (!psd && img.shownLayer != lastShown) {
        const float top = S(kRowH) * float(img.shownLayer);
        if (top < scroll) ImGui::SetScrollY(top);
        else if (top + S(kRowH) > scroll + listH) ImGui::SetScrollY(top + S(kRowH) - listH);
        lastShown = img.shownLayer;
    }
    const float rowW = width - line() - (n > kMaxRows ? S(6) : 0.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const int first = std::max(0, int(scroll / S(kRowH)));
    for (int i = first; i < n && i < first + visible + 1; ++i)
        layerRow(rows, i, origin.x, origin.y + S(kRowH) * float(i), rowW, psd);
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(rowW, S(kRowH) * float(n)));
    ImGui::EndChild();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    y += listH;
    dl->AddRectFilled(ImVec2(p.x + line(), y), ImVec2(p.x + width - line(), y + line()), t.bd);
    y += line() + S(8);

    // What the list shows, and how to step through it.
    char text[96];
    if (psd) {
        std::snprintf(text, sizeof text, "%s", img.composited ? "Composited here, blend modes as Normal"
                                                              : "The file's own composite");
    } else {
        const htv::Layer& l = img.layers[size_t(img.shownLayer)];
        const std::string name = l.name.empty() ? "Layer" : l.name;
        std::snprintf(text, sizeof text, "%s \xC2\xB7 %d of %d", name.c_str(), img.shownLayer + 1, n);
    }
    drawText(dl, fontRegular, 11, x0, y + S(7), t.tx3, ellipsize(fontRegular, 11, text, width - S(20) - (psd ? 0.0f : S(60))).c_str());
    if (!psd) {  // the keys that step through them
        const char* prev = keyText(Action::MatchPrevRegion);
        const char* next = keyText(Action::MatchNextRegion);
        float x = p.x + width - S(10) - kbdChip(dl, 0, 0, next, false);
        kbdChip(dl, x, y + S(7), next);
        x -= S(4) + kbdChip(dl, 0, 0, prev, false);
        kbdChip(dl, x, y + S(7), prev);
    }
    y += S(14) + S(10);
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(width, y - p.y));
}

}  // namespace

void toggleFramesOrLayers() {
    if (!isAnimation() && !hasLayers()) return;
    app.showFrames = !app.showFrames;
}

void framesPanel() {
    if (!app.showFrames || (!isAnimation() && !hasLayers())) return;
    const bool anim = isAnimation();
    const Theme& t = T();
    const float width = S(300);
    // Under the top-right corner, like the other analysis panels.
    if (!beginPanel("##frames", ImVec2(app.vpX + app.vpW - width - S(16), app.vpY + S(16)), width)) {
        ImGui::End();
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetWindowPos();
    const float hh = S(26);
    const char* title = anim ? "Frames" : "Layers";
    panelHeader(dl, p, width, hh, title, 12, 9);
    // The format (and how many layers) next to the title.
    char tag[48];
    if (anim) std::snprintf(tag, sizeof tag, "%s", app.image.description.substr(0, app.image.description.find(',')).c_str());
    else std::snprintf(tag, sizeof tag, "%s \xC2\xB7 %d", htv::layerKindName(app.image.layerKind), int(app.image.layers.size()));
    const float titleW = textSize(fontSemibold, 12, title).x;
    drawText(dl, fontRegular, 12, p.x + S(9) + titleW + S(8), p.y + (hh - line()) * 0.5f, t.tx3, tag);
    if (smallButton(dl, "close", Icon::Dismiss,
                    ImVec2(p.x + width - S(3) - S(20), std::round(p.y + (hh - line() - S(20)) * 0.5f)), 20, 14, "Close",
                    keyText(Action::Layers)))
        app.showFrames = false;

    if (anim) framesBody(dl, p, width);
    else layersBody(dl, p, width);
    ImGui::End();
}

}  // namespace htv::ui
