// Direct3D 11: device, swap chain, the image shader and the frame. All view math happens in one pixel shader.

#include "viewer.h"

#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <iterator>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "ui.h"

namespace htv {
namespace {

const char kShader[] = R"(
cbuffer View : register(b0) {
    float2 imageSize;      // source image in pixels
    float2 imageOrigin;    // top-left corner of the shown (rotated, tiled) image in target pixels
    float zoom;
    float exposureScale;   // 2^EV
    uint channelMode;      // 0 RGBA, 1 R, 2 G, 3 B, 4 A, 5 RGB without alpha, 6 luminance
    uint channels;         // channels the texture format stores
    uint transfer;         // sRGB encodes before display: 0, 1 or 2
    uint signedValues;     // snorm data in [-1, 1]
    uint rotation;         // quarter turns clockwise
    uint flips;            // 1 horizontal, 2 vertical
    float tiles;           // 1, or 3 for the 3x3 tiling preview
    uint reserved;         // keeps the layout
    int forcedMip;         // -1: pick the mip from the zoom
    float invGamma;
    float checkerSize;     // target pixels per checker cell
    float blackPoint;      // HDR range: values map to (v - blackPoint) * rangeScale
    float rangeScale;
    uint toneMap;          // 0 clip, 1 Reinhard, 2 ACES, 3 AgX
    uint clipping;         // zebra above the white point, magenta NaN and Inf
    float zebraWidth;      // target pixels per stripe
    float zebraPhase;      // the stripes crawl slowly so small clipped spots do not hide on a white stripe
    float padding;
    float4 checker1;       // alpha background: checker colours (equal for a solid colour)
    float4 checker2;
    float4 background;     // viewport outside the image
    float4 matchTarget;    // Color match, in the space psMatch compares in
    uint matchMode;        // 0 RGB, 1 RGBA, 2 R, 3 G, 4 B, 5 A, 6 luminance, 7 hue, 8 saturation
    float matchTolerance;
    uint matchSrgb;        // compare sRGB-encoded values: the GPU decoded the texture to linear
    uint matchOutline;     // outline the pixels set in `mask`
    float2 image2Size;     // Compare: the second image of a pair, in tex2
    float2 scale2;         // its pixels per pixel of the first
    uint channels2;
    uint srgb1;            // compare sRGB-encoded values: the GPU decoded the texture to linear
    uint srgb2;
    float diffGain;
};
Texture2D<float4> tex : register(t0);
Texture2D<float> mask : register(t1);
Texture2D<float4> tex2 : register(t2);
SamplerState trilinear : register(s0);

float4 vsMain(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float3 srgbEncode(float3 c) {
    c = saturate(c);
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    return float3(c.r <= 0.0031308 ? lo.r : hi.r, c.g <= 0.0031308 ? lo.g : hi.g, c.b <= 0.0031308 ? lo.b : hi.b);
}

// Narkowicz's fit of the ACES filmic curve.
float3 aces(float3 x) {
    x *= 0.6;
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

// AgX with the default look (Benjamin Wrensch's minimal version), linear sRGB in and out.
float3 agx(float3 v) {
    const float3x3 inset = float3x3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                                    0.0784335999999992, 0.878468636469772, 0.0784336,
                                    0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const float3x3 outset = float3x3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                                     -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                                     -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float minEv = -12.47393, maxEv = 4.026069;
    v = mul(max(v, 1e-10), inset);
    v = (clamp(log2(v), minEv, maxEv) - minEv) / (maxEv - minEv);
    float3 v2 = v * v, v4 = v2 * v2;
    v = 15.5 * v4 * v2 - 40.14 * v4 * v + 31.96 * v4 - 6.868 * v2 * v + 0.4298 * v2 + 0.1191 * v - 0.00232;
    v = mul(v, outset);
    return pow(saturate(v), 2.2);
}

// Position in the source image under a target pixel, through tiling, flips and rotation. False outside the image.
bool imagePos(float2 pos, out float2 ip) {
    float2 shownSize = (rotation & 1) ? imageSize.yx : imageSize;
    float2 d = (pos - imageOrigin) / zoom;
    ip = float2(0, 0);
    if (d.x < 0 || d.y < 0 || d.x >= shownSize.x * tiles || d.y >= shownSize.y * tiles) return false;
    d = fmod(d, shownSize);
    if (flips & 1) d.x = shownSize.x - d.x;
    if (flips & 2) d.y = shownSize.y - d.y;
    ip = d;
    if (rotation == 1) ip = float2(d.y, shownSize.x - d.x);
    else if (rotation == 2) ip = imageSize - d;
    else if (rotation == 3) ip = float2(shownSize.y - d.y, d.x);
    return true;
}

// Whether the Color match mask is set under a target pixel.
bool matchedAt(float2 pos) {
    float2 ip;
    if (!imagePos(pos, ip)) return false;
    return mask.Load(int3(clamp(floor(ip), 0, imageSize - 1), 0)) > 0.5;
}

float4 psMain(float4 pos : SV_Position) : SV_Target {
    float2 ip;
    if (!imagePos(pos.xy, ip)) return background;
    int2 texel = int2(clamp(floor(ip), 0, imageSize - 1));

    // Color match: a dashed line, black on white, along the inside edge of the matching areas.
    if (matchOutline && matchedAt(pos.xy) &&
        (!matchedAt(pos.xy + float2(1, 0)) || !matchedAt(pos.xy - float2(1, 0)) ||
         !matchedAt(pos.xy + float2(0, 1)) || !matchedAt(pos.xy - float2(0, 1)))) {
        int dash = int(floor((pos.x + pos.y) / max(zebraWidth, 1.0)));
        return (dash & 1) ? float4(1, 1, 1, 1) : float4(0, 0, 0, 1);
    }

    float4 c;
    if (forcedMip >= 0) {
        c = tex.Load(int3(texel >> forcedMip, forcedMip));
    } else if (zoom >= 1.0) {
        // Magnified or 1:1: exact texels, no filtering.
        c = tex.Load(int3(texel, 0));
    } else {
        // Minified: the mip chain averages all source pixels under each screen pixel.
        c = tex.SampleLevel(trilinear, ip / imageSize, log2(1.0 / zoom));
    }
    if (signedValues) c.rgb = c.rgb * 0.5 + 0.5;
    if (channels == 1) c = float4(c.rrr, 1);
    else if (channels == 2) c = float4(c.rg, 0, 1);

    // NaN and Inf. Written without isnan/isinf so every HLSL compiler accepts it.
    if (any(c != c) || any(abs(c) > 3.402823e38)) return clipping ? float4(1, 0, 1, 1) : float4(0, 0, 0, 1);

    float3 rgb = c.rgb;
    float a = c.a;
    if (channelMode == 1) rgb = c.rrr;
    else if (channelMode == 2) rgb = c.ggg;
    else if (channelMode == 3) rgb = c.bbb;
    else if (channelMode == 4) rgb = c.aaa;
    else if (channelMode == 6) rgb = dot(c.rgb, float3(0.2126, 0.7152, 0.0722)).xxx;
    if (channelMode != 0) a = 1;

    bool clipped = false;
    if (channelMode != 4) {
        rgb = (rgb - blackPoint) * rangeScale * exposureScale;
        clipped = clipping && any(rgb > 1.0);
        rgb = max(rgb, 0);
        if (toneMap == 1) rgb = rgb / (1 + rgb);
        else if (toneMap == 2) rgb = aces(rgb);
        else if (toneMap == 3) rgb = agx(rgb);
    }
    if (transfer >= 1) rgb = srgbEncode(rgb);
    if (transfer >= 2) rgb = srgbEncode(rgb);
    rgb = pow(saturate(rgb), invGamma);
    if (clipped) {
        // Integer stripe index: fmod on floats is emulated differently by each driver.
        int stripe = int(floor((pos.x + pos.y + zebraPhase) / max(zebraWidth, 1.0)));
        rgb = (stripe & 1) ? float3(1, 1, 1) : float3(0, 0, 0);
    }

    float2 cell = floor((pos.xy - imageOrigin) / checkerSize);
    float3 under = fmod(cell.x + cell.y, 2) < 1 ? checker1.rgb : checker2.rgb;
    return float4(lerp(under, rgb, saturate(a)), 1);
}

// Hue (a fraction of the circle) and saturation as in HSV.
float2 hueSaturation(float3 c) {
    float mx = max(max(c.r, c.g), c.b), mn = min(min(c.r, c.g), c.b), d = mx - mn;
    float hue = 0;
    if (d > 0) {
        if (mx == c.r) hue = (c.g - c.b) / d;
        else if (mx == c.g) hue = 2 + (c.b - c.r) / d;
        else hue = 4 + (c.r - c.g) / d;
        hue = frac(hue / 6 + 1);
    }
    return float2(hue, mx > 0 ? d / mx : 0);
}

// Color match mask: 1 where a texel is within the tolerance of the target.
float4 psMatch(float4 pos : SV_Position) : SV_Target {
    float4 c = tex.Load(int3(int2(pos.xy), 0));
    if (matchSrgb) c.rgb = srgbEncode(c.rgb);
    if (channels == 1) c = float4(c.rrr, 1);
    else if (channels == 2) c = float4(c.rg, 0, 1);
    float4 t = matchTarget;
    float d;
    if (matchMode == 0) d = max(max(abs(c.r - t.r), abs(c.g - t.g)), abs(c.b - t.b));
    else if (matchMode == 1) d = max(max(abs(c.r - t.r), abs(c.g - t.g)), max(abs(c.b - t.b), abs(c.a - t.a)));
    else if (matchMode <= 5) d = abs(dot(c - t, float4(matchMode == 2, matchMode == 3, matchMode == 4, matchMode == 5)));
    else if (matchMode == 6) d = abs(dot(c.rgb - t.rgb, float3(0.2126, 0.7152, 0.0722)));
    else {
        float2 hc = hueSaturation(c.rgb), ht = hueSaturation(t.rgb);
        float dh = abs(hc.x - ht.x);
        d = matchMode == 7 ? min(dh, 1 - dh) : abs(hc.y - ht.y);
    }
    return d <= matchTolerance ? 1 : 0;
}

// Compare: one and two-channel data as RGBA, sRGB re-encoded, so differences read as the files store them.
float4 storedValue(float4 c, uint ch, uint srgb) {
    if (srgb) c.rgb = srgbEncode(c.rgb);
    if (ch == 1) c = float4(c.rrr, 1);
    else if (ch == 2) c = float4(c.rg, 0, 1);
    return c;
}

// Black, red, yellow, white.
float3 heat(float t) { return saturate(float3(t * 3, t * 3 - 1, t * 3 - 2)); }

// Compare, Difference: |first - second| times the gain, through the heat ramp.
float4 psDiff(float4 pos : SV_Position) : SV_Target {
    float2 ip;
    if (!imagePos(pos.xy, ip)) return background;
    float2 ip2 = ip * scale2;
    if (ip2.x >= image2Size.x || ip2.y >= image2Size.y) return background;
    float4 a, b;
    if (zoom >= 1.0) {
        a = tex.Load(int3(clamp(floor(ip), 0, imageSize - 1), 0));
        b = tex2.Load(int3(clamp(floor(ip2), 0, image2Size - 1), 0));
    } else {
        a = tex.SampleLevel(trilinear, ip / imageSize, log2(1.0 / zoom));
        b = tex2.SampleLevel(trilinear, ip2 / image2Size, log2(scale2.x / zoom));
    }
    float4 d = abs(storedValue(a, channels, srgb1) - storedValue(b, channels2, srgb2));
    float v = max(max(d.r, d.g), d.b);
    if (channelMode >= 1 && channelMode <= 4)
        v = dot(d, float4(channelMode == 1, channelMode == 2, channelMode == 3, channelMode == 4));
    else if (channelMode == 6) v = abs(dot(storedValue(a, channels, srgb1).rgb - storedValue(b, channels2, srgb2).rgb,
                                           float3(0.2126, 0.7152, 0.0722)));
    if (v != v) return float4(1, 0, 1, 1);
    return float4(heat(v * diffGain), 1);
}

// Compare statistics: per-channel differences of mip 0 from imageOrigin on; -1 where the second image ends.
float4 psDiffRead(float4 pos : SV_Position) : SV_Target {
    int2 p = clamp(int2(imageOrigin) + int2(pos.xy), 0, int2(imageSize) - 1);
    float2 p2 = floor((float2(p) + 0.5) * scale2);
    if (p2.x >= image2Size.x || p2.y >= image2Size.y) return float4(-1, -1, -1, -1);
    float4 a = storedValue(tex.Load(int3(p, 0)), channels, srgb1);
    float4 b = storedValue(tex2.Load(int3(int2(p2), 0)), channels2, srgb2);
    return abs(a - b);
}

// Pixel readout for formats the CPU cannot decode: texels from imageOrigin on, as sampled.
float4 psRead(float4 pos : SV_Position) : SV_Target {
    int2 p = clamp(int2(imageOrigin) + int2(pos.xy), 0, int2(imageSize) - 1);
    return tex.Load(int3(p, 0));
}
)";

struct ViewConstants {
    float imageSize[2];
    float imageOrigin[2];
    float zoom;
    float exposureScale;
    uint32_t channelMode;
    uint32_t channels;
    uint32_t transfer;
    uint32_t signedValues;
    uint32_t rotation;
    uint32_t flips;
    float tiles;
    uint32_t reserved;
    int32_t forcedMip;
    float invGamma;
    float checkerSize;
    float blackPoint;
    float rangeScale;
    uint32_t toneMap;
    uint32_t clipping;
    float zebraWidth;
    float zebraPhase;
    float padding;  // checker1 starts a new 16-byte register
    float checker1[4];
    float checker2[4];
    float background[4];
    float matchTarget[4];
    uint32_t matchMode;
    float matchTolerance;
    uint32_t matchSrgb;
    uint32_t matchOutline;
    float image2Size[2];
    float scale2[2];
    uint32_t channels2;
    uint32_t srgb1;
    uint32_t srgb2;
    float diffGain;
};
static_assert(sizeof(ViewConstants) % 16 == 0, "constant buffer size must be a multiple of 16");

// d3dcompiler_47.dll ships with Windows 8.1 and later; loading it lazily keeps it off the import table.
using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                      UINT, UINT, ID3DBlob**, ID3DBlob**);

const float kCheckerSizes[3] = {8, 16, 32};
const float kSolidBackgrounds[3] = {0.0f, 0.5f, 1.0f};  // black, middle grey, white

ViewConstants makeConstants(const htv::Image& image, float originX, float originY, float zoom, float tiles,
                            float checkerSize, const UiColors& colors) {
    const htv::FormatInfo& info = htv::formatInfo(image.format);
    ViewConstants c{};
    c.imageSize[0] = float(image.width);
    c.imageSize[1] = float(image.height);
    c.imageOrigin[0] = originX;
    c.imageOrigin[1] = originY;
    c.zoom = zoom;
    const bool hdr = isHighPrecision(image);
    c.exposureScale = hdr ? std::exp2(app.exposure) : 1.0f;
    const uint32_t modes[] = {0, 1, 2, 3, 4, 6};
    c.channelMode = modes[int(app.channel)];
    if (c.channelMode == 0 && app.ignoreAlpha) c.channelMode = 5;
    c.channels = uint32_t(info.channels);
    c.transfer = (app.dataIsLinear ? 1u : 0u) + (isSrgbFormat(image.format) ? 1u : 0u);
    c.signedValues = info.snorm ? 1u : 0u;
    c.rotation = uint32_t(app.rotation);
    c.flips = (app.flipH ? 1u : 0u) | (app.flipV ? 2u : 0u);
    c.tiles = tiles;
    c.forcedMip = app.forcedMip;
    // Range, tone mapping and gamma belong to the HDR row, which only float and 16-bit images show.
    c.invGamma = hdr ? 1.0f / app.gamma : 1.0f;
    c.blackPoint = hdr ? app.blackPoint : 0.0f;
    c.rangeScale = hdr ? 1.0f / std::max(app.whitePoint - app.blackPoint, 1e-6f) : 1.0f;
    c.toneMap = hdr && app.dataIsLinear ? uint32_t(app.toneMap) : 0u;
    c.clipping = hdr && app.showClipping ? 1u : 0u;
    c.zebraWidth = std::max(1.0f, std::round(4 * app.scale));
    // One stripe pair every two seconds; the period keeps the float exact over long sessions.
    c.zebraPhase = float(std::floor(std::fmod(nowMs() * 0.001 * c.zebraWidth, 2.0 * c.zebraWidth)));
    if (app.alphaBackground == AlphaBackground::Checker) {
        c.checkerSize = checkerSize;
        for (int i = 0; i < 3; ++i) {
            c.checker1[i] = colors.checker1[i];
            c.checker2[i] = colors.checker2[i];
        }
    } else {
        c.checkerSize = 1.0f;
        for (int i = 0; i < 3; ++i)
            c.checker1[i] = c.checker2[i] = app.alphaBackground == AlphaBackground::Custom
                                                ? app.customBackground[i]
                                                : kSolidBackgrounds[int(app.alphaBackground) - 1];
    }
    for (int i = 0; i < 3; ++i) c.background[i] = colors.viewport[i];
    c.checker1[3] = c.checker2[3] = c.background[3] = 1.0f;
    c.matchOutline = app.colorMatch && app.matchValid && app.matchMask ? 1u : 0u;
    return c;
}

// `mask` is the Color match mask the outline reads, or none; `image` replaces the shown image, `second` is
// Compare's second image.
void drawImage(ID3D11RenderTargetView* target, const D3D11_VIEWPORT& vp, const ViewConstants& c, ID3D11PixelShader* ps,
               ID3D11ShaderResourceView* mask = nullptr, ID3D11ShaderResourceView* image = nullptr,
               ID3D11ShaderResourceView* second = nullptr) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(app.context->Map(app.constants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    *static_cast<ViewConstants*>(m.pData) = c;
    app.context->Unmap(app.constants.get(), 0);
    ID3D11Buffer* cb = app.constants.get();
    ID3D11ShaderResourceView* srvs[3] = {image ? image : app.srv.get(), mask, second};
    ID3D11SamplerState* sampler = app.trilinear.get();
    // Unbind the mask first: it may be the target of this draw.
    ID3D11ShaderResourceView* none = nullptr;
    app.context->PSSetShaderResources(1, 1, &none);
    app.context->OMSetRenderTargets(1, &target, nullptr);
    app.context->RSSetViewports(1, &vp);
    app.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    app.context->IASetInputLayout(nullptr);
    app.context->VSSetShader(app.vs.get(), nullptr, 0);
    app.context->PSSetShader(ps, nullptr, 0);
    app.context->PSSetConstantBuffers(0, 1, &cb);
    app.context->PSSetShaderResources(0, 3, srvs);
    app.context->PSSetSamplers(0, 1, &sampler);
    app.context->Draw(3, 0);
}

// A render target of the given size and format with a CPU-readable copy.
struct Readback {
    Com<ID3D11Texture2D> target, staging;
    Com<ID3D11RenderTargetView> rtv;
    int width = 0, height = 0;

    bool create(int n, DXGI_FORMAT format) { return create(n, n, format); }
    bool create(int w, int h, DXGI_FORMAT format) {
        if (width == w && height == h && rtv) return true;
        width = height = 0;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = UINT(w);
        d.Height = UINT(h);
        d.MipLevels = d.ArraySize = 1;
        d.Format = format;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(app.device->CreateTexture2D(&d, nullptr, target.put())) ||
            FAILED(app.device->CreateRenderTargetView(target.get(), nullptr, rtv.put())))
            return false;
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(app.device->CreateTexture2D(&d, nullptr, staging.put()))) return false;
        width = w;
        height = h;
        return true;
    }

