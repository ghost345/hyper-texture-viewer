#include "wic_loader.h"

#include "codecs.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

namespace htv {

namespace {

template <class T>
struct Ptr {
    T* p = nullptr;
    Ptr() = default;
    Ptr(const Ptr&) = delete;
    Ptr& operator=(const Ptr&) = delete;
    ~Ptr() {
        if (p) p->Release();
    }
    T** put() { return &p; }
    T* operator->() const { return p; }
};

std::string hresultText(HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

std::string codecName(IWICBitmapDecoder* decoder) {
    Ptr<IWICBitmapDecoderInfo> info;
    UINT length = 0;
    if (FAILED(decoder->GetDecoderInfo(info.put())) || FAILED(info->GetFriendlyName(0, nullptr, &length)) || !length)
        return "Windows codec";
    std::wstring name(length, L'\0');
    info->GetFriendlyName(length, name.data(), &length);
    name.resize(wcslen(name.c_str()));
    const int n = WideCharToMultiByte(CP_UTF8, 0, name.data(), int(name.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, name.data(), int(name.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string narrowText(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

// The colour profile, resolution and the common EXIF fields of the first frame, for the Info panel.
void readWicMetadata(IWICImagingFactory* factory, IWICBitmapDecoder* decoder, Image& img) {
    Ptr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.put()))) return;
    UINT count = 0;
    if (SUCCEEDED(frame->GetColorContexts(0, nullptr, &count)) && count > 0 && count < 16) {
        std::vector<IWICColorContext*> contexts(count, nullptr);
        for (IWICColorContext*& c : contexts) factory->CreateColorContext(&c);
        if (SUCCEEDED(frame->GetColorContexts(count, contexts.data(), &count))) {
            for (UINT i = 0; i < count; ++i) {
                WICColorContextType type{};
                if (!contexts[i] || FAILED(contexts[i]->GetType(&type))) continue;
                if (type == WICColorContextProfile) {
                    UINT size = 0;
                    if (FAILED(contexts[i]->GetProfileBytes(0, nullptr, &size)) || size == 0) continue;
                    std::vector<BYTE> icc(size);
                    if (SUCCEEDED(contexts[i]->GetProfileBytes(size, icc.data(), &size))) {
                        const std::string d = iccDescription(icc.data(), icc.size());
                        if (!d.empty()) img.metadata.push_back({"Color profile", d});
                    }
                } else {
                    UINT space = 0;
                    if (SUCCEEDED(contexts[i]->GetExifColorSpace(&space)) && space == 1)
                        img.metadata.push_back({"Color profile", "sRGB (EXIF)"});
                }
                break;
            }
        }
        for (IWICColorContext* c : contexts)
            if (c) c->Release();
    }

    Ptr<IWICMetadataQueryReader> reader;
    if (FAILED(frame->GetMetadataQueryReader(reader.put()))) return;
    // TIFF keeps EXIF tags in its own IFD; JPEG-style containers under app1.
    auto query = [&](const wchar_t* path, PROPVARIANT& v) {
        PropVariantInit(&v);
        for (const wchar_t* root : {L"/ifd", L"/app1/ifd"}) {
            std::wstring full = std::wstring(root) + path;
            if (SUCCEEDED(reader->GetMetadataByName(full.c_str(), &v))) return true;
        }
        return false;
    };
    auto text = [&](const wchar_t* path) {
        PROPVARIANT v;
        std::string out;
        if (query(path, v)) {
            if (v.vt == VT_LPSTR && v.pszVal) out = v.pszVal;
            else if (v.vt == VT_LPWSTR && v.pwszVal) out = narrowText(v.pwszVal);
        }
        PropVariantClear(&v);
        while (!out.empty() && (out.back() == ' ' || out.back() == '\0')) out.pop_back();
        return out;
    };
    auto rational = [&](const wchar_t* path) {
        PROPVARIANT v;
        double out = 0;
        if (query(path, v) && (v.vt == VT_UI8 || v.vt == VT_I8)) {
            const uint32_t num = uint32_t(v.uhVal.QuadPart & 0xffffffffu), den = uint32_t(v.uhVal.QuadPart >> 32);
            if (den) out = double(num) / den;
        }
        PropVariantClear(&v);
        return out;
    };
    auto integer = [&](const wchar_t* path) {
        PROPVARIANT v;
        int out = 0;
        if (query(path, v)) out = v.vt == VT_UI2 ? v.uiVal : v.vt == VT_UI4 ? int(v.ulVal) : 0;
        PropVariantClear(&v);
        return out;
    };
    const double xres = rational(L"/{ushort=282}");
    const int unit = integer(L"/{ushort=296}");
    if (xres > 1 && img.dpi == 0.0f) img.dpi = float(unit == 3 ? xres * 2.54 : xres);
    std::string camera = text(L"/{ushort=271}");
    const std::string model = text(L"/{ushort=272}");
    if (!model.empty()) camera = camera.empty() || model.rfind(camera, 0) == 0 ? model : camera + " " + model;
    const std::pair<const char*, std::string> fields[] = {
        {"Camera", camera},
        {"Taken", text(L"/exif/{ushort=36867}")},
        {"Date", text(L"/{ushort=306}")},
        {"Software", text(L"/{ushort=305}")},
        {"Artist", text(L"/{ushort=315}")},
        {"Copyright", text(L"/{ushort=33432}")},
        {"Description", text(L"/{ushort=270}")},
    };
    for (const auto& [key, value] : fields)
        if (!value.empty()) img.metadata.push_back({key, value});
}

}  // namespace

const std::vector<std::string>& wicExtensions() {
    static const std::vector<std::string> list = {"tif", "tiff", "heic", "heif", "hif", "avif", "jxl", "jxr", "wdp",
                                                  "hdp", "ico",  "cur",  "dng",  "cr2", "cr3",  "nef", "arw", "orf",
                                                  "rw2", "raf"};
    return list;
}

// Expects COM to be initialised on the calling thread.
LoadResult loadWithWic(const std::filesystem::path& path) {
    LoadResult r;
    Ptr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put()));
    if (FAILED(hr)) {
        r.error = "Windows Imaging Component is unavailable (" + hresultText(hr) + ")";
        return r;
    }
    Ptr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                            decoder.put());
    if (FAILED(hr)) {
        r.error = hr == WINCODEC_ERR_COMPONENTNOTFOUND
                      ? "Windows has no codec for this file (HEIC and AVIF need the HEIF / AV1 extensions "
                        "from Microsoft Store)"
                      : "cannot decode with Windows codecs (" + hresultText(hr) + ")";
        return r;
    }
    UINT frames = 0;
    decoder->GetFrameCount(&frames);

