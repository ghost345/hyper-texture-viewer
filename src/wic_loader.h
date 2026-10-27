// Fallback decoding through Windows Imaging Component, for formats Windows has codecs for
// (HEIC/HEIF and AVIF with the Store extensions, JPEG XR, TIFF, ICO, camera RAW...).
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "image.h"

namespace htv {

// Extensions worth handing to WIC, lowercase without the dot.
const std::vector<std::string>& wicExtensions();

LoadResult loadWithWic(const std::filesystem::path& path);

}  // namespace htv
