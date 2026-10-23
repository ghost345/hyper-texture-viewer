#include "stats.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace htv {
namespace {

constexpr int kBins = (ImageStats::kHighStop - ImageStats::kLowStop) * ImageStats::kStopBins;

struct Accumulator {
    ImageStats& s;
    float minValue = INFINITY, maxValue = -INFINITY, minLum = INFINITY, maxLum = -INFINITY;

    void add(float r, float g, float b) {
        ++s.pixels;
        if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b)) {
            ++s.nonFinite;
            return;
        }
        minValue = std::min({minValue, r, g, b});
        maxValue = std::max({maxValue, r, g, b});
        const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        maxLum = std::max(maxLum, lum);
        if (lum <= 0.0f) {
            ++s.darkPixels;
            return;
        }
        minLum = std::min(minLum, lum);
        const int bin = int(std::floor((std::log2(lum) - ImageStats::kLowStop) * ImageStats::kStopBins));
        ++s.histogram[size_t(std::clamp(bin, 0, kBins - 1))];
    }

    void finish() {
        const bool any = s.pixels > s.nonFinite;
        s.minValue = any ? minValue : 0.0f;
        s.maxValue = any ? maxValue : 0.0f;
        s.minLuminance = std::isfinite(minLum) ? minLum : 0.0f;
        s.maxLuminance = any ? maxLum : 0.0f;
        s.valid = true;
    }
};

}  // namespace

ImageStats computeStats(const Image& image) {
    ImageStats s;
    const FormatInfo& info = formatInfo(image.format);
    if (image.mips.empty() || info.compressed) return s;
    s.histogram.assign(kBins, 0);
    Accumulator acc{s};
    const Surface& surface = image.mips[0];

    if (image.format == GpuFormat::RGBA16F || image.format == GpuFormat::RG16F || image.format == GpuFormat::R16F) {
        // Half floats through a lookup table: the per-pixel conversion would dominate an 8K image.
        static const std::vector<float> table = [] {
            std::vector<float> t(65536);
            for (uint32_t i = 0; i < 65536; ++i) t[i] = halfToFloat(uint16_t(i));
            return t;
        }();
        const int n = info.channels;
        for (uint32_t y = 0; y < image.height; ++y) {
            const uint8_t* row = surface.data.data() + size_t(y) * surface.rowPitch;
            for (uint32_t x = 0; x < image.width; ++x) {
                uint16_t h[4] = {0, 0, 0, 0};
                std::memcpy(h, row + size_t(x) * n * 2, size_t(n) * 2);
                const float r = table[h[0]];
                if (n == 1) acc.add(r, r, r);
                else acc.add(r, table[h[1]], n == 2 ? 0.0f : table[h[2]]);
            }
        }
    } else {
        // readPixel turns one-channel formats into grey, as the viewer shows them.
        for (uint32_t y = 0; y < image.height; ++y)
            for (uint32_t x = 0; x < image.width; ++x) {
                float px[4];
                readPixel(image, x, y, px);
                acc.add(px[0], px[1], px[2]);
            }
    }
    acc.finish();
    return s;
}

bool hasTranslucentPixels(const Image& image) {
    const FormatInfo& info = formatInfo(image.format);
    if (info.channels < 4 || image.format == GpuFormat::BGRX8 || image.mips.empty()) return false;
    if (info.compressed)
        return image.format != GpuFormat::BC1 && image.format != GpuFormat::BC1Srgb &&
               image.format != GpuFormat::BC6HUf16 && image.format != GpuFormat::BC6HSf16;
    const Surface& surface = image.mips[0];
    if (info.bytes == 4 && image.format != GpuFormat::RGB10A2) {  // 8-bit RGBA or BGRA: alpha is the 4th byte
        for (uint32_t y = 0; y < image.height; ++y) {
            const uint8_t* row = surface.data.data() + size_t(y) * surface.rowPitch;
            for (uint32_t x = 0; x < image.width; ++x)
                if (row[x * 4 + 3] != 255) return true;
        }
        return false;
    }
    for (uint32_t y = 0; y < image.height; ++y)
        for (uint32_t x = 0; x < image.width; ++x) {
            float px[4];
            if (readPixel(image, x, y, px) && px[3] != 1.0f) return true;
        }
    return false;
}

float ImageStats::percentile(double fraction) const {
    const uint64_t finite = pixels - nonFinite;
    if (!valid || finite == 0) return 0.0f;
    const double target = std::clamp(fraction, 0.0, 1.0) * double(finite);
    double seen = double(darkPixels);
    if (seen >= target) return 0.0f;
    for (int i = 0; i < kBins; ++i) {
        const double next = seen + histogram[size_t(i)];
        if (next >= target && histogram[size_t(i)] > 0) {
            const double within = (target - seen) / double(histogram[size_t(i)]);
            const double stop = kLowStop + (i + within) / kStopBins;
            return std::clamp(float(std::exp2(stop)), minLuminance, maxLuminance);
        }
        seen = next;
    }
    return maxLuminance;
}

std::vector<float> ImageStats::histogramBetween(float lo, float hi, int bins) const {
    std::vector<float> out(size_t(std::max(bins, 1)), 0.0f);
    if (!valid || hi <= lo) return out;
    for (int i = 0; i < kBins; ++i) {
        if (!histogram[size_t(i)]) continue;
        const float stop = kLowStop + (i + 0.5f) / kStopBins;
        const int b = int(std::floor((stop - lo) / (hi - lo) * float(bins)));
        if (b >= 0 && b < bins) out[size_t(b)] += float(histogram[size_t(i)]);
    }
    return out;
}

}  // namespace htv
