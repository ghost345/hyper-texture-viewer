#include "icons.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace htv {
namespace {

struct Part {
    const char* d;
    bool fill;
};

struct IconDef {
    Part parts[4];
};

// Path data copied from the design mockup (24x24 view box, round caps and joins).
const IconDef kIcons[int(Icon::Count)] = {
    /* Open */ {{{"M3 18.5v-12A1.5 1.5 0 0 1 4.5 5H9l2 2h7.5A1.5 1.5 0 0 1 20 8.5V10", false}, {"M3 18.5h14.5L21 11H6.5z", false}}},
    /* Prev */ {{{"M15 6l-6 6 6 6", false}}},
    /* Next */ {{{"M9 6l6 6-6 6", false}}},
    /* Fit */ {{{"M4 9V4h5M20 9V4h-5M4 15v5h5M20 15v5h-5", false}}},
    /* RotateLeft */ {{{"M3.5 12a8.5 8.5 0 1 0 2.6-6.1L3.5 8.5", false}, {"M3.5 3.5v5h5", false}}},
    /* RotateRight */ {{{"M20.5 12a8.5 8.5 0 1 1-2.6-6.1l2.6 2.6", false}, {"M20.5 3.5v5h-5", false}}},
    /* FlipH */ {{{"M9 6L3 18h6z", false}, {"M15 6l6 12h-6z", false}, {"M12 3v2M12 9v2M12 15v2M12 20v1", false}}},
    /* FlipV */ {{{"M6 9L18 3v6z", false}, {"M6 15l12 6v-6z", false}, {"M3 12h2M9 12h2M15 12h2M20 12h1", false}}},
    /* AlphaBackground */ {{{"M4 4h16v16H4z", false}, {"M4 4h8v8H4z", true}, {"M12 12h8v8h-8z", true}}},
    /* Tiling */ {{{"M3 3h18v18H3zM9 3v18M15 3v18M3 9h18M3 15h18", false}, {"M9 9h6v6H9z", true}}},
    /* Grid */ {{{"M9 3v18M15 3v18M3 9h18M3 15h18", false}}},
    /* Ruler */ {{{"M3 16L16 3l5 5L8 21z", false}, {"M7.5 11.5l2 2M10.5 8.5l2 2M13.5 5.5l2 2", false}}},
    /* PixelOutline */ {{{"M4 4h3M10.5 4h3M17 4h3v3M20 10.5v3M20 17v3h-3M13.5 20h-3M7 20H4v-3M4 13.5v-3M4 7V4", false}, {"M10 10h4v4h-4z", true}}},
    /* ColorMatch */ {{{"M3 21l1.5-1.5H7l8-8", false}, {"M4.5 19.5V17l8-8", false}, {"M11 7.5l5.5 5.5M14 10l4-4a2.1 2.1 0 0 0-3-3l-4 4", false}}},
    /* Histogram */ {{{"M3 20h18M5.5 20v-6M10 20V6M14.5 20v-9M19 20v-4", false}}},
    /* Info */ {{{"M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18z", false}, {"M12 11v5.5M12 7.8v.01", false}}},
    /* Layers */ {{{"M12 3l9 5-9 5-9-5z", false}, {"M3 13l9 5 9-5", false}}},
    /* Compare */ {{{"M12 3v18", false}, {"M8 8l-4 4 4 4M16 8l4 4-4 4", false}}},
    /* OnTop */ {{{"M5 3.5h14", false}, {"M12 21V8M7 12.5l5-5 5 5", false}}},
    /* Opacity */ {{{"M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18z", false}, {"M12 3a9 9 0 0 0 0 18z", true}}},
    /* Theme */ {{{"M20 14.5A8.5 8.5 0 1 1 9.5 4a6.6 6.6 0 0 0 10.5 10.5z", false}}},
    /* Settings */ {{{"M4 7h9M18 7h2M4 17h2M11 17h9", false}, {"M15.5 4.5a2.5 2.5 0 1 0 0 5 2.5 2.5 0 0 0 0-5zM8.5 14.5a2.5 2.5 0 1 0 0 5 2.5 2.5 0 0 0 0-5z", false}}},
    /* Help */ {{{"M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18z", false}, {"M9.3 9.2a2.8 2.8 0 0 1 5.4 1c0 1.9-2.7 2.5-2.7 3.8M12 17v.01", false}}},
    /* Minimize */ {{{"M5 12h14", false}}},
    /* Maximize */ {{{"M5 5h14v14H5z", false}}},
    /* Restore */ {{{"M5 8h11v11H5z", false}, {"M8 5h11v11", false}}},
    /* Close */ {{{"M5 5l14 14M19 5L5 19", false}}},
    /* Chevron */ {{{"M6 9l6 6 6-6", false}}},
    /* Copy */ {{{"M9 9h11v11H9z", false}, {"M5 15H4V4h11v1", false}}},
    /* Pin */ {{{"M12 16v5M8 4h8M9 4v6l-3 4h12l-3-4V4", false}}},
    /* More */ {{{"M4 12a1.6 1.6 0 1 0 3.2 0 1.6 1.6 0 1 0-3.2 0z", true}, {"M10.4 12a1.6 1.6 0 1 0 3.2 0 1.6 1.6 0 1 0-3.2 0z", true}, {"M16.8 12a1.6 1.6 0 1 0 3.2 0 1.6 1.6 0 1 0-3.2 0z", true}}},
    /* Warning */ {{{"M12 3.5L2.5 20h19z", false}, {"M12 10v4.5M12 17.3v.01", false}}},
    /* Check */ {{{"M5 12.5l4.5 4.5L19 7.5", false}}},
    /* Sun */ {{{"M12 8a4 4 0 1 0 0 8 4 4 0 0 0 0-8z", false}, {"M12 2.5v2M12 19.5v2M2.5 12h2M19.5 12h2M5.3 5.3l1.4 1.4M17.3 17.3l1.4 1.4M5.3 18.7l1.4-1.4M17.3 6.7l1.4-1.4", false}}},
    /* Image */ {{{"M3 5h18v14H3z", false}, {"M3 16l5-5 4 4 3-3 6 6", false}, {"M15.5 8.5v.01", false}}},
    /* Alert */ {{{"M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18z", false}, {"M12 7.5v5.5M12 16.2v.01", false}}},
    /* Search */ {{{"M10.5 4a6.5 6.5 0 1 0 0 13 6.5 6.5 0 0 0 0-13zM15.5 15.5L20 20", false}}},
    /* Clip */ {{{"M4 4h16v16H4z", false}, {"M4 12l8-8M4 20L20 4M12 20l8-8", false}}},
    /* Dismiss */ {{{"M6 6l12 12M18 6L6 18", false}}},
    /* ClickThrough: drawn by dashedPointer */ {{{nullptr, false}}},
};

// Minimal SVG path interpreter: M L H V A C Z and their relative forms, which is all the icon set uses.
class PathRenderer {
public:
    PathRenderer(ImDrawList* dl, ImVec2 origin, float scale, ImU32 color, float thickness, bool fill)
        : dl_(dl), origin_(origin), scale_(scale), color_(color), thickness_(thickness), fill_(fill) {}

