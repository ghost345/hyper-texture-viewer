// Analysis panels over the image: Histogram and Color match.

#include "ui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace htv::ui {
namespace {

// Pixel counts of one channel per plot column, `columns` wide. `innerMax` gets the tallest column that does
// not hold the first or last bin.
std::vector<float> histogramColumns(const ImageStats::Channel& c, bool logAxis, float lo, float hi, int columns,
                                    float& innerMax) {
    std::vector<float> out(size_t(std::max(columns, 1)), 0.0f);
    const std::vector<uint32_t>& bins = logAxis ? c.log : c.linear;
    const int n = int(bins.size());
    for (int x = 0; x < columns; ++x) {
        // The bins under this column; the tallest stands for them, so narrow spikes stay visible.
        int b0, b1;
        if (logAxis) {
            const float s0 = lo + (hi - lo) * float(x) / float(columns), s1 = lo + (hi - lo) * float(x + 1) / float(columns);
            b0 = int(std::floor((s0 - ImageStats::kLowStop) * ImageStats::kStopBins));
            b1 = int(std::floor((s1 - ImageStats::kLowStop) * ImageStats::kStopBins));
        } else {
            b0 = x * n / columns;
            b1 = (x + 1) * n / columns;
        }
        b1 = std::max(b1, b0 + 1);
        float m = 0.0f;
        for (int b = std::max(b0, 0); b < std::min(b1, n); ++b) m = std::max(m, float(bins[size_t(b)]));
        out[size_t(x)] = m;
        if (b0 > 0 && b1 < n) innerMax = std::max(innerMax, m);
    }
    return out;
}

// "1/256", "1", "16": stops as values, exact and short.
std::string stopLabel(int stop) {
    char buf[32];
    if (stop >= 0) std::snprintf(buf, sizeof buf, "%.0f", std::exp2(double(stop)));
    else std::snprintf(buf, sizeof buf, "1/%.0f", std::exp2(double(-stop)));
    return buf;
}

}  // namespace

