// Compare (Ctrl+K): up to three images as split shutters, a blend or a difference.

#include "ui.h"
#include "ui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace htv::ui {
namespace {

// A small clickable window over the viewport, like ui_views' beginArea.
void compareArea(const char* id, ImVec2 a, ImVec2 b) {
    ImGui::SetNextWindowPos(a);
    ImGui::SetNextWindowSize(ImVec2(b.x - a.x, b.y - a.y));
    ImGui::Begin(id, nullptr, kBarFlags);
}

const char* kSlotNames[3] = {"A", "B", "C"};
const int kPairs[3][2] = {{0, 1}, {0, 2}, {1, 2}};

std::string slotName(int i) {
    if (i == 0) return fileName();
    const CompareSlot& s = app.compareSlots[i - 1];
    return s.path.empty() ? std::string() : utf8(s.path.filename().wstring());
}

// "1024² · PNG 8-bit": the size and what the file holds, short enough for a slot row.
std::string slotDetail(int i) {
    const htv::Image& img = i == 0 ? app.image : app.compareSlots[i - 1].image;
    if (!img.width) return "";
    char size[48];
    if (img.width == img.height) std::snprintf(size, sizeof size, "%u\xC2\xB2", img.width);
    else std::snprintf(size, sizeof size, "%u\xC3\x97%u", img.width, img.height);
    std::string d = img.description;
    const size_t comma = d.find(',');
    if (comma != std::string::npos) d = d.substr(0, comma);
    const int mips = i == 0 ? app.textureMips : app.compareSlots[i - 1].mips;
    std::string out = std::string(size) + " \xC2\xB7 " + d;
    if (mips > 1) out += " \xC2\xB7 " + std::to_string(mips) + " mips";
    return out;
}

bool slotReady(int i) { return i == 0 ? app.hasImage : app.compareSlots[i - 1].srv.get() != nullptr; }

std::string pairLabel(int p) {
    const int a = kPairs[p][0], b = kPairs[p][1];
    return std::string(kSlotNames[a]) + " \xE2\x88\x92 " + kSlotNames[b];
}

bool pairReady(int p) { return slotReady(kPairs[p][0]) && slotReady(kPairs[p][1]); }

// A slot's row: letter, thumbnail, name, details, and a menu to fill or clear it.
void slotRow(ImDrawList* dl, float x0, float x1, float y, int i) {
    const Theme& t = T();
    const float h = S(34);
    const float cy = y + h * 0.5f;
    const ImVec2 ba(x0, std::round(cy - S(10))), bb(ba.x + S(20), ba.y + S(20));
    dl->AddRectFilled(ba, bb, t.ctl, S(3));
    drawText(dl, fontMedium, 11, std::round(ba.x + (S(20) - textSize(fontMedium, 11, kSlotNames[i]).x) * 0.5f), cy,
             t.tx, kSlotNames[i]);

    const ImVec2 ta(bb.x + S(8), std::round(cy - S(13))), tb(ta.x + S(26), ta.y + S(26));
    if (ID3D11ShaderResourceView* thumb = compareThumbnail(i - 1))
        dl->AddImage(ImTextureRef(ImTextureID(reinterpret_cast<intptr_t>(thumb))), ta, tb);
    else dl->AddRectFilled(ta, tb, t.bar2);
    dl->AddRect(ta, tb, t.bd2, 0, 0, line());

    const CompareSlot* slot = i == 0 ? nullptr : &app.compareSlots[i - 1];
    const float nx = tb.x + S(8), nw = x1 - S(20) - S(4) - nx;
    std::string name = slotName(i);
    const char* hint = slot && slot->loading ? "Loading\xE2\x80\xA6" : "Empty \xC2\xB7 click to choose a file";
    if (slot && !slot->error.empty()) hint = "Cannot be opened";
    drawText(dl, fontRegular, 12, nx, cy - S(7), name.empty() ? t.tx3 : t.tx,
             ellipsize(fontRegular, 12, name.empty() ? hint : name, nw).c_str());
    const std::string detail = slot && !slot->error.empty() ? slot->error : slotDetail(i);
    if (!detail.empty())
        drawText(dl, fontMono, 11, nx, cy + S(8), slot && !slot->error.empty() ? t.err : t.tx3,
                 ellipsize(fontMono, 11, detail, nw).c_str());

    if (i == 0) {
        tipArea("slot a", ImRect(ImVec2(x0, y), ImVec2(x1, y + h)), "A is the image the viewer shows");
        return;
    }
    ImGui::PushID(i);
    ImGui::SetCursorScreenPos(ImVec2(x0, y));
    const bool clicked = ImGui::InvisibleButton("slot", ImVec2(x1 - x0 - S(24), h));
    if (ImGui::IsItemHovered())
        dl->AddRectFilled(ImVec2(x0 - S(4), y), ImVec2(x1, y + h), fade(t.ctlh, 0.5f), S(3));
    requestTip(ImRect(ImVec2(x0, y), ImVec2(x1 - S(24), y + h)),
               name.empty() ? "Choose a file for this slot" : "Choose another file; drop a file here too", nullptr, true);
    Row r;
    r.dl = dl;
    r.x = x1 - S(20);
    r.cy = cy;
    const bool menu = r.button("slot menu", Icon::Chevron, nullptr, 0, "Recent files and clearing", nullptr, 10, 20, 5);
    toggleMenuOnPress(menu, "slot menu");
    std::filesystem::path chosen;
    bool clear = false, choose = clicked;
    if (beginMenu("slot menu", ImVec2(x0, y + h + S(4)), ImVec2(0, 0), x1 - x0)) {
        if (menuItem({"Open file\xE2\x80\xA6", nullptr, false, true, false, Icon::Open})) choose = true;
        if (!app.previousPath.empty() &&
            menuItem({ellipsize(fontRegular, 12, utf8(app.previousPath.filename().wstring()), S(220)).c_str(),
                      "previous", false, true}))
            chosen = app.previousPath;
        int shown = 0;
        for (const RecentFile& rf : app.recent) {
            if (rf.path == app.shownPath || rf.path == app.previousPath || ++shown > 5) continue;
            if (menuItem({ellipsize(fontRegular, 12, utf8(rf.path.filename().wstring()), S(220)).c_str()}))
                chosen = rf.path;
        }
        if (slot && !slot->path.empty()) {
            menuSeparator();
            if (menuItem({"Clear slot", nullptr, false, true, false, Icon::Dismiss})) clear = true;
        }
        endMenu();
    }
    ImGui::PopID();
    if (choose) {
        std::filesystem::path p;
        if (pickImageFile(p)) chosen = p;
    }
    if (clear) loadCompareSlot(i - 1, {});
    else if (!chosen.empty()) loadCompareSlot(i - 1, chosen);
}

}  // namespace