    // Copies the target to the CPU; `row` gets each row's bytes.
    template <class F>
    bool read(F row) {
        app.context->CopyResource(staging.get(), target.get());
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(app.context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        for (int y = 0; y < height; ++y) row(y, static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch);
        app.context->Unmap(staging.get(), 0);
        return true;
    }
};

}  // namespace

D3DCompileFn loadCompiler() {
    static D3DCompileFn fn = [] {
        HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
        return compiler ? reinterpret_cast<D3DCompileFn>(reinterpret_cast<void (*)()>(GetProcAddress(compiler, "D3DCompile")))
                        : nullptr;
    }();
    return fn;
}

}  // namespace htv

// The ImGui Direct3D 11 backend is compiled with D3DCompile renamed to this, so it shares the lazily loaded compiler.
extern "C" HRESULT WINAPI htvD3DCompile(LPCVOID src, SIZE_T size, LPCSTR name, const D3D_SHADER_MACRO* defines,
                                        ID3DInclude* include, LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2,
                                        ID3DBlob** code, ID3DBlob** errors) {
    auto compile = htv::loadCompiler();
    if (!compile) return E_NOTIMPL;
    return compile(src, size, name, defines, include, entry, target, flags1, flags2, code, errors);
}

namespace htv {

bool createDevice() {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        HRESULT hr = D3D11CreateDevice(nullptr, type, nullptr, 0, levels, UINT(std::size(levels)), D3D11_SDK_VERSION,
                                       app.device.put(), &app.featureLevel, app.context.put());
        if (SUCCEEDED(hr)) {
            app.warp = type == D3D_DRIVER_TYPE_WARP;
            return true;
        }
    }
    return false;
}