void histogramPanel() {
    if (!app.showHistogram || !app.hasImage) return;
    ensureChannelStats();
    const Theme& t = T();
    const float width = S(300);
    if (!beginPanel("##histogram", ImVec2(app.vpX + S(16), app.vpY + S(16)), width)) {
        ImGui::End();
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetWindowPos();
    const float hh = S(26);
    panelHeader(dl, p, width, hh, "Histogram", 12, 9);
    if (smallButton(dl, "close", Icon::Dismiss, ImVec2(p.x + width - S(3) - S(20), std::round(p.y + (hh - line() - S(20)) * 0.5f)),
                    20, 14, "Close", keyText(Action::Histogram)))
        app.showHistogram = false;

    const ImageStats& st = app.stats;
    const htv::FormatInfo& info = htv::formatInfo(app.image.format);
    const bool logAxis = isFloatFormat(info);         // HDR values: a log2 axis in stops
    const bool eightBit = !isHighPrecision(app.image);  // values read as 0..255
    const float x0 = p.x + S(10), x1 = p.x + width - S(10);
    float y = p.y + hh + S(8);

    static const char* modes[] = {"RGB", "R", "G", "B", "L", "Parade"};
    static const char* modeTips[] = {"Red, green and blue overlaid", "Red", "Green", "Blue", "Luminance",
                                     "Red, green and blue side by side"};
    static const char* scales[] = {"Lin", "Log"};
    static const char* scaleTips[] = {"Pixel counts on a linear scale", "Pixel counts on a log scale: rare values show"};
    segmented(dl, "mode", x0, y + S(10), modes, 6, app.histogramMode, true, modeTips);
    int scale = app.histogramLog ? 1 : 0;
    if (segmented(dl, "scale", x1 - segmentedWidth(scales, 2, true), y + S(10), scales, 2, scale, true, scaleTips))
        app.histogramLog = scale == 1;
    y += S(20) + S(8);

    // Plot.
    const ImVec2 a(x0, y), b(x1, y + S(110));
    dl->AddRectFilled(a, b, app.darkTheme ? hex(0x141414) : t.bar2);
    dl->AddRect(a, b, t.bd, 0, 0, line());
    const float px0 = a.x + line(), px1 = b.x - line(), top = a.y + S(6), bottom = b.y - line();
    const int columns = int(px1 - px0);

    // Which channels, and where: one plot, or three side by side for the parade.
    const int mode = app.histogramMode;
    int shown[3] = {0, 1, 2}, count = 3;
    if (mode >= 1 && mode <= 4) {
        shown[0] = mode - 1;
        count = 1;
    }
    const bool parade = mode == 5;

    float lo = 0.0f, hi = 1.0f;
    if (st.channelsValid && logAxis) {
        // From the darkest to the brightest stop that holds any pixels, at most 24 stops.
        int first = INT32_MAX, last = -1;
        for (int c = 0; c < 4; ++c)
            for (int i = 0; i < int(st.channel[c].log.size()); ++i)
                if (st.channel[c].log[size_t(i)]) {
                    first = std::min(first, i);
                    last = std::max(last, i);
                }
        if (last >= 0) {
            hi = std::ceil(ImageStats::kLowStop + float(last + 1) / ImageStats::kStopBins);
            lo = std::floor(ImageStats::kLowStop + float(first) / ImageStats::kStopBins);
            hi = std::max(hi, 1.0f);
            lo = std::max(lo, hi - 24.0f);
            if (hi - lo < 4.0f) lo = hi - 4.0f;
        } else {
            lo = -8.0f;
            hi = 1.0f;
        }
    }

    if (!st.channelsValid) {
        const char* msg = "Not available for this image";
        drawText(dl, fontRegular, 12, std::round((a.x + b.x - textSize(fontRegular, 12, msg).x) * 0.5f),
                 (a.y + b.y) * 0.5f, t.tx3, msg);
    } else {
        const int partW = parade ? (columns - 2 * int(S(4))) / 3 : columns;
        std::vector<float> cols[3];
        float peak = 0.0f, inner = 0.0f;
        for (int k = 0; k < count; ++k) {
            cols[k] = histogramColumns(st.channel[shown[k]], logAxis, lo, hi, partW, inner);
            for (float v : cols[k]) peak = std::max(peak, v);
        }
        // On a linear scale a spike at pure black or white (masks, alpha cut-outs) would flatten the rest:
        // the end columns may run off the top.
        const float linearMax = inner > 0.0f ? inner : peak;
        static const ImU32 fills[4] = {IM_COL32(255, 70, 60, 140), IM_COL32(70, 220, 90, 115), IM_COL32(70, 130, 255, 115),
                                       IM_COL32(200, 200, 200, 115)};
        static const ImU32 strokes[4] = {hex(0xff6a5e), hex(0x5fe07a), hex(0x6f9bff), hex(0xcccccc)};
        const float plotH = bottom - top;
        dl->PushClipRect(ImVec2(px0, a.y + line()), ImVec2(px1, bottom), true);
        for (int k = 0; k < count; ++k) {
            const int c = shown[k];
            const float left = px0 + (parade ? float(k) * (float(partW) + S(4)) : 0.0f);
            std::vector<ImVec2> outline;
            outline.reserve(size_t(partW));
            for (int x = 0; x < partW; ++x) {
                const float v = cols[k][size_t(x)];
                float h = 0.0f;
                if (v > 0.0f)
                    h = app.histogramLog ? std::log1p(v) / std::log1p(std::max(peak, 1.0f)) : v / std::max(linearMax, 1.0f);
                h = std::min(h, 1.0f) * plotH;
                const float cx = left + float(x);
                if (h > 0.0f) dl->AddRectFilled(ImVec2(cx, bottom - h), ImVec2(cx + 1.0f, bottom), fills[c]);
                outline.push_back(ImVec2(cx + 0.5f, std::round(bottom - h) + 0.5f));
            }
            if (outline.size() > 1)
                dl->AddPolyline(outline.data(), int(outline.size()), app.darkTheme ? strokes[c] : fade(strokes[c], 1.0f),
                                ImDrawFlags_None, std::max(1.0f, s * 0.75f));
        }
        dl->PopClipRect();

        // Black and white points.
        if (!parade) {
            float bp = 0.0f, wp = 1.0f;
            if (isHighPrecision(app.image)) {
                bp = app.blackPoint;
                wp = app.whitePoint;
            }
            auto at = [&](float v) -> float {
                if (logAxis) {
                    if (v <= 0.0f) return -1.0f;
                    return (std::log2(v) - lo) / (hi - lo);
                }
                return v;
            };
            for (float v : {bp, wp}) {
                const float f = at(v);
                if (f < 0.0f || f > 1.0f) continue;
                const float x = std::clamp(std::round(px0 + f * float(columns)), px0, px1 - line());
                dashedLine(dl, ImVec2(x, a.y + line()), ImVec2(x, bottom));
            }
        }

        // Hover: the value under the mouse and the counts.
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("plot", ImVec2(b.x - a.x, b.y - a.y));
        if (ImGui::IsItemHovered()) {
            const float mx = ImGui::GetIO().MousePos.x - px0;
            int k = 0, x = int(mx);
            if (parade) {
                k = std::clamp(int(mx / (float(partW) + S(4))), 0, 2);
                x = int(mx - float(k) * (float(partW) + S(4)));
            }
            if (x >= 0 && x < partW) {
                char buf[160];
                const float f = (float(x) + 0.5f) / float(partW);
                if (logAxis) std::snprintf(buf, sizeof buf, "%.4g", std::exp2(double(lo + f * (hi - lo))));
                else if (eightBit) std::snprintf(buf, sizeof buf, "%d", int(f * 255.0f));
                else std::snprintf(buf, sizeof buf, "%.3f", double(f));
                std::string tip = buf;
                static const char* names[4] = {"R", "G", "B", "L"};
                for (int j = 0; j < count; ++j) {
                    if (parade && j != k) continue;
                    std::snprintf(buf, sizeof buf, "  %s %.0f", names[shown[j]], double(cols[j][size_t(x)]));
                    tip += buf;
                }
                requestTip(ImRect(ImVec2(px0 + float(x), a.y), ImVec2(px0 + float(x) + 1.0f, b.y)), tip.c_str(), nullptr);
            }
        }
    }
    y = b.y + S(4);

    // Axis labels: five along one plot, the two ends of each part of the parade.
    const float ly = y + S(6);
    static const char* eight[] = {"0", "64", "128", "192", "255"};
    static const char* unit[] = {"0", "0.25", "0.5", "0.75", "1"};
    auto label = [&](int i, std::string& text) -> float {  // text and position (0..1) of tick i of 5
        if (!logAxis) {
            text = eightBit ? eight[i] : unit[i];
            return float(i) / 4.0f;
        }
        const int stop = int(std::round(lo + (hi - lo) * float(i) / 4.0f));
        text = stopLabel(stop);
        return (float(stop) - lo) / (hi - lo);
    };
    const int parts = parade ? 3 : 1;
    const float partSpan = parade ? (x1 - x0 - 2 * S(4)) / 3.0f : x1 - x0;
    for (int k = 0; k < parts; ++k) {
        const float l = x0 + float(k) * (partSpan + S(4)), r = l + partSpan;
        for (int i = 0; i < 5; ++i) {
            if (parade && i != 0 && i != 4) continue;
            std::string text;
            const float f = label(i, text);
            const float w = textSize(fontMono, 10, text.c_str()).x;
            const float x = i == 0 ? l : i == 4 ? r - w : std::round(l + f * (r - l) - w * 0.5f);
            drawText(dl, fontMono, 10, x, ly, t.tx3, text.c_str());
        }
    }
    y += S(12) + S(8);

    // Mean, min and max of the channels shown.
    if (st.channelsValid) {
        static const char* names[4] = {"R", "G", "B", "L"};
        static const ImU32 colors[4] = {hex(0xff8a80), hex(0x8fe39a), hex(0x8ab6ff), 0};
        const float colW = (x1 - x0 - S(44)) / 3.0f, rowH = S(15) + S(2);
        auto value = [&](double v, bool mean) {
            char buf[32];
            if (eightBit) std::snprintf(buf, sizeof buf, mean ? "%.1f" : "%.0f", v * 255.0);
            else std::snprintf(buf, sizeof buf, "%.4g", v);
            return std::string(buf);
        };
        for (int k = 0; k < count; ++k) {
            const int c = shown[k];
            const float cx = x0 + S(44) + colW * float(k);
            const ImU32 col = colors[c] ? (app.darkTheme ? colors[c] : t.tx2) : t.tx2;
            drawText(dl, fontMono, 11, cx, y + rowH * 0.5f, col, names[c]);
            const ImageStats::Channel& ch = st.channel[c];
            drawText(dl, fontMono, 11, cx, y + rowH * 1.5f, t.tx, value(ch.mean, true).c_str());
            drawText(dl, fontMono, 11, cx, y + rowH * 2.5f, t.tx, value(ch.min, false).c_str());
            drawText(dl, fontMono, 11, cx, y + rowH * 3.5f, t.tx, value(ch.max, false).c_str());
        }
        static const char* rows[] = {"Mean", "Min", "Max"};
        for (int i = 0; i < 3; ++i) drawText(dl, fontMono, 11, x0, y + rowH * (1.5f + float(i)), t.tx3, rows[i]);
        y += rowH * 4 - S(2);
    }
    y += S(10);
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(width, y - p.y));
    ImGui::End();
}

