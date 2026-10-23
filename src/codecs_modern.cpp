// AVIF, JPEG XL and KTX loaders. Each part is compiled only when its library is available.

#include "codecs.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef HTV_HAVE_AVIF
#include <avif/avif.h>
#endif
#ifdef HTV_HAVE_JXL
#include <jxl/decode.h>
#include <jxl/resizable_parallel_runner.h>
#include <jxl/version.h>
#endif
#ifdef HTV_HAVE_KTX
#include <ktx.h>
#endif

namespace htv {

namespace {

[[maybe_unused]] LoadResult unavailable(const char* what) {
    LoadResult r;
    r.error = std::string(what) + " support is not built in";
    return r;
}

[[maybe_unused]] Surface surfaceFor(GpuFormat format, uint32_t width, uint32_t height) {
    Surface s;
    s.width = width;
    s.height = height;
    s.rowPitch = width * formatInfo(format).bytes;
    s.data.resize(size_t(s.rowPitch) * height);
    return s;
}

float srgbToLinear(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }

// SMPTE ST 2084 EOTF: encoded value -> nits.
float pqToNits(float e) {
    const float m1 = 2610.0f / 16384.0f, m2 = 2523.0f / 4096.0f * 128.0f;
    const float c1 = 3424.0f / 4096.0f, c2 = 2413.0f / 4096.0f * 32.0f, c3 = 2392.0f / 4096.0f * 32.0f;
    const float p = std::pow(std::clamp(e, 0.0f, 1.0f), 1.0f / m2);
    return 10000.0f * std::pow(std::max(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
}

// ARIB STD-B67 inverse OETF: encoded value -> relative scene light in [0, 1].
float hlgToScene(float e) {
    const float a = 0.17883277f, b = 0.28466892f, c = 0.55991073f;
    e = std::clamp(e, 0.0f, 1.0f);
    return e <= 0.5f ? e * e / 3.0f : (std::exp((e - c) / a) + b) / 12.0f;
}

constexpr float kReferenceWhiteNits = 203.0f;  // ITU-R BT.2408: HDR graphics white maps to 1.0

}  // namespace

float encodedToLinearHalf(const float* rgba, size_t pixels, Transfer transfer, bool bt2020, uint16_t* out) {
    // BT.2020 -> BT.709 primaries (linear light).
    const float m[3][3] = {{1.6605f, -0.5876f, -0.0728f}, {-0.1246f, 1.1329f, -0.0083f}, {-0.0182f, -0.1006f, 1.1187f}};
    float brightest = 0.0f;
    for (size_t i = 0; i < pixels; ++i) {
        float c[3] = {};
        for (int k = 0; k < 3; ++k) {
            const float v = rgba[i * 4 + k];
            switch (transfer) {
                case Transfer::Linear: c[k] = v; break;
                case Transfer::Srgb: c[k] = srgbToLinear(v); break;
                case Transfer::Pq: c[k] = pqToNits(v) / kReferenceWhiteNits; break;
                case Transfer::Hlg: c[k] = hlgToScene(v); break;
            }
        }
        if (transfer == Transfer::Hlg) {
            // BT.2100 OOTF for a 1000-nit display (system gamma 1.2), applied on BT.2020 luminance.
            const float ys = 0.2627f * c[0] + 0.6780f * c[1] + 0.0593f * c[2];
            const float gain = 1000.0f * std::pow(std::max(ys, 1e-6f), 0.2f) / kReferenceWhiteNits;
            for (float& v : c) v *= gain;
        }
        if (bt2020) {
            const float r = c[0], g = c[1], b = c[2];
            for (int k = 0; k < 3; ++k) c[k] = m[k][0] * r + m[k][1] * g + m[k][2] * b;
        }
        for (int k = 0; k < 3; ++k) {
            out[i * 4 + k] = floatToHalf(c[k]);
            if (std::isfinite(c[k])) brightest = std::max(brightest, c[k]);
        }
        out[i * 4 + 3] = floatToHalf(rgba[i * 4 + 3]);
    }
    return brightest;
}

// ---- AVIF (libavif with dav1d) ----

LoadResult loadAvif(const uint8_t* data, size_t size) {
#ifndef HTV_HAVE_AVIF
    (void)data;
    (void)size;
    return unavailable("AVIF");
#else
    LoadResult r;
    avifDecoder* decoder = avifDecoderCreate();
    if (!decoder) {
        r.error = "out of memory";
        return r;
    }
    struct Guard {
        avifDecoder* d;
        ~Guard() { avifDecoderDestroy(d); }
    } guard{decoder};
    decoder->maxThreads = int(std::max(1u, std::thread::hardware_concurrency()));
    avifResult res = avifDecoderSetIOMemory(decoder, data, size);
    if (res == AVIF_RESULT_OK) res = avifDecoderParse(decoder);
    if (res == AVIF_RESULT_OK) res = avifDecoderNextImage(decoder);
    if (res != AVIF_RESULT_OK) {
        r.error = std::string("cannot decode AVIF: ") + avifResultToString(res);
        return r;
    }
    const avifImage* im = decoder->image;
    const bool pq = im->transferCharacteristics == AVIF_TRANSFER_CHARACTERISTICS_PQ;
    const bool hlg = im->transferCharacteristics == AVIF_TRANSFER_CHARACTERISTICS_HLG;
    const bool linear = im->transferCharacteristics == AVIF_TRANSFER_CHARACTERISTICS_LINEAR;
    const bool bt2020 = im->colorPrimaries == AVIF_COLOR_PRIMARIES_BT2020;

    avifRGBImage rgb;
    avifRGBImageSetDefaults(&rgb, im);
    rgb.format = AVIF_RGB_FORMAT_RGBA;
    rgb.depth = im->depth > 8 ? 16 : 8;
    rgb.rowBytes = 0;
    rgb.pixels = nullptr;
    std::vector<uint8_t> buffer(size_t(im->width) * im->height * 4 * (rgb.depth / 8));
    rgb.pixels = buffer.data();
    rgb.rowBytes = im->width * 4 * (rgb.depth / 8);
    res = avifImageYUVToRGB(im, &rgb);
    if (res != AVIF_RESULT_OK) {
        r.error = std::string("cannot convert AVIF colours: ") + avifResultToString(res);
        return r;
    }

    Image& img = r.image;
    img.width = im->width;
    img.height = im->height;
    const size_t pixels = size_t(im->width) * im->height;
    if (pq || hlg || linear) {
        // HDR: decode the transfer function to linear light and keep it as half floats.
        std::vector<float> encoded(pixels * 4);
        const float scale = rgb.depth == 16 ? 1.0f / 65535.0f : 1.0f / 255.0f;
        for (size_t i = 0; i < pixels * 4; ++i)
            encoded[i] = (rgb.depth == 16 ? reinterpret_cast<const uint16_t*>(buffer.data())[i] : buffer[i]) * scale;
        img.format = GpuFormat::RGBA16F;
        Surface s = surfaceFor(img.format, img.width, img.height);
        img.maxValue = encodedToLinearHalf(encoded.data(), pixels,
                                           pq ? Transfer::Pq : hlg ? Transfer::Hlg : Transfer::Linear, bt2020,
                                           reinterpret_cast<uint16_t*>(s.data.data()));
        img.mips.push_back(std::move(s));
    } else {
        img.format = rgb.depth == 16 ? GpuFormat::RGBA16Unorm : GpuFormat::RGBA8Srgb;
        Surface s = surfaceFor(img.format, img.width, img.height);
        std::memcpy(s.data.data(), buffer.data(), s.data.size());
        img.mips.push_back(std::move(s));
    }
    img.description = "AVIF, " + std::to_string(im->depth) + " bit" + (pq ? ", PQ HDR" : hlg ? ", HLG HDR" : "") +
                      (bt2020 ? ", BT.2020" : "") + (im->alphaPlane ? ", alpha" : "");
    if (decoder->imageCount > 1) img.note = "sequence: frame 1 of " + std::to_string(decoder->imageCount) + " shown";
    r.ok = true;
    return r;
#endif
}

// ---- JPEG XL (libjxl) ----

LoadResult loadJxl(const uint8_t* data, size_t size) {
#ifndef HTV_HAVE_JXL
    (void)data;
    (void)size;
    return unavailable("JPEG XL");
#else
    LoadResult r;
    JxlDecoder* dec = JxlDecoderCreate(nullptr);
    void* runner = JxlResizableParallelRunnerCreate(nullptr);
    struct Guard {
        JxlDecoder* d;
        void* r;
        ~Guard() {
            JxlDecoderDestroy(d);
            JxlResizableParallelRunnerDestroy(r);
        }
    } guard{dec, runner};
    if (!dec || !runner || JxlDecoderSetParallelRunner(dec, JxlResizableParallelRunner, runner) != JXL_DEC_SUCCESS ||
        JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE) !=
            JXL_DEC_SUCCESS ||
        JxlDecoderSetInput(dec, data, size) != JXL_DEC_SUCCESS) {
        r.error = "cannot start the JPEG XL decoder";
        return r;
    }
    JxlDecoderCloseInput(dec);

    JxlBasicInfo info{};
    bool hdr = false;
    Transfer transfer = Transfer::Srgb;
    bool bt2020 = false;
    JxlPixelFormat format{4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    std::vector<float> floats;
    Image& img = r.image;
    Surface s;

    for (;;) {
        const JxlDecoderStatus status = JxlDecoderProcessInput(dec);
        if (status == JXL_DEC_ERROR || status == JXL_DEC_NEED_MORE_INPUT) {
            r.error = "cannot decode JPEG XL";
            return r;
        }
        if (status == JXL_DEC_BASIC_INFO) {
            JxlDecoderGetBasicInfo(dec, &info);
            JxlResizableParallelRunnerSetThreads(
                runner, JxlResizableParallelRunnerSuggestThreads(info.xsize, info.ysize));
            hdr = info.exponent_bits_per_sample > 0 || info.bits_per_sample > 16 || info.intensity_target > 255.0f;
            if (hdr) {
                // Ask for linear sRGB primaries; lossy (XYB) files honour this, others keep their own encoding.
                JxlColorEncoding linear{};
                linear.color_space = JXL_COLOR_SPACE_RGB;
                linear.white_point = JXL_WHITE_POINT_D65;
                linear.primaries = JXL_PRIMARIES_SRGB;
                linear.transfer_function = JXL_TRANSFER_FUNCTION_LINEAR;
                linear.rendering_intent = JXL_RENDERING_INTENT_RELATIVE;
                JxlDecoderSetPreferredColorProfile(dec, &linear);
            }
        } else if (status == JXL_DEC_COLOR_ENCODING) {
            JxlColorEncoding ce;
#if JPEGXL_NUMERIC_VERSION >= JPEGXL_COMPUTE_NUMERIC_VERSION(0, 9, 0)
            const bool known = JxlDecoderGetColorAsEncodedProfile(dec, JXL_COLOR_PROFILE_TARGET_DATA, &ce) ==
                               JXL_DEC_SUCCESS;
#else
            const bool known = JxlDecoderGetColorAsEncodedProfile(dec, nullptr, JXL_COLOR_PROFILE_TARGET_DATA, &ce) ==
                               JXL_DEC_SUCCESS;
#endif
            if (known) {
                transfer = ce.transfer_function == JXL_TRANSFER_FUNCTION_PQ    ? Transfer::Pq
                           : ce.transfer_function == JXL_TRANSFER_FUNCTION_HLG ? Transfer::Hlg
                           : ce.transfer_function == JXL_TRANSFER_FUNCTION_LINEAR ? Transfer::Linear
                                                                                  : Transfer::Srgb;
                bt2020 = ce.primaries == JXL_PRIMARIES_2100;
                if (transfer == Transfer::Pq || transfer == Transfer::Hlg) hdr = true;
            }
        } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            img.width = info.xsize;
            img.height = info.ysize;
            if (uint64_t(img.width) * img.height * 16 > 0x7fffffffull) {
                r.error = "image too large";
                return r;
            }
            size_t needed = 0;
            if (hdr) {
                format.data_type = JXL_TYPE_FLOAT;
                floats.resize(size_t(img.width) * img.height * 4);
                JxlDecoderImageOutBufferSize(dec, &format, &needed);
                if (needed != floats.size() * sizeof(float) ||
                    JxlDecoderSetImageOutBuffer(dec, &format, floats.data(), needed) != JXL_DEC_SUCCESS) {
                    r.error = "cannot set up the JPEG XL output";
                    return r;
                }
            } else {
                const bool deep = info.bits_per_sample > 8;
                format.data_type = deep ? JXL_TYPE_UINT16 : JXL_TYPE_UINT8;
                img.format = deep ? GpuFormat::RGBA16Unorm : GpuFormat::RGBA8Srgb;
                s = surfaceFor(img.format, img.width, img.height);
                JxlDecoderImageOutBufferSize(dec, &format, &needed);
                if (needed != s.data.size() ||
                    JxlDecoderSetImageOutBuffer(dec, &format, s.data.data(), needed) != JXL_DEC_SUCCESS) {
                    r.error = "cannot set up the JPEG XL output";
                    return r;
                }
            }
        } else if (status == JXL_DEC_FULL_IMAGE || status == JXL_DEC_SUCCESS) {
            break;  // the first frame is enough
        }
    }
    if (img.width == 0) {
        r.error = "JPEG XL file has no image";
        return r;
    }
    if (hdr) {
        img.format = GpuFormat::RGBA16F;
        s = surfaceFor(img.format, img.width, img.height);
        img.maxValue = encodedToLinearHalf(floats.data(), size_t(img.width) * img.height, transfer, bt2020,
                                           reinterpret_cast<uint16_t*>(s.data.data()));
    }
    img.mips.push_back(std::move(s));
    img.description = "JPEG XL, " + std::to_string(info.bits_per_sample) + " bit" +
                      (info.exponent_bits_per_sample ? " float" : "") +
                      (transfer == Transfer::Pq ? ", PQ HDR" : transfer == Transfer::Hlg ? ", HLG HDR" : "") +
                      (info.uses_original_profile ? ", lossless/original colours" : ", lossy") +
                      (info.alpha_bits ? ", alpha" : "");
    if (info.have_animation) img.note = "animation: first frame shown";
    r.ok = true;
    return r;
#endif
}

// ---- KTX and KTX2 (libktx); Basis Universal is transcoded to BC7 ----

#ifdef HTV_HAVE_KTX
namespace {

GpuFormat fromVkFormat(uint32_t vk) {
    switch (vk) {
        case 9: return GpuFormat::R8;
        case 16: return GpuFormat::RG8;
        case 37: return GpuFormat::RGBA8;
        case 43: return GpuFormat::RGBA8Srgb;
        case 44: return GpuFormat::BGRA8;
        case 50: return GpuFormat::BGRA8Srgb;
        case 64: return GpuFormat::RGB10A2;
        case 70: return GpuFormat::R16Unorm;
        case 76: return GpuFormat::R16F;
        case 77: return GpuFormat::RG16Unorm;
        case 83: return GpuFormat::RG16F;
        case 91: return GpuFormat::RGBA16Unorm;
        case 97: return GpuFormat::RGBA16F;
        case 100: return GpuFormat::R32F;
        case 103: return GpuFormat::RG32F;
        case 109: return GpuFormat::RGBA32F;
        case 122: return GpuFormat::RG11B10F;
        case 131: case 133: return GpuFormat::BC1;
        case 132: case 134: return GpuFormat::BC1Srgb;
        case 135: return GpuFormat::BC2;
        case 136: return GpuFormat::BC2Srgb;
        case 137: return GpuFormat::BC3;
        case 138: return GpuFormat::BC3Srgb;
        case 139: return GpuFormat::BC4;
        case 140: return GpuFormat::BC4Snorm;
        case 141: return GpuFormat::BC5;
        case 142: return GpuFormat::BC5Snorm;
        case 143: return GpuFormat::BC6HUf16;
        case 144: return GpuFormat::BC6HSf16;
        case 145: return GpuFormat::BC7;
        case 146: return GpuFormat::BC7Srgb;
        default: return GpuFormat::Unknown;
    }
}

GpuFormat fromGlFormat(uint32_t gl) {
    switch (gl) {
        case 0x83F0: case 0x83F1: return GpuFormat::BC1;
        case 0x8C4C: case 0x8C4D: return GpuFormat::BC1Srgb;
        case 0x83F2: return GpuFormat::BC2;
        case 0x8C4E: return GpuFormat::BC2Srgb;
        case 0x83F3: return GpuFormat::BC3;
        case 0x8C4F: return GpuFormat::BC3Srgb;
        case 0x8DBB: return GpuFormat::BC4;
        case 0x8DBC: return GpuFormat::BC4Snorm;
        case 0x8DBD: return GpuFormat::BC5;
        case 0x8DBE: return GpuFormat::BC5Snorm;
        case 0x8E8C: return GpuFormat::BC7;
        case 0x8E8D: return GpuFormat::BC7Srgb;
        case 0x8E8E: return GpuFormat::BC6HSf16;
        case 0x8E8F: return GpuFormat::BC6HUf16;
        case 0x8058: return GpuFormat::RGBA8;
        case 0x8C43: return GpuFormat::RGBA8Srgb;
        case 0x881A: return GpuFormat::RGBA16F;
        case 0x8814: return GpuFormat::RGBA32F;
        case 0x8229: return GpuFormat::R8;
        case 0x822B: return GpuFormat::RG8;
        case 0x822D: return GpuFormat::R16F;
        case 0x822E: return GpuFormat::R32F;
        case 0x822F: return GpuFormat::RG16F;
        case 0x8230: return GpuFormat::RG32F;
        default: return GpuFormat::Unknown;
    }
}

}  // namespace
#endif

LoadResult loadKtx(const uint8_t* data, size_t size) {
#ifndef HTV_HAVE_KTX
    (void)data;
    (void)size;
    return unavailable("KTX");
#else
    LoadResult r;
    ktxTexture* tex = nullptr;
    KTX_error_code err =
        ktxTexture_CreateFromMemory(data, size, KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &tex);
    if (err != KTX_SUCCESS) {
        r.error = std::string("cannot read KTX: ") + ktxErrorString(err);
        return r;
    }
    struct Guard {
        ktxTexture* t;
        ~Guard() { ktxTexture_Destroy(t); }
    } guard{tex};

    std::string container = "KTX";
    std::string encoding;
    GpuFormat format = GpuFormat::Unknown;
    if (tex->classId == ktxTexture2_c) {
        auto* tex2 = reinterpret_cast<ktxTexture2*>(tex);
        container = "KTX2";
        if (ktxTexture2_NeedsTranscoding(tex2)) {
            encoding = tex2->supercompressionScheme == KTX_SS_BASIS_LZ ? ", Basis ETC1S" : ", Basis UASTC";
            err = ktxTexture2_TranscodeBasis(tex2, KTX_TTF_BC7_RGBA, 0);
            if (err != KTX_SUCCESS) {
                r.error = std::string("cannot transcode Basis Universal: ") + ktxErrorString(err);
                return r;
            }
            encoding += " -> BC7";
        }
        format = fromVkFormat(tex2->vkFormat);
        if (format == GpuFormat::Unknown) {
            r.error = "KTX2 format VkFormat " + std::to_string(tex2->vkFormat) +
                      " is not supported yet (ASTC and ETC need a CPU decoder)";
            return r;
        }
    } else {
        format = fromGlFormat(reinterpret_cast<ktxTexture1*>(tex)->glInternalformat);
        if (format == GpuFormat::Unknown) {
            r.error = "this KTX1 pixel format is not supported yet";
            return r;
        }
    }

    const FormatInfo& info = formatInfo(format);
    Image& img = r.image;
    img.width = tex->baseWidth;
    img.height = tex->baseHeight;
    img.format = format;
    const uint8_t* base = ktxTexture_GetData(tex);
    for (uint32_t level = 0; level < tex->numLevels; ++level) {
        const uint32_t w = std::max(1u, img.width >> level), h = std::max(1u, img.height >> level);
        ktx_size_t offset = 0;
        if (ktxTexture_GetImageOffset(tex, level, 0, 0, &offset) != KTX_SUCCESS) break;
        Surface s;
        s.width = w;
        s.height = h;
        size_t rows = h;
        if (info.compressed) {
            s.rowPitch = std::max(1u, (w + 3) / 4) * info.bytes;
            rows = std::max(1u, (h + 3) / 4);
        } else {
            s.rowPitch = ktxTexture_GetRowPitch(tex, level);  // includes KTX1 row padding
        }
        const size_t bytes = s.rowPitch * rows;
        if (offset + bytes > ktxTexture_GetDataSize(tex)) break;
        s.data.assign(base + offset, base + offset + bytes);
        img.mips.push_back(std::move(s));
    }
    if (img.mips.empty()) {
        r.error = "KTX file has no image data";
        return r;
    }
    if (format == GpuFormat::RGBA16F || format == GpuFormat::RGBA32F) {
        float px[4], m = 0.0f;
        for (uint32_t y = 0; y < img.height; ++y)
            for (uint32_t x = 0; x < img.width; ++x)
                if (readPixel(img, x, y, px))
                    for (int c = 0; c < 3; ++c)
                        if (std::isfinite(px[c])) m = std::max(m, px[c]);
        img.maxValue = m;
    }
    img.description = container + ", " + info.name + encoding + ", " + std::to_string(img.mips.size()) +
                      (img.mips.size() == 1 ? " mip" : " mips");
    if (tex->isCubemap)
        img.note = "cubemap: face 0 of 6 shown";
    else if (tex->numLayers > 1)
        img.note = "array: layer 0 of " + std::to_string(tex->numLayers) + " shown";
    else if (tex->baseDepth > 1)
        img.note = "volume: slice 0 shown";
    r.ok = true;
    return r;
#endif
}

}  // namespace htv
