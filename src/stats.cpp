#include "stats.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace htv {
namespace {

constexpr int kBins = (ImageStats::kHighStop - ImageStats::kLowStop) * ImageStats::kStopBins;

// Log2 bin from the float's bits: the exponent picks the stop, the top mantissa bits split it into
// kStopBins steps. Much cheaper than log2 for every value of an 8K image, and close enough for a histogram.
int logBin(float v) {
    static_assert(ImageStats::kStopBins == 16, "four mantissa bits make the bins of a stop");
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    const int exponent = int((bits >> 23) & 255) - 127;
    const int bin = (exponent - ImageStats::kLowStop) * ImageStats::kStopBins + int((bits >> 19) & 15);
    return std::clamp(bin, 0, kBins - 1);
}

}  // namespace

StatsBuilder::StatsBuilder(bool channels) : channels_(channels) {
    s_.histogram.assign(kBins, 0);
    if (channels)
        for (ImageStats::Channel& c : s_.channel) {
            c.linear.assign(ImageStats::kLinearBins, 0);
            c.log.assign(kBins, 0);
        }
}

void StatsBuilder::add(float r, float g, float b) {
    ++s_.pixels;
    if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b)) {
        ++s_.nonFinite;
        return;
    }
    minValue_ = std::min({minValue_, r, g, b});
    maxValue_ = std::max({maxValue_, r, g, b});
    const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    maxLum_ = std::max(maxLum_, lum);
    if (channels_) {
        const float v[4] = {r, g, b, lum};
        for (int i = 0; i < 4; ++i) {
            ImageStats::Channel& c = s_.channel[i];
            sum_[i] += v[i];
            min_[i] = std::min(min_[i], v[i]);
            max_[i] = std::max(max_[i], v[i]);
            const int lin = int(std::clamp(v[i], 0.0f, 1.0f) * (ImageStats::kLinearBins - 1) + 0.5f);
            ++c.linear[size_t(lin)];
            if (v[i] > 0.0f) ++c.log[size_t(logBin(v[i]))];
        }
    }
    if (lum <= 0.0f) {
        ++s_.darkPixels;
        return;
    }
    minLum_ = std::min(minLum_, lum);
    ++s_.histogram[size_t(logBin(lum))];
}

ImageStats StatsBuilder::finish() {
    const uint64_t finite = s_.pixels - s_.nonFinite;
    s_.minValue = finite ? minValue_ : 0.0f;
    s_.maxValue = finite ? maxValue_ : 0.0f;
    s_.minLuminance = std::isfinite(minLum_) ? minLum_ : 0.0f;
    s_.maxLuminance = finite ? maxLum_ : 0.0f;
    s_.valid = true;
    if (channels_) {
        for (int i = 0; i < 4; ++i) {
            s_.channel[i].mean = finite ? sum_[i] / double(finite) : 0.0;
            s_.channel[i].min = finite ? min_[i] : 0.0f;
            s_.channel[i].max = finite ? max_[i] : 0.0f;
        }
        s_.channelsValid = true;
    }
    return std::move(s_);
}

ImageStats computeStats(const Image& image, bool channels) {
    const FormatInfo& info = formatInfo(image.format);
    if (image.mips.empty() || info.compressed) return {};
    StatsBuilder acc(channels);
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
    return acc.finish();
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