void toggleCompare() {
    if (app.compare) {
        app.compare = false;
        return;
    }
    if (!app.hasImage) return;
    app.compare = true;
    // The file shown before this one is the obvious B; the user can change it.
    if (app.compareSlots[0].path.empty() && !app.previousPath.empty()) loadCompareSlot(0, app.previousPath);
}

// The dashed shutters and the A / B / C labels over the image.
void compareOverlay() {
    int ids[3];
    const int n = app.compare ? compareLayers(ids) : 0;
    if (n < 1) return;
    const Theme& t = T();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float w, h;
    imageSizeOnScreen(w, h);
    const float span = w * app.zoom;
    const float top = std::max(app.vpY, app.originY), bottom = std::min(app.vpY + app.vpH, app.originY + h * app.zoom);

    if (app.compareMode == CompareMode::Split) {
        // Each shutter has a handle straddling its line, lettered with the images on either side. Two shutters
        // put their handles at different heights, so they never cover each other even where the lines meet.
        for (int k = 0; k + 1 < n; ++k) {
            const float x = std::round(app.originX + app.compareShutters[k] * span);
            if (x > app.vpX && x < app.vpX + app.vpW) dashedLine(dl, ImVec2(x, top), ImVec2(x, bottom));
            const float at = n > 2 ? (k == 0 ? 0.3f : 0.7f) : 0.5f;
            const float hh = S(22), half = S(20);
            const float my = std::round(std::clamp(top + (bottom - top) * at, app.vpY + hh, app.vpY + app.vpH - hh));
            const ImVec2 ha(x - half, my - hh * 0.5f), hb(x + half, my + hh * 0.5f);
            ImGui::PushID(k);
            compareArea(k == 0 ? "##shutter0" : "##shutter1", ha, hb);
            ImGui::SetCursorScreenPos(ha);
            ImGui::InvisibleButton("handle", ImVec2(hb.x - ha.x, hb.y - ha.y));
            const bool active = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();
            ImDrawList* hdl = ImGui::GetWindowDrawList();
            hdl->AddRectFilled(ha, hb, active ? IM_COL32(70, 70, 70, 235) : hovered ? IM_COL32(50, 50, 50, 230)
                                                                                   : IM_COL32(24, 24, 24, 220),
                               S(4));
            hdl->AddRect(ha, hb, active || hovered ? kWhite : IM_COL32(255, 255, 255, 150), S(4), 0, line());
            hdl->AddRectFilled(ImVec2(x, ha.y), ImVec2(x + line(), hb.y), IM_COL32(255, 255, 255, 150));
            const char* left = kSlotNames[ids[k]];
            const char* right = kSlotNames[ids[k + 1]];
            const float cy = my;
            drawText(hdl, fontMedium, 12, std::round(x - half * 0.5f - textSize(fontMedium, 12, left).x * 0.5f), cy,
                     kWhite, left);
            drawText(hdl, fontMedium, 12, std::round(x + half * 0.5f - textSize(fontMedium, 12, right).x * 0.5f), cy,
                     kWhite, right);
            ImGui::End();
            ImGui::PopID();
            const std::string tip = std::string(left) + "  " + slotName(ids[k]) + "\n" + right + "  " + slotName(ids[k + 1]);
            requestTip(ImRect(ha, hb), tip.c_str(), nullptr);
            if (active && span > 1.0f) {
                // A shutter stops at its neighbour instead of pushing it, so the part between can always be
                // opened again from either side.
                const float v = std::clamp((ImGui::GetIO().MousePos.x - app.originX) / span, 0.0f, 1.0f);
                const float lo = k == 1 ? app.compareShutters[0] : 0.0f;
                const float hi = k == 0 && n > 2 ? app.compareShutters[1] : 1.0f;
                app.compareShutters[k] = std::clamp(v, lo, hi);
            }
        }
        return;
    }
    // Blend and Difference: one label saying what the viewport shows.
    if (!pairReady(app.comparePair)) return;
    char text[64];
    if (app.compareMode == CompareMode::Blend)
        std::snprintf(text, sizeof text, "%s \xE2\x86\x92 %s  %d%%", kSlotNames[kPairs[app.comparePair][0]],
                      kSlotNames[kPairs[app.comparePair][1]], int(std::round(app.compareBlend * 100)));
    else
        std::snprintf(text, sizeof text, "|%s \xE2\x88\x92 %s|  \xC3\x97 %g", kSlotNames[kPairs[app.comparePair][0]],
                      kSlotNames[kPairs[app.comparePair][1]], double(app.compareGain));
    const float tw = textSize(fontMono, 12, text).x;
    const ImVec2 a(std::round(std::max(app.originX, app.vpX)) + S(10), std::round(std::max(top, app.vpY)) + S(10));
    const ImVec2 b(a.x + S(10) + tw + S(10), a.y + S(24));
    if (b.x > app.vpX + app.vpW) return;
    dl->AddRectFilled(a, b, IM_COL32(24, 24, 24, 214), S(4));
    dl->AddRect(a, b, IM_COL32(255, 255, 255, 36), S(4), 0, line());
    drawText(dl, fontMono, 12, a.x + S(10), (a.y + b.y) * 0.5f, hex(0xe8e8e8), text);
    (void)t;
}