    struct Target {
        WICPixelFormatGUID guid;
        GpuFormat format;
        uint32_t bytes;
    };
    // Decodes one frame: float data stays half (scRGB, linear), >8-bit integer data 16 bit, the rest RGBA8.
    UINT bitsPerPixel = 32;
    bool isFloat = false;
    std::string error;
    auto decodeFrame = [&](UINT index, Image& out) {
        Ptr<IWICBitmapFrameDecode> frame;
        UINT width = 0, height = 0;
        WICPixelFormatGUID source{};
        if (FAILED(decoder->GetFrame(index, frame.put())) || FAILED(frame->GetSize(&width, &height)) ||
            FAILED(frame->GetPixelFormat(&source)) || width == 0 || height == 0) {
            error = "cannot read the image";
            return false;
        }
        UINT channels = 4;
        bitsPerPixel = 32;
        WICPixelFormatNumericRepresentation numeric = WICPixelFormatNumericRepresentationUnsignedInteger;
        Ptr<IWICComponentInfo> component;
        Ptr<IWICPixelFormatInfo2> formatInfo2;
        if (SUCCEEDED(factory->CreateComponentInfo(source, component.put())) &&
            SUCCEEDED(component->QueryInterface(IID_PPV_ARGS(formatInfo2.put())))) {
            formatInfo2->GetBitsPerPixel(&bitsPerPixel);
            formatInfo2->GetChannelCount(&channels);
            formatInfo2->GetNumericRepresentation(&numeric);
        }
        isFloat = numeric == WICPixelFormatNumericRepresentationFloat || numeric == WICPixelFormatNumericRepresentationFixed;
        const bool deep = !isFloat && channels > 0 && bitsPerPixel / channels > 8;
        std::vector<Target> targets;
        if (isFloat) targets.push_back({GUID_WICPixelFormat64bppRGBAHalf, GpuFormat::RGBA16F, 8});
        if (deep) targets.push_back({GUID_WICPixelFormat64bppRGBA, GpuFormat::RGBA16Unorm, 8});
        targets.push_back({GUID_WICPixelFormat32bppRGBA, GpuFormat::RGBA8Srgb, 4});

        for (const Target& t : targets) {
            if (uint64_t(width) * height * t.bytes > (uint64_t(1) << 34)) {
                error = "image too large";
                return false;
            }
            Ptr<IWICFormatConverter> converter;
            if (FAILED(factory->CreateFormatConverter(converter.put())) ||
                FAILED(converter->Initialize(frame.p, t.guid, WICBitmapDitherTypeNone, nullptr, 0.0,
                                             WICBitmapPaletteTypeCustom)))
                continue;
            Surface s;
            s.width = width;
            s.height = height;
            s.rowPitch = width * t.bytes;
            s.data.resize(size_t(s.rowPitch) * height);
            // In bands of rows: CopyPixels takes the buffer size as 32 bits.
            const uint32_t band = std::max(1u, uint32_t((1u << 30) / s.rowPitch));
            bool copied = true;
            for (uint32_t y = 0; y < height && copied; y += band) {
                const WICRect rect{0, INT(y), INT(width), INT(std::min(band, height - y))};
                copied = SUCCEEDED(converter->CopyPixels(&rect, s.rowPitch, UINT(size_t(s.rowPitch) * UINT(rect.Height)),
                                                         s.data.data() + size_t(y) * s.rowPitch));
            }
            if (!copied) continue;
            out.width = width;
            out.height = height;
            out.format = t.format;
            out.maxValue = 0.0f;
            if (t.format == GpuFormat::RGBA16F) {
                const auto* h = reinterpret_cast<const uint16_t*>(s.data.data());
                float m = 0.0f;
                for (size_t i = 0; i < size_t(width) * height * 4; ++i) {
                    if (i % 4 == 3) continue;
                    const float v = halfToFloat(h[i]);
                    if (std::isfinite(v)) m = std::max(m, v);
                }
                out.maxValue = m;
            }
            out.mips.clear();
            out.mips.push_back(std::move(s));
            return true;
        }
        error = "cannot convert the pixels of this file";
        return false;
    };

