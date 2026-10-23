// Value statistics of a decoded image for the HDR controls: range marks, the mini histogram, Auto range
// and the NaN count. Independent of Windows so it can be unit-tested anywhere.
#pragma once

#include <cstdint>
#include <vector>

#include "image.h"

namespace htv {

struct ImageStats {
    bool valid = false;        // false for block-compressed images, whose values the CPU cannot read
    float minValue = 0.0f;     // smallest finite R, G or B value
    float maxValue = 0.0f;     // largest finite R, G or B value
    float minLuminance = 0.0f; // smallest luminance above zero, 0 when there is none
    float maxLuminance = 0.0f;
    uint64_t pixels = 0;
    uint64_t nonFinite = 0;    // pixels with NaN or Inf in R, G or B
    uint64_t darkPixels = 0;   // luminance at or below zero (not in the histogram)

    // Luminance histogram on a log2 scale: kStopBins bins per stop from kLowStop to kHighStop.
    static constexpr int kLowStop = -24, kHighStop = 24, kStopBins = 16;
    std::vector<uint32_t> histogram;

    // Luminance below which `fraction` (0..1) of the finite pixels lie.
    float percentile(double fraction) const;
    // Pixel counts re-binned into `bins` equal steps between the log2 luminances lo and hi.
    std::vector<float> histogramBetween(float lo, float hi, int bins) const;
};

// Full pass over mip 0. Luminance uses Rec. 709 weights on the stored values.
ImageStats computeStats(const Image& image);

// Whether any pixel is not fully opaque. Block-compressed formats answer from the format alone.
bool hasTranslucentPixels(const Image& image);

}  // namespace htv
