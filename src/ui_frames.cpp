// Frames (L on an animation): step or play through a GIF or animated WebP's frames.
// Layers (L otherwise): the file's layers; PSD and EXR layer extraction is still to come, so for now
// a single-image file shows its one composite layer.

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
    std::snprintf(delay, sizeof delay, "delay %d ms", app.image.frames[size_t(app.frameIndex)].delayMs);
    if (app.image.loopCount <= 0) std::snprintf(loop, sizeof loop, "loop \xE2\x88\x9E");
    else std::snprintf(loop, sizeof loop, "loop %d\xC3\x97", app.image.loopCount);
    drawText(dl, fontMono, 11, x0, y + S(6), t.tx3, delay);
    drawText(dl, fontMono, 11, x1 - textSize(fontMono, 11, loop).x, y + S(6), t.tx3, loop);
    y += S(12) + S(10);
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(width, y - p.y));
}

void layersBody(ImDrawList* dl, ImVec2 p, float width) {
    const Theme& t = T();
    const float x0 = p.x + S(10), x1 = p.x + width - S(10);
    float y = p.y + S(26);

    // The one composite layer every file has; real PSD / EXR layers are still to come.
    const float h = S(34), cy = y + h * 0.5f;
    drawIcon(dl, Icon::Info, ImVec2(x0, std::round(cy - S(7))), S(14), t.tx2);
    const ImVec2 ta(x0 + S(14) + S(8), std::round(cy - S(13)));
    if (ID3D11ShaderResourceView* thumb = compareThumbnail(-1)) dl->AddImage(ImTextureRef(ImTextureID(reinterpret_cast<intptr_t>(thumb))), ta,
                                                                            ImVec2(ta.x + S(26), ta.y + S(26)));
    dl->AddRect(ta, ImVec2(ta.x + S(26), ta.y + S(26)), t.bd2, 0, 0, line());
    drawText(dl, fontRegular, 12, ta.x + S(26) + S(8), cy, t.tx, "Composite");
    y += h;
    dl->AddRectFilled(ImVec2(p.x + line(), y), ImVec2(p.x + width - line(), y + line()), t.bd);
    y += line() + S(8);
    const char* msg = "This file has a single layer.";
    drawText(dl, fontRegular, 12, x0, y + S(7), t.tx3, msg);
    y += S(14) + S(4);
    drawText(dl, fontRegular, 11, x0, y + S(7), t.tx3,
             ellipsize(fontRegular, 11, "PSD and EXR layer lists are planned.", x1 - x0).c_str());
    y += S(14) + S(10);
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(width, y - p.y));
}

}  // namespace

void toggleFramesOrLayers() {
    if (!app.hasImage) return;
    app.showFrames = !app.showFrames;
}

void framesPanel() {
    if (!app.showFrames || !app.hasImage) return;
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
    panelHeader(dl, p, width, hh, anim ? "Frames" : "Layers", 12, 9);
    // The format and count next to the title.
    char tag[32];
    if (anim) {
        const std::string& d = app.image.description;
        std::snprintf(tag, sizeof tag, "%s", d.compare(0, 3, "GIF") == 0 ? "GIF" : "WebP");
    } else {
        std::snprintf(tag, sizeof tag, "1");
    }
    const float titleW = textSize(fontSemibold, 12, anim ? "Frames" : "Layers").x;
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