// The classic blt model on every Windows version. Flip-model swap chains present around the window's
// redirection surface, and a window that has one can't be layered (translucent or click-through) and still
// show what UpdateLayeredWindow gives it; the blt model leaves that surface alone.
bool createSwapChain() {
    Com<IDXGIDevice> dxgiDevice;
    Com<IDXGIAdapter> adapter;
    Com<IDXGIFactory> factory;
    if (FAILED(app.device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgiDevice.put()))) ||
        FAILED(dxgiDevice->GetAdapter(adapter.put())) ||
        FAILED(adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(factory.put()))))
        return false;
    DXGI_SWAP_CHAIN_DESC d{};
    d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 1;
    d.OutputWindow = app.hwnd;
    d.Windowed = TRUE;
    d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    if (FAILED(factory->CreateSwapChain(app.device.get(), &d, app.swapChain.put()))) return false;
    app.swapEffect = "discard";
    factory->MakeWindowAssociation(app.hwnd, DXGI_MWA_NO_ALT_ENTER);
    return true;
}

bool createRenderTarget() {
    app.rtv.reset();
    app.rtvOnLayer = false;
    Com<ID3D11Texture2D> back;
    if (FAILED(app.swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(back.put()))))
        return false;
    return SUCCEEDED(app.device->CreateRenderTargetView(back.get(), nullptr, app.rtv.put()));
}

