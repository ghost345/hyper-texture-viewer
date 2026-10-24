// Photoshop layers. stb_image reads a PSD's own composite; this adds the layer records and their pixels so the
// Layers panel can hide and show them, and composites the visible ones again when it does.

#include "codecs.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "stb_image.h"  // its zlib inflater, for ZIP-compressed channels

namespace htv {
namespace {

// Big-endian reads that stop at the end of the data: a short read sets `bad` and returns 0.
struct Reader {
    const uint8_t* p = nullptr;
    size_t size = 0;
    size_t pos = 0;
    bool bad = false;

    bool has(size_t n) const { return !bad && n <= size - std::min(pos, size); }
    uint8_t u8() {
        if (!has(1)) return fail();
        return p[pos++];
    }
    uint16_t u16() {
        if (!has(2)) return fail();
        const uint16_t v = uint16_t(p[pos] << 8 | p[pos + 1]);
        pos += 2;
        return v;
    }
    uint32_t u32() {
        if (!has(4)) return fail();
        const uint32_t v = uint32_t(p[pos]) << 24 | uint32_t(p[pos + 1]) << 16 | uint32_t(p[pos + 2]) << 8 | p[pos + 3];
        pos += 4;
        return v;
    }
    int32_t i32() { return int32_t(u32()); }
    void skip(size_t n) {
        if (!has(n)) fail();
        else pos += n;
    }
    void seek(size_t to) {
        if (to > size) fail();
        else pos = to;
    }
    uint8_t fail() {
        bad = true;
        pos = size;
        return 0;
    }
};

constexpr uint32_t tag(const char* s) {
    return uint32_t(uint8_t(s[0])) << 24 | uint32_t(uint8_t(s[1])) << 16 | uint32_t(uint8_t(s[2])) << 8 | uint8_t(s[3]);
}

struct Channel {
    int id = 0;          // 0 R (or grey), 1 G, 2 B, -1 alpha, -2 layer mask, -3 vector-based mask
    uint32_t length = 0;  // compressed data, including the compression method
    std::vector<uint8_t> plane;  // decoded, big-endian samples
};

struct Record {
    int top = 0, left = 0, bottom = 0, right = 0;
    std::vector<Channel> channels;
    uint32_t blend = 0;
    uint8_t opacity = 255;
    bool clipped = false;
    bool hidden = false;
    int maskTop = 0, maskLeft = 0, maskBottom = 0, maskRight = 0;
    uint8_t maskDefault = 255;
    bool maskOn = false;
    std::string name;
    uint32_t section = 0;  // 1 or 2: a group (open, closed); 3: the hidden end-of-group marker
};

std::string utf16beToUtf8(const uint8_t* p, size_t count) {
    std::string out;
    for (size_t i = 0; i < count; ++i) {
        uint32_t c = uint32_t(p[i * 2] << 8 | p[i * 2 + 1]);
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < count) {
            const uint32_t lo = uint32_t(p[i * 2 + 2] << 8 | p[i * 2 + 3]);
            if (lo >= 0xDC00 && lo < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        if (c == 0) break;
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | c >> 6);
            out += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += char(0xE0 | c >> 12);
            out += char(0x80 | (c >> 6 & 0x3F));
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xF0 | c >> 18);
            out += char(0x80 | (c >> 12 & 0x3F));
            out += char(0x80 | (c >> 6 & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// Pascal names are in the system code page; anything past ASCII is dropped (the 'luni' name usually replaces it).
std::string asciiName(const uint8_t* p, size_t n) {
    std::string out;
    for (size_t i = 0; i < n; ++i) out += p[i] < 0x80 ? char(p[i]) : '?';
    return out;
}

bool readRecords(Reader& r, size_t end, std::vector<Record>& out) {
    int count = int16_t(r.u16());
    count = std::abs(count);  // negative: the first alpha channel is the merged transparency
    if (count == 0 || count > 8000) return false;
    for (int i = 0; i < count && !r.bad; ++i) {
        Record rec;
        rec.top = r.i32();
        rec.left = r.i32();
        rec.bottom = r.i32();
        rec.right = r.i32();
        const int n = r.u16();
        if (n > 56) return false;
        for (int c = 0; c < n; ++c) {
            Channel ch;
            ch.id = int16_t(r.u16());
            ch.length = r.u32();
            rec.channels.push_back(ch);
        }
        if (r.u32() != tag("8BIM")) return false;
        rec.blend = r.u32();
        rec.opacity = r.u8();
        rec.clipped = r.u8() != 0;
        rec.hidden = (r.u8() & 0x02) != 0;
        r.skip(1);
        const uint32_t extra = r.u32();
        const size_t extraEnd = r.pos + extra;
        if (extraEnd > end) return false;

        const uint32_t maskSize = r.u32();
        const size_t maskEnd = r.pos + maskSize;
        if (maskSize >= 20) {
            rec.maskTop = r.i32();
            rec.maskLeft = r.i32();
            rec.maskBottom = r.i32();
            rec.maskRight = r.i32();
            rec.maskDefault = r.u8();
            rec.maskOn = (r.u8() & 0x02) == 0;  // bit 1: the mask is turned off
        }
        r.seek(maskEnd);
        r.skip(r.u32());  // blending ranges
        const uint8_t nameLength = r.u8();
        if (r.has(nameLength)) rec.name = asciiName(r.p + r.pos, nameLength);
        r.skip(((nameLength + 1 + 3) & ~3) - 1);  // padded to 4 bytes with the length byte

        // Additional layer information: the Unicode name and the group markers.
        while (!r.bad && r.pos + 12 <= extraEnd) {
            const uint32_t sig = r.u32();
            if (sig != tag("8BIM") && sig != tag("8B64")) break;
            const uint32_t key = r.u32();
            const uint32_t length = r.u32();
            const size_t dataEnd = r.pos + length;
            if (dataEnd > extraEnd) break;
            if (key == tag("luni") && length >= 4) {
                const uint32_t chars = r.u32();
                if (chars > 0 && size_t(chars) * 2 <= length - 4) rec.name = utf16beToUtf8(r.p + r.pos, chars);
            } else if ((key == tag("lsct") || key == tag("lsdk")) && length >= 4) {
                rec.section = r.u32();
            }
            r.seek(dataEnd);
        }
        r.seek(extraEnd);
        out.push_back(std::move(rec));
    }
    return !r.bad;
}

// PackBits, one row at a time.
bool unpackBits(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstSize) {
    size_t i = 0, o = 0;
    while (o < dstSize) {
        if (i >= srcSize) return false;
        const int n = int8_t(src[i++]);
        if (n >= 0) {
            const size_t count = size_t(n) + 1;
            if (i + count > srcSize || o + count > dstSize) return false;
            std::memcpy(dst + o, src + i, count);
            i += count;
            o += count;
        } else if (n != -128) {
            const size_t count = size_t(1 - n);
            if (i >= srcSize || o + count > dstSize) return false;
            std::memset(dst + o, src[i++], count);
            o += count;
        }
    }
    return true;
}

// Decodes a channel of `rows` x `cols` samples of `bytes` each, from `data` (its compression method first).
bool decodeChannel(const uint8_t* data, size_t size, int rows, int cols, int bytes, std::vector<uint8_t>& plane) {
    const size_t rowBytes = size_t(cols) * bytes;
    plane.assign(rowBytes * rows, 0);
    if (size < 2) return false;
    const int method = data[0] << 8 | data[1];
    data += 2;
    size -= 2;
    if (method == 0) {
        if (size < plane.size()) return false;
        std::memcpy(plane.data(), data, plane.size());
        return true;
    }
    if (method == 1) {
        if (size < size_t(rows) * 2) return false;
        size_t at = size_t(rows) * 2;
        for (int y = 0; y < rows; ++y) {
            const size_t n = size_t(data[y * 2] << 8 | data[y * 2 + 1]);
            if (at + n > size || !unpackBits(data + at, n, plane.data() + rowBytes * y, rowBytes)) return false;
            at += n;
        }
        return true;
    }
    if (method == 2 || method == 3) {
        int outLength = 0;
        char* out = stbi_zlib_decode_malloc_guesssize_headerflag(reinterpret_cast<const char*>(data), int(size),
                                                                 int(plane.size()), &outLength, 1);
        if (!out) return false;
        const bool whole = size_t(outLength) >= plane.size();
        if (whole) std::memcpy(plane.data(), out, plane.size());
        stbi_image_free(out);
        if (!whole) return false;
        if (method == 3) {  // each sample stored as the difference from the one before it in the row
            for (int y = 0; y < rows; ++y) {
                uint8_t* row = plane.data() + rowBytes * y;
                if (bytes == 1) {
                    for (int x = 1; x < cols; ++x) row[x] = uint8_t(row[x] + row[x - 1]);
                } else if (bytes == 2) {
                    for (int x = 1; x < cols; ++x) {
                        const int v = (row[x * 2] << 8 | row[x * 2 + 1]) + (row[x * 2 - 2] << 8 | row[x * 2 - 1]);
                        row[x * 2] = uint8_t(v >> 8);
                        row[x * 2 + 1] = uint8_t(v);
                    }
                } else {
                    return false;
                }
            }
        }
        return true;
    }
    return false;
}

float sampleAt(const Layer& l, bool sixteen, int cx, int cy, int c) {
    const int x = cx - l.x, y = cy - l.y;
    if (l.mips.empty() || x < 0 || y < 0 || x >= int(l.width) || y >= int(l.height)) return 0.0f;
    const Surface& s = l.mips[0];
    if (sixteen) {
        uint16_t v;
        std::memcpy(&v, s.data.data() + size_t(y) * s.rowPitch + size_t(x) * 8 + c * 2, 2);
        return v / 65535.0f;
    }
    return s.data[size_t(y) * s.rowPitch + size_t(x) * 4 + c] / 255.0f;
}

}  // namespace

void readPsdLayers(const uint8_t* data, size_t size, Image& img) {
    Reader r{data, size};
    if (size < 26 || std::memcmp(data, "8BPS", 4) != 0) return;
    r.pos = 4;
    if (r.u16() != 1) return;  // PSB (large documents) lay out their lengths differently
    r.skip(6);
    r.u16();  // channels
    const uint32_t height = r.u32(), width = r.u32();
    const int depth = r.u16(), mode = r.u16();
    if (width != img.width || height != img.height) return;

    r.skip(r.u32());  // colour mode data
    // Image resources: the print resolution.
    const uint32_t resourcesLength = r.u32();
    const size_t resourcesEnd = r.pos + resourcesLength;
    while (!r.bad && r.pos + 12 <= resourcesEnd) {
        if (r.u32() != tag("8BIM")) break;
        const int id = r.u16();
        const uint8_t nameLength = r.u8();
        r.skip(nameLength + ((nameLength + 1) & 1));
        const uint32_t length = r.u32();
        const size_t next = r.pos + length + (length & 1);
        if (id == 0x03ED && length >= 16 && img.dpi == 0.0f) {
            const float h = float(r.u32()) / 65536.0f;
            const int unit = r.u16();
            img.dpi = unit == 2 ? h * 2.54f : h;
        }
        r.seek(next);
    }
    r.seek(resourcesEnd);
    if (r.bad || (mode != 1 && mode != 3) || (depth != 8 && depth != 16)) return;

    // Layer and mask information. 16-bit files keep the layers in an 'Lr16' block after the global mask.
    const uint32_t sectionLength = r.u32();
    const size_t sectionEnd = r.pos + sectionLength;
    if (sectionLength == 0 || sectionEnd > size) return;
    std::vector<Record> records;
    const uint32_t infoLength = r.u32();
    size_t channelsAt = 0, channelsEnd = 0;
    if (infoLength > 0) {
        const size_t infoEnd = r.pos + infoLength;
        if (infoEnd > sectionEnd || !readRecords(r, infoEnd, records)) return;
        channelsAt = r.pos;
        channelsEnd = infoEnd;
    } else {
        r.skip(r.u32());  // global layer mask
        while (!r.bad && r.pos + 12 <= sectionEnd) {
            const uint32_t sig = r.u32();
            if (sig != tag("8BIM") && sig != tag("8B64")) break;
            const uint32_t key = r.u32();
            const uint32_t length = r.u32();
            const size_t blockEnd = r.pos + length;
            if (blockEnd > sectionEnd) break;
            if (key == tag("Lr16") || key == tag("Lr32")) {
                if (key == tag("Lr32") || !readRecords(r, blockEnd, records)) return;
                channelsAt = r.pos;
                channelsEnd = blockEnd;
                break;
            }
            r.seek(blockEnd + (length & 3 ? 4 - (length & 3) : 0));
        }
    }
    if (records.empty()) return;

    // Channel data, layer by layer in the records' order.
    const int bytes = depth / 8;
    size_t at = channelsAt;
    for (Record& rec : records) {
        for (Channel& ch : rec.channels) {
            if (at + ch.length > channelsEnd) return;
            const bool mask = ch.id == -2;
            const int rows = mask ? rec.maskBottom - rec.maskTop : rec.bottom - rec.top;
            const int cols = mask ? rec.maskRight - rec.maskLeft : rec.right - rec.left;
            if (rows > 0 && cols > 0 && rows <= 300000 && cols <= 300000 && ch.id >= -2 &&
                size_t(rows) * size_t(cols) <= size_t(1) << 28)
                if (!decodeChannel(data + at, ch.length, rows, cols, bytes, ch.plane)) ch.plane.clear();
            at += ch.length;
        }
    }

    // Records go bottom to top; a group's record comes after its layers, with a hidden marker before them.
    const GpuFormat format = depth == 16 ? GpuFormat::RGBA16Unorm : GpuFormat::RGBA8Srgb;
    std::vector<Layer> layers;
    std::vector<int> groups;  // open groups, innermost last
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        const Record& rec = *it;
        if (rec.section == 3) {
            if (!groups.empty()) groups.pop_back();
            continue;
        }
        Layer l;
        l.name = rec.name;
        l.opacity = rec.opacity / 255.0f;
        l.visible = !rec.hidden;
        l.visibleInFile = l.visible;
        l.parent = groups.empty() ? -1 : groups.back();
        l.depth = int(groups.size());
        l.blend.assign({char(rec.blend >> 24), char(rec.blend >> 16), char(rec.blend >> 8), char(rec.blend)});
        l.clipped = rec.clipped;
        l.format = format;
        if (rec.section == 1 || rec.section == 2) {
            l.group = true;
            layers.push_back(std::move(l));
            groups.push_back(int(layers.size()) - 1);
            continue;
        }
        const int w = rec.right - rec.left, h = rec.bottom - rec.top;
        const Channel* planes[4] = {};
        const Channel* mask = nullptr;
        for (const Channel& ch : rec.channels) {
            if (ch.plane.empty()) continue;
            if (ch.id >= 0 && ch.id < 3) planes[ch.id] = &ch;
            else if (ch.id == -1) planes[3] = &ch;
            else if (ch.id == -2 && rec.maskOn) mask = &ch;
        }
        if (w > 0 && h > 0 && planes[0]) {
            l.x = rec.left;
            l.y = rec.top;
            l.width = uint32_t(w);
            l.height = uint32_t(h);
            Surface s;
            s.width = l.width;
            s.height = l.height;
            s.rowPitch = l.width * 4 * bytes;
            s.data.resize(size_t(s.rowPitch) * h);
            const int mw = rec.maskRight - rec.maskLeft;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const size_t i = size_t(y) * w + x;
                    uint32_t v[4];
                    for (int c = 0; c < 4; ++c) {
                        const Channel* ch = planes[mode == 1 && c < 3 ? 0 : c];
                        const uint32_t full = bytes == 2 ? 65535 : 255;
                        if (!ch) v[c] = full;
                        else if (bytes == 2) v[c] = uint32_t(ch->plane[i * 2] << 8 | ch->plane[i * 2 + 1]);
                        else v[c] = ch->plane[i];
                    }
                    if (mask) {  // the layer mask fades alpha; outside its rectangle it has its default value
                        const int mx = rec.left + x - rec.maskLeft, my = rec.top + y - rec.maskTop;
                        uint32_t m = bytes == 2 ? rec.maskDefault * 257u : rec.maskDefault;
                        if (mx >= 0 && my >= 0 && mx < mw && my < rec.maskBottom - rec.maskTop) {
                            const size_t j = size_t(my) * mw + mx;
                            m = bytes == 2 ? uint32_t(mask->plane[j * 2] << 8 | mask->plane[j * 2 + 1]) : mask->plane[j];
                        }
                        v[3] = v[3] * m / (bytes == 2 ? 65535u : 255u);
                    }
                    if (bytes == 2) {
                        uint16_t* o = reinterpret_cast<uint16_t*>(s.data.data() + size_t(y) * s.rowPitch) + x * 4;
                        for (int c = 0; c < 4; ++c) o[c] = uint16_t(v[c]);
                    } else {
                        uint8_t* o = s.data.data() + size_t(y) * s.rowPitch + x * 4;
                        for (int c = 0; c < 4; ++c) o[c] = uint8_t(v[c]);
                    }
                }
            l.mips.push_back(std::move(s));
        }
        layers.push_back(std::move(l));
    }
    if (layers.empty()) return;
    img.layers = std::move(layers);
    img.layerKind = LayerKind::PsdLayers;
    img.fileComposite = img.mips[0];

    // Without "Maximize compatibility" Photoshop saves a blank composite: build it from the layers instead.
    const Surface& c = img.mips[0];
    const size_t px = size_t(4) * bytes;
    bool uniform = true;
    for (size_t i = px; i < c.data.size() && uniform; i += px) uniform = std::memcmp(&c.data[i], &c.data[0], px) == 0;
    bool content = false;
    for (int i = 0; i < int(img.layers.size()) && !content; ++i)
        content = !img.layers[size_t(i)].mips.empty() && layerShows(img, i);
    if (uniform && content) {
        img.fileComposite = Surface();
        compositeLayers(img);
    }
}

bool compositeLayers(Image& img) {
    if (img.layerKind != LayerKind::PsdLayers || img.mips.empty()) return false;
    bool asSaved = !img.fileComposite.data.empty();
    for (const Layer& l : img.layers) asSaved = asSaved && l.visible == l.visibleInFile;
    if (asSaved) {  // the file's own composite has the blend modes and effects right
        img.mips[0] = img.fileComposite;
        img.composited = false;
        return true;
    }
    const bool sixteen = img.format == GpuFormat::RGBA16Unorm;
    const uint32_t w = img.width, h = img.height;
    // The layers that show, bottom first, with their opacity inside their groups and what they clip to.
    struct Draw {
        const Layer* layer;
        float opacity;
        const Layer* clip;
    };
    std::vector<Draw> draws;
    int base = -1;  // the layer a clipping mask clips to
    for (int i = int(img.layers.size()) - 1; i >= 0; --i) {
        const Layer& l = img.layers[size_t(i)];
        if (l.group) continue;
        if (!l.clipped) base = i;
        if (l.mips.empty() || !layerShows(img, i)) continue;
        float opacity = l.opacity;
        for (int g = l.parent; g >= 0; g = img.layers[size_t(g)].parent) opacity *= img.layers[size_t(g)].opacity;
        const Layer* clip = l.clipped && base >= 0 && base != i ? &img.layers[size_t(base)] : nullptr;
        if (clip && !layerShows(img, base)) continue;
        draws.push_back({&l, opacity, clip});
    }
    // Row by row, so the working copy stays one row of floats however large the canvas.
    std::vector<float> row(size_t(w) * 4);
    Surface& s = img.mips[0];
    for (uint32_t y = 0; y < h; ++y) {
        std::fill(row.begin(), row.end(), 0.0f);
        for (const Draw& d : draws) {
            const Layer& l = *d.layer;
            if (int(y) < l.y || int(y) >= l.y + int(l.height)) continue;
            const int x0 = std::max(0, l.x), x1 = std::min(int(w), l.x + int(l.width));
            for (int x = x0; x < x1; ++x) {
                float a = sampleAt(l, sixteen, x, int(y), 3) * d.opacity;
                if (d.clip) a *= sampleAt(*d.clip, sixteen, x, int(y), 3);
                if (a <= 0.0f) continue;
                float* o = &row[size_t(x) * 4];
                const float outA = a + o[3] * (1.0f - a);
                for (int c = 0; c < 3; ++c) o[c] = (sampleAt(l, sixteen, x, int(y), c) * a + o[c] * o[3] * (1.0f - a)) / outA;
                o[3] = outA;
            }
        }
        for (uint32_t x = 0; x < w; ++x) {
            const float* v = &row[size_t(x) * 4];
            if (sixteen) {
                uint16_t* o = reinterpret_cast<uint16_t*>(s.data.data() + size_t(y) * s.rowPitch) + x * 4;
                for (int c = 0; c < 4; ++c) o[c] = uint16_t(std::clamp(v[c], 0.0f, 1.0f) * 65535.0f + 0.5f);
            } else {
                uint8_t* o = s.data.data() + size_t(y) * s.rowPitch + x * 4;
                for (int c = 0; c < 4; ++c) o[c] = uint8_t(std::clamp(v[c], 0.0f, 1.0f) * 255.0f + 0.5f);
            }
        }
    }
    img.composited = true;
    return true;
}

}  // namespace htv
