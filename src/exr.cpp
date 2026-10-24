// OpenEXR through the OpenEXR Core library: every compression (including DWAA/DWAB, B44, PXR24 and HTJ2K),
// scanline and tiled parts, multipart files. Channels group into layers by their prefix ("diffuse.R") and
// part; each layer is shown as RGBA.

#include "codecs.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <openexr.h>

namespace htv {
namespace {

struct Memory {
    const uint8_t* data;
    size_t size;
};

int64_t readMemory(exr_const_context_t, void* user, void* buffer, uint64_t count, uint64_t offset,
                   exr_stream_error_func_ptr_t) {
    const Memory* m = static_cast<const Memory*>(user);
    if (offset >= m->size) return 0;
    const size_t n = size_t(std::min<uint64_t>(count, m->size - offset));
    std::memcpy(buffer, m->data + offset, n);
    return int64_t(n);
}

int64_t memorySize(exr_const_context_t, void* user) { return int64_t(static_cast<const Memory*>(user)->size); }

thread_local std::string lastError;

void onError(exr_const_context_t, exr_result_t code, const char* message) {
    lastError = message ? message : exr_get_default_error_message(code);
}

const char* compressionName(int c) {
    static const char* names[] = {"uncompressed", "RLE", "ZIPS", "ZIP", "PIZ", "PXR24", "B44", "B44A",
                                  "DWAA", "DWAB", "HTJ2K256", "HTJ2K32"};
    return c >= 0 && c < 12 ? names[c] : "unknown compression";
}

struct Channel {
    std::string name;     // as in the file, e.g. "diffuse.R"
    int part = 0;
    int type = 1;         // 0 uint, 1 half, 2 float
    std::vector<uint8_t> plane;  // width x height samples of the part, 2 or 4 bytes each
};

struct Part {
    std::string name;
    uint32_t width = 0, height = 0;
    int compression = 0;
};

// Decodes level 0 of a scanline or tiled part into its channels' planes. Returns how many chunks failed,
// or -1 when nothing could be decoded.
int readPart(exr_context_t f, int part, const exr_attr_box2i_t& window, bool tiled, std::vector<Channel*>& channels) {
    const uint32_t width = uint32_t(window.max.x - window.min.x + 1);
    exr_decode_pipeline_t decoder{};  // what EXR_DECODE_PIPELINE_INITIALIZER does, without the warnings
    bool started = false;
    int failed = 0, chunks = 0;
    auto point = [&](int x0, int y0) {  // the decoder writes this chunk into the planes at x0, y0
        for (int c = 0; c < decoder.channel_count; ++c) {
            exr_coding_channel_info_t& out = decoder.channels[c];
            Channel* ch = nullptr;
            for (Channel* candidate : channels)
                if (candidate->name == out.channel_name) ch = candidate;
            if (!ch || ch->plane.empty() || out.x_samples != 1 || out.y_samples != 1) {
                out.decode_to_ptr = nullptr;  // skipped
                continue;
            }
            const int bytes = out.user_bytes_per_element;
            out.decode_to_ptr = ch->plane.data() + (size_t(y0) * width + size_t(x0)) * bytes;
            out.user_pixel_stride = bytes;
            out.user_line_stride = int32_t(width) * bytes;
        }
    };
    auto run = [&](const exr_chunk_info_t& info, int x0, int y0) {
        ++chunks;
        if (!started) {
            if (exr_decoding_initialize(f, part, &info, &decoder) != EXR_ERR_SUCCESS) return false;
            started = true;
            point(x0, y0);
            if (exr_decoding_choose_default_routines(f, part, &decoder) != EXR_ERR_SUCCESS) return false;
        } else {
            if (exr_decoding_update(f, part, &info, &decoder) != EXR_ERR_SUCCESS) return false;
            point(x0, y0);
        }
        return exr_decoding_run(f, part, &decoder) == EXR_ERR_SUCCESS;
    };

    if (tiled) {
        uint32_t tileW = 0, tileH = 0;
        exr_tile_level_mode_t levels{};
        exr_tile_round_mode_t round{};
        int32_t countX = 0, countY = 0;
        if (exr_get_tile_descriptor(f, part, &tileW, &tileH, &levels, &round) != EXR_ERR_SUCCESS ||
            exr_get_tile_counts(f, part, 0, 0, &countX, &countY) != EXR_ERR_SUCCESS)
            return -1;
        for (int ty = 0; ty < countY; ++ty)
            for (int tx = 0; tx < countX; ++tx) {
                exr_chunk_info_t info{};
                if (exr_read_tile_chunk_info(f, part, tx, ty, 0, 0, &info) != EXR_ERR_SUCCESS ||
                    !run(info, tx * int(tileW), ty * int(tileH)))
                    ++failed;
            }
    } else {
        int32_t lines = 1;
        if (exr_get_scanlines_per_chunk(f, part, &lines) != EXR_ERR_SUCCESS || lines < 1) return -1;
        for (int64_t y = window.min.y; y <= window.max.y; y += lines) {
            exr_chunk_info_t info{};
            if (exr_read_scanline_chunk_info(f, part, int(y), &info) != EXR_ERR_SUCCESS ||
                !run(info, 0, info.start_y - window.min.y))
                ++failed;
        }
    }
    if (started) exr_decoding_destroy(f, &decoder);
    return failed == chunks ? -1 : failed;
}

std::string box(const exr_attr_box2i_t& b) {
    char t[96];
    std::snprintf(t, sizeof t, "%d \xC3\x97 %d at %d, %d", b.max.x - b.min.x + 1, b.max.y - b.min.y + 1, b.min.x, b.min.y);
    return t;
}

}  // namespace

LoadResult loadExr(const uint8_t* data, size_t size) {
    LoadResult r;
    if (size < 8 || data[0] != 0x76 || data[1] != 0x2f || data[2] != 0x31 || data[3] != 0x01) {
        r.error = "not a valid EXR file";
        return r;
    }
    Memory memory{data, size};
    exr_context_initializer_t init = EXR_DEFAULT_CONTEXT_INITIALIZER;
    init.user_data = &memory;
    init.read_fn = readMemory;
    init.size_fn = memorySize;
    init.error_handler_fn = onError;
    exr_context_t f = nullptr;
    lastError.clear();
    if (exr_start_read(&f, "memory", &init) != EXR_ERR_SUCCESS) {
        r.error = "cannot read EXR header" + (lastError.empty() ? std::string() : ": " + lastError);
        return r;
    }
    struct Guard {
        exr_context_t* f;
        ~Guard() { exr_finish(f); }
    } guard{&f};

    int partCount = 0;
    exr_get_count(f, &partCount);
    std::vector<Part> parts;
    std::vector<Channel> channels;
    std::vector<int> partIndex;  // file part of each entry in `parts`
    bool deep = false;
    for (int p = 0; p < partCount && p < 64; ++p) {
        exr_storage_t storage{};
        exr_attr_box2i_t window{};
        const exr_attr_chlist_t* list = nullptr;
        exr_compression_t compression{};
        if (exr_get_storage(f, p, &storage) != EXR_ERR_SUCCESS || exr_get_data_window(f, p, &window) != EXR_ERR_SUCCESS ||
            exr_get_channels(f, p, &list) != EXR_ERR_SUCCESS || !list)
            continue;
        if (storage == EXR_STORAGE_DEEP_SCANLINE || storage == EXR_STORAGE_DEEP_TILED) {
            deep = true;
            continue;
        }
        const int64_t w = int64_t(window.max.x) - window.min.x + 1, h = int64_t(window.max.y) - window.min.y + 1;
        if (w < 1 || h < 1 || w > 65536 || h > 65536 || w * h > (int64_t(1) << 28)) {
            r.error = "EXR data window of " + std::to_string(w) + " \xC3\x97 " + std::to_string(h) + " is too large";
            return r;
        }
        exr_get_compression(f, p, &compression);
        Part part;
        const char* name = nullptr;
        if (exr_get_name(f, p, &name) == EXR_ERR_SUCCESS && name) part.name = name;
        part.width = uint32_t(w);
        part.height = uint32_t(h);
        part.compression = int(compression);
        const int index = int(parts.size());
        std::vector<Channel*> decoded;
        const size_t first = channels.size();
        for (int c = 0; c < list->num_channels; ++c) {
            const exr_attr_chlist_entry_t& e = list->entries[c];
            Channel ch;
            ch.name.assign(e.name.str, size_t(e.name.length));
            ch.part = index;
            ch.type = int(e.pixel_type);
            if (e.x_sampling == 1 && e.y_sampling == 1)  // subsampled (luminance/chroma) channels are left out
                ch.plane.assign(size_t(w) * size_t(h) * (ch.type == 1 ? 2 : 4), 0);
            channels.push_back(std::move(ch));
        }
        for (size_t c = first; c < channels.size(); ++c) decoded.push_back(&channels[c]);
        const int failed = readPart(f, p, window, storage == EXR_STORAGE_TILED, decoded);
        if (failed < 0) {
            channels.resize(first);
            if (parts.empty() && p + 1 >= partCount) {
                r.error = "cannot decode EXR" + (lastError.empty() ? std::string() : ": " + lastError);
                return r;
            }
            continue;
        }
        if (failed > 0) r.image.note = std::to_string(failed) + " damaged blocks left black";
        channels.erase(std::remove_if(channels.begin() + long(first), channels.end(),
                                      [](const Channel& c) { return c.plane.empty(); }),
                       channels.end());
        parts.push_back(part);
        partIndex.push_back(p);
    }
    if (parts.empty() || channels.empty()) {
        r.error = deep ? "deep EXR (samples per pixel) is not supported" : "EXR file has no image data";
        if (!lastError.empty() && !deep) r.error += ": " + lastError;
        return r;
    }

    // Layers: per part, by the name before the last dot. The main layer (no prefix, first part) comes first.
    struct Group {
        int part;
        std::string prefix;
        std::vector<int> channels;
    };
    std::vector<Group> groups;
    for (int i = 0; i < int(channels.size()); ++i) {
        const std::string& n = channels[size_t(i)].name;
        const size_t dot = n.rfind('.');
        const std::string prefix = dot == std::string::npos ? std::string() : n.substr(0, dot);
        auto it = std::find_if(groups.begin(), groups.end(),
                               [&](const Group& g) { return g.part == channels[size_t(i)].part && g.prefix == prefix; });
        if (it == groups.end()) {
            groups.push_back({channels[size_t(i)].part, prefix, {}});
            it = groups.end() - 1;
        }
        it->channels.push_back(i);
    }
    std::stable_partition(groups.begin(), groups.end(), [](const Group& g) { return g.part == 0 && g.prefix.empty(); });
    auto suffix = [&](int c) {
        const std::string& n = channels[size_t(c)].name;
        const size_t dot = n.rfind('.');
        return dot == std::string::npos ? n : n.substr(dot + 1);
    };
    auto groupName = [&](const Group& g) {
        if (parts.size() > 1) {
            const std::string part = parts[size_t(g.part)].name.empty() ? "Part " + std::to_string(g.part + 1) : parts[size_t(g.part)].name;
            return g.prefix.empty() || g.prefix == part ? part : part + "." + g.prefix;
        }
        return g.prefix.empty() ? std::string("Main") : g.prefix;
    };

    bool anyFloat = false, anyHalf = false;
    for (const Channel& c : channels) {
        anyFloat |= c.type != 1;
        anyHalf |= c.type == 1;
    }
    // Half data stays half all the way to the GPU; any 32-bit channel keeps the whole image at full precision.
    const GpuFormat format = anyFloat ? GpuFormat::RGBA32F : GpuFormat::RGBA16F;

    // One layer's channels as RGBA: R, G, B, A by name (or X, Y, Z), a lone channel (with or without alpha)
    // as grey, otherwise the first three. `detail` lists the channels.
    auto build = [&](const Group& g, float& maxValue, std::string& detail) {
        int src[4] = {-1, -1, -1, -1};
        auto find = [&](char name) {
            for (int c : g.channels) {
                const std::string s = suffix(c);
                if (s.size() == 1 && std::toupper(static_cast<unsigned char>(s[0])) == name) return c;
            }
            return -1;
        };
        const bool gray = g.channels.size() == 1 ||
                          (g.channels.size() == 2 && find('A') >= 0 && find('R') < 0 && find('G') < 0 && find('B') < 0);
        if (gray) {
            src[0] = src[1] = src[2] = g.channels[0] == find('A') ? g.channels[1] : g.channels[0];
            src[3] = g.channels.size() == 2 ? find('A') : -1;
        } else {
            for (int c = 0; c < 4; ++c) src[c] = find("RGBA"[c]);
            if (src[0] < 0 && src[1] < 0 && src[2] < 0)
                for (int c = 0; c < 3; ++c) src[c] = find("XYZ"[c]);
            if (src[0] < 0 && src[1] < 0 && src[2] < 0)
                for (size_t c = 0; c < 3 && c < g.channels.size(); ++c) src[c] = g.channels[c];
        }
        detail.clear();
        for (int c : g.channels) detail += (detail.empty() ? "" : " ") + suffix(c);
        if (gray) detail += " (grey)";

        const Part& part = parts[size_t(g.part)];
        const size_t pixels = size_t(part.width) * part.height;
        Surface s;
        s.width = part.width;
        s.height = part.height;
        s.rowPitch = part.width * formatInfo(format).bytes;
        s.data.resize(size_t(s.rowPitch) * part.height);
        maxValue = 0.0f;
        for (int c = 0; c < 4; ++c) {
            const Channel* ch = src[c] >= 0 ? &channels[size_t(src[c])] : nullptr;
            if (anyFloat) {
                auto* out = reinterpret_cast<float*>(s.data.data());
                for (size_t i = 0; i < pixels; ++i) {
                    float v = c == 3 ? 1.0f : 0.0f;
                    if (ch && ch->type == 1) {
                        uint16_t h;
                        std::memcpy(&h, &ch->plane[i * 2], 2);
                        v = halfToFloat(h);
                    } else if (ch) {
                        uint32_t u;
                        std::memcpy(&u, &ch->plane[i * 4], 4);
                        if (ch->type == 0) v = float(u);
                        else std::memcpy(&v, &u, 4);
                    }
                    out[i * 4 + c] = v;
                    if (c < 3 && std::isfinite(v)) maxValue = std::max(maxValue, v);
                }
            } else {
                auto* out = reinterpret_cast<uint16_t*>(s.data.data());
                uint16_t brightest = 0;  // for positive finite halves, the bit pattern orders like the value
                for (size_t i = 0; i < pixels; ++i) {
                    uint16_t v = c == 3 ? 0x3c00 : 0;
                    if (ch) std::memcpy(&v, &ch->plane[i * 2], 2);
                    out[i * 4 + c] = v;
                    if (c < 3 && v < 0x7c00 && v > brightest) brightest = v;
                }
                if (c < 3) maxValue = std::max(maxValue, halfToFloat(brightest));
            }
        }
        return s;
    };

    Image& img = r.image;
    const Part& main = parts[size_t(groups[0].part)];
    img.width = main.width;
    img.height = main.height;
    img.format = format;
    std::string detail;
    img.mips.push_back(build(groups[0], img.maxValue, detail));
    if (groups.size() > 1) {
        img.layerKind = LayerKind::ExrLayers;
        for (size_t i = 0; i < groups.size(); ++i) {
            Layer l;
            const Part& part = parts[size_t(groups[i].part)];
            l.width = part.width;
            l.height = part.height;
            l.format = format;
            if (i == 0) {
                l.maxValue = img.maxValue;
                l.detail = detail;
            } else {
                l.mips.push_back(build(groups[i], l.maxValue, l.detail));
            }
            l.name = groupName(groups[i]);
            img.layers.push_back(std::move(l));
        }
    }

    // Header details for the Info panel, from the first part.
    const int p0 = partIndex[0];
    std::vector<Property>& details = img.exr;
    {
        std::string compression = compressionName(parts[0].compression);
        bool same = true;
        for (const Part& p : parts) same &= p.compression == parts[0].compression;
        if (!same) {
            compression.clear();
            for (size_t i = 0; i < parts.size(); ++i)
                compression += (i ? ", " : "") + std::string(compressionName(parts[i].compression)) +
                               (parts[i].name.empty() ? "" : " (" + parts[i].name + ")");
        }
        details.push_back({"Compression", compression});
    }
    if (groups.size() > 1) {
        std::string names;
        for (const Group& g : groups) names += (names.empty() ? "" : ", ") + groupName(g);
        details.push_back({"Layers", std::to_string(groups.size()) + ": " + names});
    }
    if (parts.size() > 1) details.push_back({"Parts", std::to_string(parts.size())});
    {
        static const char* kTypes[] = {"uint", "half", "float"};
        bool sameType = true;
        for (const Channel& c : channels) sameType &= c.type == channels[0].type;
        std::string list;
        for (const Channel& c : channels)
            list += (list.empty() ? "" : " ") + c.name + (sameType ? "" : std::string(" (") + kTypes[std::clamp(c.type, 0, 2)] + ")");
        if (sameType) list += std::string(" \xC2\xB7 ") + kTypes[std::clamp(channels[0].type, 0, 2)];
        details.push_back({"Channels", list});
    }
    exr_attr_box2i_t window{};
    if (exr_get_data_window(f, p0, &window) == EXR_ERR_SUCCESS) details.push_back({"Data window", box(window)});
    if (exr_get_display_window(f, p0, &window) == EXR_ERR_SUCCESS) details.push_back({"Display window", box(window)});
    uint32_t tileW = 0, tileH = 0;
    exr_tile_level_mode_t levels{};
    exr_tile_round_mode_t round{};
    if (exr_get_tile_descriptor(f, p0, &tileW, &tileH, &levels, &round) == EXR_ERR_SUCCESS) {
        static const char* kLevels[] = {"", ", mipmapped (level 0 shown)", ", ripmapped (level 0 shown)"};
        details.push_back({"Tiles", std::to_string(tileW) + " \xC3\x97 " + std::to_string(tileH) +
                                        kLevels[std::clamp(int(levels), 0, 2)]});
    }
    float aspect = 1.0f;
    if (exr_get_pixel_aspect_ratio(f, p0, &aspect) == EXR_ERR_SUCCESS && aspect != 1.0f && aspect > 0.0f) {
        char t[32];
        std::snprintf(t, sizeof t, "%g", double(aspect));
        details.push_back({"Pixel aspect", t});
    }
    exr_lineorder_t order{};
    static const char* kOrder[] = {"Increasing Y", "Decreasing Y", "Random Y"};
    if (exr_get_lineorder(f, p0, &order) == EXR_ERR_SUCCESS && int(order) >= 0 && int(order) < 3)
        details.push_back({"Line order", kOrder[int(order)]});
    // Other attributes the file carries: strings and numbers.
    static const char* kStandard[] = {"channels", "compression", "dataWindow", "displayWindow", "lineOrder",
                                      "pixelAspectRatio", "screenWindowCenter", "screenWindowWidth", "tiles",
                                      "type", "name", "version", "chunkCount"};
    int32_t attributes = 0;
    exr_get_attribute_count(f, p0, &attributes);
    for (int32_t i = 0, shown = 0; i < attributes && shown < 32; ++i) {
        const exr_attribute_t* a = nullptr;
        if (exr_get_attribute_by_index(f, p0, EXR_ATTR_LIST_FILE_ORDER, i, &a) != EXR_ERR_SUCCESS || !a || !a->name) continue;
        if (std::find_if(std::begin(kStandard), std::end(kStandard), [&](const char* s) { return std::strcmp(s, a->name) == 0; }) !=
            std::end(kStandard))
            continue;
        std::string value;
        char t[48];
        if (a->type == EXR_ATTR_STRING && a->string && a->string->str) {
            value.assign(a->string->str, size_t(std::clamp(a->string->length, 0, 200)));
            value = value.c_str();  // up to a NUL
        } else if (a->type == EXR_ATTR_FLOAT) {
            std::snprintf(t, sizeof t, "%g", double(a->f));
            value = t;
        } else if (a->type == EXR_ATTR_DOUBLE) {
            std::snprintf(t, sizeof t, "%g", a->d);
            value = t;
        } else if (a->type == EXR_ATTR_INT) {
            std::snprintf(t, sizeof t, "%d", int(a->i));
            value = t;
        }
        if (!value.empty()) {
            details.push_back({a->name, value});
            ++shown;
        }
    }

    std::string channelList;
    int listed = 0;
    for (const Channel& c : channels) {
        if (c.part != groups[0].part) continue;
        if (listed++ == 8) {
            channelList += ",...";
            break;
        }
        channelList += (channelList.empty() ? "" : ",") + c.name;
    }
    img.description = std::string("EXR, ") + (anyFloat ? (anyHalf ? "half/float " : "float ") : "half ") + channelList +
                      ", " + compressionName(parts[0].compression);
    if (deep) img.note = "deep parts are not shown";
    if (groups.size() == 1 && detail.find("(grey)") != std::string::npos && img.note.empty())
        img.note = "no RGB channels: the channel is shown as grey";
    r.ok = true;
    return r;
}

}  // namespace htv
