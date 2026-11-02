// Animated PNG. stb_image reads a PNG's default image; this decodes every APNG frame by wrapping its data in a
// PNG of its own for stb_image, and composites the frames onto the canvas with their dispose and blend
// operations, the way browsers play them.

#include "codecs.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "stb_image.h"

namespace htv {
namespace {

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

void put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (24 - 8 * i)));
}

// stb_image does not check CRCs, so the chunks it gets here carry none.
void putChunk(std::vector<uint8_t>& out, const char* type, const uint8_t* data, size_t size) {
    put32(out, uint32_t(size));
    out.insert(out.end(), type, type + 4);
    if (size) out.insert(out.end(), data, data + size);
    put32(out, 0);
}

struct ApngFrame {
    uint32_t width = 0, height = 0, x = 0, y = 0;
    int delayMs = 0;
    uint8_t dispose = 0;  // 0 none, 1 clear to transparent, 2 back to the previous canvas
    uint8_t blend = 0;    // 0 replace, 1 over
    std::vector<uint8_t> data;  // the frame's zlib stream: IDAT or fdAT contents
};

}  // namespace

void readApngFrames(const uint8_t* data, size_t size, Image& img) {
    if (size < 8 + 25 || std::memcmp(data, "\x89PNG\r\n\x1a\n", 8) != 0) return;
    std::vector<uint8_t> ihdr, preamble;  // IHDR, and the chunks every frame needs (PLTE, tRNS...)
    std::vector<ApngFrame> frames;
    uint32_t declared = 0, plays = 0;
    bool seenData = false;
    size_t at = 8;
    while (at + 12 <= size) {
        const size_t length = be32(data + at);
        if (length > size - at - 12) break;
        const char* type = reinterpret_cast<const char*>(data + at + 4);
        const uint8_t* d = data + at + 8;
        if (std::memcmp(type, "IHDR", 4) == 0 && length == 13) {
            ihdr.assign(d, d + 13);
        } else if (std::memcmp(type, "acTL", 4) == 0 && length >= 8) {
            declared = be32(d);
            plays = be32(d + 4);
        } else if (std::memcmp(type, "fcTL", 4) == 0 && length >= 26) {
            ApngFrame f;
            f.width = be32(d + 4);
            f.height = be32(d + 8);
            f.x = be32(d + 12);
            f.y = be32(d + 16);
            const int num = be16(d + 20), den = be16(d + 22);
            f.delayMs = num * 1000 / (den ? den : 100);
            f.dispose = d[24];
            f.blend = d[25];
            frames.push_back(std::move(f));
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            seenData = true;
            if (!frames.empty()) frames.back().data.insert(frames.back().data.end(), d, d + length);  // the default image is frame 0
        } else if (std::memcmp(type, "fdAT", 4) == 0 && length > 4) {
            if (!frames.empty()) frames.back().data.insert(frames.back().data.end(), d + 4, d + length);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            break;
        } else if (!seenData && std::memcmp(type, "IHDR", 4) != 0) {
            preamble.insert(preamble.end(), data + at, data + at + 12 + length);
        }
        at += 12 + length;
    }
    if (declared < 2 || frames.size() < 2 || ihdr.empty()) return;
    const uint32_t width = be32(ihdr.data()), height = be32(ihdr.data() + 4);
    if (width != img.width || height != img.height || uint64_t(width) * height * 4 * frames.size() >= (uint64_t(1) << 31))
        return;

    std::vector<uint8_t> canvas(size_t(width) * height * 4, 0), previous;
    std::vector<Frame> out;
    for (size_t i = 0; i < frames.size(); ++i) {
        const ApngFrame& f = frames[i];
        if (f.width == 0 || f.height == 0 || f.x + uint64_t(f.width) > width || f.y + uint64_t(f.height) > height ||
            f.data.empty())
            break;
        // A PNG of just this frame.
        std::vector<uint8_t> png(data, data + 8);
        std::vector<uint8_t> header = ihdr;
        for (int k = 0; k < 4; ++k) {
            header[k] = uint8_t(f.width >> (24 - 8 * k));
            header[4 + k] = uint8_t(f.height >> (24 - 8 * k));
        }
        putChunk(png, "IHDR", header.data(), header.size());
        png.insert(png.end(), preamble.begin(), preamble.end());
        putChunk(png, "IDAT", f.data.data(), f.data.size());
        putChunk(png, "IEND", nullptr, 0);
        int w = 0, h = 0, n = 0;
        stbi_uc* px = stbi_load_from_memory(png.data(), int(png.size()), &w, &h, &n, 4);
        if (!px || uint32_t(w) != f.width || uint32_t(h) != f.height) {
            if (px) stbi_image_free(px);
            break;
        }
        if (f.dispose == 2) previous = canvas;
        for (uint32_t y = 0; y < f.height; ++y)
            for (uint32_t x = 0; x < f.width; ++x) {
                const uint8_t* src = px + (size_t(y) * f.width + x) * 4;
                uint8_t* dst = canvas.data() + (size_t(f.y + y) * width + f.x + x) * 4;
                if (f.blend == 0 || src[3] == 255) {
                    std::memcpy(dst, src, 4);
                } else if (src[3] > 0) {
                    const float sa = src[3] / 255.0f, da = dst[3] / 255.0f;
                    const float oa = sa + da * (1.0f - sa);
                    for (int c = 0; c < 3; ++c) dst[c] = uint8_t((src[c] * sa + dst[c] * da * (1.0f - sa)) / oa + 0.5f);
                    dst[3] = uint8_t(oa * 255.0f + 0.5f);
                }
            }
        stbi_image_free(px);
        Frame frame;
        frame.surface.width = width;
        frame.surface.height = height;
        frame.surface.rowPitch = width * 4;
        frame.surface.data = canvas;
        frame.delayMs = f.delayMs;
        out.push_back(std::move(frame));
        if (f.dispose == 1 || (f.dispose == 2 && i == 0)) {  // the first frame has no previous canvas to go back to
            for (uint32_t y = 0; y < f.height; ++y)
                std::memset(canvas.data() + (size_t(f.y + y) * width + f.x) * 4, 0, size_t(f.width) * 4);
        } else if (f.dispose == 2) {
            canvas = previous;
        }
    }
    if (out.size() < 2) return;
    img.format = GpuFormat::RGBA8Srgb;
    img.mips.clear();
    img.mips.push_back(out[0].surface);
    img.frames = std::move(out);
    img.loopCount = int(plays);
    img.description = "APNG, 8 bit RGBA, " + std::to_string(img.frames.size()) + " frames";
    if (img.frames.size() < declared)
        img.note = std::to_string(img.frames.size()) + " of " + std::to_string(declared) + " frames read";
}

}  // namespace htv
