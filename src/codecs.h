// Loaders backed by optional libraries. Each is compiled only when CMake finds its library
// (HTV_HAVE_AVIF, HTV_HAVE_JXL, HTV_HAVE_KTX); image.cpp dispatches to them.
#pragma once

#include <cstddef>
#include <cstdint>

#include "image.h"

namespace htv {

LoadResult loadAvif(const uint8_t* data, size_t size);
LoadResult loadJxl(const uint8_t* data, size_t size);
LoadResult loadKtx(const uint8_t* data, size_t size);

// HDR transfer functions shared by the AVIF and JPEG XL loaders.
enum class Transfer { Linear, Srgb, Pq, Hlg };

// Converts encoded RGB(A) values in [0, 1] (or linear floats for Transfer::Linear) to linear light where
// SDR reference white (203 nits for PQ and HLG) is 1.0, optionally converting BT.2020 primaries to BT.709,
// and writes RGBA16F. Returns the brightest RGB value.
float encodedToLinearHalf(const float* rgba, size_t pixels, Transfer transfer, bool bt2020, uint16_t* out);

}  // namespace htv