void resizeBuffers() {
    if (!app.swapChain) return;
    app.rtv.reset();
    app.context->OMSetRenderTargets(0, nullptr, nullptr);
    app.swapChain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
    createRenderTarget();
}

bool createPipeline(std::string& error) {
    auto compile = loadCompiler();
    if (!compile) {
        error = "d3dcompiler_47.dll not found";
        return false;
    }
    Com<ID3DBlob> vsBlob, psBlob, readBlob, matchBlob, diffBlob, diffReadBlob, log;
    const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS;
    auto build = [&](const char* entry, const char* target, Com<ID3DBlob>& out) {
        return SUCCEEDED(compile(kShader, sizeof(kShader) - 1, "view.hlsl", nullptr, nullptr, entry, target, flags, 0,
                                 out.put(), log.put()));
    };
    if (!build("vsMain", "vs_4_0", vsBlob) || !build("psMain", "ps_4_0", psBlob) || !build("psRead", "ps_4_0", readBlob) ||
        !build("psMatch", "ps_4_0", matchBlob) || !build("psDiff", "ps_4_0", diffBlob) ||
        !build("psDiffRead", "ps_4_0", diffReadBlob)) {
        error = log ? std::string(static_cast<const char*>(log->GetBufferPointer()), log->GetBufferSize())
                    : "shader compilation failed";
        return false;
    }
    if (FAILED(app.device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
                                              app.vs.put())) ||
        FAILED(app.device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr,
                                             app.ps.put())) ||
        FAILED(app.device->CreatePixelShader(readBlob->GetBufferPointer(), readBlob->GetBufferSize(), nullptr,
                                             app.psRead.put())) ||
        FAILED(app.device->CreatePixelShader(matchBlob->GetBufferPointer(), matchBlob->GetBufferSize(), nullptr,
                                             app.psMatch.put())) ||
        FAILED(app.device->CreatePixelShader(diffBlob->GetBufferPointer(), diffBlob->GetBufferSize(), nullptr,
                                             app.psDiff.put())) ||
        FAILED(app.device->CreatePixelShader(diffReadBlob->GetBufferPointer(), diffReadBlob->GetBufferSize(), nullptr,
                                             app.psDiffRead.put()))) {
        error = "cannot create shaders";
        return false;
    }

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(ViewConstants);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_BLEND_FACTOR;
    blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_BLEND_FACTOR;
    blend.RenderTarget[0].BlendOp = blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(app.device->CreateBuffer(&bd, nullptr, app.constants.put())) ||
        FAILED(app.device->CreateSamplerState(&sd, app.trilinear.put())) ||
        FAILED(app.device->CreateBlendState(&blend, app.blendFactor.put()))) {
        error = "cannot create pipeline state";
        return false;
    }
    return true;
}

bool createImageTexture(const htv::Image& img, Com<ID3D11ShaderResourceView>& out, int& mips, std::string& error) {
    const DXGI_FORMAT format = DXGI_FORMAT(img.format);
    const htv::FormatInfo& info = htv::formatInfo(img.format);
    UINT support = 0;
    app.device->CheckFormatSupport(format, &support);
    if (!(support & D3D11_FORMAT_SUPPORT_TEXTURE2D)) {
        error = std::string(info.name) + " needs a Direct3D 11 class GPU";
        return false;
    }
    const UINT maxSize = app.featureLevel >= D3D_FEATURE_LEVEL_11_0 ? 16384 : 8192;
    if (img.width > maxSize || img.height > maxSize) {
        error = "images over " + std::to_string(maxSize) + " px are not supported yet (tiling is planned)";
        return false;
    }
    if (info.compressed && (img.width % 4 || img.height % 4)) {
        error = "compressed textures must be a multiple of 4 px for now";
        return false;
    }

    D3D11_TEXTURE2D_DESC d{};
    d.Width = img.width;
    d.Height = img.height;
    d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    const bool autoMips = img.mips.size() == 1 && (img.width > 1 || img.height > 1) &&
                          (support & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN) && (support & D3D11_FORMAT_SUPPORT_RENDER_TARGET);
    Com<ID3D11Texture2D> texture;
    if (autoMips) {
        d.MipLevels = 0;
        d.BindFlags |= D3D11_BIND_RENDER_TARGET;
        d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if (FAILED(app.device->CreateTexture2D(&d, nullptr, texture.put()))) {
            error = "cannot create texture";
            return false;
        }
        app.context->UpdateSubresource(texture.get(), 0, nullptr, img.mips[0].data.data(), img.mips[0].rowPitch, 0);
    } else {
        d.MipLevels = UINT(img.mips.size());
        std::vector<D3D11_SUBRESOURCE_DATA> init(img.mips.size());
        for (size_t i = 0; i < img.mips.size(); ++i) {
            init[i].pSysMem = img.mips[i].data.data();
            init[i].SysMemPitch = img.mips[i].rowPitch;
        }
        if (FAILED(app.device->CreateTexture2D(&d, init.data(), texture.put()))) {
            error = "cannot create texture";
            return false;
        }
    }
    texture->GetDesc(&d);
    Com<ID3D11ShaderResourceView> srv;
    if (FAILED(app.device->CreateShaderResourceView(texture.get(), nullptr, srv.put()))) {
        error = "cannot create texture view";
        return false;
    }
    if (autoMips) app.context->GenerateMips(srv.get());
    out = std::move(srv);
    mips = int(d.MipLevels);
    return true;
}

