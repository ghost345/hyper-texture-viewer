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

#ifdef HTV_HAVE_JXL
// Lossless JPEG XL: 8-bit sRGB from testPixels, or 32-bit float linear from hdrPixels.
Bytes makeJxl(int w, int h, bool hdr) {
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
    if (hdr)
        JxlEncoderAddImageFrame(settings, &format, f.data(), f.size() * sizeof(float));
    else
        JxlEncoderAddImageFrame(settings, &format, b.data(), b.size());
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
    }
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
    testGarbage();
    testStats();
    std::printf(failures ? "%d check(s) failed\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
