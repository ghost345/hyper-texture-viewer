// Value statistics of a decoded image for the HDR controls: range marks, the mini histogram, Auto range
// and the NaN count. Independent of Windows so it can be unit-tested anywhere.
#pragma once

#include <cmath>
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

    // Per-channel data for the Histogram panel: R, G, B and luminance. Only filled on request.
    static constexpr int kLinearBins = 256;  // equal steps over [0, 1]; 8-bit values land one per bin
    struct Channel {
        double mean = 0.0;
        float min = 0.0f, max = 0.0f;
        std::vector<uint32_t> linear;  // values outside [0, 1] count in the end bins
        std::vector<uint32_t> log;     // bins like `histogram`; values at or below zero are left out
    };
    bool channelsValid = false;
    Channel channel[4];

    // Luminance below which `fraction` (0..1) of the finite pixels lie.
    float percentile(double fraction) const;
    // Pixel counts re-binned into `bins` equal steps between the log2 luminances lo and hi.
    std::vector<float> histogramBetween(float lo, float hi, int bins) const;
};

// Collects statistics from pixels handed in one by one, for sources other than an Image in memory.
class StatsBuilder {
public:
    explicit StatsBuilder(bool channels);
    StatsBuilder(const StatsBuilder&) = delete;
    StatsBuilder& operator=(const StatsBuilder&) = delete;
    void add(float r, float g, float b);
    ImageStats finish();

private:
    ImageStats s_;
    bool channels_;
    float minValue_ = INFINITY, maxValue_ = -INFINITY, minLum_ = INFINITY, maxLum_ = -INFINITY;
    double sum_[4] = {0, 0, 0, 0};
    float min_[4] = {INFINITY, INFINITY, INFINITY, INFINITY}, max_[4] = {-INFINITY, -INFINITY, -INFINITY, -INFINITY};
};

// Full pass over mip 0. Luminance uses Rec. 709 weights on the stored values. `channels` also fills the
// per-channel data, which costs about as much again.
ImageStats computeStats(const Image& image, bool channels = false);

// Areas of a mask, for Color match: 4-connected regions of non-zero bytes.
struct MaskRegion {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // bounding box, x1 and y1 exclusive
    uint64_t pixels = 0;
};
struct MaskRegions {
    uint64_t pixels = 0;   // non-zero bytes
    uint64_t regions = 0;  // all of them, even past `boxes`
    std::vector<MaskRegion> boxes;  // in reading order of each region's first pixel
};
MaskRegions findRegions(const uint8_t* mask, int width, int height, size_t pitch, size_t maxBoxes = 100000);

// Whether any pixel is not fully opaque. Block-compressed formats answer from the format alone.
bool hasTranslucentPixels(const Image& image);

}  // namespace htv
