// The image shader's constants, filled the same way for Direct3D (render.cpp) and WebGL (web/render_web.cpp).
// The layout is the HLSL constant buffer's, which std140 uniform blocks share.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "viewer.h"
#include "ui.h"

namespace htv {

struct ViewConstants {
    float imageSize[2];
    float imageOrigin[2];
    float zoom;
    float exposureScale;
    uint32_t channelMode;
    uint32_t channels;
    uint32_t transfer;
    uint32_t signedValues;
    uint32_t rotation;
    uint32_t flips;
    float tiles;
    uint32_t reserved;
    int32_t forcedMip;
    float invGamma;
    float checkerSize;
    float blackPoint;
    float rangeScale;
    uint32_t toneMap;
    uint32_t clipping;
    float zebraWidth;
    float zebraPhase;
    float padding;  // checker1 starts a new 16-byte register
    float checker1[4];
    float checker2[4];
    float background[4];
    float matchTarget[4];
    uint32_t matchMode;
    float matchTolerance;
    uint32_t matchSrgb;
    uint32_t matchOutline;
    float image2Size[2];
    float scale2[2];
    uint32_t channels2;
    uint32_t srgb1;
    uint32_t srgb2;
    float diffGain;
    float tileOrigin[2];
    float tileSize[2];
    float tile2Origin[2];
    float tile2Size[2];
    uint32_t firstPass;
    float padding2[3];
};
static_assert(sizeof(ViewConstants) % 16 == 0, "constant buffer size must be a multiple of 16");

inline constexpr float kCheckerSizes[3] = {8, 16, 32};
inline constexpr float kSolidBackgrounds[3] = {0.0f, 0.5f, 1.0f};  // black, middle grey, white

inline ViewConstants makeConstants(const htv::Image& image, float originX, float originY, float zoom, float tiles,
                                   float checkerSize, const UiColors& colors) {
    const htv::FormatInfo& info = htv::formatInfo(image.format);
    ViewConstants c{};
    c.imageSize[0] = float(image.width);
    c.imageSize[1] = float(image.height);
    c.imageOrigin[0] = originX;
    c.imageOrigin[1] = originY;
    c.zoom = zoom;
    const bool hdr = isHighPrecision(image);
    c.exposureScale = hdr ? std::exp2(app.exposure) : 1.0f;
    const uint32_t modes[] = {0, 1, 2, 3, 4, 6};
    c.channelMode = modes[int(app.channel)];
    if (c.channelMode == 0 && app.ignoreAlpha) c.channelMode = 5;
    c.channels = uint32_t(info.channels);
    c.transfer = (app.dataIsLinear ? 1u : 0u) + (isSrgbFormat(image.format) ? 1u : 0u);
    c.signedValues = info.snorm ? 1u : 0u;
    c.rotation = uint32_t(app.rotation);
    c.flips = (app.flipH ? 1u : 0u) | (app.flipV ? 2u : 0u);
    c.tiles = tiles;
    c.forcedMip = app.forcedMip;
    // Range, tone mapping and gamma belong to the HDR row, which only float and 16-bit images show.
    c.invGamma = hdr ? 1.0f / app.gamma : 1.0f;
    c.blackPoint = hdr ? app.blackPoint : 0.0f;
    c.rangeScale = hdr ? 1.0f / std::max(app.whitePoint - app.blackPoint, 1e-6f) : 1.0f;
    c.toneMap = hdr && app.dataIsLinear ? uint32_t(app.toneMap) : 0u;
    c.clipping = hdr && app.showClipping ? 1u : 0u;
    c.zebraWidth = std::max(1.0f, std::round(4 * app.scale));
    // One stripe pair every two seconds; the period keeps the float exact over long sessions.
    c.zebraPhase = float(std::floor(std::fmod(nowMs() * 0.001 * c.zebraWidth, 2.0 * c.zebraWidth)));
    if (app.alphaBackground == AlphaBackground::Checker) {
        c.checkerSize = checkerSize;
        for (int i = 0; i < 3; ++i) {
            c.checker1[i] = colors.checker1[i];
            c.checker2[i] = colors.checker2[i];
        }
    } else {
        c.checkerSize = 1.0f;
        for (int i = 0; i < 3; ++i)
            c.checker1[i] = c.checker2[i] = app.alphaBackground == AlphaBackground::Custom
                                                ? app.customBackground[i]
                                                : kSolidBackgrounds[int(app.alphaBackground) - 1];
    }
    for (int i = 0; i < 3; ++i) c.background[i] = colors.viewport[i];
    c.checker1[3] = c.checker2[3] = c.background[3] = 1.0f;
    c.matchOutline = app.colorMatch && app.matchValid && app.matchMask ? 1u : 0u;
    return c;
}

}  // namespace htv