bool uploadImage(const htv::Image& img, std::string& error) {
    Com<ID3D11ShaderResourceView> srv;
    int mips = 1;
    if (!createImageTexture(img, srv, mips, error)) return false;
    app.srv = std::move(srv);
    app.textureMips = mips;
    ++app.imageGeneration;
    return true;
}

bool uploadSurface(const htv::Surface& surface, htv::GpuFormat format, std::string& error) {
    htv::Image one;
    one.width = surface.width;
    one.height = surface.height;
    one.format = format;
    one.mips.push_back(surface);
    Com<ID3D11ShaderResourceView> srv;
    int mips = 1;
    if (!createImageTexture(one, srv, mips, error)) return false;
    app.srv = std::move(srv);
    app.textureMips = mips;
    ++app.imageGeneration;
    return true;
}

void releaseImage() {
    app.srv.reset();
    app.hasImage = false;
    app.image = htv::Image();
    app.stats = htv::ImageStats();
    app.pixelLocked = false;
    ++app.imageGeneration;
}

// ---- Compare ----

namespace {

// An image Compare draws: A (0, the shown one), B (1) or C (2).
struct CompareImage {
    const htv::Image* image = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    int generation = 0;
};

CompareImage layer(int i) {
    if (i == 0) return {&app.image, app.hasImage ? app.srv.get() : nullptr, app.imageGeneration};
    const CompareSlot& s = app.compareSlots[i - 1];
    return {&s.image, s.srv.get(), s.generation};
}

// Zoom and the top-left corner of a layer's shown area. B and C cover A's area when sizes are matched, and
// otherwise start at A's corner at A's zoom.
void layerView(const CompareImage& l, float& zoom, float& x0, float& y0) {
    const float w = float(app.rotation & 1 ? l.image->height : l.image->width);
    const float h = float(app.rotation & 1 ? l.image->width : l.image->height);
    float aw, ah;
    imageSizeOnScreen(aw, ah);
    zoom = app.zoom;
    if (l.image != &app.image && app.compareSameSize && w > 0) zoom = app.zoom * aw / w;
    const float around = app.tiling != Tiling::Off ? 1.0f : 0.0f;
    x0 = app.originX - around * w * zoom;
    y0 = app.originY - around * h * zoom;
}

// Constants for drawing layer i into the viewport.
ViewConstants layerConstants(int i, const UiColors& colors) {
    const CompareImage l = layer(i);
    float zoom, x0, y0;
    layerView(l, zoom, x0, y0);
    ViewConstants c = makeConstants(*l.image, x0, y0, zoom, app.tiling == Tiling::Off ? 1.0f : 3.0f,
                                    std::round(kCheckerSizes[app.checkerSize] * app.scale), colors);
    if (i != 0) {
        c.matchOutline = 0;  // the mask is A's
        c.forcedMip = -1;
    }
    return c;
}

const int kPairs[3][2] = {{0, 1}, {0, 2}, {1, 2}};

// The second image of a pair, as psDiff and psDiffRead read it.
void setSecond(ViewConstants& c, const CompareImage& first, const CompareImage& second) {
    c.image2Size[0] = float(second.image->width);
    c.image2Size[1] = float(second.image->height);
    // Matched sizes: B and C over A, and therefore over each other.
    const bool stretch = app.compareSameSize && first.image->width && first.image->height;
    c.scale2[0] = stretch ? float(second.image->width) / float(first.image->width) : 1.0f;
    c.scale2[1] = stretch ? float(second.image->height) / float(first.image->height) : 1.0f;
    c.channels2 = uint32_t(htv::formatInfo(second.image->format).channels);
    c.srgb1 = isSrgbFormat(first.image->format) ? 1u : 0u;
    c.srgb2 = isSrgbFormat(second.image->format) ? 1u : 0u;
    c.diffGain = app.compareGain;
}

void drawCompare(const UiColors& colors) {
    ID3D11RenderTargetView* target = app.rtv.get();
    const float vx0 = app.vpX, vx1 = app.vpX + app.vpW;
    auto draw = [&](int i, float left, float right, ID3D11PixelShader* ps, ID3D11ShaderResourceView* second,
                    const ViewConstants& c) {
        left = std::max(left, vx0);
        right = std::min(right, vx1);
        if (right <= left) return;
        D3D11_VIEWPORT vp{left, app.vpY, right - left, app.vpH, 0, 1};
        drawImage(target, vp, c, ps, c.matchOutline ? app.matchMask.get() : nullptr, layer(i).srv, second);
    };
    if (app.compareMode == CompareMode::Split) {
        int ids[3];
        const int n = compareLayers(ids);
        float aw, ah;
        imageSizeOnScreen(aw, ah);
        for (int k = 0; k < n; ++k) {
            const float left = k == 0 ? vx0 : app.originX + app.compareShutters[k - 1] * aw * app.zoom;
            const float right = k == n - 1 ? vx1 : app.originX + app.compareShutters[k] * aw * app.zoom;
            draw(ids[k], left, right, app.ps.get(), nullptr, layerConstants(ids[k], colors));
        }
        return;
    }
    const int first = kPairs[app.comparePair][0], second = kPairs[app.comparePair][1];
    const CompareImage a = layer(first), b = layer(second);
    if (!a.srv || !b.srv) {  // the pair is not loaded yet: A alone
        draw(0, vx0, vx1, app.ps.get(), nullptr, layerConstants(0, colors));
        return;
    }
    if (app.compareMode == CompareMode::Blend) {
        draw(first, vx0, vx1, app.ps.get(), nullptr, layerConstants(first, colors));
        const float f = std::clamp(app.compareBlend, 0.0f, 1.0f);
        const float factor[4] = {f, f, f, f};
        app.context->OMSetBlendState(app.blendFactor.get(), factor, 0xffffffff);
        ViewConstants c = layerConstants(second, colors);
        c.matchOutline = 0;
        draw(second, vx0, vx1, app.ps.get(), nullptr, c);
        app.context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        return;
    }
    ViewConstants c = layerConstants(first, colors);
    c.matchOutline = 0;
    setSecond(c, a, b);
    draw(first, vx0, vx1, app.psDiff.get(), b.srv, c);
}

}  // namespace

