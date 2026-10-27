// File metadata for the Info panel: print resolution, colour profile, EXIF (camera, exposure, software...)
// and text chunks, read from the bytes of JPEG, PNG, WebP and PSD files. The Windows-codec, AVIF and JPEG XL
// loaders hand their EXIF and ICC blocks to the same parsers.

#include "codecs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "stb_image.h"  // zlib, for compressed PNG profiles and text

namespace htv {
namespace {

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]; }

// Printable text: control characters become spaces, trailing spaces and NULs go, and it stays short.
std::string clean(const uint8_t* p, size_t n, size_t maxLength = 200) {
    std::string s;
    for (size_t i = 0; i < n && p[i]; ++i) s += p[i] < 0x20 ? ' ' : char(p[i]);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    size_t start = s.find_first_not_of(' ');
    s = start == std::string::npos ? std::string() : s.substr(start);
    if (s.size() > maxLength) s = s.substr(0, maxLength) + "\xE2\x80\xA6";
    return s;
}

std::string utf16be(const uint8_t* p, size_t chars) {
    std::string out;
    for (size_t i = 0; i < chars; ++i) {
        const uint32_t c = be16(p + i * 2);
        if (c == 0) break;
        if (c < 0x80) out += char(c);
        else if (c < 0x800) {
            out += char(0xC0 | c >> 6);
            out += char(0x80 | (c & 0x3F));
        } else if (c < 0xD800 || c >= 0xE000) {
            out += char(0xE0 | c >> 12);
            out += char(0x80 | (c >> 6 & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::string number(double v) {
    char t[32];
    std::snprintf(t, sizeof t, "%g", v);
    return t;
}

void add(Image& img, const char* key, const std::string& value) {
    if (value.empty()) return;
    for (const Property& p : img.metadata)
        if (p.key == key) return;  // the first source wins
    img.metadata.push_back({key, value});
}

std::vector<uint8_t> inflate(const uint8_t* p, size_t n) {
    int length = 0;
    char* out = stbi_zlib_decode_malloc_guesssize_headerflag(reinterpret_cast<const char*>(p), int(n), 4096, &length, 1);
    if (!out) return {};
    std::vector<uint8_t> v(out, out + length);
    stbi_image_free(out);
    return v;
}

// ---- EXIF: a TIFF structure ----

struct Tiff {
    const uint8_t* p;
    size_t size;
    bool little;
    uint16_t u16(size_t at) const { return at + 2 <= size ? (little ? uint16_t(p[at] | p[at + 1] << 8) : be16(p + at)) : 0; }
    uint32_t u32(size_t at) const { return at + 4 <= size ? (little ? le32(p + at) : be32(p + at)) : 0; }
};

struct ExifValues {
    std::string make, model, software, artist, copyright, description, taken, modified, lens;
    double exposure = 0, fnumber = 0, focal = 0, xres = 0;
    int iso = 0, orientation = 0, resUnit = 2, colorSpace = 0;
};

void readIfd(const Tiff& t, size_t ifd, ExifValues& v, int depth) {
    if (depth > 2 || ifd + 2 > t.size) return;
    const int count = t.u16(ifd);
    for (int i = 0; i < count && i < 512; ++i) {
        const size_t e = ifd + 2 + size_t(i) * 12;
        if (e + 12 > t.size) return;
        const uint16_t tag = t.u16(e), type = t.u16(e + 2);
        const uint32_t n = t.u32(e + 4);
        static const int kSizes[13] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8};
        const size_t bytes = type < 13 ? size_t(kSizes[type]) * n : 0;
        const size_t at = bytes <= 4 ? e + 8 : t.u32(e + 8);
        if (bytes == 0 || at + bytes > t.size) continue;
        auto text = [&] { return clean(t.p + at, bytes); };
        auto rational = [&] {
            const uint32_t d = t.u32(at + 4);
            return d ? double(t.u32(at)) / d : 0.0;
        };
        auto integer = [&] { return type == 3 ? int(t.u16(at)) : int(t.u32(at)); };
        switch (tag) {
            case 0x010E: if (type == 2) v.description = text(); break;
            case 0x010F: if (type == 2) v.make = text(); break;
            case 0x0110: if (type == 2) v.model = text(); break;
            case 0x0112: v.orientation = integer(); break;
            case 0x011A: if (type == 5) v.xres = rational(); break;
            case 0x0128: v.resUnit = integer(); break;
            case 0x0131: if (type == 2) v.software = text(); break;
            case 0x0132: if (type == 2) v.modified = text(); break;
            case 0x013B: if (type == 2) v.artist = text(); break;
            case 0x8298: if (type == 2) v.copyright = text(); break;
            case 0x829A: if (type == 5) v.exposure = rational(); break;
            case 0x829D: if (type == 5) v.fnumber = rational(); break;
            case 0x8827: v.iso = integer(); break;
            case 0x9003: if (type == 2) v.taken = text(); break;
            case 0x920A: if (type == 5) v.focal = rational(); break;
            case 0xA001: v.colorSpace = integer(); break;
            case 0xA434: if (type == 2) v.lens = text(); break;
            case 0x8769: readIfd(t, t.u32(at), v, depth + 1); break;  // the Exif sub-IFD
            default: break;
        }
    }
}

// ---- ICC profiles ----

void readIccInto(const uint8_t* p, size_t n, Image& img) {
    const std::string d = iccDescription(p, n);
    if (d.empty()) return;
    // An embedded profile says more than EXIF's colour space tag.
    auto& m = img.metadata;
    m.erase(std::remove_if(m.begin(), m.end(), [](const Property& p) {
                return p.key == "Color profile" && p.value.find("(EXIF)") != std::string::npos;
            }),
            m.end());
    add(img, "Color profile", d);
}

// ---- containers ----

void readJpeg(const uint8_t* p, size_t n, Image& img) {
    std::vector<uint8_t> icc;
    std::string encoding;
    size_t at = 2;
    while (at + 4 <= n && p[at] == 0xFF) {
        const uint8_t marker = p[at + 1];
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
            at += 2;
            continue;
        }
        const size_t length = be16(p + at + 2);
        if (length < 2 || at + 2 + length > n) break;
        const uint8_t* d = p + at + 4;
        const size_t dn = length - 2;
        if (marker == 0xE0 && dn >= 12 && std::memcmp(d, "JFIF", 5) == 0) {
            const int units = d[7];
            const double x = be16(d + 8);
            if (units == 1 && x > 1 && img.dpi == 0.0f) img.dpi = float(x);
            if (units == 2 && x > 1 && img.dpi == 0.0f) img.dpi = float(x * 2.54);
        } else if (marker == 0xE1 && dn > 6 && std::memcmp(d, "Exif\0\0", 6) == 0) {
            readExif(d + 6, dn - 6, img);
        } else if (marker == 0xE2 && dn > 14 && std::memcmp(d, "ICC_PROFILE", 12) == 0) {
            icc.insert(icc.end(), d + 14, d + dn);  // chunks come in order in practice
        } else if ((marker >= 0xC0 && marker <= 0xC2) && dn >= 6) {
            const int components = d[5];
            encoding = marker == 0xC2 ? "Progressive" : "Baseline";
            if (components == 3 && dn >= 6 + 9) {
                const int h = d[7] >> 4, v = d[7] & 15;
                encoding += h == 2 && v == 2 ? ", 4:2:0" : h == 2 && v == 1 ? ", 4:2:2" : h == 1 && v == 1 ? ", 4:4:4" : "";
            } else if (components == 1) {
                encoding += ", greyscale";
            }
        } else if (marker == 0xDA) {
            break;
        }
        at += 2 + length;
    }
    if (!icc.empty()) readIccInto(icc.data(), icc.size(), img);
    add(img, "JPEG", encoding);
}

void readPng(const uint8_t* p, size_t n, Image& img) {
    size_t at = 8;
    std::vector<std::pair<std::string, std::string>> texts;
    while (at + 12 <= n) {
        const size_t length = be32(p + at);
        const uint8_t* type = p + at + 4;
        const uint8_t* d = p + at + 8;
        if (length > n - at - 12) break;
        if (std::memcmp(type, "pHYs", 4) == 0 && length >= 9 && d[8] == 1 && img.dpi == 0.0f) {
            img.dpi = float(std::round(be32(d) * 0.0254 * 100.0) / 100.0);
        } else if (std::memcmp(type, "iCCP", 4) == 0 && length > 2) {
            const size_t name = strnlen(reinterpret_cast<const char*>(d), std::min<size_t>(length, 80));
            if (name + 2 < length) {
                const std::vector<uint8_t> icc = inflate(d + name + 2, length - name - 2);
                if (!icc.empty()) readIccInto(icc.data(), icc.size(), img);
                else add(img, "Color profile", clean(d, name));
            }
        } else if (std::memcmp(type, "sRGB", 4) == 0) {
            add(img, "Color profile", "sRGB (sRGB chunk)");
        } else if (std::memcmp(type, "gAMA", 4) == 0 && length >= 4) {
            const double g = be32(d) / 100000.0;
            if (g > 0) add(img, "Gamma", number(std::round(1.0 / g * 100.0) / 100.0));
        } else if (std::memcmp(type, "eXIf", 4) == 0) {
            readExif(d, length, img);
        } else if (std::memcmp(type, "tEXt", 4) == 0 || std::memcmp(type, "zTXt", 4) == 0 ||
                   std::memcmp(type, "iTXt", 4) == 0) {
            const size_t key = strnlen(reinterpret_cast<const char*>(d), std::min<size_t>(length, 80));
            if (key < length) {
                std::string value;
                const uint8_t* v = d + key + 1;
                size_t vn = length - key - 1;
                if (type[0] == 't') {
                    value = clean(v, vn);
                } else if (type[0] == 'z' && vn > 1) {
                    const std::vector<uint8_t> t = inflate(v + 1, vn - 1);
                    value = clean(t.data(), t.size());
                } else if (type[0] == 'i' && vn > 2) {
                    const bool compressed = v[0] != 0;
                    size_t skip = 2;
                    for (int z = 0; z < 2 && skip < vn; ++z) skip += strnlen(reinterpret_cast<const char*>(v + skip), vn - skip) + 1;
                    if (skip < vn) {
                        if (compressed) {
                            const std::vector<uint8_t> t = inflate(v + skip, vn - skip);
                            value = clean(t.data(), t.size());
                        } else {
                            value = clean(v + skip, vn - skip);
                        }
                    }
                }
                const std::string k = clean(d, key);
                // XMP packets and raw profiles are not for reading.
                if (k.rfind("XML:", 0) != 0 && k.rfind("Raw profile", 0) != 0 && !value.empty()) texts.push_back({k, value});
            }
        }
        at += 12 + length;
    }
    for (const auto& [k, v] : texts) add(img, k.c_str(), v);
}

void readWebp(const uint8_t* p, size_t n, Image& img) {
    if (n < 12 || std::memcmp(p, "RIFF", 4) != 0 || std::memcmp(p + 8, "WEBP", 4) != 0) return;
    size_t at = 12;
    while (at + 8 <= n) {
        const size_t length = le32(p + at + 4);
        if (length > n - at - 8) break;
        const uint8_t* d = p + at + 8;
        if (std::memcmp(p + at, "ICCP", 4) == 0) readIccInto(d, length, img);
        else if (std::memcmp(p + at, "EXIF", 4) == 0) readExif(length > 6 && std::memcmp(d, "Exif\0\0", 6) == 0 ? d + 6 : d,
                                                             length > 6 && std::memcmp(d, "Exif\0\0", 6) == 0 ? length - 6 : length, img);
        at += 8 + length + (length & 1);
    }
}

void readPsdResources(const uint8_t* p, size_t n, Image& img) {
    if (n < 30 || std::memcmp(p, "8BPS", 4) != 0) return;
    size_t at = 26;
    at += 4 + size_t(be32(p + at));  // colour mode data
    if (at + 4 > n) return;
    const size_t end = std::min(n, at + 4 + size_t(be32(p + at)));
    at += 4;
    while (at + 12 <= end && std::memcmp(p + at, "8BIM", 4) == 0) {
        const int id = be16(p + at + 4);
        const size_t nameLength = p[at + 6];
        const size_t q = at + 6 + ((nameLength + 2) & ~size_t(1));  // the Pascal name, padded to even
        if (q + 4 > end) break;
        const size_t length = be32(p + q);
        const uint8_t* d = p + q + 4;
        if (q + 4 + length > end) break;
        if (id == 0x040F) readIccInto(d, length, img);
        else if (id == 0x0422) readExif(d, length, img);
        at = q + 4 + length + (length & 1);
    }
}

}  // namespace

std::string iccDescription(const uint8_t* p, size_t n) {
    if (n < 132 || std::memcmp(p + 36, "acsp", 4) != 0) return {};
    std::string desc;
    const uint32_t tags = be32(p + 128);
    for (uint32_t i = 0; i < tags && i < 256 && 132 + (i + 1) * 12 <= n; ++i) {
        const uint8_t* e = p + 132 + i * 12;
        if (std::memcmp(e, "desc", 4) != 0) continue;
        const size_t at = be32(e + 4), size = be32(e + 8);
        if (at + size > n || size < 12) break;
        const uint8_t* d = p + at;
        if (std::memcmp(d, "desc", 4) == 0) {
            const size_t count = be32(d + 8);
            if (12 + count <= size) desc = clean(d + 12, count);
        } else if (std::memcmp(d, "mluc", 4) == 0 && size >= 16) {
            const uint32_t records = be32(d + 8), recordSize = be32(d + 12);
            if (records > 0 && recordSize >= 12 && 16 + recordSize <= size) {
                const size_t length = be32(d + 20), offset = be32(d + 24);
                if (offset + length <= size) desc = utf16be(d + offset, length / 2);
            }
        }
        break;
    }
    if (desc.empty()) desc = "ICC profile";
    const std::string space = clean(p + 16, 4);
    char version[16];
    std::snprintf(version, sizeof version, "v%d.%d", p[8], p[9] >> 4);
    return desc + " (ICC " + version + (space.empty() ? "" : ", " + space) + ")";
}

void readExif(const uint8_t* p, size_t n, Image& img) {
    // Some containers put "Exif\0\0" or a 4-byte offset in front of the TIFF header.
    for (size_t skip = 0; skip <= 10 && skip + 4 <= n; ++skip)
        if ((p[skip] == 'I' && p[skip + 1] == 'I' && p[skip + 2] == 42) || (p[skip] == 'M' && p[skip + 1] == 'M' && p[skip + 3] == 42)) {
            p += skip;
            n -= skip;
            break;
        }
    if (n < 8 || !((p[0] == 'I' && p[1] == 'I') || (p[0] == 'M' && p[1] == 'M'))) return;
    const Tiff t{p, n, p[0] == 'I'};
    if (t.u16(2) != 42) return;
    ExifValues v;
    readIfd(t, t.u32(4), v, 0);
    if (v.xres > 1 && img.dpi == 0.0f) img.dpi = float(v.resUnit == 3 ? v.xres * 2.54 : v.xres);
    std::string camera = v.make;
    if (!v.model.empty()) camera = v.model.rfind(v.make, 0) == 0 || camera.empty() ? v.model : camera + " " + v.model;
    add(img, "Camera", camera);
    add(img, "Lens", v.lens);
    std::string exposure;
    auto part = [&](const std::string& s) {
        if (!s.empty()) exposure += (exposure.empty() ? "" : " \xC2\xB7 ") + s;
    };
    if (v.exposure > 0) part(v.exposure < 1 ? "1/" + number(std::round(1.0 / v.exposure)) + " s" : number(v.exposure) + " s");
    if (v.fnumber > 0) part("f/" + number(std::round(v.fnumber * 10) / 10));
    if (v.iso > 0) part("ISO " + std::to_string(v.iso));
    if (v.focal > 0) part(number(std::round(v.focal * 10) / 10) + " mm");
    add(img, "Exposure", exposure);
    // "2026:10:20 14:30:00" reads better with dashes in the date.
    auto date = [](std::string d) {
        if (d.size() >= 10 && d[4] == ':' && d[7] == ':') d[4] = d[7] = '-';
        return d;
    };
    add(img, "Taken", date(v.taken));
    add(img, "Date", date(v.modified));
    add(img, "Software", v.software);
    add(img, "Artist", v.artist);
    add(img, "Copyright", v.copyright);
    add(img, "Description", v.description);
    static const char* kOrientation[9] = {"", "", "Mirrored", "Rotated 180\xC2\xB0", "Flipped vertically",
                                          "Mirrored, rotated 90\xC2\xB0 CCW", "Rotated 90\xC2\xB0 CW",
                                          "Mirrored, rotated 90\xC2\xB0 CW", "Rotated 90\xC2\xB0 CCW"};
    if (v.orientation >= 2 && v.orientation <= 8)
        add(img, "Orientation", std::string(kOrientation[v.orientation]) + " (not applied)");
    if (v.colorSpace == 1) add(img, "Color profile", "sRGB (EXIF)");
    else if (v.colorSpace == 0xFFFF) add(img, "Color profile", "Uncalibrated (EXIF)");
}

void readMetadata(const uint8_t* data, size_t size, const std::string& extension, Image& img) {
    if (extension == "jpg" || extension == "jpeg") {
        if (size > 4 && data[0] == 0xFF && data[1] == 0xD8) readJpeg(data, size, img);
    } else if (extension == "png") {
        if (size > 8 && std::memcmp(data, "\x89PNG", 4) == 0) readPng(data, size, img);
    } else if (extension == "webp") {
        readWebp(data, size, img);
    } else if (extension == "psd") {
        readPsdResources(data, size, img);
    }
}

}  // namespace htv