    void run(const char* d) {
        p_ = d;
        char cmd = 0;
        while (skipSpace()) {
            if (std::isalpha(static_cast<unsigned char>(*p_))) cmd = *p_++;
            const bool rel = std::islower(static_cast<unsigned char>(cmd)) != 0;
            const float ox = rel ? x_ : 0.0f, oy = rel ? y_ : 0.0f;
            switch (std::toupper(static_cast<unsigned char>(cmd))) {
                case 'M':
                    flush(false);
                    x_ = ox + num();
                    y_ = oy + num();
                    startX_ = x_;
                    startY_ = y_;
                    point(x_, y_);
                    cmd = rel ? 'l' : 'L';  // further pairs are implicit line-tos
                    break;
                case 'L':
                    x_ = ox + num();
                    y_ = oy + num();
                    point(x_, y_);
                    break;
                case 'H':
                    x_ = ox + num();
                    point(x_, y_);
                    break;
                case 'V':
                    y_ = oy + num();
                    point(x_, y_);
                    break;
                case 'C': {
                    const float x1 = ox + num(), y1 = oy + num(), x2 = ox + num(), y2 = oy + num();
                    const float x = ox + num(), y = oy + num();
                    cubic(x1, y1, x2, y2, x, y);
                    break;
                }
                case 'A': {
                    const float r = num();
                    num();  // ry: the icons only use circular arcs
                    num();  // x-axis rotation
                    const bool large = flag(), sweep = flag();
                    const float x = ox + num(), y = oy + num();
                    arc(r, large, sweep, x, y);
                    break;
                }
                case 'Z':
                    x_ = startX_;
                    y_ = startY_;
                    flush(true);
                    break;
                default: return;
            }
        }
        flush(false);
    }

private:
    bool skipSpace() {
        while (*p_ == ' ' || *p_ == ',') ++p_;
        return *p_ != 0;
    }
    float num() {
        skipSpace();
        char* end = nullptr;
        const float v = std::strtof(p_, &end);
        p_ = end;
        return v;
    }
    bool flag() {  // arc flags may be written without separators ("0 0 1" or "001")
        skipSpace();
        return *p_++ == '1';
    }
    void point(float x, float y) {
        if (pts_ == 0) firstX_ = x, firstY_ = y;
        lastX_ = x;
        lastY_ = y;
        pts_++;
        dl_->PathLineTo(ImVec2(origin_.x + x * scale_, origin_.y + y * scale_));
    }
    void cubic(float x1, float y1, float x2, float y2, float x, float y) {
        const float x0 = x_, y0 = y_;
        for (int i = 1; i <= 12; ++i) {
            const float t = i / 12.0f, u = 1 - t;
            point(u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x,
                  u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y);
        }
        x_ = x;
        y_ = y;
    }
    // Endpoint to centre parameterisation (SVG 1.1 appendix F.6) for a circle of radius r.
    void arc(float r, bool large, bool sweep, float x, float y) {
        const float x0 = x_, y0 = y_;
        x_ = x;
        y_ = y;
        const float dx = (x0 - x) * 0.5f, dy = (y0 - y) * 0.5f;
        const float d2 = dx * dx + dy * dy;
        if (d2 == 0.0f) return;
        r = std::max(r, std::sqrt(d2));
        float k = std::sqrt(std::max(0.0f, (r * r - d2) / d2));
        if (large == sweep) k = -k;
        const float cx = (x0 + x) * 0.5f + k * dy, cy = (y0 + y) * 0.5f - k * dx;
        const float a0 = std::atan2(y0 - cy, x0 - cx);
        float a1 = std::atan2(y - cy, x - cx);
        float delta = a1 - a0;
        const float pi = 3.14159265f;
        if (sweep && delta < 0) delta += 2 * pi;
        if (!sweep && delta > 0) delta -= 2 * pi;
        const int n = std::max(2, int(std::ceil(std::fabs(delta) * r * scale_ / 2.0f)));
        for (int i = 1; i <= n; ++i) {
            const float a = a0 + delta * i / n;
            point(cx + r * std::cos(a), cy + r * std::sin(a));
        }
        x_ = x;
        y_ = y;
    }
    void flush(bool closed) {
        if (pts_ == 2 && !fill_ && std::fabs(lastX_ - firstX_) + std::fabs(lastY_ - firstY_) < 0.1f) {
            // "v.01" is how the icons draw a dot: a round cap with no length, which needs a disc here.
            dl_->AddCircleFilled(ImVec2(origin_.x + lastX_ * scale_, origin_.y + lastY_ * scale_), thickness_ * 0.6f, color_);
        } else if (pts_ > 1) {
            if (fill_) dl_->PathFillConvex(color_);
            else dl_->PathStroke(color_, closed ? ImDrawFlags_Closed : 0, thickness_);
        }
        dl_->PathClear();
        pts_ = 0;
        if (closed) point(x_, y_);  // a path may continue from the closing point
    }