int compareLayers(int* out) {
    int n = 0;
    if (!app.hasImage) return 0;
    out[n++] = 0;
    for (int i = 0; i < 2; ++i)
        if (app.compareSlots[i].srv) out[n++] = i + 1;
    return n;
}

void updateCompareStats() {
    static int key[5] = {-1, -1, -1, -1, -1};
    if (!app.compare || app.compareMode != CompareMode::Difference || !app.psDiffRead) {
        key[0] = -1;
        return;
    }
    const int first = kPairs[app.comparePair][0], second = kPairs[app.comparePair][1];
    const CompareImage a = layer(first), b = layer(second);
    if (!a.srv || !b.srv) {
        app.compareStats = CompareStats();
        key[0] = -1;
        return;
    }
    const int now[5] = {app.comparePair, a.generation, b.generation, app.compareSameSize ? 1 : 0, 1};
    if (std::equal(now, now + 5, key) && app.compareStats.valid) return;
    std::copy(now, now + 5, key);
    app.compareStats = CompareStats();

    // Strips of whole rows of the first image, as scanImageGpu reads.
    const int w = int(a.image->width), h = int(a.image->height);
    const int strip = std::clamp((1 << 20) / std::max(w, 1), 1, h);
    Readback rb;
    if (!rb.create(w, strip, DXGI_FORMAT_R32G32B32A32_FLOAT)) return;
    CompareStats st;
    double sum = 0, sumSq = 0;
    for (int y0 = 0; y0 < h; y0 += strip) {
        ViewConstants c{};
        c.imageSize[0] = float(w);
        c.imageSize[1] = float(h);
        c.imageOrigin[1] = float(y0);
        c.channels = uint32_t(htv::formatInfo(a.image->format).channels);
        setSecond(c, a, b);
        D3D11_VIEWPORT vp{0, 0, float(w), float(strip), 0, 1};
        drawImage(rb.rtv.get(), vp, c, app.psDiffRead.get(), nullptr, a.srv, b.srv);
        const int rows = std::min(strip, h - y0);
        if (!rb.read([&](int y, const uint8_t* p) {
                if (y >= rows) return;
                const float* d = reinterpret_cast<const float*>(p);
                for (int x = 0; x < w; ++x, d += 4) {
                    if (d[0] < 0) continue;  // past the second image
                    const double m = std::max({d[0], d[1], d[2]});
                    if (!(m == m)) continue;  // NaN
                    st.maxDelta = std::max(st.maxDelta, m);
                    sum += double(d[0]) + d[1] + d[2];
                    sumSq += double(d[0]) * d[0] + double(d[1]) * d[1] + double(d[2]) * d[2];
                    ++st.pixels;
                    if (m > 2.5 / 255.0) ++st.pixelsOver;
                }
            }))
            return;
    }
    if (st.pixels) {
        st.meanDelta = sum / (3.0 * double(st.pixels));
        st.mse = sumSq / (3.0 * double(st.pixels));
    }
    st.valid = true;
    app.compareStats = st;
}

namespace {

// Window opacity and click-through need WS_EX_LAYERED, and a layered window shows only what
// UpdateLayeredWindow gives it, whatever a swap chain presents. So while layered, frames are drawn into a
// GDI-compatible texture of the window's size and handed over through its DC with the opacity as the
// constant alpha. If Windows refuses, the window goes back to opaque and normal: a layered window without
// content would be invisible to the mouse.
bool useLayer(bool layered) {
    if (layered != app.layeredStyle) {
        const LONG_PTR ex = GetWindowLongPtrW(app.hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(app.hwnd, GWL_EXSTYLE, layered ? (ex | WS_EX_LAYERED) : (ex & ~LONG_PTR(WS_EX_LAYERED)));
        app.layeredStyle = layered;
    }
    if (!layered) {
        if (app.rtvOnLayer) createRenderTarget();
        app.layer.reset();
        return false;
    }
    D3D11_TEXTURE2D_DESC have{};
    if (app.layer) app.layer->GetDesc(&have);
    // The window's size right now, not the last WM_SIZE: UpdateLayeredWindow resizes the window to the bitmap.
    RECT rc{};
    GetWindowRect(app.hwnd, &rc);
    const UINT w = UINT(std::max(1L, rc.right - rc.left)), h = UINT(std::max(1L, rc.bottom - rc.top));
    if (!app.rtvOnLayer || have.Width != w || have.Height != h) {
        app.rtv.reset();
        app.layer.reset();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // the one format GDI can read
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET;
        d.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
        if (FAILED(app.device->CreateTexture2D(&d, nullptr, app.layer.put())) ||
            FAILED(app.device->CreateRenderTargetView(app.layer.get(), nullptr, app.rtv.put()))) {
            app.layer.reset();
            createRenderTarget();
            return false;
        }
        app.rtvOnLayer = true;
    }
    return true;
}

std::string hexCode(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08X", v);
    return buf;
}

bool presentLayered() {
    app.context->OMSetRenderTargets(0, nullptr, nullptr);  // GetDC wants the texture unbound
    Com<IDXGISurface1> surface;
    HRESULT hr = app.layer->QueryInterface(__uuidof(IDXGISurface1), reinterpret_cast<void**>(surface.put()));
    HDC dc = nullptr;
    if (SUCCEEDED(hr)) hr = surface->GetDC(FALSE, &dc);
    if (FAILED(hr)) {
        app.layerError = "GetDC failed (0x" + hexCode(uint32_t(hr)) + ")";
        return false;
    }
    D3D11_TEXTURE2D_DESC d{};
    app.layer->GetDesc(&d);
    POINT origin{0, 0};
    SIZE size{LONG(d.Width), LONG(d.Height)};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, BYTE(std::lround(app.opacity * 255)), 0};  // constant alpha only
    const BOOL ok = UpdateLayeredWindow(app.hwnd, nullptr, nullptr, &size, dc, &origin, 0, &blend, ULW_ALPHA);
    const DWORD err = ok ? 0 : GetLastError();
    surface->ReleaseDC(nullptr);
    if (!ok) {
        app.layerError = "UpdateLayeredWindow failed (error " + std::to_string(err) + ")";
        return false;
    }
    app.layerError.clear();
    return true;
}

}  // namespace

