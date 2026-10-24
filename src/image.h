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

// One frame of an animation (GIF, animated WebP): a full-canvas RGBA8 image and how long it stays.
struct Frame {
    Surface surface;
    int delayMs = 0;
};

// A picture the file holds besides the one on screen: a cubemap face, a texture array or volume slice, a TIFF
// page, an EXR layer or a PSD layer.
struct Layer {
    std::string name;
    uint32_t width = 0;
    uint32_t height = 0;
    GpuFormat format = GpuFormat::Unknown;
    std::vector<Surface> mips;  // empty while this one is on screen (one-at-a-time kinds): Image::mips has them
    float maxValue = 0.0f;
    std::string detail;         // shown after the name when it tells layers apart, e.g. "RGBA half" or "512 × 256"
    // PSD only: the layer's pixels (in the image's format) sit at x, y on the canvas.
    int x = 0;
    int y = 0;
    float opacity = 1.0f;
    bool visible = true;
    bool visibleInFile = true;
    bool group = false;         // a layer group: no pixels, hides or fades the layers inside it
    int parent = -1;            // the group it is in
    int depth = 0;              // how many groups deep
    std::string blend;          // Photoshop's blend key: "norm", "mul ", "pass" (groups)...
    bool clipped = false;       // a clipping mask on the layer below
};

enum class LayerKind {
    None,
    CubeFaces,     // one at a time
    ArraySlices,
    VolumeSlices,
    Pages,
    IconSizes,
    ExrLayers,
    PsdLayers,     // composited: each has its own eye
};

struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    GpuFormat format = GpuFormat::Unknown;
    std::vector<Surface> mips;   // mip 0 first; a single entry means the file had no mip chain
    std::string description;     // e.g. "EXR, half RGBA, PIZ"
    std::string note;            // what the prototype left out, e.g. "cubemap: face 0 of 6 shown"
    float maxValue = 0.0f;       // brightest RGB value for float images, 0 otherwise
    float dpi = 0.0f;            // print resolution the file states, 0 when it states none
    std::vector<Frame> frames;   // more than one: an animation. frame 0's pixels equal mips[0].
    int loopCount = 0;           // 0 loops forever
    std::vector<Layer> layers;   // more than one, or none. Listed top to bottom for PSD.
    LayerKind layerKind = LayerKind::None;
    int shownLayer = 0;          // one-at-a-time kinds: the layer in mips; PSD: unused
    bool composited = false;     // PSD: mips[0] was rebuilt from the layers instead of being the file's own composite
    Surface fileComposite;       // PSD: the file's own composite, for when every layer is back as saved
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

// Why the image looks like a normal map whose Z (blue) was left out to be rebuilt as sqrt(1 - x² - y²),
// or empty when it doesn't. A guess from the format (two channels, e.g. BC5), the file name ("normal",
// "nrm", "_n") and the colours (red and green around 0.5, blue empty: yellow-green instead of the bluish
// tangent-space look). `name` is the file name, lower or upper case.
std::string missingNormalZ(const Image& image, const std::string& name);

// Puts layer `index` on screen for the one-at-a-time kinds: its pixels, size and format move into the image,
// and the previous layer's go back to its entry. False when there is nothing to change.
bool showLayer(Image& image, int index);

// PSD: rebuilds mips[0] from the visible layers, blending each over the ones below as Normal (blend modes,
// clipping and layer effects are not reproduced). False when the image has no PSD layers.
bool compositeLayers(Image& image);

// Whether a PSD layer shows: it and every group around it are visible.
bool layerShows(const Image& image, int index);

// "Cubemap", "Array", "Volume", "Pages", "Sizes", "EXR", "PSD".
const char* layerKindName(LayerKind kind);

uint16_t floatToHalf(float value);
float halfToFloat(uint16_t value);

}  // namespace htv