    ImDrawList* dl_;
    ImVec2 origin_;
    float scale_;
    ImU32 color_;
    float thickness_;
    bool fill_;
    const char* p_ = nullptr;
    float x_ = 0, y_ = 0, startX_ = 0, startY_ = 0;
    float firstX_ = 0, firstY_ = 0, lastX_ = 0, lastY_ = 0;
    int pts_ = 0;
};

// ClickThrough: an arrow pointer outlined with dashes, a cursor that is not really there.
void dashedPointer(ImDrawList* dl, ImVec2 pos, float scale, ImU32 color, float thickness) {
    static const ImVec2 kOutline[] = {{5, 2.5f}, {5, 20}, {9.6f, 15.8f}, {12.7f, 22.2f}, {15.6f, 21}, {12.6f, 14.6f}, {18.6f, 14.6f}};
    const int n = int(sizeof kOutline / sizeof kOutline[0]);
    const float dash = 3.2f, gap = 2.2f;
    float phase = 0;  // distance into the current dash + gap period, carried around the corners
    for (int i = 0; i < n; ++i) {
        const ImVec2 a = kOutline[i], b = kOutline[(i + 1) % n];
        const float len = std::hypot(b.x - a.x, b.y - a.y);
        float t = 0;
        while (t < len) {
            const bool on = phase < dash;
            const float step = std::min(len - t, on ? dash - phase : dash + gap - phase);
            if (on) {
                const float u0 = t / len, u1 = (t + step) / len;
                dl->AddLine(ImVec2(pos.x + (a.x + (b.x - a.x) * u0) * scale, pos.y + (a.y + (b.y - a.y) * u0) * scale),
                            ImVec2(pos.x + (a.x + (b.x - a.x) * u1) * scale, pos.y + (a.y + (b.y - a.y) * u1) * scale), color,
                            thickness);
            }
            t += step;
            phase = std::fmod(phase + step, dash + gap);
        }
    }
}

}  // namespace

void drawIcon(ImDrawList* dl, Icon icon, ImVec2 pos, float size, ImU32 color, float stroke) {
    const float scale = size / 24.0f;
    if (icon == Icon::ClickThrough) return dashedPointer(dl, pos, scale, color, stroke * scale);
    for (const Part& part : kIcons[int(icon)].parts) {
        if (!part.d) break;
        PathRenderer(dl, pos, scale, color, stroke * scale, part.fill).run(part.d);
    }
}

}  // namespace htv