void comparePanel() {
    if (!app.compare) return;
    const Theme& t = T();
    const float width = S(300);
    if (!beginPanel("##compare", ImVec2(app.vpX + app.vpW - width - S(16), app.vpY + S(16)), width)) {
        ImGui::End();
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetWindowPos();
    const float hh = S(26);
    panelHeader(dl, p, width, hh, "Compare", 12, 9);
    if (smallButton(dl, "close", Icon::Dismiss,
                    ImVec2(p.x + width - S(3) - S(20), std::round(p.y + (hh - line() - S(20)) * 0.5f)), 20, 14, "Close",
                    keyText(Action::Compare)))
        app.compare = false;

    const float x0 = p.x + S(10), x1 = p.x + width - S(10), controlX = x0 + S(60);
    float y = p.y + hh + S(6);
    Row r;
    r.dl = dl;

    for (int i = 0; i < 3; ++i) {
        slotRow(dl, x0, x1, y, i);
        y += S(34);
    }
    drawText(dl, fontRegular, 11, x0, y + S(9), t.tx3, "Drop a file on a slot, or pick from open files");
    y += S(18) + S(8);
    dl->AddRectFilled(ImVec2(p.x + line(), y), ImVec2(p.x + width - line(), y + line()), t.bd);
    y += line() + S(6);

    const float rowH = S(30);
    float cy = y + rowH * 0.5f;
    drawText(dl, fontRegular, 12, x0, cy, t.tx, "Mode");
    static const char* modes[] = {"Split", "Blend", "Difference"};
    static const char* modeTips[] = {"Each image in its own part of the view, with draggable shutters",
                                     "One image fading into the other",
                                     "Where two images differ, amplified through a heat ramp"};
    int mode = int(app.compareMode);
    if (segmented(dl, "mode", controlX, cy, modes, 3, mode, false, modeTips)) app.compareMode = CompareMode(mode);
    y += rowH;

    int ids[3];
    const int layers = compareLayers(ids);
    if (app.compareMode == CompareMode::Split) {
        cy = y + rowH * 0.5f;
        drawText(dl, fontRegular, 12, x0, cy, t.tx, "Shutters");
        char text[64];
        std::snprintf(text, sizeof text, "%d \xC2\xB7 drag on image", std::max(layers - 1, 0));
        drawText(dl, fontMono, 12, controlX, cy, t.tx2, text);
        tipArea("shutters", ImRect(ImVec2(x0, cy - S(10)), ImVec2(x1, cy + S(10))),
                "One shutter for two images, two for three; drag them on the image");
        y += rowH;
    } else {
        cy = y + rowH * 0.5f;
        drawText(dl, fontRegular, 12, x0, cy, t.tx, "Pair");
        r.x = controlX;
        r.cy = cy;
        ImRect dr;
        std::string current = pairLabel(app.comparePair);
        const std::string a = slotName(kPairs[app.comparePair][0]), b = slotName(kPairs[app.comparePair][1]);
        toggleMenuOnPress(r.dropdown("pair", current.c_str(), fontRegular, 12, 22, 8, 4,
                                     "Which two images are compared", nullptr, &dr, false, nullptr, x1 - controlX),
                          "pair menu");
        if (beginMenu("pair menu", ImVec2(dr.Min.x, dr.Max.y + S(4)), ImVec2(0, 0), dr.GetWidth())) {
            for (int i = 0; i < 3; ++i)
                if (menuItem({pairLabel(i).c_str(), nullptr, app.comparePair == i, pairReady(i), true}))
                    app.comparePair = i;
            endMenu();
        }
        y += rowH;

        if (app.compareMode == CompareMode::Blend) {
            cy = y + rowH * 0.5f;
            drawText(dl, fontRegular, 12, x0, cy, t.tx, "Blend");
            bool reset = false;
            float v = app.compareBlend;
            if (slider(dl, "blend", controlX + S(4), cy, 150, v, 0.0f, &reset, 0.5f, 0.5f)) app.compareBlend = v;
            if (reset) app.compareBlend = 0.5f;
            char text[32];
            std::snprintf(text, sizeof text, "%d%%", int(std::round(app.compareBlend * 100)));
            r.x = x1 - textSize(fontMono, 12, "100%").x;
            r.cy = cy;
            r.field("blend value", text, fontMono, 12, t.tx, "How much of the second image shows", "100%");
            requestTip(ImRect(ImVec2(x0, cy - S(10)), ImVec2(controlX + S(158), cy + S(10))),
                       "How much of the second image shows; the mouse wheel over the image works too", nullptr, true);
            y += rowH;
        } else {
            cy = y + rowH * 0.5f;
            drawText(dl, fontRegular, 12, x0, cy, t.tx, "Gain");
            // 1 to 256, in whole stops: a difference of one 8-bit step is visible at x 256.
            bool reset = false;
            float v = std::log2(std::clamp(app.compareGain, 1.0f, 256.0f)) / 8.0f;
            if (slider(dl, "gain", controlX + S(4), cy, 120, v, 0.0f, &reset, -1.0f, 3.0f / 8.0f))
                app.compareGain = std::exp2(std::round(v * 8.0f));
            if (reset) app.compareGain = 8.0f;
            requestTip(ImRect(ImVec2(x0, cy - S(10)), ImVec2(controlX + S(128), cy + S(10))),
                       "How much the difference is amplified, in whole stops", nullptr, true);
            char text[32];
            std::snprintf(text, sizeof text, "\xC3\x97 %g", double(app.compareGain));
            float gv = app.compareGain;
            if (numberField(dl, "gain value", x1 - S(54), cy, 54, text, gv, "Type a gain, 1\xE2\x80\x93" "256"))
                app.compareGain = std::clamp(gv, 1.0f, 256.0f);
            y += rowH;

            // Ramp: the difference values the colours stand for, in the file's own steps.
            cy = y + rowH * 0.5f;
            drawText(dl, fontRegular, 12, x0, cy - S(4), t.tx, "Ramp");
            const float ra = controlX, rb = x1;
            const ImVec2 ga(ra, std::round(cy - S(10))), gb(rb, ga.y + S(8));
            for (float x = ga.x; x < gb.x; x += 1.0f) {
                const float f = (x - ga.x) / std::max(gb.x - ga.x, 1.0f);
                const auto ch = [&](float v) { return int(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
                dl->AddRectFilled(ImVec2(x, ga.y), ImVec2(x + 1.0f, gb.y),
                                  IM_COL32(ch(f * 3), ch(f * 3 - 1), ch(f * 3 - 2), 255));
            }
            dl->AddRect(ga, gb, t.bd2, 0, 0, line());
            for (int i = 0; i < 4; ++i) {
                char lab[16];
                const double value = 255.0 * double(i) / 3.0 / double(app.compareGain);
                std::snprintf(lab, sizeof lab, i == 3 ? "%.0f+" : "%.0f", value);
                const float lw = textSize(fontMono, 10, lab).x;
                const float lx = i == 0 ? ga.x : i == 3 ? gb.x - lw : std::round(ga.x + (gb.x - ga.x) * i / 3.0f - lw * 0.5f);
                drawText(dl, fontMono, 10, lx, gb.y + S(7), t.tx3, lab);
            }
            tipArea("ramp", ImRect(ga, ImVec2(gb.x, gb.y + S(12))),
                    "Difference in 8-bit steps: black none, white the brightest the gain shows");
            y += rowH;

            // Statistics of the pair.
            y += S(4);
            const CompareStats& st = app.compareStats;
            static const char* names[4] = {"Max \xCE\x94", "Mean \xCE\x94", "Pixels \xCE\x94 > 2", "PSNR"};
            static const char* tips[4] = {"The largest difference of any channel",
                                          "The average difference over all channels",
                                          "Pixels differing by more than 2 of 255 in any channel",
                                          "Peak signal-to-noise ratio: over 40 dB is hard to see"};
            for (int i = 0; i < 4; ++i) {
                const float ly = y + S(10) + float(i) * S(16);
                drawText(dl, fontMono, 12, x0, ly, t.tx3, names[i]);
                char value[64] = "\xE2\x80\x94";
                if (st.valid && st.pixels) {
                    const double scale = 255.0;
                    switch (i) {
                        case 0: std::snprintf(value, sizeof value, "%.0f", st.maxDelta * scale); break;
                        case 1: std::snprintf(value, sizeof value, "%.2f", st.meanDelta * scale); break;
                        case 2: {
                            const double share = 100.0 * double(st.pixelsOver) / double(st.pixels);
                            std::string n = std::to_string(st.pixelsOver), g;
                            for (size_t k = 0; k < n.size(); ++k) {
                                if (k && (n.size() - k) % 3 == 0) g += ' ';
                                g += n[k];
                            }
                            std::snprintf(value, sizeof value, "%s \xC2\xB7 %.1f%%", g.c_str(), share);
                            break;
                        }
                        default:
                            if (st.mse <= 0) std::snprintf(value, sizeof value, "\xE2\x88\x9E");
                            else std::snprintf(value, sizeof value, "%.1f dB", 10.0 * std::log10(1.0 / st.mse));
                    }
                }
                const float vw = textSize(fontMono, 12, value).x;
                drawText(dl, fontMono, 12, x1 - vw, ly, t.tx, value);
                tipArea(names[i], ImRect(ImVec2(x0, ly - S(8)), ImVec2(x1, ly + S(8))), tips[i]);
            }
            y += S(16) * 4 + S(4);
        }
    }

    // Matched sizes: B and C stretched over A, so a half-resolution version lines up.
    cy = y + rowH * 0.5f;
    bool same = app.compareSameSize;
    if (checkbox(dl, "same size", x0, cy, "Match sizes", same,
                 "Stretch B and C over A when their sizes differ; off compares pixel for pixel"))
        app.compareSameSize = same;
    y += rowH + S(4);

    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(width, y - p.y));
    ImGui::End();
}

}  // namespace htv::ui
