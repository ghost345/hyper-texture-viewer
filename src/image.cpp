#include "image.h"

#include "codecs.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

// The third-party decoders are compiled here, once.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define TINYEXR_IMPLEMENTATION
#define TINYEXR_USE_MINIZ 0
#define TINYEXR_USE_STB_ZLIB 1
#define TINYEXR_USE_OPENMP 0
#define TINYEXR_USE_THREAD 1
#include "tinyexr.h"

#define QOI_IMPLEMENTATION
#define QOI_NO_STDIO
#include "qoi.h"

#include <webp/decode.h>
#include <webp/demux.h>

namespace htv {

namespace {

const FormatInfo kFormats[] = {
    {GpuFormat::RGBA32F, "RGBA32F", 16, false, true, false, 4},
    {GpuFormat::RGBA16F, "RGBA16F", 8, false, true, false, 4},
    {GpuFormat::RGBA16Unorm, "RGBA16", 8, false, false, false, 4},
    {GpuFormat::RG32F, "RG32F", 8, false, true, false, 2},
    {GpuFormat::RGB10A2, "RGB10A2", 4, false, false, false, 4},
    {GpuFormat::RG11B10F, "RG11B10F", 4, false, true, false, 4},
    {GpuFormat::RGBA8, "RGBA8", 4, false, false, false, 4},
    {GpuFormat::RGBA8Srgb, "RGBA8 sRGB", 4, false, true, false, 4},
    {GpuFormat::RG16F, "RG16F", 4, false, true, false, 2},
    {GpuFormat::RG16Unorm, "RG16", 4, false, false, false, 2},
    {GpuFormat::R32F, "R32F", 4, false, true, false, 1},
    {GpuFormat::RG8, "RG8", 2, false, false, false, 2},
    {GpuFormat::R16F, "R16F", 2, false, true, false, 1},
    {GpuFormat::R16Unorm, "R16", 2, false, false, false, 1},
    {GpuFormat::R8, "R8", 1, false, false, false, 1},
    {GpuFormat::BC1, "BC1", 8, true, false, false, 4},
    {GpuFormat::BC1Srgb, "BC1 sRGB", 8, true, true, false, 4},
    {GpuFormat::BC2, "BC2", 16, true, false, false, 4},
    {GpuFormat::BC2Srgb, "BC2 sRGB", 16, true, true, false, 4},
    {GpuFormat::BC3, "BC3", 16, true, false, false, 4},
    {GpuFormat::BC3Srgb, "BC3 sRGB", 16, true, true, false, 4},
    {GpuFormat::BC4, "BC4", 8, true, false, false, 1},
    {GpuFormat::BC4Snorm, "BC4 snorm", 8, true, false, true, 1},
    {GpuFormat::BC5, "BC5", 16, true, false, false, 2},
    {GpuFormat::BC5Snorm, "BC5 snorm", 16, true, false, true, 2},
    {GpuFormat::BGRA8, "BGRA8", 4, false, false, false, 4},
    {GpuFormat::BGRX8, "BGRX8", 4, false, false, false, 4},
    {GpuFormat::BGRA8Srgb, "BGRA8 sRGB", 4, false, true, false, 4},
    {GpuFormat::BC6HUf16, "BC6H", 16, true, true, false, 4},
    {GpuFormat::BC6HSf16, "BC6H signed", 16, true, true, false, 4},
    {GpuFormat::BC7, "BC7", 16, true, false, false, 4},
    {GpuFormat::BC7Srgb, "BC7 sRGB", 16, true, true, false, 4},
};

const FormatInfo kUnknownFormat = {GpuFormat::Unknown, "unknown", 0, false, false, false, 0};

bool isKnown(uint32_t dxgi) {
    for (const FormatInfo& f : kFormats)
        if (static_cast<uint32_t>(f.format) == dxgi) return true;
    return false;
}

Surface makeSurface(const FormatInfo& info, uint32_t width, uint32_t height) {
    Surface s;
    s.width = width;
    s.height = height;
    if (info.compressed) {
        s.rowPitch = std::max(1u, (width + 3) / 4) * info.bytes;
        s.data.resize(size_t(s.rowPitch) * std::max(1u, (height + 3) / 4));
    } else {
        s.rowPitch = width * info.bytes;
        s.data.resize(size_t(s.rowPitch) * height);
    }
    return s;
}

float maxRgb(const float* rgba, size_t pixels) {
    float m = 0.0f;
    for (size_t i = 0; i < pixels; ++i)
        for (int c = 0; c < 3; ++c) {
            float v = rgba[i * 4 + c];
            if (std::isfinite(v)) m = std::max(m, v);
        }
    return m;
}

// ---- stb_image: PNG, JPEG, TGA, BMP, PSD (composite), GIF (first frame), HDR ----

LoadResult loadStb(const uint8_t* data, size_t size, const char* label) {
    LoadResult r;
    int w = 0, h = 0, n = 0;
    const int len = static_cast<int>(size);
    if (!stbi_info_from_memory(data, len, &w, &h, &n)) {
        r.error = std::string("cannot decode ") + label + ": " + stbi_failure_reason();
        return r;
    }
    Image& img = r.image;
    img.width = uint32_t(w);
    img.height = uint32_t(h);
    const char* channelNames[] = {"", "gray", "gray+alpha", "RGB", "RGBA"};

    if (stbi_is_hdr_from_memory(data, len)) {
        float* px = stbi_loadf_from_memory(data, len, &w, &h, &n, 4);
        if (!px) {
            r.error = std::string("cannot decode ") + label + ": " + stbi_failure_reason();
            return r;
        }
        img.format = GpuFormat::RGBA16F;
        Surface s = makeSurface(formatInfo(img.format), img.width, img.height);
        auto* out = reinterpret_cast<uint16_t*>(s.data.data());
        const size_t count = size_t(w) * h * 4;
        for (size_t i = 0; i < count; ++i) out[i] = floatToHalf(px[i]);
        img.maxValue = maxRgb(px, size_t(w) * h);
        stbi_image_free(px);
        img.mips.push_back(std::move(s));
        img.description = std::string(label) + ", float RGB";
    } else if (stbi_is_16_bit_from_memory(data, len)) {
        stbi_us* px = stbi_load_16_from_memory(data, len, &w, &h, &n, 4);
        if (!px) {
            r.error = std::string("cannot decode ") + label + ": " + stbi_failure_reason();
            return r;
        }
        img.format = GpuFormat::RGBA16Unorm;
        Surface s = makeSurface(formatInfo(img.format), img.width, img.height);
        std::memcpy(s.data.data(), px, s.data.size());
        stbi_image_free(px);
        img.mips.push_back(std::move(s));
        img.description = std::string(label) + ", 16 bit " + channelNames[n];
    } else {
        stbi_uc* px = stbi_load_from_memory(data, len, &w, &h, &n, 4);
        if (!px) {
            r.error = std::string("cannot decode ") + label + ": " + stbi_failure_reason();
            return r;
        }
        img.format = GpuFormat::RGBA8Srgb;
        Surface s = makeSurface(formatInfo(img.format), img.width, img.height);
        std::memcpy(s.data.data(), px, s.data.size());
        stbi_image_free(px);
        img.mips.push_back(std::move(s));
        img.description = std::string(label) + ", 8 bit " + channelNames[n];
    }
    r.ok = true;
    return r;
}

// ---- OpenEXR via tinyexr (prototype; OpenEXR proper adds DWAA/DWAB) ----

// Compression ids from the OpenEXR file format; tinyexr 1.0 decodes 0-4 only.
const char* exrCompressionName(int c) {
    static const char* names[] = {"uncompressed", "RLE", "ZIPS", "ZIP", "PIZ", "PXR24", "B44", "B44A", "DWAA", "DWAB"};
    return c >= 0 && c < 10 ? names[c] : "unknown compression";
}

LoadResult loadExr(const uint8_t* data, size_t size) {
    LoadResult r;
    EXRVersion version;
    if (ParseEXRVersionFromMemory(&version, data, size) != TINYEXR_SUCCESS) {
        r.error = "not a valid EXR file";
        return r;
    }
    if (version.multipart) {
        r.error = "multipart EXR is not supported in the prototype yet";
        return r;
    }

    EXRHeader header;
    InitEXRHeader(&header);
    const char* err = nullptr;
    if (ParseEXRHeaderFromMemory(&header, &version, data, size, &err) != TINYEXR_SUCCESS) {
        r.error = std::string("cannot read EXR header: ") + (err ? err : "");
        FreeEXRErrorMessage(err);
        return r;
    }
    struct HeaderGuard {
        EXRHeader* h;
        ~HeaderGuard() { FreeEXRHeader(h); }
    } headerGuard{&header};

    bool anyFloat = false, anyHalf = false;
    for (int i = 0; i < header.num_channels; ++i) {
        anyFloat |= header.pixel_types[i] != TINYEXR_PIXELTYPE_HALF;
        anyHalf |= header.pixel_types[i] == TINYEXR_PIXELTYPE_HALF;
    }
    std::string channelList;
    for (int i = 0; i < header.num_channels && i < 8; ++i) {
        if (i) channelList += ",";
        channelList += header.channels[i].name;
    }
    if (header.num_channels > 8) channelList += ",...";
    const int compression = header.compression_type;
    if (compression >= 5 && compression <= 9) {
        r.error = std::string("EXR ") + exrCompressionName(compression) +
                  " compression needs the OpenEXR library (planned)";
        return r;
    }
    if (header.tiled) {
        r.error = "tiled EXR is not supported in the prototype yet";
        return r;
    }

    // Half data stays half all the way to the GPU; any 32-bit channel keeps the whole image at full precision.
    for (int i = 0; i < header.num_channels; ++i)
        header.requested_pixel_types[i] =
            header.pixel_types[i] == TINYEXR_PIXELTYPE_HALF && !anyFloat ? TINYEXR_PIXELTYPE_HALF
            : header.pixel_types[i] == TINYEXR_PIXELTYPE_UINT            ? TINYEXR_PIXELTYPE_UINT
                                                                         : TINYEXR_PIXELTYPE_FLOAT;

    EXRImage exr;
    InitEXRImage(&exr);
    if (LoadEXRImageFromMemory(&exr, &header, data, size, &err) != TINYEXR_SUCCESS) {
        r.error = std::string("cannot decode EXR: ") + (err ? err : "");
        FreeEXRErrorMessage(err);
        return r;
    }
    struct ImageGuard {
        EXRImage* i;
        ~ImageGuard() { FreeEXRImage(i); }
    } imageGuard{&exr};

    // Channels group into layers by the name before the last dot: "R", "G", "B", "A" are the main layer,
    // "diffuse.R" ... another, "depth.Z" one more.
    struct ExrLayer {
        std::string name;
        std::vector<int> channels;
    };
    std::vector<ExrLayer> groups;
    for (int i = 0; i < header.num_channels; ++i) {
        const std::string n = header.channels[i].name;
        const size_t dot = n.rfind('.');
        const std::string prefix = dot == std::string::npos ? std::string() : n.substr(0, dot);
        auto it = std::find_if(groups.begin(), groups.end(), [&](const ExrLayer& g) { return g.name == prefix; });
        if (it == groups.end()) {
            groups.push_back({prefix, {}});
            it = groups.end() - 1;
        }
        it->channels.push_back(i);
    }
    std::stable_partition(groups.begin(), groups.end(), [](const ExrLayer& g) { return g.name.empty(); });
    auto suffix = [&](int ch) {
        const char* n = header.channels[ch].name;
        const char* dot = std::strrchr(n, '.');
        return std::string(dot ? dot + 1 : n);
    };

    const size_t pixels = size_t(exr.width) * exr.height;
    const GpuFormat format = anyFloat ? GpuFormat::RGBA32F : GpuFormat::RGBA16F;
    // One layer's channels as RGBA: R, G, B, A by name (or X, Y, Z), a lone channel as grey, otherwise the
    // first three. `detail` names what went where.
    auto build = [&](const ExrLayer& g, float& maxValue, std::string& detail) {
        int src[4] = {-1, -1, -1, -1};
        auto find = [&](const char* names) {
            for (int c : g.channels) {
                const std::string sfx = suffix(c);
                if (sfx.size() == 1 && std::toupper(static_cast<unsigned char>(sfx[0])) == names[0]) return c;
            }
            return -1;
        };
        const bool gray = g.channels.size() == 1 ||
                          (g.channels.size() == 2 && find("A") >= 0 && find("R") < 0 && find("G") < 0 && find("B") < 0);
        if (gray) {
            src[0] = src[1] = src[2] = g.channels[0] == find("A") ? g.channels[1] : g.channels[0];
            src[3] = g.channels.size() == 2 ? find("A") : -1;
        } else {
            for (int c = 0; c < 4; ++c) src[c] = find(&"RGBA"[c]);
            if (src[0] < 0 && src[1] < 0 && src[2] < 0)
                for (int c = 0; c < 3; ++c) src[c] = find(&"XYZ"[c]);
            if (src[0] < 0 && src[1] < 0 && src[2] < 0)
                for (size_t c = 0; c < 3 && c < g.channels.size(); ++c) src[c] = g.channels[c];
        }
        detail.clear();
        for (int c : g.channels) detail += (detail.empty() ? "" : " ") + suffix(c);
        if (gray) detail += " (grey)";

        Surface s = makeSurface(formatInfo(format), uint32_t(exr.width), uint32_t(exr.height));
        maxValue = 0.0f;
        if (anyFloat) {
            auto* out = reinterpret_cast<float*>(s.data.data());
            for (int c = 0; c < 4; ++c) {
                const int ch = src[c];
                if (ch < 0) {
                    for (size_t i = 0; i < pixels; ++i) out[i * 4 + c] = c == 3 ? 1.0f : 0.0f;
                    continue;
                }
                const bool isUint = header.requested_pixel_types[ch] == TINYEXR_PIXELTYPE_UINT;
                const auto* f = reinterpret_cast<const float*>(exr.images[ch]);
                const auto* u = reinterpret_cast<const uint32_t*>(exr.images[ch]);
                for (size_t i = 0; i < pixels; ++i) {
                    const float v = isUint ? float(u[i]) : f[i];
                    out[i * 4 + c] = v;
                    if (c < 3 && std::isfinite(v)) maxValue = std::max(maxValue, v);
                }
            }
        } else {
            auto* out = reinterpret_cast<uint16_t*>(s.data.data());
            for (int c = 0; c < 4; ++c) {
                const int ch = src[c];
                if (ch < 0) {
                    for (size_t i = 0; i < pixels; ++i) out[i * 4 + c] = c == 3 ? 0x3c00 : 0;
                    continue;
                }
                const auto* hsrc = reinterpret_cast<const uint16_t*>(exr.images[ch]);
                uint16_t brightest = 0;  // for positive finite halves, the bit pattern orders like the value
                for (size_t i = 0; i < pixels; ++i) {
                    const uint16_t v = hsrc[i];
                    out[i * 4 + c] = v;
                    if (c < 3 && v < 0x7c00 && v > brightest) brightest = v;
                }
                if (c < 3) maxValue = std::max(maxValue, halfToFloat(brightest));
            }
        }
        return s;
    };

    Image& img = r.image;
    img.width = uint32_t(exr.width);
    img.height = uint32_t(exr.height);
    img.format = format;
    std::string detail;
    img.mips.push_back(build(groups[0], img.maxValue, detail));
    if (groups.size() > 1) {
        img.layerKind = LayerKind::ExrLayers;
        for (size_t i = 0; i < groups.size(); ++i) {
            Layer l;
            l.width = img.width;
            l.height = img.height;
            l.format = format;
            if (i == 0) {
                l.maxValue = img.maxValue;
                l.detail = detail;
            } else {
                l.mips.push_back(build(groups[i], l.maxValue, l.detail));
            }
            l.name = groups[i].name.empty() ? "Main" : groups[i].name;
            img.layers.push_back(std::move(l));
        }
    }
    img.description = std::string("EXR, ") + (anyFloat ? (anyHalf ? "half/float " : "float ") : "half ") +
                      channelList + ", " + exrCompressionName(compression);
    if (groups.size() == 1 && detail.find("(grey)") != std::string::npos)
        img.note = "no RGB channels: the channel is shown as grey";
    r.ok = true;
    return r;
}

// ---- WebP via libwebp; animations show their first frame ----

LoadResult loadWebp(const uint8_t* data, size_t size) {
    LoadResult r;
    WebPBitstreamFeatures features;
    if (WebPGetFeatures(data, size, &features) != VP8_STATUS_OK) {
        r.error = "not a valid WebP file";
        return r;
    }
    Image& img = r.image;
    img.format = GpuFormat::RGBA8Srgb;
    const FormatInfo& info = formatInfo(img.format);

    if (features.has_animation) {
        WebPData webpData{data, size};
        WebPAnimDecoderOptions options;
        WebPAnimDecoderOptionsInit(&options);
        options.color_mode = MODE_RGBA;
        WebPAnimDecoder* decoder = WebPAnimDecoderNew(&webpData, &options);
        WebPAnimInfo anim;
        uint8_t* frame = nullptr;
        int timestamp = 0;
        if (!decoder || !WebPAnimDecoderGetInfo(decoder, &anim) || !WebPAnimDecoderGetNext(decoder, &frame, &timestamp)) {
            WebPAnimDecoderDelete(decoder);
            r.error = "cannot decode animated WebP";
            return r;
        }
        img.width = anim.canvas_width;
        img.height = anim.canvas_height;
        img.loopCount = int(anim.loop_count);
        int prev = 0;
        do {
            Frame f;
            f.surface = makeSurface(info, img.width, img.height);
            std::memcpy(f.surface.data.data(), frame, f.surface.data.size());
            f.delayMs = std::max(timestamp - prev, 0);
            prev = timestamp;
            img.frames.push_back(std::move(f));
        } while (WebPAnimDecoderGetNext(decoder, &frame, &timestamp));
        WebPAnimDecoderDelete(decoder);
        img.mips.push_back(img.frames[0].surface);
        if (img.frames.size() > 1) img.description = "";  // frame count goes in the description below
        else img.frames.clear();
    } else {
        img.width = uint32_t(features.width);
        img.height = uint32_t(features.height);
        Surface s = makeSurface(info, img.width, img.height);
        if (!WebPDecodeRGBAInto(data, size, s.data.data(), s.data.size(), int(s.rowPitch))) {
            r.error = "cannot decode WebP";
            return r;
        }
        img.mips.push_back(std::move(s));
    }
    const char* kind = features.format == 2 ? "lossless" : features.format == 1 ? "lossy" : "mixed";
    img.description = std::string("WebP, ") + kind + (features.has_alpha ? " RGBA" : " RGB");
    if (img.frames.size() > 1) img.description += ", " + std::to_string(img.frames.size()) + " frames";
    r.ok = true;
    return r;
}

// ---- QOI ----

LoadResult loadQoi(const uint8_t* data, size_t size) {
    LoadResult r;
    qoi_desc desc;
    void* px = qoi_decode(data, int(size), &desc, 4);
    if (!px) {
        r.error = "not a valid QOI file";
        return r;
    }
    Image& img = r.image;
    img.width = desc.width;
    img.height = desc.height;
    // QOI flags linear data explicitly; everything else is sRGB.
    img.format = desc.colorspace == QOI_LINEAR ? GpuFormat::RGBA8 : GpuFormat::RGBA8Srgb;
    Surface s = makeSurface(formatInfo(img.format), img.width, img.height);
    std::memcpy(s.data.data(), px, s.data.size());
    free(px);
    img.mips.push_back(std::move(s));
    img.description = std::string("QOI, ") + (desc.channels == 4 ? "RGBA" : "RGB") +
                      (desc.colorspace == QOI_LINEAR ? ", linear" : "");
    r.ok = true;
    return r;
}

// ---- DDS: compressed data goes to the GPU untouched ----

uint32_t readU32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

constexpr uint32_t fourCC(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) |
           (uint32_t(uint8_t(d)) << 24);
}

LoadResult loadDds(const uint8_t* data, size_t size) {
    LoadResult r;
    if (size < 128 || readU32(data) != fourCC('D', 'D', 'S', ' ') || readU32(data + 4) != 124) {
        r.error = "not a valid DDS file";
        return r;
    }
    const uint8_t* hdr = data + 4;
    const uint32_t height = readU32(hdr + 8);
    const uint32_t width = readU32(hdr + 12);
    const uint32_t depth = readU32(hdr + 20);
    const uint32_t mipCount = std::max(1u, readU32(hdr + 24));
    const uint32_t pfFlags = readU32(hdr + 76);
    const uint32_t pfFourCC = readU32(hdr + 80);
    const uint32_t bitCount = readU32(hdr + 84);
    const uint32_t rMask = readU32(hdr + 88), gMask = readU32(hdr + 92), bMask = readU32(hdr + 96),
                   aMask = readU32(hdr + 100);
    const uint32_t caps2 = readU32(hdr + 108);

    constexpr uint32_t kFourCCFlag = 0x4, kRgbFlag = 0x40, kAlphaPixels = 0x1, kLuminance = 0x20000;
    size_t offset = 128;
    uint32_t dxgi = 0;
    uint32_t faces = 1;
    bool expandRgb24 = false;
    bool dxgiCube = false;
    bool volume = (caps2 & 0x200000) != 0 && depth > 1;

    if ((pfFlags & kFourCCFlag) && pfFourCC == fourCC('D', 'X', '1', '0')) {
        if (size < 148) {
            r.error = "truncated DDS header";
            return r;
        }
        dxgi = readU32(data + 128);
        const uint32_t dimension = readU32(data + 132);
        const uint32_t misc = readU32(data + 136);
        dxgiCube = (misc & 0x4) != 0;
        faces = std::max(1u, readU32(data + 140)) * (dxgiCube ? 6 : 1);
        volume = dimension == 4;
        offset = 148;
    } else if (pfFlags & kFourCCFlag) {
        switch (pfFourCC) {
            case fourCC('D', 'X', 'T', '1'): dxgi = uint32_t(GpuFormat::BC1); break;
            case fourCC('D', 'X', 'T', '2'):
            case fourCC('D', 'X', 'T', '3'): dxgi = uint32_t(GpuFormat::BC2); break;
            case fourCC('D', 'X', 'T', '4'):
            case fourCC('D', 'X', 'T', '5'): dxgi = uint32_t(GpuFormat::BC3); break;
            case fourCC('A', 'T', 'I', '1'):
            case fourCC('B', 'C', '4', 'U'): dxgi = uint32_t(GpuFormat::BC4); break;
            case fourCC('B', 'C', '4', 'S'): dxgi = uint32_t(GpuFormat::BC4Snorm); break;
            case fourCC('A', 'T', 'I', '2'):
            case fourCC('B', 'C', '5', 'U'): dxgi = uint32_t(GpuFormat::BC5); break;
            case fourCC('B', 'C', '5', 'S'): dxgi = uint32_t(GpuFormat::BC5Snorm); break;
            case 36: dxgi = uint32_t(GpuFormat::RGBA16Unorm); break;
            case 111: dxgi = uint32_t(GpuFormat::R16F); break;
            case 112: dxgi = uint32_t(GpuFormat::RG16F); break;
            case 113: dxgi = uint32_t(GpuFormat::RGBA16F); break;
            case 114: dxgi = uint32_t(GpuFormat::R32F); break;
            case 115: dxgi = uint32_t(GpuFormat::RG32F); break;
            case 116: dxgi = uint32_t(GpuFormat::RGBA32F); break;
            default: break;
        }
        if (caps2 & 0x200) faces = 6;
    } else if ((pfFlags & kRgbFlag) && bitCount == 32) {
        if (rMask == 0xff && gMask == 0xff00 && bMask == 0xff0000)
            dxgi = uint32_t(GpuFormat::RGBA8);
        else if (rMask == 0xff0000 && gMask == 0xff00 && bMask == 0xff)
            dxgi = uint32_t((pfFlags & kAlphaPixels) && aMask ? GpuFormat::BGRA8 : GpuFormat::BGRX8);
        else if (rMask == 0x3ff && gMask == 0xffc00 && bMask == 0x3ff00000)
            dxgi = uint32_t(GpuFormat::RGB10A2);
        else if (rMask == 0xffff && gMask == 0xffff0000)
            dxgi = uint32_t(GpuFormat::RG16Unorm);
        if (caps2 & 0x200) faces = 6;
    } else if ((pfFlags & kRgbFlag) && bitCount == 24 && rMask == 0xff0000 && gMask == 0xff00 && bMask == 0xff) {
        dxgi = uint32_t(GpuFormat::RGBA8);
        expandRgb24 = true;
        if (caps2 & 0x200) faces = 6;
    } else if ((pfFlags & kLuminance) && bitCount == 8) {
        dxgi = uint32_t(GpuFormat::R8);
    } else if ((pfFlags & kLuminance) && bitCount == 16 && rMask == 0xffff) {
        dxgi = uint32_t(GpuFormat::R16Unorm);
    }

    if (!dxgi || !isKnown(dxgi)) {
        r.error = "unsupported DDS pixel format" + (dxgi ? " (DXGI " + std::to_string(dxgi) + ")" : std::string());
        return r;
    }
    if (width == 0 || height == 0 || width > 65536 || height > 65536) {
        r.error = "invalid DDS dimensions";
        return r;
    }

    Image& img = r.image;
    img.width = width;
    img.height = height;
    img.format = GpuFormat(dxgi);
    const FormatInfo& info = formatInfo(img.format);

    // Reads one surface at `offset`; false when the file ends before it.
    auto readSurface = [&](uint32_t w, uint32_t h, Surface& s) {
        s = makeSurface(info, w, h);
        const size_t srcPitch = expandRgb24 ? size_t(w) * 3 : s.rowPitch;
        const size_t rows = info.compressed ? std::max(1u, (h + 3) / 4) : h;
        const size_t srcSize = srcPitch * rows;
        if (offset + srcSize > size) return false;
        if (expandRgb24) {
            const uint8_t* src = data + offset;
            for (size_t i = 0; i < size_t(w) * h; ++i) {
                s.data[i * 4 + 0] = src[i * 3 + 2];
                s.data[i * 4 + 1] = src[i * 3 + 1];
                s.data[i * 4 + 2] = src[i * 3 + 0];
                s.data[i * 4 + 3] = 255;
            }
        } else {
            std::memcpy(s.data.data(), data + offset, srcSize);
        }
        offset += srcSize;
        return true;
    };
    auto maxOf = [&](const std::vector<Surface>& mips) {
        if (img.format != GpuFormat::RGBA16F && img.format != GpuFormat::RGBA32F) return 0.0f;
        Image probe;
        probe.width = width;
        probe.height = height;
        probe.format = img.format;
        probe.mips.push_back(mips[0]);
        float m = 0.0f, px[4];
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
                if (readPixel(probe, x, y, px))
                    for (int c = 0; c < 3; ++c)
                        if (std::isfinite(px[c])) m = std::max(m, px[c]);
        return m;
    };

    // Each face or array slice has its whole mip chain in turn. A volume has, per mip level, all of that level's
    // depth slices; only level 0's slices are read, and the GPU makes their mips.
    std::vector<std::vector<Surface>> pictures;
    if (volume) {
        for (uint32_t z = 0; z < std::min(depth, 2048u); ++z) {
            Surface s;
            if (!readSurface(width, height, s)) break;
            pictures.push_back({std::move(s)});
        }
    } else {
        const uint32_t levels = std::min(mipCount, 17u);
        for (uint32_t face = 0; face < std::min(faces, 2048u); ++face) {
            std::vector<Surface> mips;
            for (uint32_t level = 0; level < levels; ++level) {
                Surface s;
                if (!readSurface(std::max(1u, width >> level), std::max(1u, height >> level), s)) break;
                mips.push_back(std::move(s));
            }
            if (mips.empty()) break;
            const bool complete = mips.size() == levels;
            pictures.push_back(std::move(mips));
            if (!complete) break;  // the file ends here: keep the levels that are there
        }
    }
    if (pictures.empty()) {
        r.error = "truncated DDS data";
        return r;
    }

    const size_t count = pictures.size();
    if (count > 1) {
        static const char* kFaceNames[6] = {"+X", "\xE2\x88\x92X", "+Y", "\xE2\x88\x92Y", "+Z", "\xE2\x88\x92Z"};
        const bool cube = !volume && faces % 6 == 0 && (faces == 6 || (caps2 & 0x200) || dxgiCube);
        img.layerKind = volume ? LayerKind::VolumeSlices : cube ? LayerKind::CubeFaces : LayerKind::ArraySlices;
        for (size_t i = 0; i < count; ++i) {
            Layer l;
            if (cube && faces > 6) l.name = "Cube " + std::to_string(i / 6) + " " + kFaceNames[i % 6];
            else if (cube) l.name = kFaceNames[i % 6];
            else l.name = "Slice " + std::to_string(i);
            l.width = width;
            l.height = height;
            l.format = img.format;
            l.maxValue = maxOf(pictures[i]);
            if (i > 0) l.mips = std::move(pictures[i]);
            img.layers.push_back(std::move(l));
        }
        img.maxValue = img.layers[0].maxValue;
    } else {
        img.maxValue = maxOf(pictures[0]);
    }
    img.mips = std::move(pictures[0]);

    img.description = std::string("DDS, ") + info.name + ", " + std::to_string(img.mips.size()) +
                      (img.mips.size() == 1 ? " mip" : " mips");
    const uint32_t expected = volume ? depth : faces;
    if (count < expected) img.note = std::to_string(count) + " of " + std::to_string(expected) + " " +
                                     (volume ? "slices" : faces % 6 == 0 ? "faces" : "slices") + " in the file";
    r.ok = true;
    return r;
}

std::string upper(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = char(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

}  // namespace

const FormatInfo& formatInfo(GpuFormat format) {
    for (const FormatInfo& f : kFormats)
        if (f.format == format) return f;
    return kUnknownFormat;
}

const std::vector<std::string>& supportedExtensions() {
    static const std::vector<std::string> list = [] {
        std::vector<std::string> l = {"exr", "dds", "hdr", "png", "jpg", "jpeg", "webp", "qoi", "tga",
                                      "bmp", "psd", "gif", "pic", "pnm", "ppm", "pgm"};
#ifdef HTV_HAVE_AVIF
        l.push_back("avif");
#endif
#ifdef HTV_HAVE_JXL
        l.push_back("jxl");
#endif
#ifdef HTV_HAVE_KTX
        l.push_back("ktx");
        l.push_back("ktx2");
#endif
        return l;
    }();
    return list;
}

// ---- GIF: every frame, through stb_image's animation API ----

LoadResult loadGif(const uint8_t* data, size_t size) {
    LoadResult r;
    int w = 0, h = 0, count = 0, comp = 0;
    int* delays = nullptr;
    stbi_uc* px = stbi_load_gif_from_memory(data, int(size), &delays, &w, &h, &count, &comp, 4);
    if (!px || count < 1) {
        if (px) stbi_image_free(px);
        if (delays) STBI_FREE(delays);
        r.error = std::string("cannot decode GIF: ") + stbi_failure_reason();
        return r;
    }
    Image& img = r.image;
    img.width = uint32_t(w);
    img.height = uint32_t(h);
    img.format = GpuFormat::RGBA8Srgb;
    const FormatInfo& info = formatInfo(img.format);
    const size_t frameBytes = size_t(w) * h * 4;
    for (int i = 0; i < count; ++i) {
        Frame f;
        f.surface = makeSurface(info, img.width, img.height);
        std::memcpy(f.surface.data.data(), px + size_t(i) * frameBytes, f.surface.data.size());
        f.delayMs = delays ? std::max(delays[i], 0) : 0;
        img.frames.push_back(std::move(f));
    }
    stbi_image_free(px);
    if (delays) STBI_FREE(delays);
    img.mips.push_back(img.frames[0].surface);
    img.description = count > 1 ? "GIF, 8 bit RGBA, " + std::to_string(count) + " frames" : "GIF, 8 bit RGBA";
    if (count == 1) img.frames.clear();
    r.ok = true;
    return r;
}

LoadResult loadImage(const uint8_t* data, size_t size, const std::string& extension) {
    LoadResult r;
    if (!data || size == 0) {
        r.error = "empty file";
        return r;
    }
    if (size > size_t(0x7fffffff)) {
        r.error = "files over 2 GB are not supported yet";
        return r;
    }
    if (extension == "exr") return loadExr(data, size);
    if (extension == "dds") return loadDds(data, size);
    if (extension == "webp") return loadWebp(data, size);
    if (extension == "qoi") return loadQoi(data, size);
    if (extension == "avif") return loadAvif(data, size);
    if (extension == "jxl") return loadJxl(data, size);
    if (extension == "ktx" || extension == "ktx2") return loadKtx(data, size);
    if (extension == "gif") return loadGif(data, size);
    r = loadStb(data, size, upper(extension == "jpeg" ? "jpg" : extension).c_str());
    if (r.ok && extension == "psd") readPsdLayers(data, size, r.image);
    return r;
}

bool readPixel(const Image& image, uint32_t x, uint32_t y, float rgba[4]) {
    if (image.mips.empty() || x >= image.width || y >= image.height) return false;
    const FormatInfo& info = formatInfo(image.format);
    if (info.compressed) return false;
    const Surface& s = image.mips[0];
    const uint8_t* p = s.data.data() + size_t(y) * s.rowPitch + size_t(x) * info.bytes;
    rgba[0] = rgba[1] = rgba[2] = 0.0f;
    rgba[3] = 1.0f;
    auto u16 = [&](int i) {
        uint16_t v;
        std::memcpy(&v, p + i * 2, 2);
        return v;
    };
    auto f32 = [&](int i) {
        float v;
        std::memcpy(&v, p + i * 4, 4);
        return v;
    };
    switch (image.format) {
        case GpuFormat::RGBA32F: for (int i = 0; i < 4; ++i) rgba[i] = f32(i); break;
        case GpuFormat::RG32F: rgba[0] = f32(0); rgba[1] = f32(1); break;
        case GpuFormat::R32F: rgba[0] = rgba[1] = rgba[2] = f32(0); break;
        case GpuFormat::RGBA16F: for (int i = 0; i < 4; ++i) rgba[i] = halfToFloat(u16(i)); break;
        case GpuFormat::RG16F: rgba[0] = halfToFloat(u16(0)); rgba[1] = halfToFloat(u16(1)); break;
        case GpuFormat::R16F: rgba[0] = rgba[1] = rgba[2] = halfToFloat(u16(0)); break;
        case GpuFormat::RGBA16Unorm: for (int i = 0; i < 4; ++i) rgba[i] = u16(i) / 65535.0f; break;
        case GpuFormat::RG16Unorm: rgba[0] = u16(0) / 65535.0f; rgba[1] = u16(1) / 65535.0f; break;
        case GpuFormat::R16Unorm: rgba[0] = rgba[1] = rgba[2] = u16(0) / 65535.0f; break;
        case GpuFormat::RGBA8:
        case GpuFormat::RGBA8Srgb: for (int i = 0; i < 4; ++i) rgba[i] = p[i] / 255.0f; break;
        case GpuFormat::BGRA8:
        case GpuFormat::BGRA8Srgb:
        case GpuFormat::BGRX8:
            rgba[0] = p[2] / 255.0f;
            rgba[1] = p[1] / 255.0f;
            rgba[2] = p[0] / 255.0f;
            rgba[3] = image.format == GpuFormat::BGRX8 ? 1.0f : p[3] / 255.0f;
            break;
        case GpuFormat::RG8: rgba[0] = p[0] / 255.0f; rgba[1] = p[1] / 255.0f; break;
        case GpuFormat::R8: rgba[0] = rgba[1] = rgba[2] = p[0] / 255.0f; break;
        case GpuFormat::RGB10A2: {
            const uint32_t v = readU32(p);
            rgba[0] = (v & 0x3ff) / 1023.0f;
            rgba[1] = ((v >> 10) & 0x3ff) / 1023.0f;
            rgba[2] = ((v >> 20) & 0x3ff) / 1023.0f;
            rgba[3] = (v >> 30) / 3.0f;
            break;
        }
        default: return false;
    }
    return true;
}

uint16_t floatToHalf(float value) {
    uint32_t x;
    std::memcpy(&x, &value, 4);
    const uint16_t sign = uint16_t((x >> 16) & 0x8000);
    uint32_t mant = x & 0x7fffff;
    const int exp = int((x >> 23) & 0xff);
    if (exp == 255) return uint16_t(sign | 0x7c00 | (mant ? 0x200 : 0));
    const int e = exp - 127 + 15;
    if (e >= 31) return uint16_t(sign | 0x7c00);
    if (e <= 0) {
        if (e < -10) return sign;
        mant |= 0x800000;
        const int shift = 14 - e;
        uint32_t h = mant >> shift;
        const uint32_t rem = mant & ((1u << shift) - 1), halfway = 1u << (shift - 1);
        if (rem > halfway || (rem == halfway && (h & 1))) ++h;
        return uint16_t(sign | h);
    }
    uint32_t h = (uint32_t(e) << 10) | (mant >> 13);
    const uint32_t rem = mant & 0x1fff;
    if (rem > 0x1000 || (rem == 0x1000 && (h & 1))) ++h;  // a carry into the exponent is the correct rounding
    return uint16_t(sign | h);
}

float halfToFloat(uint16_t value) {
    const uint32_t sign = uint32_t(value & 0x8000) << 16;
    uint32_t exp = (value >> 10) & 0x1f;
    uint32_t mant = value & 0x3ff;
    uint32_t x;
    if (exp == 0) {
        if (mant == 0) {
            x = sign;
        } else {
            int e = 1;
            while (!(mant & 0x400)) {
                mant <<= 1;
                --e;
            }
            mant &= 0x3ff;
            x = sign | (uint32_t(e + 112) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        x = sign | 0x7f800000 | (mant << 13);
    } else {
        x = sign | ((exp + 112) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}

std::string missingNormalZ(const Image& image, const std::string& name) {
    if (!image.width || image.mips.empty()) return {};
    const FormatInfo& info = formatInfo(image.format);
    if (info.channels == 2) {
        if (image.format == GpuFormat::BC5 || image.format == GpuFormat::BC5Snorm)
            return "BC5 stores X and Y only";
        return std::string(info.name) + " stores two channels: X and Y only";
    }
    std::string lower = name;
    for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
    const std::string stem = lower.substr(0, lower.rfind('.'));
    const bool named = stem.find("normal") != std::string::npos || stem.find("nrm") != std::string::npos ||
                       (stem.size() > 2 && stem.compare(stem.size() - 2, 2, "_n") == 0);
    if (info.compressed || info.channels < 3) return {};
    // Up to 64x64 samples spread over the image.
    const uint32_t stepX = std::max(1u, image.width / 64), stepY = std::max(1u, image.height / 64);
    double sum[3] = {0, 0, 0};
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
    int n = 0;
    for (uint32_t y = stepY / 2; y < image.height; y += stepY)
        for (uint32_t x = stepX / 2; x < image.width; x += stepX) {
            float p[4];
            if (!readPixel(image, x, y, p)) return {};
            for (int c = 0; c < 3; ++c) {
                sum[c] += p[c];
                lo[c] = std::min(lo[c], p[c]);
                hi[c] = std::max(hi[c], p[c]);
            }
            ++n;
        }
    if (!n) return {};
    const float meanR = float(sum[0] / n), meanG = float(sum[1] / n);
    const bool blueEmpty = hi[2] <= 2.5f / 255.0f;
    const bool centred = std::fabs(meanR - 0.5f) < 0.15f && std::fabs(meanG - 0.5f) < 0.15f;
    const bool varies = hi[0] - lo[0] > 8.0f / 255.0f || hi[1] - lo[1] > 8.0f / 255.0f;
    if (blueEmpty && varies && (centred || named)) return "Blue is empty and red and green centre on 0.5";
    return {};
}

bool showLayer(Image& image, int index) {
    if (image.layerKind == LayerKind::None || image.layerKind == LayerKind::PsdLayers) return false;
    if (index < 0 || index >= int(image.layers.size()) || index == image.shownLayer) return false;
    Layer& old = image.layers[size_t(image.shownLayer)];
    Layer& next = image.layers[size_t(index)];
    if (next.mips.empty()) return false;
    old.mips = std::move(image.mips);
    image.mips = std::move(next.mips);
    next.mips.clear();
    image.width = next.width;
    image.height = next.height;
    image.format = next.format;
    image.maxValue = next.maxValue;
    image.shownLayer = index;
    return true;
}

bool layerShows(const Image& image, int index) {
    for (int i = index; i >= 0 && i < int(image.layers.size()); i = image.layers[size_t(i)].parent)
        if (!image.layers[size_t(i)].visible) return false;
    return index >= 0 && index < int(image.layers.size());
}

const char* layerKindName(LayerKind kind) {
    switch (kind) {
        case LayerKind::CubeFaces: return "Cubemap";
        case LayerKind::ArraySlices: return "Array";
        case LayerKind::VolumeSlices: return "Volume";
        case LayerKind::Pages: return "Pages";
        case LayerKind::IconSizes: return "Sizes";
        case LayerKind::ExrLayers: return "EXR";
        case LayerKind::PsdLayers: return "PSD";
        case LayerKind::None: break;
    }
    return "";
}

}  // namespace htv