    Image& img = r.image;
    if (!decodeFrame(0, img)) {
        r.error = error;
        return r;
    }
    img.description = codecName(decoder.p) + ", " + std::to_string(bitsPerPixel) + " bpp" + (isFloat ? " float" : "") +
                      " (Windows codec)";
    r.ok = true;

    readWicMetadata(factory.p, decoder.p, img);

    // TIFF pages and the sizes in an icon go to the Layers panel; other formats' extra frames are previews.
    std::wstring ext = path.extension().wstring();
    for (wchar_t& c : ext) c = wchar_t(towlower(c));
    const bool pages = ext == L".tif" || ext == L".tiff" || ext == L".ico" || ext == L".cur";
    if (frames > 1 && !pages) img.note = "frame 1 of " + std::to_string(frames) + " shown";
    if (frames <= 1 || !pages) return r;
    const bool icon = ext == L".ico" || ext == L".cur";
    for (UINT i = 0; i < std::min(frames, 1024u); ++i) {
        Image page;
        if (i > 0 && !decodeFrame(i, page)) continue;
        const Image& from = i == 0 ? img : page;
        Layer l;
        l.width = from.width;
        l.height = from.height;
        l.format = from.format;
        l.maxValue = from.maxValue;
        l.name = icon ? std::to_string(from.width) + " \xC3\x97 " + std::to_string(from.height) : "Page " + std::to_string(i + 1);
        if (icon) l.detail = from.format == GpuFormat::RGBA8Srgb ? "" : formatInfo(from.format).name;
        if (i > 0) l.mips = std::move(page.mips);
        img.layers.push_back(std::move(l));
    }
    if (img.layers.size() <= 1) {
        img.layers.clear();
        return r;
    }
    img.layerKind = icon ? LayerKind::IconSizes : LayerKind::Pages;
    if (icon) {  // the largest size first and on screen, as Explorer shows it
        img.layers[0].mips = std::move(img.mips);
        std::stable_sort(img.layers.begin(), img.layers.end(),
                         [](const Layer& a, const Layer& b) { return a.width * a.height > b.width * b.height; });
        Layer& first = img.layers[0];
        img.mips = std::move(first.mips);
        first.mips.clear();
        img.width = first.width;
        img.height = first.height;
        img.format = first.format;
        img.maxValue = first.maxValue;
    }
    return r;
}

}  // namespace htv
