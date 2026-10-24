// Loaders backed by optional libraries. Each is compiled only when CMake finds its library
// (HTV_HAVE_AVIF, HTV_HAVE_JXL, HTV_HAVE_KTX); image.cpp dispatches to them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "image.h"

namespace htv {

LoadResult loadAvif(const uint8_t* data, size_t size);
LoadResult loadJxl(const uint8_t* data, size_t size);
LoadResult loadKtx(const uint8_t* data, size_t size);

// Adds a PSD's layers (8 and 16 bit, RGB and greyscale) to `image`, which holds the file's composite, and its
// print resolution. Leaves the image as it is when the file has no layers it can read.
void readPsdLayers(const uint8_t* data, size_t size, Image& image);

// Metadata for the Info panel (Image::metadata, and dpi): EXIF, ICC profile, resolution and text from JPEG,
// PNG, WebP and PSD files. The pieces are public for loaders that get EXIF or ICC blocks from a library.
void readMetadata(const uint8_t* data, size_t size, const std::string& extension, Image& image);
void readExif(const uint8_t* data, size_t size, Image& image);  // a TIFF structure ("II*" or "MM*")
// "Display P3 (ICC v4.0, RGB)", or empty when the data is not an ICC profile.
std::string iccDescription(const uint8_t* data, size_t size);

// HDR transfer functions shared by the AVIF and JPEG XL loaders.
enum class Transfer { Linear, Srgb, Pq, Hlg };
enum class Primaries { Bt709, Bt2020, DisplayP3 };

// Converts encoded RGB(A) values in [0, 1] (or linear floats for Transfer::Linear) to linear light where
// SDR reference white (203 nits for PQ and HLG) is 1.0, converting BT.2020 or Display P3 primaries to BT.709,
// and writes RGBA16F. Returns the brightest RGB value.
float encodedToLinearHalf(const float* rgba, size_t pixels, Transfer transfer, Primaries primaries, uint16_t* out);

}  // namespace htv