// ---- Color match ----

void toggleColorMatch() {
    if (app.colorMatch || app.matchPicking) {
        app.colorMatch = app.matchPicking = false;
        return;
    }
    if (!app.hasImage) return;
    app.colorMatch = true;
    // The target: the locked pixel, the pixel under the cursor, or the next click on the image.
    int x = app.lockedX, y = app.lockedY;
    if (app.pixelLocked ? setMatchTarget(x, y)
                        : windowToPixel(float(app.mouseX), float(app.mouseY), x, y) && setMatchTarget(x, y))
        return;
    if (!app.matchHasTarget) app.matchPicking = true;
}

void stepMatchRegion(int delta) {
    if (!app.colorMatch || !app.matchValid || app.matchRegions.boxes.empty()) return;
    const int n = int(app.matchRegions.boxes.size());
    const int i = app.matchRegion < 0 ? (delta > 0 ? 0 : n - 1) : (app.matchRegion + delta + n) % n;
    showMatchRegion(i);
}

namespace {

// "123 930": thousands apart, as the design writes counts.
std::string groupDigits(uint64_t v) {
    std::string d = std::to_string(v), out;
    for (size_t i = 0; i < d.size(); ++i) {
        if (i && (d.size() - i) % 3 == 0) out += ' ';
        out += d[i];
    }
    return out;
}

const char* kMatchModes[] = {"Whole color (RGB)", "Whole color (RGBA)", "Red", "Green", "Blue", "Alpha",
                             "Luminance", "Hue", "Saturation"};

}  // namespace

