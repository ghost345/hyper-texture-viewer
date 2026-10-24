#include "wic_loader.h"

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
            if (uint64_t(width) * height * t.bytes > 0x7fffffffull) {
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
            if (FAILED(converter->CopyPixels(nullptr, s.rowPitch, UINT(s.data.size()), s.data.data()))) continue;
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
