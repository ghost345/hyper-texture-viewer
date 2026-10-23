// Loader tests. Builds small images in memory, decodes them and checks the result.
// `htv_tests --write-samples <dir>` also saves the same images as files for the startup benchmark.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "image.h"
#include "stb_image_write.h"
#include "tinyexr.h"

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

Bytes makePng(int w, int h) {
    std::vector<uint8_t> px(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &px[(size_t(y) * w + x) * 4];
            p[0] = uint8_t(x * 255 / (w - 1));
            p[1] = uint8_t(y * 255 / (h - 1));
            p[2] = ((x / 8 + y / 8) & 1) ? 255 : 0;
            p[3] = uint8_t(255 - x * 255 / (w - 1));
        }
    Bytes out;
    stbi_write_png_to_func(appendBytes, &out, w, h, 4, px.data(), w * 4);
    return out;
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

// DDS with a full mip chain of BC1 blocks (legacy header) or BC7 blocks (DX10 header).
Bytes makeDds(uint32_t w, uint32_t h, bool bc7) {
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
        put32(b, 128, uint32_t(GpuFormat::BC7Srgb));
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

    bc7.resize(150);
    r = loadImage(bc7.data(), bc7.size(), "dds");
    CHECK(!r.ok);
}

void testGarbage() {
    const uint8_t junk[64] = {1, 2, 3};
    for (const char* ext : {"png", "exr", "dds", "hdr"}) CHECK(!loadImage(junk, sizeof(junk), ext).ok);
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
                  writeFile(dir / "spot_2048.hdr", makeHdr(2048, 2048)) &&
                  writeFile(dir / "spot_4096_half.exr", makeExr(4096, 4096, true)) &&
                  writeFile(dir / "spot_2048_float.exr", makeExr(2048, 2048, false)) &&
                  writeFile(dir / "blocks_4096_bc1.dds", makeDds(4096, 4096, false)) &&
                  writeFile(dir / "blocks_4096_bc7.dds", makeDds(4096, 4096, true));
        std::printf(ok ? "samples written to %s\n" : "failed to write samples to %s\n", argv[2]);
        return ok ? 0 : 1;
    }
    testHalf();
    testPng();
    testHdr();
    testExr();
    testDds();
    testGarbage();
    std::printf(failures ? "%d check(s) failed\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