void render() {
    static bool inRender = false;  // WM_PAINT can arrive while a frame is being built
    if (inRender) return;
    const bool layered = useLayer(app.opacity < 1.0f || app.clickThrough);
    if (!app.rtv) return;
    inRender = true;

    updateColorMatch();
    updateCompareStats();
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    uiFrame();
    ImGui::Render();

    const UiColors colors = uiColors();
    float clear[4] = {colors.viewport[0], colors.viewport[1], colors.viewport[2], 1.0f};
    app.context->ClearRenderTargetView(app.rtv.get(), clear);
    int layers[3];
    if (app.compare && app.hasImage && app.srv && compareLayers(layers) > 1) {
        drawCompare(colors);
    } else if (app.hasImage && app.srv) {
        float x0, y0, x1, y1;
        shownArea(x0, y0, x1, y1);
        const ViewConstants c = makeConstants(app.image, x0, y0, app.zoom, app.tiling == Tiling::Off ? 1.0f : 3.0f,
                                              std::round(kCheckerSizes[app.checkerSize] * app.scale), colors);
        // SV_Position is in window pixels whatever the viewport; the viewport only clips to the image area.
        D3D11_VIEWPORT vp{app.vpX, app.vpY, app.vpW, app.vpH, 0, 1};
        drawImage(app.rtv.get(), vp, c, app.ps.get(), c.matchOutline ? app.matchMask.get() : nullptr);
    }
    ID3D11RenderTargetView* rtv = app.rtv.get();
    app.context->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    if (layered && app.rtvOnLayer) {
        if (!presentLayered()) {
            app.opacity = 1.0f;
            app.clickThrough = false;
            const LONG_PTR ex = GetWindowLongPtrW(app.hwnd, GWL_EXSTYLE);
            SetWindowLongPtrW(app.hwnd, GWL_EXSTYLE, ex & ~LONG_PTR(WS_EX_TRANSPARENT));
            InvalidateRect(app.hwnd, nullptr, FALSE);  // the next frame drops the layered style and presents
        }
    } else {
        app.swapChain->Present(1, 0);
    }
    inRender = false;

    if (!app.firstFrameDone) {
        app.firstFrameDone = true;
        if (!app.benchPath.empty()) {
            writeBench(app.hasImage);
            PostQuitMessage(app.hasImage ? 0 : 1);
        }
    }
}

namespace {

std::vector<uint8_t> renderThumbnailOf(const htv::Image& image, ID3D11ShaderResourceView* srv, int size,
                                       bool contain = false) {
    std::vector<uint8_t> out;
    static Readback rb;
    if (!srv || !rb.create(size, DXGI_FORMAT_R8G8B8A8_UNORM)) return out;
    // Cover the square, like the design's thumbnails (or fit inside it with `contain`); rotation and flips as
    // on screen, no tiling.
    const float w = float(app.rotation & 1 ? image.height : image.width);
    const float h = float(app.rotation & 1 ? image.width : image.height);
    const float zoom = float(size) / (contain ? std::max(w, h) : std::min(w, h));
    ViewConstants c =
        makeConstants(image, (size - w * zoom) * 0.5f, (size - h * zoom) * 0.5f, zoom, 1.0f, 4.0f, uiColors());
    c.clipping = 0;
    c.matchOutline = 0;
    c.forcedMip = -1;
    D3D11_VIEWPORT vp{0, 0, float(size), float(size), 0, 1};
    drawImage(rb.rtv.get(), vp, c, app.ps.get(), nullptr, srv);
    out.resize(size_t(size) * size * 4);
    if (!rb.read([&](int y, const uint8_t* row) { std::copy(row, row + size * 4, out.data() + size_t(y) * size * 4); }))
        out.clear();
    return out;
}

}  // namespace

std::vector<uint8_t> renderThumbnail(int size) {
    if (!app.hasImage) return {};
    return renderThumbnailOf(app.image, app.srv.get(), size);
}

ID3D11ShaderResourceView* compareThumbnail(int slot) {
    constexpr int kSize = 64;
    static Com<ID3D11ShaderResourceView> a;
    static int aGeneration = -1, aRotation = -1;
    if (slot < 0) {
        if (!app.hasImage) return nullptr;
        if (aGeneration != app.imageGeneration || aRotation != app.rotation) {
            const std::vector<uint8_t> px = renderThumbnailOf(app.image, app.srv.get(), kSize);
            a = px.empty() ? Com<ID3D11ShaderResourceView>() : createTexture(px.data(), kSize, kSize);
            aGeneration = app.imageGeneration;
            aRotation = app.rotation;
        }
        return a.get();
    }
    CompareSlot& s = app.compareSlots[slot];
    if (s.srv && !s.thumbnail) {
        const std::vector<uint8_t> px = renderThumbnailOf(s.image, s.srv.get(), kSize);
        if (!px.empty()) s.thumbnail = createTexture(px.data(), kSize, kSize);
    }
    return s.thumbnail.get();
}

ID3D11ShaderResourceView* layerThumbnail(int index) {
    constexpr int kSize = 64;
    static std::vector<Com<ID3D11ShaderResourceView>> thumbs;
    static int fileGeneration = -1, rotation = -1, madeInFrame = -1, made = 0;
    if (!app.hasImage || index < 0 || index >= int(app.image.layers.size())) return nullptr;
    if (fileGeneration != app.fileGeneration || rotation != app.rotation) {
        thumbs.clear();
        fileGeneration = app.fileGeneration;
        rotation = app.rotation;
    }
    thumbs.resize(app.image.layers.size());
    if (thumbs[size_t(index)]) return thumbs[size_t(index)].get();
    if (app.image.layers[size_t(index)].group) return nullptr;
    // A few a frame, so a volume with hundreds of slices doesn't stall the window.
    const int frame = ImGui::GetFrameCount();
    if (madeInFrame != frame) {
        madeInFrame = frame;
        made = 0;
    }
    if (made >= 3) {
        requestRedraw();
        return nullptr;
    }
    ++made;
    std::vector<uint8_t> px;
    const htv::Layer& l = app.image.layers[size_t(index)];
    const bool onScreen = l.mips.empty() && app.image.layerKind != htv::LayerKind::PsdLayers;
    if (onScreen && index == app.image.shownLayer) {
        px = renderThumbnailOf(app.image, app.srv.get(), kSize);
    } else if (!l.mips.empty()) {
        // The smallest mip that still covers the thumbnail is enough.
        size_t level = 0;
        while (level + 1 < l.mips.size() && std::min(l.mips[level + 1].width, l.mips[level + 1].height) >= uint32_t(kSize))
            ++level;
        htv::Image one;
        one.width = l.mips[level].width;
        one.height = l.mips[level].height;
        one.format = l.format;
        one.maxValue = l.maxValue;
        one.mips.push_back(l.mips[level]);
        Com<ID3D11ShaderResourceView> srv;
        int mips = 1;
        std::string error;
        // PSD layers come in any shape: the whole of each fits in its square.
        if (createImageTexture(one, srv, mips, error))
            px = renderThumbnailOf(one, srv.get(), kSize, app.image.layerKind == htv::LayerKind::PsdLayers);
    }
    Com<ID3D11ShaderResourceView> texture;
    if (!px.empty()) texture = createTexture(px.data(), kSize, kSize);
    if (!texture) texture = createTexture(std::vector<uint8_t>(size_t(kSize) * kSize * 4, 0).data(), kSize, kSize);
    thumbs[size_t(index)] = std::move(texture);
    return thumbs[size_t(index)].get();
}