void colorMatchPanel() {
    if (!app.colorMatch || !app.hasImage) return;
    const Theme& t = T();
    const float width = S(300);
    // Top right; under Info when that is open in its place.
    if (!beginPanel("##colormatch", ImVec2(app.vpX + app.vpW - width - S(16), app.vpY + S(16)), width)) {
        ImGui::End();
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetWindowPos();
    const float hh = S(26);
    panelHeader(dl, p, width, hh, "Color match", 12, 9);
    if (smallButton(dl, "close", Icon::Dismiss, ImVec2(p.x + width - S(3) - S(20), std::round(p.y + (hh - line() - S(20)) * 0.5f)),
                    20, 14, "Close", keyText(Action::ColorMatch)))
        app.colorMatch = app.matchPicking = false;

    const float x0 = p.x + S(10), x1 = p.x + width - S(10), controlX = x0 + S(86);
    const float rowH = S(22), gap = S(8);
    float y = p.y + hh + S(10);
    Row r;
    r.dl = dl;
    auto label = [&](const char* text, float cy, const char* tip) {
        drawText(dl, fontRegular, 12, x0, cy, t.tx2, text);
        if (tip) {
            ImGui::PushID(text);
            tipArea("label", ImRect(ImVec2(x0, cy - rowH * 0.5f), ImVec2(controlX - S(8), cy + rowH * 0.5f)), tip);
            ImGui::PopID();
        }
    };
    const bool eightBit = !isHighPrecision(app.image);

    // Target: swatch, value and the eyedropper.
    float cy = y + rowH * 0.5f;
    label("Target", cy, "The colour pixels are compared with");
    {
        const float* c = app.matchTarget;
        const bool srgb = isSrgbFormat(app.image.format);
        // The swatch shows the target as the viewer shows it: sRGB values as they are, linear data encoded.
        auto shown = [&](float v) {
            v = std::clamp(v, 0.0f, 1.0f);
            if (app.dataIsLinear && !srgb) v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
            return int(v * 255.0f + 0.5f);
        };
        const ImVec2 sa(controlX, std::round(cy - S(11))), sb(sa.x + S(22), sa.y + S(22));
        if (app.matchHasTarget) dl->AddRectFilled(sa, sb, IM_COL32(shown(c[0]), shown(c[1]), shown(c[2]), 255));
        else dashedBox(dl, sa, sb, t.bd2);
        dl->AddRect(sa, sb, t.bd2, 0, 0, line());
        char text[64] = "none";
        if (app.matchHasTarget) {
            auto byte = [](float v) { return int(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
            const bool alpha = app.matchMode == 1 || app.matchMode == 5;
            if (eightBit && alpha)
                std::snprintf(text, sizeof text, "#%02X%02X%02X%02X", byte(c[0]), byte(c[1]), byte(c[2]), byte(c[3]));
            else if (eightBit)
                std::snprintf(text, sizeof text, "#%02X%02X%02X", byte(c[0]), byte(c[1]), byte(c[2]));
            else
                std::snprintf(text, sizeof text, "%.4g %.4g %.4g", double(c[0]), double(c[1]), double(c[2]));
        }
        const float tx = sb.x + S(8);
        drawText(dl, fontMono, 12, tx, cy, app.matchHasTarget ? t.tx : t.tx3,
                 ellipsize(fontMono, 12, text, x1 - S(22) - S(8) - tx).c_str());
        tipArea("target", ImRect(sa, ImVec2(x1 - S(22) - S(8), sb.y)),
                app.matchHasTarget ? "Target colour, as stored in the file" : "Pick a pixel on the image");
        r.x = x1 - S(22);
        r.cy = cy;
        if (r.button("pick", Icon::ColorMatch, nullptr, kBordered | (app.matchPicking ? kOn : 0),
                     app.matchPicking ? "Click a pixel on the image (Esc cancels)" : "Pick the target from the image",
                     "Alt+Click", 14, 22, 3))
            app.matchPicking = !app.matchPicking;
    }
    y += rowH + gap;

    // Compare by.
    cy = y + rowH * 0.5f;
    label("Compare by", cy, "Which part of the colour has to match");
    {
        r.x = controlX;
        r.cy = cy;
        ImRect dr;
        toggleMenuOnPress(r.dropdown("mode", kMatchModes[app.matchMode], fontRegular, 12, 22, 8, 4,
                                     "Which part of the colour has to match", nullptr, &dr, false, "Whole color (RGBA)",
                                     x1 - controlX),
                          "mode menu");
        if (beginMenu("mode menu", ImVec2(dr.Min.x, dr.Max.y + S(4)), ImVec2(0, 0), dr.GetWidth())) {
            const int channels = htv::formatInfo(app.image.format).channels;
            for (int i = 0; i < 9; ++i) {
                // Only what the format stores: no alpha modes without alpha, no colour modes for grey.
                const bool usable = !((i == 1 || i == 5) && channels < 4) && !(channels == 1 && (i == 3 || i == 4 || i >= 7)) &&
                                    !(channels == 2 && i == 4);
                if (menuItem({kMatchModes[i], nullptr, app.matchMode == i, usable, true})) app.matchMode = i;
                if (i == 1 || i == 5) menuSeparator();
            }
            endMenu();
        }
    }
    y += rowH + gap;

    // Tolerance: a square-law slider, so the small values that matter most get most of its length.
    cy = y + rowH * 0.5f;
    label("Tolerance", cy, nullptr);
    {
        const char* tip = app.matchMode == 7 ? "How far the hue may differ, in 1/255 of the colour circle"
                          : eightBit         ? "How far each value may differ, in 8-bit steps"
                                             : "How far each value may differ, in steps of 1/255";
        tipArea("tolerance label", ImRect(ImVec2(x0, cy - rowH * 0.5f), ImVec2(controlX - S(8), cy + rowH * 0.5f)), tip);
        float tpos = std::sqrt(float(app.matchTolerance) / 255.0f);
        bool reset = false;
        if (slider(dl, "tolerance", controlX + S(4), cy, 120, tpos, 0.0f, &reset, -1.0f, std::sqrt(2.0f / 255.0f)))
            app.matchTolerance = std::clamp(int(std::round(tpos * tpos * 255.0f)), 0, 255);
        if (reset) app.matchTolerance = 2;
        requestTip(ImRect(ImVec2(controlX, cy - S(8)), ImVec2(controlX + S(128), cy + S(8))), tip, nullptr, true);
        char text[16];
        std::snprintf(text, sizeof text, "\xC2\xB1 %d", app.matchTolerance);
        float v = float(app.matchTolerance);
        if (numberField(dl, "tolerance value", x1 - S(54), cy, 54, text, v, "Type a tolerance, 0\xE2\x80\x93" "255"))
            app.matchTolerance = std::clamp(int(std::round(v)), 0, 255);
    }
    y += rowH + gap;

    dl->AddRectFilled(ImVec2(p.x + line(), y - S(6)), ImVec2(p.x + width - line(), y - S(6) + line()), t.bd);
    y += S(4);

    // Found: the count, its share of the image and the regions.
    const MaskRegions& m = app.matchRegions;
    const bool ready = app.matchValid;
    cy = y + S(12);
    label("Found", cy, nullptr);
    {
        const std::string count = ready ? groupDigits(m.pixels) : app.matchHasTarget ? "\xE2\x80\xA6" : "\xE2\x80\x94";
        drawText(dl, fontMono, 20, controlX, cy, t.tx, count.c_str());
        const float cw = textSize(fontMono, 20, count.c_str()).x;
        drawText(dl, fontMono, 12, controlX + cw + S(6), cy + S(3), t.tx3, "px");
        tipArea("found", ImRect(ImVec2(x0, cy - S(12)), ImVec2(x1, cy + S(12))), "Pixels within the tolerance of the target");
    }
    y += S(24) + gap;
    cy = y + S(8);
    if (ready) {
        const double total = double(app.image.width) * double(app.image.height);
        char text[96];
        const double share = total > 0 ? 100.0 * double(m.pixels) / total : 0.0;
        char pct[16];
        if (share > 0 && share < 0.1) std::snprintf(pct, sizeof pct, "<0.1");
        else std::snprintf(pct, sizeof pct, "%.1f", share);
        std::snprintf(text, sizeof text, "%s%% of image \xC2\xB7 %s region%s", pct, groupDigits(m.regions).c_str(),
                      m.regions == 1 ? "" : "s");
        drawText(dl, fontMono, 12, controlX, cy, t.tx2, text);
        tipArea("share", ImRect(ImVec2(controlX, cy - S(8)), ImVec2(x1, cy + S(8))),
                "Regions are areas of matching pixels that touch by an edge");
    } else if (!app.matchHasTarget) {
        drawText(dl, fontRegular, 12, controlX, cy, t.tx3, "Click a pixel to pick the target");
    }
    y += S(16) + gap;

    // Region: step through them; the view centres on each.
    cy = y + rowH * 0.5f;
    label("Region", cy, nullptr);
    {
        const int n = ready ? int(m.boxes.size()) : 0;
        const int off = n ? 0 : kDisabled;
        r.x = controlX;
        r.cy = cy;
        r.gap = S(4);
        if (r.button("prev region", Icon::Prev, nullptr, kBordered | off, "Previous region",
                     keyText(Action::MatchPrevRegion), 14, 22, 3))
            stepMatchRegion(-1);
        char text[48];
        if (n && app.matchRegion >= 0) std::snprintf(text, sizeof text, "%d / %d", app.matchRegion + 1, n);
        else std::snprintf(text, sizeof text, "\xE2\x80\x93 / %d", n);
        r.field("region index", text, fontMono, 12, n ? t.tx : t.tx3, "Region shown, of all regions", "000 / 000");
        if (r.button("next region", Icon::Next, nullptr, kBordered | off, "Next region",
                     keyText(Action::MatchNextRegion), 14, 22, 3))
            stepMatchRegion(+1);
        r.gap = 0;
        const char* copyLabel = "Copy";
        const float bw = line() + S(8) + textSize(fontRegular, 12, copyLabel).x + S(8) + line();
        r.x = x1 - bw;
        const ImGuiID copyId = ImGui::GetID("copy count");
        if (r.button("copy count", Icon::Count, copyLabel, kBordered | (ready ? 0 : kDisabled),
                     copiedRecently(copyId) ? "Copied" : "Copy the number of matching pixels", nullptr, 16, 22, 8)) {
            ImGui::SetClipboardText(std::to_string(m.pixels).c_str());
            flashCopied(copyId);
        }
    }
    y += rowH + S(10);
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(width, y - p.y));
    ImGui::End();
}

}  // namespace htv::ui
