// Loader tests. Builds small images in memory, decodes them and checks the result.
// `htv_tests --write-samples <dir>` also saves the same images as files for the startup benchmark.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "codecs.h"
#include "image.h"
#include "stats.h"
#include "stb_image_write.h"
#include "qoi.h"
#include "tinyexr.h"

#include <webp/encode.h>

#ifdef HTV_HAVE_JXL
#include <jxl/encode.h>
#endif
#ifdef HTV_HAVE_KTX
#include <ktx.h>
#endif

using namespace htv;

namespace {

int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

using Bytes = std::vector<uint8_t>;

void appendBytes(void* ctx, void* data, int size) {
    auto* out = static_cast<Bytes*>(ctx);
    out->insert(out->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
}

void put32(Bytes& b, size_t at, uint32_t v) { std::memcpy(b.data() + at, &v, 4); }

// Gradient with a bright spot, so HDR files really exceed 1.0.
std::vector<float> hdrPixels(int w, int h) {
    std::vector<float> px(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float* p = &px[(size_t(y) * w + x) * 4];
            p[0] = float(x) / (w - 1);
            p[1] = float(y) / (h - 1);
            p[2] = 0.25f;
            p[3] = 1.0f;
            if (x == w / 2 && y == h / 2) p[0] = p[1] = p[2] = 16.0f;
        }
    return px;
}

std::vector<uint8_t> testPixels(int w, int h) {
    std::vector<uint8_t> px(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &px[(size_t(y) * w + x) * 4];
            p[0] = uint8_t(x * 255 / (w - 1));
            p[1] = uint8_t(y * 255 / (h - 1));
            p[2] = ((x / 8 + y / 8) & 1) ? 255 : 0;
            p[3] = uint8_t(255 - x * 255 / (w - 1));
        }
    return px;
}

Bytes makePng(int w, int h) {
    std::vector<uint8_t> px = testPixels(w, h);
    Bytes out;
    stbi_write_png_to_func(appendBytes, &out, w, h, 4, px.data(), w * 4);
    return out;
}

Bytes makeWebp(int w, int h) {
    std::vector<uint8_t> px = testPixels(w, h);
    uint8_t* out = nullptr;
    const size_t size = WebPEncodeLosslessRGBA(px.data(), w, h, w * 4, &out);
    Bytes file(out, out + size);
    WebPFree(out);
    return file;
}

Bytes makeQoi(int w, int h) {
    std::vector<uint8_t> px = testPixels(w, h);
    qoi_desc desc{uint32_t(w), uint32_t(h), 4, QOI_SRGB};
    int size = 0;
    void* out = qoi_encode(px.data(), &desc, &size);
    Bytes file(static_cast<uint8_t*>(out), static_cast<uint8_t*>(out) + size);
    free(out);
    return file;
}

Bytes makeHdr(int w, int h) {
    std::vector<float> px = hdrPixels(w, h);
    std::vector<float> rgb(size_t(w) * h * 3);
    for (size_t i = 0; i < size_t(w) * h; ++i)
        for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = px[i * 4 + c];
    Bytes out;
    stbi_write_hdr_to_func(appendBytes, &out, w, h, 3, rgb.data());
    return out;
}

Bytes makeExr(int w, int h, bool half) {
    std::vector<float> px = hdrPixels(w, h);
    unsigned char* buf = nullptr;
    const char* err = nullptr;
    int size = SaveEXRToMemory(px.data(), w, h, 4, half ? 1 : 0, &buf, &err);
    Bytes out;
    if (size > 0) out.assign(buf, buf + size);
    free(buf);
    if (err) FreeEXRErrorMessage(err);
    return out;
}

// A half EXR with layers: R G B A (0.5), diffuse.R G B (2.0) and depth.Z (7.0), w x h.
Bytes makeLayeredExr(int w, int h) {
    const char* names[] = {"A", "B", "G", "R", "depth.Z", "diffuse.B", "diffuse.G", "diffuse.R"};
    const float values[] = {1.0f, 0.5f, 0.5f, 0.5f, 7.0f, 2.0f, 2.0f, 2.0f};
    const int n = 8;
    std::vector<std::vector<uint16_t>> planes(n);
    std::vector<unsigned char*> pointers(n);
    for (int i = 0; i < n; ++i) {
        planes[size_t(i)].assign(size_t(w) * h, floatToHalf(values[i]));
        pointers[size_t(i)] = reinterpret_cast<unsigned char*>(planes[size_t(i)].data());
    }
    EXRHeader header;
    InitEXRHeader(&header);
    std::vector<EXRChannelInfo> channels(n);
    std::vector<int> types(n, TINYEXR_PIXELTYPE_HALF);
    for (int i = 0; i < n; ++i) {
        std::memset(&channels[size_t(i)], 0, sizeof(EXRChannelInfo));
        std::strncpy(channels[size_t(i)].name, names[i], 255);
    }
    header.num_channels = n;
    header.channels = channels.data();
    header.pixel_types = types.data();
    header.requested_pixel_types = types.data();
    header.compression_type = TINYEXR_COMPRESSIONTYPE_ZIP;
    EXRImage image;
    InitEXRImage(&image);
    image.num_channels = n;
    image.images = pointers.data();
    image.width = w;
    image.height = h;
    unsigned char* buf = nullptr;
    const char* err = nullptr;
    const size_t size = SaveEXRImageToMemory(&image, &header, &buf, &err);
    Bytes out;
    if (size > 0) out.assign(buf, buf + size);
    free(buf);
    if (err) FreeEXRErrorMessage(err);
    return out;
}

#ifdef HTV_HAVE_JXL
// Lossless JPEG XL: 8-bit sRGB from testPixels, or 32-bit float linear from hdrPixels. With `frames` > 1, an
// animation (1000 ticks a second, 3 loops) whose frame i lasts 40 * (i + 1) ms and has its first byte set to i.
Bytes makeJxl(int w, int h, bool hdr, int frames = 1) {
    JxlEncoder* enc = JxlEncoderCreate(nullptr);
    JxlBasicInfo info;
    JxlEncoderInitBasicInfo(&info);
    info.xsize = uint32_t(w);
    info.ysize = uint32_t(h);
    info.num_color_channels = 3;
    info.num_extra_channels = 1;
    info.alpha_bits = hdr ? 32 : 8;
    info.alpha_exponent_bits = hdr ? 8 : 0;
    info.bits_per_sample = hdr ? 32 : 8;
    info.exponent_bits_per_sample = hdr ? 8 : 0;
    info.uses_original_profile = JXL_TRUE;
    if (frames > 1) {
        info.have_animation = JXL_TRUE;
        info.animation.tps_numerator = 1000;
        info.animation.tps_denominator = 1;
        info.animation.num_loops = 3;
    }
    JxlEncoderSetBasicInfo(enc, &info);
    JxlColorEncoding color;
    if (hdr)
        JxlColorEncodingSetToLinearSRGB(&color, JXL_FALSE);
    else
        JxlColorEncodingSetToSRGB(&color, JXL_FALSE);
    JxlEncoderSetColorEncoding(enc, &color);
    JxlEncoderFrameSettings* settings = JxlEncoderFrameSettingsCreate(enc, nullptr);
    JxlEncoderSetFrameLossless(settings, JXL_TRUE);
    std::vector<float> f = hdrPixels(w, h);
    std::vector<uint8_t> b = testPixels(w, h);
    JxlPixelFormat format{4, hdr ? JXL_TYPE_FLOAT : JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    for (int i = 0; i < frames; ++i) {
        if (frames > 1) {
            JxlFrameHeader header;
            JxlEncoderInitFrameHeader(&header);
            header.duration = uint32_t(40 * (i + 1));
            JxlEncoderSetFrameHeader(settings, &header);
            b[0] = uint8_t(i);
        }
        if (hdr)
            JxlEncoderAddImageFrame(settings, &format, f.data(), f.size() * sizeof(float));
        else
            JxlEncoderAddImageFrame(settings, &format, b.data(), b.size());
    }
    JxlEncoderCloseInput(enc);
    Bytes out(1 << 16);
    uint8_t* next = out.data();
    size_t avail = out.size();
    JxlEncoderStatus st;
    while ((st = JxlEncoderProcessOutput(enc, &next, &avail)) == JXL_ENC_NEED_MORE_OUTPUT) {
        const size_t used = size_t(next - out.data());
        out.resize(out.size() * 2);
        next = out.data() + used;
        avail = out.size() - used;
    }
    out.resize(st == JXL_ENC_SUCCESS ? size_t(next - out.data()) : 0);
    JxlEncoderDestroy(enc);
    return out;
}
#endif

#ifdef HTV_HAVE_KTX
// KTX2 RGBA8 cubemap (or a 3D texture `depth` deep) whose picture i is filled with i + 1.
Bytes makeKtx2Layers(int w, int h, bool cube, int depth) {
    ktxTextureCreateInfo ci{};
    ci.vkFormat = 37;  // VK_FORMAT_R8G8B8A8_UNORM
    ci.baseWidth = uint32_t(w);
    ci.baseHeight = uint32_t(h);
    ci.baseDepth = uint32_t(depth);
    ci.numDimensions = depth > 1 ? 3 : 2;
    ci.numLevels = 1;
    ci.numLayers = 1;
    ci.numFaces = cube ? 6 : 1;
    ktxTexture2* tex = nullptr;
    if (ktxTexture2_Create(&ci, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &tex) != KTX_SUCCESS) return {};
    const int pictures = cube ? 6 : depth;
    for (int i = 0; i < pictures; ++i) {  // faceSlice is the face, or the depth slice of a 3D texture
        std::vector<uint8_t> px(size_t(w) * h * 4, uint8_t(i + 1));
        ktxTexture_SetImageFromMemory(ktxTexture(tex), 0, 0, uint32_t(i), px.data(), px.size());
    }
    ktx_uint8_t* data = nullptr;
    ktx_size_t size = 0;
    ktxTexture_WriteToMemory(ktxTexture(tex), &data, &size);
    Bytes out(data, data + size);
    free(data);
    ktxTexture_Destroy(ktxTexture(tex));
    return out;
}

// KTX2 with an RGBA8 sRGB image, optionally Basis Universal (UASTC) compressed.
Bytes makeKtx2(int w, int h, bool basis) {
    ktxTextureCreateInfo ci{};
    ci.vkFormat = 43;  // VK_FORMAT_R8G8B8A8_SRGB
    ci.baseWidth = uint32_t(w);
    ci.baseHeight = uint32_t(h);
    ci.baseDepth = 1;
    ci.numDimensions = 2;
    ci.numLevels = 1;
    ci.numLayers = 1;
    ci.numFaces = 1;
    ktxTexture2* tex = nullptr;
    if (ktxTexture2_Create(&ci, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &tex) != KTX_SUCCESS) return {};
    std::vector<uint8_t> px = testPixels(w, h);
    ktxTexture_SetImageFromMemory(ktxTexture(tex), 0, 0, 0, px.data(), px.size());
    if (basis) {
        ktxBasisParams params{};
        params.structSize = sizeof(params);
        params.uastc = KTX_TRUE;
        params.threadCount = 1;
        if (ktxTexture2_CompressBasisEx(tex, &params) != KTX_SUCCESS) {
            ktxTexture_Destroy(ktxTexture(tex));
            return {};
        }
    }
    ktx_uint8_t* data = nullptr;
    ktx_size_t size = 0;
    ktxTexture_WriteToMemory(ktxTexture(tex), &data, &size);
    Bytes out(data, data + size);
    free(data);
    ktxTexture_Destroy(ktxTexture(tex));
    return out;
}
#endif

// DDS with a full mip chain of BC1 blocks (legacy header) or 16-byte blocks of `dx10` (DX10 header).
Bytes makeDds(uint32_t w, uint32_t h, bool bc7, GpuFormat dx10 = GpuFormat::BC7Srgb) {
    uint32_t levels = 1;
    while ((w >> levels) || (h >> levels)) ++levels;
    const uint32_t blockBytes = bc7 ? 16 : 8;
    size_t dataSize = 0;
    for (uint32_t l = 0; l < levels; ++l)
        dataSize += size_t(std::max(1u, ((w >> l) + 3) / 4)) * std::max(1u, ((h >> l) + 3) / 4) * blockBytes;
    const size_t headerSize = bc7 ? 148 : 128;
    Bytes b(headerSize + dataSize, 0);
    std::memcpy(b.data(), "DDS ", 4);
    put32(b, 4, 124);
    put32(b, 8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000);
    put32(b, 12, h);
    put32(b, 16, w);
    put32(b, 28, levels);
    put32(b, 76, 32);
    put32(b, 80, 0x4);
    std::memcpy(b.data() + 84, bc7 ? "DX10" : "DXT1", 4);
    put32(b, 108, 0x1000 | 0x400000 | 0x8);
    if (bc7) {
        put32(b, 128, uint32_t(dx10));
        put32(b, 132, 3);  // TEXTURE2D
        put32(b, 140, 1);  // array size
    }
    // BC1 blocks: red/blue endpoints, alternating index rows. BC7 blocks: mode 6 with mid-grey endpoints.
    for (size_t at = headerSize; at + blockBytes <= b.size(); at += blockBytes) {
        if (bc7) {
            b[at] = 0x40;
            for (size_t i = 1; i < 16; ++i) b[at + i] = 0x7f;
        } else {
            put32(b, at, 0xf800u | (0x001fu << 16));
            put32(b, at + 4, 0x55aa55aau);
        }
    }
    return b;
}

// Uncompressed RGBA8 DDS with a DX10 header: `slices` array slices (times 6 faces for a cubemap) of `levels`
// mips each, or a volume `depth` deep. Every pixel of picture i holds the value i + 1, mip level l adds 100 * l.
Bytes makeDdsLayers(uint32_t w, uint32_t h, uint32_t levels, uint32_t slices, bool cube, uint32_t depth = 1) {
    const uint32_t pictures = depth > 1 ? depth : slices * (cube ? 6 : 1);
    Bytes b(148, 0);
    std::memcpy(b.data(), "DDS ", 4);
    put32(b, 4, 124);
    put32(b, 8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | (depth > 1 ? 0x800000 : 0));
    put32(b, 12, h);
    put32(b, 16, w);
    put32(b, 24, depth);
    put32(b, 28, levels);
    put32(b, 76, 32);
    put32(b, 80, 0x4);
    std::memcpy(b.data() + 84, "DX10", 4);
    put32(b, 108, 0x1000 | (depth > 1 ? 0x200000 : 0) | (cube ? 0xFE00 : 0));
    put32(b, 128, uint32_t(GpuFormat::RGBA8));
    put32(b, 132, depth > 1 ? 4 : 3);
    put32(b, 136, cube ? 0x4 : 0);
    put32(b, 140, depth > 1 ? 1 : slices);
    auto fill = [&](uint32_t pw, uint32_t ph, uint8_t v) { b.insert(b.end(), size_t(pw) * ph * 4, v); };
    if (depth > 1) {
        for (uint32_t l = 0; l < levels; ++l)
            for (uint32_t z = 0; z < std::max(1u, depth >> l); ++z)
                fill(std::max(1u, w >> l), std::max(1u, h >> l), uint8_t(z + 1 + 100 * l));
    } else {
        for (uint32_t i = 0; i < pictures; ++i)
            for (uint32_t l = 0; l < levels; ++l) fill(std::max(1u, w >> l), std::max(1u, h >> l), uint8_t(i + 1 + 100 * l));
    }
    return b;
}

// A PSD 4 x 2 pixels, 8-bit RGB, raw channels. From the top: group "Group" holding "Top" (blue over the right
// half, alpha 128), then "Bottom" (opaque red over the whole canvas). The composite is `composite` (RGBA rows).
Bytes makePsd(const std::vector<uint8_t>& composite) {
    Bytes b;
    auto u8 = [&](uint32_t v) { b.push_back(uint8_t(v)); };
    auto u16 = [&](uint32_t v) { u8(v >> 8); u8(v); };
    auto u32 = [&](uint32_t v) { u16(v >> 16); u16(v); };
    auto str = [&](const char* t) { b.insert(b.end(), t, t + std::strlen(t)); };
    const int W = 4, H = 2;
    str("8BPS");
    u16(1);
    b.insert(b.end(), 6, 0);
    u16(4);
    u32(H);
    u32(W);
    u16(8);
    u16(3);
    u32(0);  // colour mode data
    u32(28);  // image resources: 300 dpi
    str("8BIM");
    u16(0x03ED);
    u16(0);
    u32(16);
    u32(300u << 16);
    u16(1);
    u16(1);
    u32(300u << 16);
    u16(1);
    u16(1);

    struct Rec {
        const char* name;
        int top, left, bottom, right;
        uint32_t section;
        uint8_t r, g, bl, a;
    };
    // Bottom to top, as PSD stores them.
    const Rec recs[] = {{"Bottom", 0, 0, 2, 4, 0, 255, 0, 0, 255},
                        {"</Layer group>", 0, 0, 0, 0, 3, 0, 0, 0, 0},
                        {"Top", 0, 2, 2, 4, 0, 0, 0, 255, 128},
                        {"Group", 0, 0, 0, 0, 1, 0, 0, 0, 0}};
    Bytes info;
    std::swap(b, info);
    u16(4);
    for (const Rec& r : recs) {
        u32(uint32_t(r.top));
        u32(uint32_t(r.left));
        u32(uint32_t(r.bottom));
        u32(uint32_t(r.right));
        const uint32_t pixels = uint32_t((r.bottom - r.top) * (r.right - r.left));
        u16(4);
        for (int id : {0, 1, 2, -1}) {
            u16(uint16_t(int16_t(id)));
            u32(2 + pixels);
        }
        str("8BIM");
        str(r.section ? "pass" : "norm");
        u8(255);
        u8(0);
        u8(0);
        u8(0);
        const size_t nameLength = std::strlen(r.name);
        const size_t namePadded = (nameLength + 1 + 3) & ~size_t(3);
        u32(uint32_t(4 + 4 + namePadded + (r.section ? 16 : 0)));
        u32(0);
        u32(0);
        u8(uint32_t(nameLength));
        str(r.name);
        b.insert(b.end(), namePadded - nameLength - 1, 0);
        if (r.section) {
            str("8BIM");
            str("lsct");
            u32(4);
            u32(r.section);
        }
    }
    for (const Rec& r : recs) {
        const size_t pixels = size_t((r.bottom - r.top) * (r.right - r.left));
        for (uint8_t v : {r.r, r.g, r.bl, r.a}) {
            u16(0);
            b.insert(b.end(), pixels, v);
        }
    }
    if (b.size() & 1) u8(0);
    std::swap(b, info);
    u32(uint32_t(4 + info.size() + 4));
    u32(uint32_t(info.size()));
    b.insert(b.end(), info.begin(), info.end());
    u32(0);  // global layer mask
    u16(0);  // composite, raw, planar
    for (int c = 0; c < 4; ++c)
        for (int i = 0; i < W * H; ++i) u8(composite[size_t(i) * 4 + c]);
    return b;
}

// An RGBA8 image whose blue is empty and red and green wander around 0.5: a normal map with Z left out.
Image makeRgba8(uint32_t w, uint32_t h, uint8_t blue) {
    Image img;
    img.width = w;
    img.height = h;
    img.format = GpuFormat::RGBA8;
    Surface s;
    s.width = w;
    s.height = h;
    s.rowPitch = w * 4;
    s.data.resize(size_t(s.rowPitch) * h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = s.data.data() + size_t(y) * s.rowPitch + x * 4;
            p[0] = uint8_t(96 + (x * 64) / w);
            p[1] = uint8_t(96 + (y * 64) / h);
            p[2] = blue;
            p[3] = 255;
        }
    img.mips.push_back(std::move(s));
    return img;
}

void testNormalHint() {
    CHECK(!missingNormalZ(makeRgba8(64, 64, 0), "rock.png").empty());
    CHECK(missingNormalZ(makeRgba8(64, 64, 255), "rock_normal.png").empty());  // tangent space with Z: bluish
    Image bc5;
    bc5.width = bc5.height = 4;
    bc5.format = GpuFormat::BC5;
    bc5.mips.resize(1);
    CHECK(!missingNormalZ(bc5, "x.dds").empty());
}

void testHalf() {
    const float values[] = {0.0f, -0.0f, 1.0f, -2.5f, 0.1f, 65504.0f, 6.1035156e-05f, 5.9604645e-08f, 1000.25f};
    for (float v : values) {
        const float back = halfToFloat(floatToHalf(v));
        CHECK(std::fabs(back - v) <= std::fabs(v) * 0.001f + 1e-7f);
    }
    CHECK(std::isinf(halfToFloat(floatToHalf(1e6f))));
    CHECK(std::isnan(halfToFloat(floatToHalf(std::nanf("")))));
    CHECK(floatToHalf(1.0f) == 0x3c00);
    CHECK(floatToHalf(65520.0f) == 0x7c00);  // rounds up to infinity
}

void testPng() {
    Bytes file = makePng(64, 32);
    LoadResult r = loadImage(file.data(), file.size(), "png");
    CHECK(r.ok);
    CHECK(r.image.width == 64 && r.image.height == 32);
    CHECK(r.image.format == GpuFormat::RGBA8Srgb);
    CHECK(r.image.mips.size() == 1);
    float px[4];
    CHECK(readPixel(r.image, 63, 0, px));
    CHECK(std::fabs(px[0] - 1.0f) < 1e-6f && std::fabs(px[3]) < 1e-6f);
}

void testWebp() {
    Bytes file = makeWebp(48, 24);
    CHECK(!file.empty());
    LoadResult r = loadImage(file.data(), file.size(), "webp");
    CHECK(r.ok);
    CHECK(r.image.width == 48 && r.image.height == 24);
    CHECK(r.image.format == GpuFormat::RGBA8Srgb);
    CHECK(r.image.description == "WebP, lossless RGBA");
    // Lossless: every visible pixel comes back exactly (the encoder may rewrite RGB where alpha is 0).
    std::vector<uint8_t> expected = testPixels(48, 24);
    const std::vector<uint8_t>& got = r.image.mips[0].data;
    CHECK(got.size() == expected.size());
    size_t mismatches = 0;
    for (size_t i = 0; i + 3 < expected.size() && got.size() == expected.size(); i += 4)
        if (expected[i + 3] != 0 && std::memcmp(&got[i], &expected[i], 4) != 0) ++mismatches;
    CHECK(mismatches == 0);
}

void testQoi() {
    Bytes file = makeQoi(40, 30);
    LoadResult r = loadImage(file.data(), file.size(), "qoi");
    CHECK(r.ok);
    CHECK(r.image.format == GpuFormat::RGBA8Srgb);
    CHECK(r.image.mips[0].data == testPixels(40, 30));
}

void testJxl() {
#ifdef HTV_HAVE_JXL
    Bytes sdr = makeJxl(32, 16, false);
    CHECK(!sdr.empty());
    LoadResult r = loadImage(sdr.data(), sdr.size(), "jxl");
    CHECK(r.ok);
    if (!r.ok) std::printf("  %s\n", r.error.c_str());
    CHECK(r.image.format == GpuFormat::RGBA8Srgb);
    if (r.ok) CHECK(r.image.mips[0].data == testPixels(32, 16));

    Bytes hdr = makeJxl(32, 16, true);
    CHECK(!hdr.empty());
    r = loadImage(hdr.data(), hdr.size(), "jxl");
    CHECK(r.ok);
    if (!r.ok) std::printf("  %s\n", r.error.c_str());
    CHECK(r.image.format == GpuFormat::RGBA16F);
    CHECK(std::fabs(r.image.maxValue - 16.0f) < 0.01f);
    float px[4];
    CHECK(readPixel(r.image, 31, 0, px));
    CHECK(std::fabs(px[0] - 1.0f) < 1e-3f && std::fabs(px[2] - 0.25f) < 1e-3f);

    Bytes anim = makeJxl(16, 8, false, 3);
    r = loadImage(anim.data(), anim.size(), "jxl");
    CHECK(r.ok && r.image.frames.size() == 3);
    if (r.image.frames.size() == 3) {
        CHECK(r.image.frames[1].surface.data[0] == 1 && r.image.frames[2].surface.data[0] == 2);
        CHECK(r.image.frames[0].delayMs == 40 && r.image.frames[2].delayMs == 120);
        CHECK(r.image.loopCount == 3 && r.image.mips[0].data == r.image.frames[0].surface.data);
    }
#endif
}

void testAvif() {
#ifdef HTV_HAVE_AVIF
    auto load = [](const char* name) {
        std::ifstream f(std::filesystem::path(HTV_AVIF_TEST_DATA) / name, std::ios::binary);
        const Bytes bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(!bytes.empty());
        LoadResult r = loadImage(bytes.data(), bytes.size(), "avif");
        if (!r.ok) std::printf("  %s: %s\n", name, r.error.c_str());
        CHECK(r.ok);
        return r;
    };
    LoadResult r = load("white_1x1.avif");
    float px[4];
    CHECK(r.image.format == GpuFormat::RGBA8Srgb);
    CHECK(readPixel(r.image, 0, 0, px) && px[0] > 0.99f && px[1] > 0.99f && px[2] > 0.99f);

    r = load("colors_sdr_srgb.avif");
    CHECK(r.image.format == GpuFormat::RGBA8Srgb);

    // PQ in BT.2020: linear half floats, brighter than SDR white.
    r = load("colors_hdr_rec2020.avif");
    CHECK(r.image.format == GpuFormat::RGBA16F);
    CHECK(r.image.maxValue > 1.5f);
    std::printf("  %s, max %.2f\n", r.image.description.c_str(), r.image.maxValue);
    r = load("colors_hdr_p3.avif");
    CHECK(r.image.format == GpuFormat::RGBA16F);
    CHECK(r.image.description.find("Display P3") != std::string::npos);

    // An image sequence: every frame, each with its duration.
    r = load("colors-animated-8bpc.avif");
    CHECK(r.image.frames.size() > 1);
    for (const Frame& f : r.image.frames) CHECK(f.delayMs > 0 && f.surface.width == r.image.width);
    std::printf("  animated AVIF: %d frames, first %d ms\n", int(r.image.frames.size()),
                r.image.frames.empty() ? 0 : r.image.frames[0].delayMs);
#endif
}

void testKtx() {
#ifdef HTV_HAVE_KTX
    Bytes plain = makeKtx2(32, 16, false);
    LoadResult r = loadImage(plain.data(), plain.size(), "ktx2");
    CHECK(r.ok);
    if (!r.ok) std::printf("  %s\n", r.error.c_str());
    CHECK(r.image.format == GpuFormat::RGBA8Srgb);
    if (r.ok) CHECK(r.image.mips[0].data == testPixels(32, 16));

    Bytes basis = makeKtx2(64, 32, true);
    CHECK(!basis.empty());
    r = loadImage(basis.data(), basis.size(), "ktx2");
    CHECK(r.ok);
    if (!r.ok) std::printf("  %s\n", r.error.c_str());
    CHECK(r.image.format == GpuFormat::BC7Srgb);
    if (r.ok) CHECK(r.image.mips[0].data.size() == size_t(16 * 8 * 16));

    Bytes cube = makeKtx2Layers(8, 8, true, 1);
    r = loadImage(cube.data(), cube.size(), "ktx2");
    CHECK(r.ok && r.image.layerKind == LayerKind::CubeFaces && r.image.layers.size() == 6);
    if (r.image.layers.size() == 6) CHECK(r.image.layers[4].name == "+Z" && r.image.layers[4].mips[0].data[0] == 5);
    Bytes volume = makeKtx2Layers(4, 4, false, 3);
    r = loadImage(volume.data(), volume.size(), "ktx2");
    CHECK(r.ok && r.image.layerKind == LayerKind::VolumeSlices && r.image.layers.size() == 3);
    if (r.image.layers.size() == 3) CHECK(r.image.layers[2].mips[0].data[0] == 3);
#endif
}

void testHdrTransfer() {
    // PQ code value 0.58 is about 203 nits, which is reference white (1.0).
    const float pq[4] = {0.5806f, 0.5806f, 0.5806f, 1.0f};
    uint16_t out[4];
    encodedToLinearHalf(pq, 1, Transfer::Pq, Primaries::Bt709, out);
    CHECK(std::fabs(halfToFloat(out[0]) - 1.0f) < 0.02f);
    const float srgb[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    encodedToLinearHalf(srgb, 1, Transfer::Srgb, Primaries::Bt709, out);
    CHECK(std::fabs(halfToFloat(out[0]) - 0.214f) < 0.002f && halfToFloat(out[3]) == 0.5f);
}

void testHdr() {
    Bytes file = makeHdr(33, 17);
    LoadResult r = loadImage(file.data(), file.size(), "hdr");
    CHECK(r.ok);
    CHECK(r.image.format == GpuFormat::RGBA16F);
    CHECK(r.image.maxValue > 15.0f && r.image.maxValue < 17.0f);
    float px[4];
    CHECK(readPixel(r.image, 16, 8, px));
    CHECK(px[0] > 15.0f);
}

void testExr() {
    for (bool half : {true, false}) {
        Bytes file = makeExr(40, 20, half);
        CHECK(!file.empty());
        LoadResult r = loadImage(file.data(), file.size(), "exr");
        CHECK(r.ok);
        if (!r.ok) std::printf("  %s\n", r.error.c_str());
        CHECK(r.image.format == (half ? GpuFormat::RGBA16F : GpuFormat::RGBA32F));
        CHECK(r.image.maxValue == 16.0f);
        float px[4];
        CHECK(readPixel(r.image, 39, 0, px));
        CHECK(std::fabs(px[0] - 1.0f) < 1e-3f && std::fabs(px[2] - 0.25f) < 1e-3f);
        CHECK(r.image.description.find("ZIP") != std::string::npos);
        CHECK(r.image.layers.empty());
    }

    Bytes layered = makeLayeredExr(8, 4);
    LoadResult r = loadImage(layered.data(), layered.size(), "exr");
    CHECK(r.ok && r.image.layerKind == LayerKind::ExrLayers && r.image.layers.size() == 3);
    if (r.image.layers.size() != 3) return;
    const Image& img = r.image;
    CHECK(img.layers[0].name == "Main" && img.layers[0].detail == "A B G R");
    CHECK(img.layers[1].name == "depth" && img.layers[1].detail == "Z (grey)" && img.layers[1].maxValue == 7.0f);
    CHECK(img.layers[2].name == "diffuse" && img.layers[2].maxValue == 2.0f);
    float px[4];
    CHECK(readPixel(img, 1, 1, px) && px[0] == 0.5f && px[3] == 1.0f);
    r.image.shownLayer = 0;
    CHECK(showLayer(r.image, 1) && readPixel(r.image, 1, 1, px) && px[0] == 7.0f && px[2] == 7.0f && px[3] == 1.0f);
    CHECK(r.image.maxValue == 7.0f);
}

void testDds() {
    Bytes bc1 = makeDds(256, 128, false);
    LoadResult r = loadImage(bc1.data(), bc1.size(), "dds");
    CHECK(r.ok);
    CHECK(r.image.format == GpuFormat::BC1);
    CHECK(r.image.mips.size() == 9);
    CHECK(r.image.mips[0].rowPitch == 64 * 8);
    CHECK(r.image.mips[8].width == 1 && r.image.mips[8].height == 1 && r.image.mips[8].data.size() == 8);
    float px[4];
    CHECK(!readPixel(r.image, 0, 0, px));

    Bytes bc7 = makeDds(64, 64, true);
    r = loadImage(bc7.data(), bc7.size(), "dds");
    CHECK(r.ok);
    CHECK(r.image.format == GpuFormat::BC7Srgb);
    CHECK(r.image.mips.size() == 7);

    Bytes bc6 = makeDds(64, 32, true, GpuFormat::BC6HUf16);
    r = loadImage(bc6.data(), bc6.size(), "dds");
    CHECK(r.ok);
    CHECK(r.image.format == GpuFormat::BC6HUf16);
    CHECK(formatInfo(r.image.format).linear);

    bc7.resize(150);
    r = loadImage(bc7.data(), bc7.size(), "dds");
    CHECK(!r.ok);
}

void testDdsLayers() {
    Bytes cube = makeDdsLayers(8, 8, 4, 1, true);
    LoadResult r = loadImage(cube.data(), cube.size(), "dds");
    CHECK(r.ok);
    CHECK(r.image.layerKind == LayerKind::CubeFaces);
    CHECK(r.image.layers.size() == 6);
    CHECK(r.image.mips.size() == 4 && r.image.mips[0].data[0] == 1 && r.image.mips[2].data[0] == 201);
    CHECK(r.image.layers[1].name == "\xE2\x88\x92X");
    CHECK(r.image.layers[0].mips.empty() && r.image.layers[5].mips.size() == 4);
    CHECK(showLayer(r.image, 5));
    CHECK(r.image.shownLayer == 5 && r.image.mips[0].data[0] == 6 && r.image.mips[1].data[0] == 106);
    CHECK(r.image.layers[5].mips.empty() && r.image.layers[0].mips.size() == 4 && r.image.layers[0].mips[0].data[0] == 1);
    CHECK(!showLayer(r.image, 5) && !showLayer(r.image, 6));
    CHECK(showLayer(r.image, 0) && r.image.mips[0].data[0] == 1);

    Bytes array = makeDdsLayers(4, 4, 1, 3, false);
    r = loadImage(array.data(), array.size(), "dds");
    CHECK(r.ok && r.image.layerKind == LayerKind::ArraySlices && r.image.layers.size() == 3);
    CHECK(r.image.layers[2].name == "Slice 2" && r.image.layers[2].mips[0].data[0] == 3);

    Bytes cubes = makeDdsLayers(4, 4, 1, 2, true);
    r = loadImage(cubes.data(), cubes.size(), "dds");
    CHECK(r.ok && r.image.layerKind == LayerKind::CubeFaces && r.image.layers.size() == 12);
    CHECK(r.image.layers[7].name == "Cube 1 \xE2\x88\x92X");

    Bytes volume = makeDdsLayers(4, 4, 3, 1, false, 4);
    r = loadImage(volume.data(), volume.size(), "dds");
    CHECK(r.ok && r.image.layerKind == LayerKind::VolumeSlices && r.image.layers.size() == 4);
    CHECK(r.image.mips.size() == 1 && r.image.layers[3].mips[0].data[0] == 4);

    // A file cut short keeps the faces that are whole.
    cube.resize(148 + (8 * 8 + 4 * 4 + 2 * 2 + 1) * 4 * 2 + 8 * 8 * 4 + 10);
    r = loadImage(cube.data(), cube.size(), "dds");
    CHECK(r.ok && r.image.layers.size() == 3 && r.image.layers[2].mips.size() == 1);
    CHECK(r.image.note == "3 of 6 faces in the file");
}

void testPsdLayers() {
    // The composite Photoshop would save: red on the left, red under half-transparent blue on the right.
    std::vector<uint8_t> merged;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 4; ++x) {
            if (x < 2) merged.insert(merged.end(), {255, 0, 0, 255});
            else merged.insert(merged.end(), {127, 0, 128, 255});
        }
    Bytes psd = makePsd(merged);
    LoadResult r = loadImage(psd.data(), psd.size(), "psd");
    CHECK(r.ok);
    CHECK(r.image.layerKind == LayerKind::PsdLayers);
    CHECK(r.image.dpi == 300.0f);
    CHECK(r.image.layers.size() == 3);
    if (r.image.layers.size() != 3) return;
    Image& img = r.image;
    CHECK(img.layers[0].name == "Group" && img.layers[0].group && img.layers[0].depth == 0);
    CHECK(img.layers[1].name == "Top" && img.layers[1].parent == 0 && img.layers[1].depth == 1);
    CHECK(img.layers[1].x == 2 && img.layers[1].width == 2 && img.layers[1].mips[0].data[2] == 255);
    CHECK(img.layers[2].name == "Bottom" && img.layers[2].parent == -1 && img.layers[2].width == 4);
    CHECK(!img.composited && img.mips[0].data[2 * 4] == 127);

    // Hiding the group hides the layer in it.
    img.layers[0].visible = false;
    CHECK(!layerShows(img, 1) && layerShows(img, 2));
    CHECK(compositeLayers(img) && img.composited);
    CHECK(img.mips[0].data[3 * 4] == 255 && img.mips[0].data[3 * 4 + 2] == 0);
    // Bottom hidden: the left is empty, the right is Top alone.
    img.layers[0].visible = true;
    img.layers[2].visible = false;
    CHECK(compositeLayers(img));
    CHECK(img.mips[0].data[3] == 0);
    CHECK(img.mips[0].data[2 * 4 + 2] == 255 && img.mips[0].data[2 * 4 + 3] == 128);
    // Back as saved: the file's own composite again.
    img.layers[2].visible = true;
    CHECK(compositeLayers(img) && !img.composited && img.mips[0].data[2 * 4] == 127);

    // A blank composite (saved without Maximize compatibility) is rebuilt from the layers.
    psd = makePsd(std::vector<uint8_t>(4 * 2 * 4, 255));
    r = loadImage(psd.data(), psd.size(), "psd");
    CHECK(r.ok && r.image.composited);
    CHECK(r.image.mips[0].data[0] == 255 && r.image.mips[0].data[1] == 0 && r.image.mips[0].data[2 * 4 + 2] >= 127);

    // Damaged layer data leaves the composite.
    psd = makePsd(merged);
    psd.resize(psd.size() / 2);
    r = loadImage(psd.data(), psd.size(), "psd");
    CHECK(r.image.layers.empty());
}

std::string metaValue(const Image& img, const char* key) {
    for (const Property& p : img.metadata)
        if (p.key == key) return p.value;
    return {};
}

void testMetadata() {
    // An ICC v2 profile whose description is "Test RGB".
    Bytes icc(165, 0);
    auto be = [](Bytes& b, size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (24 - 8 * i));
    };
    be(icc, 0, 165);
    icc[8] = 2;
    icc[9] = 0x10;
    std::memcpy(&icc[16], "RGB ", 4);
    std::memcpy(&icc[36], "acsp", 4);
    be(icc, 128, 1);  // one tag: desc at 144, 21 bytes
    std::memcpy(&icc[132], "desc", 4);
    be(icc, 136, 144);
    be(icc, 140, 21);
    std::memcpy(&icc[144], "desc", 4);
    be(icc, 152, 9);
    std::memcpy(&icc[156], "Test RGB", 9);
    CHECK(iccDescription(icc.data(), icc.size()) == "Test RGB (ICC v2.1, RGB)");
    CHECK(iccDescription(icc.data(), 100).empty());

    // EXIF (big-endian TIFF): Make "Canon", XResolution 300, Orientation 6.
    Bytes exif(64, 0);
    std::memcpy(exif.data(), "MM\0*", 4);
    be(exif, 4, 8);
    exif[8] = 0;
    exif[9] = 3;
    auto entry = [&](size_t at, uint16_t tag, uint16_t type, uint32_t count, uint32_t value) {
        exif[at] = uint8_t(tag >> 8);
        exif[at + 1] = uint8_t(tag);
        exif[at + 3] = uint8_t(type);
        be(exif, at + 4, count);
        be(exif, at + 8, value);
    };
    entry(10, 0x010F, 2, 6, 50);
    entry(22, 0x0112, 3, 1, 6u << 16);
    entry(34, 0x011A, 5, 1, 56);
    std::memcpy(&exif[50], "Canon", 6);
    be(exif, 56, 300);
    be(exif, 60, 1);
    Image img;
    readExif(exif.data(), exif.size(), img);
    CHECK(metaValue(img, "Camera") == "Canon");
    CHECK(metaValue(img, "Orientation").find("90") != std::string::npos);
    CHECK(img.dpi == 300.0f);

    // PNG with pHYs (150 dpi, 5906 px/m) and a tEXt Software chunk, after IHDR.
    Bytes png = makePng(4, 4);
    auto chunk = [](const char* type, const Bytes& data) {
        Bytes c(8);
        const uint32_t n = uint32_t(data.size());
        for (int i = 0; i < 4; ++i) c[i] = uint8_t(n >> (24 - 8 * i));
        std::memcpy(&c[4], type, 4);
        c.insert(c.end(), data.begin(), data.end());
        c.insert(c.end(), 4, 0);  // CRC: not checked
        return c;
    };
    Bytes phys = {0, 0, 0x17, 0x12, 0, 0, 0x17, 0x12, 1};
    const char text[] = "Software\0Substance Designer";
    Bytes software(text, text + sizeof text - 1);
    Bytes extra = chunk("pHYs", phys);
    Bytes t = chunk("tEXt", software);
    extra.insert(extra.end(), t.begin(), t.end());
    png.insert(png.begin() + 33, extra.begin(), extra.end());
    LoadResult r = loadImage(png.data(), png.size(), "png");
    CHECK(r.ok);
    CHECK(std::fabs(r.image.dpi - 150.0f) < 0.1f);
    CHECK(metaValue(r.image, "Software") == "Substance Designer");
}

// The zlib stream of a PNG written by stb_image_write: its IDAT contents.
Bytes idatOf(const Bytes& png) {
    Bytes out;
    for (size_t at = 8; at + 12 <= png.size();) {
        const size_t n = size_t(png[at]) << 24 | size_t(png[at + 1]) << 16 | size_t(png[at + 2]) << 8 | png[at + 3];
        if (std::memcmp(&png[at + 4], "IDAT", 4) == 0) out.insert(out.end(), png.begin() + long(at + 8), png.begin() + long(at + 8 + n));
        at += 12 + n;
    }
    return out;
}

// APNG 4 x 4: frame 0 opaque red (100 ms); frame 1 a 2 x 2 half-transparent blue square at 1, 1 blended over
// it (20/100 s), then cleared; frame 2 an empty 1 x 1 at 0, 0, plays twice.
Bytes makeApng() {
    auto solid = [](int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        std::vector<uint8_t> px;
        for (int i = 0; i < w * h; ++i) px.insert(px.end(), {r, g, b, a});
        Bytes out;
        stbi_write_png_to_func(appendBytes, &out, w, h, 4, px.data(), w * 4);
        return out;
    };
    const Bytes red = solid(4, 4, 255, 0, 0, 255), blue = solid(2, 2, 0, 0, 255, 128), clear = solid(1, 1, 0, 0, 0, 0);
    Bytes b(red.begin(), red.begin() + 33);  // signature and IHDR
    auto chunk = [&](const char* type, const Bytes& data) {
        const uint32_t n = uint32_t(data.size());
        for (int i = 0; i < 4; ++i) b.push_back(uint8_t(n >> (24 - 8 * i)));
        b.insert(b.end(), type, type + 4);
        b.insert(b.end(), data.begin(), data.end());
        b.insert(b.end(), 4, 0);
    };
    auto u32 = [](Bytes& d, uint32_t v) {
        for (int i = 0; i < 4; ++i) d.push_back(uint8_t(v >> (24 - 8 * i)));
    };
    auto fctl = [&](uint32_t seq, uint32_t w, uint32_t h, uint32_t x, uint32_t y, uint16_t num, uint16_t den, uint8_t dispose,
                    uint8_t blend) {
        Bytes d;
        u32(d, seq);
        u32(d, w);
        u32(d, h);
        u32(d, x);
        u32(d, y);
        d.insert(d.end(), {uint8_t(num >> 8), uint8_t(num), uint8_t(den >> 8), uint8_t(den), dispose, blend});
        chunk("fcTL", d);
    };
    Bytes actl;
    u32(actl, 3);
    u32(actl, 2);
    chunk("acTL", actl);
    fctl(0, 4, 4, 0, 0, 1, 10, 0, 0);
    chunk("IDAT", idatOf(red));
    fctl(1, 2, 2, 1, 1, 20, 100, 1, 1);
    Bytes fdat;
    u32(fdat, 2);
    const Bytes blueData = idatOf(blue);
    fdat.insert(fdat.end(), blueData.begin(), blueData.end());
    chunk("fdAT", fdat);
    fctl(3, 1, 1, 0, 0, 0, 0, 0, 1);
    Bytes fdat2;
    u32(fdat2, 4);
    const Bytes clearData = idatOf(clear);
    fdat2.insert(fdat2.end(), clearData.begin(), clearData.end());
    chunk("fdAT", fdat2);
    chunk("IEND", {});
    return b;
}

void testApng() {
    Bytes apng = makeApng();
    LoadResult r = loadImage(apng.data(), apng.size(), "png");
    CHECK(r.ok && r.image.frames.size() == 3);
    if (r.image.frames.size() != 3) return;
    const Image& img = r.image;
    CHECK(img.loopCount == 2 && img.format == GpuFormat::RGBA8Srgb);
    CHECK(img.frames[0].delayMs == 100 && img.frames[1].delayMs == 200 && img.frames[2].delayMs == 0);
    const uint8_t* f1 = img.frames[1].surface.data.data();
    const uint8_t* p11 = f1 + (1 * 4 + 1) * 4;  // blue at half alpha over red
    CHECK(p11[0] > 120 && p11[0] < 135 && p11[2] > 120 && p11[2] < 135 && p11[3] == 255);
    CHECK(f1[0] == 255 && f1[2] == 0);
    // Frame 1 was cleared after showing: its square is transparent in frame 2, the rest still red.
    const uint8_t* f2 = img.frames[2].surface.data.data();
    CHECK(f2[(1 * 4 + 1) * 4 + 3] == 0 && f2[(3 * 4 + 3) * 4] == 255);
    // A plain PNG stays a single image.
    Bytes png = makePng(4, 4);
    r = loadImage(png.data(), png.size(), "png");
    CHECK(r.ok && r.image.frames.empty());
}

void testGarbage() {
    const uint8_t junk[64] = {1, 2, 3};
    for (const char* ext : {"png", "exr", "dds", "hdr", "webp", "qoi", "jxl", "ktx2", "avif"}) CHECK(!loadImage(junk, sizeof(junk), ext).ok);
}

void testStats() {
    Image img;
    img.width = 4;
    img.height = 1;
    img.format = GpuFormat::RGBA32F;
    const float px[16] = {0, 0, 0, 1, 0.5f, 0.5f, 0.5f, 1, 4, 4, 4, 1, std::nanf(""), 1, 1, 1};
    Surface s;
    s.width = 4;
    s.height = 1;
    s.rowPitch = sizeof(px);
    s.data.assign(reinterpret_cast<const uint8_t*>(px), reinterpret_cast<const uint8_t*>(px) + sizeof(px));
    img.mips.push_back(s);
    const ImageStats st = computeStats(img);
    CHECK(st.valid);
    CHECK(st.pixels == 4 && st.nonFinite == 1 && st.darkPixels == 1);
    CHECK(st.minValue == 0.0f && st.maxValue == 4.0f);
    CHECK(std::fabs(st.minLuminance - 0.5f) < 1e-5f && std::fabs(st.maxLuminance - 4.0f) < 1e-4f);
    CHECK(st.percentile(0.2) == 0.0f);  // a third of the finite pixels are black
    CHECK(st.percentile(1.0) <= 4.0f && st.percentile(0.9) > 2.0f);
    const std::vector<float> h = st.histogramBetween(-2, 3, 5);  // stops -2..3: 0.5 is in bin 1, 4 in bin 4
    CHECK(h.size() == 5 && h[1] == 1.0f && h[4] == 1.0f && h[0] == 0.0f);
    CHECK(!st.channelsValid);

    const ImageStats ch = computeStats(img, true);
    CHECK(ch.channelsValid && ch.percentile(0.2) == st.percentile(0.2));
    CHECK(std::fabs(ch.channel[0].mean - 1.5) < 1e-9 && ch.channel[0].min == 0.0f && ch.channel[0].max == 4.0f);
    CHECK(ch.channel[0].linear[0] == 1 && ch.channel[0].linear[128] == 1 && ch.channel[0].linear[255] == 1);
    uint64_t logged = 0;  // zero is left out of the log bins, NaN out of everything
    for (uint32_t n : ch.channel[1].log) logged += n;
    CHECK(logged == 2);

    // Two regions: an L shape joined through the row below, and a lone pixel; a diagonal does not join.
    const uint8_t mask[4 * 5] = {1, 0, 1, 0, 0,
                                 1, 0, 1, 0, 0,
                                 1, 1, 1, 0, 0,
                                 0, 0, 0, 1, 0};
    const MaskRegions regions = findRegions(mask, 5, 4, 5);
    CHECK(regions.pixels == 8 && regions.regions == 2 && regions.boxes.size() == 2);
    CHECK(regions.boxes[0].x0 == 0 && regions.boxes[0].x1 == 3 && regions.boxes[0].y1 == 3 && regions.boxes[0].pixels == 7);
    CHECK(regions.boxes[1].x0 == 3 && regions.boxes[1].y0 == 3 && regions.boxes[1].pixels == 1);

    Image bc;
    bc.width = bc.height = 4;
    bc.format = GpuFormat::BC7;
    bc.mips.resize(1);
    CHECK(!computeStats(bc).valid);
    CHECK(hasTranslucentPixels(bc) && !hasTranslucentPixels(img));
    reinterpret_cast<float*>(img.mips[0].data.data())[7] = 0.5f;
    CHECK(hasTranslucentPixels(img));
}

bool writeFile(const std::filesystem::path& path, const Bytes& data) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    return bool(f);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--write-samples") {
        std::filesystem::path dir = argv[2];
        std::filesystem::create_directories(dir);
        bool ok = writeFile(dir / "gradient_2048.png", makePng(2048, 2048)) &&
                  writeFile(dir / "gradient_2048.webp", makeWebp(2048, 2048)) &&
                  writeFile(dir / "gradient_2048.qoi", makeQoi(2048, 2048)) &&
#ifdef HTV_HAVE_JXL
                  writeFile(dir / "gradient_2048.jxl", makeJxl(2048, 2048, false)) &&
                  writeFile(dir / "spot_2048_float.jxl", makeJxl(2048, 2048, true)) &&
#endif
#ifdef HTV_HAVE_KTX
                  writeFile(dir / "gradient_2048_uastc.ktx2", makeKtx2(2048, 2048, true)) &&
#endif
#ifdef HTV_HAVE_AVIF
                  std::filesystem::copy_file(std::filesystem::path(HTV_AVIF_TEST_DATA) / "colors_hdr_rec2020.avif",
                                             dir / "colors_hdr_rec2020.avif",
                                             std::filesystem::copy_options::overwrite_existing) &&
                  std::filesystem::copy_file(std::filesystem::path(HTV_AVIF_TEST_DATA) / "colors_sdr_srgb.avif",
                                             dir / "colors_sdr_srgb.avif",
                                             std::filesystem::copy_options::overwrite_existing) &&
#endif
                  writeFile(dir / "spot_2048.hdr", makeHdr(2048, 2048)) &&
                  writeFile(dir / "spot_4096_half.exr", makeExr(4096, 4096, true)) &&
                  writeFile(dir / "spot_2048_float.exr", makeExr(2048, 2048, false)) &&
                  writeFile(dir / "blocks_4096_bc1.dds", makeDds(4096, 4096, false)) &&
                  writeFile(dir / "blocks_4096_bc7.dds", makeDds(4096, 4096, true));
        std::printf(ok ? "samples written to %s\n" : "failed to write samples to %s\n", argv[2]);
        return ok ? 0 : 1;
    }
    testHalf();
    testNormalHint();
    testPng();
    testWebp();
    testQoi();
    testJxl();
    testKtx();
    testAvif();
    testHdrTransfer();
    testHdr();
    testExr();
    testDds();
    testDdsLayers();
    testPsdLayers();
    testMetadata();
    testApng();
    testGarbage();
    testStats();
    std::printf(failures ? "%d check(s) failed\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
