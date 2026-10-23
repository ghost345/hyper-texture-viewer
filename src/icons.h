// Line icons from the UI design, drawn from their SVG path data so they stay sharp at any DPI.
#pragma once

#include "imgui.h"

namespace htv {

enum class Icon {
    Open, Prev, Next, Fit, RotateLeft, RotateRight, FlipH, FlipV, AlphaBackground, Tiling, Grid, Ruler,
    PixelOutline, ColorMatch, Histogram, Info, Layers, Compare, OnTop, Opacity, Theme, Settings, Help,
    Minimize, Maximize, Restore, Close, Chevron, Copy, Pin, More, Warning, Check, Sun, Image, Alert, Search, Clip,
    Dismiss, ClickThrough,
    Count
};

// Draws `icon` in a size x size box at `pos` (24-unit design grid scaled to size).
// `stroke` is the line width in design units (1.5 for toolbar icons, 1.2 for window buttons).
void drawIcon(ImDrawList* dl, Icon icon, ImVec2 pos, float size, ImU32 color, float stroke = 1.5f);

}  // namespace htv
