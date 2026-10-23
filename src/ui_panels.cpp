// Analysis panels over the image: Histogram.

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
                    20, 14, "Close", "Ctrl+H"))
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

}  // namespace htv::ui
