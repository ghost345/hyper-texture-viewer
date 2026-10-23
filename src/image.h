// Image loading, independent of Windows so it can be unit-tested anywhere.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace htv {

// Values match DXGI_FORMAT so a decoded image can be handed to Direct3D as is.
enum class GpuFormat : uint32_t {
    Unknown = 0,
    RGBA32F = 2,
    RGBA16F = 10,
    RGBA16Unorm = 11,
    RG32F = 16,
    RGB10A2 = 24,
    RG11B10F = 26,
    RGBA8 = 28,
    RGBA8Srgb = 29,
    RG16F = 34,
    RG16Unorm = 35,
    R32F = 41,
    RG8 = 49,
    R16F = 54,
    R16Unorm = 56,
    R8 = 61,
    BC1 = 71,
    BC1Srgb = 72,
    BC2 = 74,
    BC2Srgb = 75,
    BC3 = 77,
    BC3Srgb = 78,
    BC4 = 80,
    BC4Snorm = 81,
    BC5 = 83,
    BC5Snorm = 84,
    BGRA8 = 87,
    BGRX8 = 88,
    BGRA8Srgb = 91,
    BC6HUf16 = 95,
    BC6HSf16 = 96,
    BC7 = 98,
    BC7Srgb = 99,
};

struct FormatInfo {
    GpuFormat format;
    const char* name;
    uint32_t bytes;    // per pixel, or per 4x4 block when compressed
    bool compressed;
    bool linear;       // sampled values are linear light and need sRGB encoding for display
    bool snorm;        // sampled values are in [-1, 1]
    int channels;      // channels the format stores (1, 2 or 4)
};

const FormatInfo& formatInfo(GpuFormat format);

struct Surface {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t rowPitch = 0;   // bytes per row of pixels, or per row of 4x4 blocks
    std::vector<uint8_t> data;
};

struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    GpuFormat format = GpuFormat::Unknown;
    std::vector<Surface> mips;   // mip 0 first; a single entry means the file had no mip chain
    std::string description;     // e.g. "EXR, half RGBA, PIZ"
    std::string note;            // what the prototype left out, e.g. "cubemap: face 0 of 6 shown"
    float maxValue = 0.0f;       // brightest RGB value for float images, 0 otherwise
};

struct LoadResult {
    bool ok = false;
    std::string error;
    Image image;
};

// `extension` is lowercase without the dot ("exr", "dds"); it decides the decoder.
LoadResult loadImage(const uint8_t* data, size_t size, const std::string& extension);

// Extensions loadImage understands, lowercase without the dot.
const std::vector<std::string>& supportedExtensions();

// Reads one pixel of mip 0 as floats. Returns false for compressed formats.
bool readPixel(const Image& image, uint32_t x, uint32_t y, float rgba[4]);

uint16_t floatToHalf(float value);
float halfToFloat(uint16_t value);

}  // namespace htv