bool readPixelsGpu(int x, int y, int n, float* rgba) {
    static Readback rb;
    if (!app.hasImage || !app.srv || !app.psRead || !rb.create(5, DXGI_FORMAT_R32G32B32A32_FLOAT)) return false;
    // The same block is asked for every frame while the mouse rests; read it from the GPU only once.
    static int cachedX = -1, cachedY = -1, cachedN = 0, cachedGeneration = -1;
    static float cached[25 * 4];
    if (x != cachedX || y != cachedY || n != cachedN || app.imageGeneration != cachedGeneration) {
        ViewConstants c{};
        c.imageSize[0] = float(app.image.width);
        c.imageSize[1] = float(app.image.height);
        c.imageOrigin[0] = float(x - n / 2);
        c.imageOrigin[1] = float(y - n / 2);
        D3D11_VIEWPORT vp{0, 0, float(n), float(n), 0, 1};
        drawImage(rb.rtv.get(), vp, c, app.psRead.get());
        if (!rb.read([&](int row, const uint8_t* p) {
                if (row < n) std::memcpy(cached + row * n * 4, p, size_t(n) * 16);
            }))
            return false;
        cachedX = x;
        cachedY = y;
        cachedN = n;
        cachedGeneration = app.imageGeneration;
    }
    std::memcpy(rgba, cached, size_t(n) * n * 16);
    return true;
}

bool scanImageGpu(const std::function<void(const float* rgba, int width)>& row) {
    if (!app.hasImage || !app.srv || !app.psRead) return false;
    // Strips of whole rows, about 16 MB each, so even an 8K image needs no big buffer.
    const int w = int(app.image.width), h = int(app.image.height);
    const int strip = std::clamp((1 << 20) / std::max(w, 1), 1, h);
    Readback rb;
    if (!rb.create(w, strip, DXGI_FORMAT_R32G32B32A32_FLOAT)) return false;
    for (int y0 = 0; y0 < h; y0 += strip) {
        ViewConstants c{};
        c.imageSize[0] = float(w);
        c.imageSize[1] = float(h);
        c.imageOrigin[1] = float(y0);
        D3D11_VIEWPORT vp{0, 0, float(w), float(strip), 0, 1};
        drawImage(rb.rtv.get(), vp, c, app.psRead.get());
        const int rows = std::min(strip, h - y0);
        if (!rb.read([&](int y, const uint8_t* p) {
                if (y < rows) row(reinterpret_cast<const float*>(p), w);
            }))
            return false;
    }
    return true;
}

namespace {

// As the shader's srgbEncode.
float srgbEncode(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

}  // namespace

bool setMatchTarget(int x, int y) {
    float c[4];
    if (!readPixelsGpu(x, y, 1, c)) return false;
    if (isSrgbFormat(app.image.format))
        for (int i = 0; i < 3; ++i) c[i] = srgbEncode(c[i]);
    const int channels = htv::formatInfo(app.image.format).channels;
    if (channels == 1) c[1] = c[2] = c[0], c[3] = 1.0f;
    else if (channels == 2) c[2] = 0.0f, c[3] = 1.0f;
    std::copy(c, c + 4, app.matchTarget);
    app.matchHasTarget = true;
    app.matchValid = false;
    return true;
}

void updateColorMatch() {
    // What the current mask was made from.
    static int generation = -1, mode = -1, tolerance = -1;
    static float target[4];
    if (!app.colorMatch || !app.matchHasTarget || !app.hasImage || !app.srv || !app.psMatch) {
        app.matchValid = false;
        generation = -1;
        return;
    }
    if (generation == app.imageGeneration && mode == app.matchMode && tolerance == app.matchTolerance &&
        std::equal(target, target + 4, app.matchTarget) && app.matchValid)
        return;
    if (generation != app.imageGeneration) app.matchRegion = -1;
    generation = app.imageGeneration;
    mode = app.matchMode;
    tolerance = app.matchTolerance;
    std::copy(app.matchTarget, app.matchTarget + 4, target);
    app.matchValid = false;

    // One byte per pixel: rendered on the GPU, read back for the count and the regions.
    static Com<ID3D11Texture2D> texture, staging;
    static Com<ID3D11RenderTargetView> rtv;
    static int width = 0, height = 0;
    const int w = int(app.image.width), h = int(app.image.height);
    if (w != width || h != height || !rtv) {
        width = height = 0;
        app.matchMask.reset();
        rtv.reset();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = UINT(w);
        d.Height = UINT(h);
        d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(app.device->CreateTexture2D(&d, nullptr, texture.put())) ||
            FAILED(app.device->CreateRenderTargetView(texture.get(), nullptr, rtv.put())) ||
            FAILED(app.device->CreateShaderResourceView(texture.get(), nullptr, app.matchMask.put())))
            return;
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(app.device->CreateTexture2D(&d, nullptr, staging.put()))) return;
        width = w;
        height = h;
    }
    ViewConstants c{};
    c.imageSize[0] = float(w);
    c.imageSize[1] = float(h);
    c.channels = uint32_t(htv::formatInfo(app.image.format).channels);
    std::copy(app.matchTarget, app.matchTarget + 4, c.matchTarget);
    c.matchMode = uint32_t(app.matchMode);
    // Half a step more, so values that went through decoding and encoding still meet at ± 0.
    c.matchTolerance = (float(app.matchTolerance) + 0.5f) / 255.0f;
    c.matchSrgb = isSrgbFormat(app.image.format) ? 1u : 0u;
    D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    drawImage(rtv.get(), vp, c, app.psMatch.get());
    app.context->CopyResource(staging.get(), texture.get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(app.context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m))) return;
    app.matchRegions = htv::findRegions(static_cast<const uint8_t*>(m.pData), w, h, m.RowPitch);
    app.context->Unmap(staging.get(), 0);
    if (app.matchRegion >= int(app.matchRegions.boxes.size())) app.matchRegion = -1;
    app.matchValid = true;
}

Com<ID3D11ShaderResourceView> createTexture(const uint8_t* rgba, int width, int height) {
    Com<ID3D11ShaderResourceView> srv;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = UINT(width);
    d.Height = UINT(height);
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{rgba, UINT(width * 4), 0};
    Com<ID3D11Texture2D> tex;
    if (SUCCEEDED(app.device->CreateTexture2D(&d, &init, tex.put())))
        app.device->CreateShaderResourceView(tex.get(), nullptr, srv.put());
    return srv;
}

// HDR monitors report their peak brightness; the Range slider marks it relative to SDR white (80 nits).
void queryDisplayPeak() {
    app.displayPeak = 0.0f;
    Com<IDXGIOutput> output;
    Com<IDXGIOutput6> output6;
    DXGI_OUTPUT_DESC1 desc{};
    if (app.swapChain && SUCCEEDED(app.swapChain->GetContainingOutput(output.put())) &&
        SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(output6.put()))) &&
        SUCCEEDED(output6->GetDesc1(&desc)) && desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
        app.displayPeak = desc.MaxLuminance / 80.0f;
}

}  // namespace htv
