// Direct3D 11: device, swap chain, the image shader and the frame. All view math happens in one pixel shader.

#include "viewer.h"

#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <cstring>
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
    uint normalMap;        // 1 rebuild Z from X and Y, 2 flip green
    int forcedMip;         // -1: pick the mip from the zoom
    float invGamma;
    float checkerSize;     // target pixels per checker cell
    float blackPoint;      // HDR range: values map to (v - blackPoint) * rangeScale
    float rangeScale;
    uint toneMap;          // 0 clip, 1 Reinhard, 2 ACES, 3 AgX
    uint clipping;         // zebra above the white point, magenta NaN and Inf
    float zebraWidth;      // target pixels per stripe
    float2 padding;
    float4 checker1;       // alpha background: checker colours (equal for a solid colour)
    float4 checker2;
    float4 background;     // viewport outside the image
};
Texture2D<float4> tex : register(t0);
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

float4 psMain(float4 pos : SV_Position) : SV_Target {
    float2 shownSize = (rotation & 1) ? imageSize.yx : imageSize;
    float2 d = (pos.xy - imageOrigin) / zoom;
    if (d.x < 0 || d.y < 0 || d.x >= shownSize.x * tiles || d.y >= shownSize.y * tiles) return background;
    d = fmod(d, shownSize);
    if (flips & 1) d.x = shownSize.x - d.x;
    if (flips & 2) d.y = shownSize.y - d.y;
    float2 ip = d;
    if (rotation == 1) ip = float2(d.y, shownSize.x - d.x);
    else if (rotation == 2) ip = imageSize - d;
    else if (rotation == 3) ip = float2(shownSize.y - d.y, d.x);
    int2 texel = int2(clamp(floor(ip), 0, imageSize - 1));

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
    if (normalMap & 2) c.g = 1 - c.g;
    if (normalMap & 1) {
        float2 n = c.rg * 2 - 1;
        c.b = sqrt(saturate(1 - dot(n, n))) * 0.5 + 0.5;
    }

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
    if (clipped) rgb = fmod(floor((pos.x + pos.y) / zebraWidth), 2) < 1 ? float3(0, 0, 0) : float3(1, 1, 1);

    float2 cell = floor((pos.xy - imageOrigin) / checkerSize);
    float3 under = fmod(cell.x + cell.y, 2) < 1 ? checker1.rgb : checker2.rgb;
    return float4(lerp(under, rgb, saturate(a)), 1);
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
    uint32_t normalMap;
    int32_t forcedMip;
    float invGamma;
    float checkerSize;
    float blackPoint;
    float rangeScale;
    uint32_t toneMap;
    uint32_t clipping;
    float zebraWidth;
    float padding[2];  // checker1 starts a new 16-byte register
    float checker1[4];
    float checker2[4];
    float background[4];
};
static_assert(sizeof(ViewConstants) % 16 == 0, "constant buffer size must be a multiple of 16");

// d3dcompiler_47.dll ships with Windows 8.1 and later; loading it lazily keeps it off the import table.
using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                      UINT, UINT, ID3DBlob**, ID3DBlob**);

const float kCheckerSizes[3] = {8, 16, 32};
const float kSolidBackgrounds[3] = {0.0f, 0.5f, 1.0f};  // black, middle grey, white

ViewConstants makeConstants(float originX, float originY, float zoom, float tiles, float checkerSize,
                            const UiColors& colors) {
    const htv::FormatInfo& info = htv::formatInfo(app.image.format);
    ViewConstants c{};
    c.imageSize[0] = float(app.image.width);
    c.imageSize[1] = float(app.image.height);
    c.imageOrigin[0] = originX;
    c.imageOrigin[1] = originY;
    c.zoom = zoom;
    const bool hdr = isHighPrecision(app.image);
    c.exposureScale = hdr ? std::exp2(app.exposure) : 1.0f;
    const uint32_t modes[] = {0, 1, 2, 3, 4, 6};
    c.channelMode = modes[int(app.channel)];
    if (c.channelMode == 0 && app.ignoreAlpha) c.channelMode = 5;
    c.channels = uint32_t(info.channels);
    c.transfer = (app.dataIsLinear ? 1u : 0u) + (isSrgbFormat(app.image.format) ? 1u : 0u);
    c.signedValues = info.snorm ? 1u : 0u;
    c.rotation = uint32_t(app.rotation);
    c.flips = (app.flipH ? 1u : 0u) | (app.flipV ? 2u : 0u);
    c.tiles = tiles;
    c.normalMap = (app.normalZ ? 1u : 0u) | (app.flipGreen ? 2u : 0u);
    c.forcedMip = app.forcedMip;
    c.invGamma = 1.0f / app.gamma;
    // Range and tone mapping belong to the HDR row, which only float and 16-bit images show.
    c.blackPoint = hdr ? app.blackPoint : 0.0f;
    c.rangeScale = hdr ? 1.0f / std::max(app.whitePoint - app.blackPoint, 1e-6f) : 1.0f;
    c.toneMap = hdr && app.dataIsLinear ? uint32_t(app.toneMap) : 0u;
    c.clipping = hdr && app.showClipping ? 1u : 0u;
    c.zebraWidth = std::round(4 * app.scale);
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
    return c;
}

void drawImage(ID3D11RenderTargetView* target, const D3D11_VIEWPORT& vp, const ViewConstants& c, ID3D11PixelShader* ps) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(app.context->Map(app.constants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    *static_cast<ViewConstants*>(m.pData) = c;
    app.context->Unmap(app.constants.get(), 0);
    ID3D11Buffer* cb = app.constants.get();
    ID3D11ShaderResourceView* srv = app.srv.get();
    ID3D11SamplerState* sampler = app.trilinear.get();
    app.context->OMSetRenderTargets(1, &target, nullptr);
    app.context->RSSetViewports(1, &vp);
    app.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    app.context->IASetInputLayout(nullptr);
    app.context->VSSetShader(app.vs.get(), nullptr, 0);
    app.context->PSSetShader(ps, nullptr, 0);
    app.context->PSSetConstantBuffers(0, 1, &cb);
    app.context->PSSetShaderResources(0, 1, &srv);
    app.context->PSSetSamplers(0, 1, &sampler);
    app.context->Draw(3, 0);
}

// A render target of the given size and format with a CPU-readable copy.
struct Readback {
    Com<ID3D11Texture2D> target, staging;
    Com<ID3D11RenderTargetView> rtv;
    int size = 0;

    bool create(int n, DXGI_FORMAT format) {
        if (size == n && rtv) return true;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = d.Height = UINT(n);
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
        size = n;
        return true;
    }

    // Copies the target to the CPU; `row` gets each row's bytes.
    template <class F>
    bool read(F row) {
        app.context->CopyResource(staging.get(), target.get());
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(app.context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        for (int y = 0; y < size; ++y) row(y, static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch);
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

// Flip model where available (Windows 10, then 8), the classic blt model on Windows 7. A translucent
// (layered) window needs the blt model, so changing the window opacity recreates the swap chain.
bool createSwapChain(bool allowFlip) {
    Com<IDXGIDevice> dxgiDevice;
    Com<IDXGIAdapter> adapter;
    Com<IDXGIFactory> factory;
    if (FAILED(app.device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgiDevice.put()))) ||
        FAILED(dxgiDevice->GetAdapter(adapter.put())) ||
        FAILED(adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(factory.put()))))
        return false;

    Com<IDXGIFactory2> factory2;
    if (allowFlip &&
        SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(factory2.put())))) {
        const std::pair<DXGI_SWAP_EFFECT, const char*> effects[] = {{DXGI_SWAP_EFFECT(4), "flip-discard"},
                                                                    {DXGI_SWAP_EFFECT(3), "flip-sequential"}};
        for (auto [effect, name] : effects) {
            DXGI_SWAP_CHAIN_DESC1 d{};
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            d.BufferCount = 2;
            d.SwapEffect = effect;
            Com<IDXGISwapChain1> sc1;
            if (SUCCEEDED(factory2->CreateSwapChainForHwnd(app.device.get(), app.hwnd, &d, nullptr, nullptr, sc1.put()))) {
                sc1->QueryInterface(__uuidof(IDXGISwapChain), reinterpret_cast<void**>(app.swapChain.put()));
                app.swapEffect = name;
                break;
            }
        }
    }
    if (!app.swapChain) {
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
    }
    factory->MakeWindowAssociation(app.hwnd, DXGI_MWA_NO_ALT_ENTER);
    return true;
}

bool createRenderTarget() {
    app.rtv.reset();
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
    Com<ID3DBlob> vsBlob, psBlob, readBlob, log;
    const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS;
    auto build = [&](const char* entry, const char* target, Com<ID3DBlob>& out) {
        return SUCCEEDED(compile(kShader, sizeof(kShader) - 1, "view.hlsl", nullptr, nullptr, entry, target, flags, 0,
                                 out.put(), log.put()));
    };
    if (!build("vsMain", "vs_4_0", vsBlob) || !build("psMain", "ps_4_0", psBlob) || !build("psRead", "ps_4_0", readBlob)) {
        error = log ? std::string(static_cast<const char*>(log->GetBufferPointer()), log->GetBufferSize())
                    : "shader compilation failed";
        return false;
    }
    if (FAILED(app.device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
                                              app.vs.put())) ||
        FAILED(app.device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr,
                                             app.ps.put())) ||
        FAILED(app.device->CreatePixelShader(readBlob->GetBufferPointer(), readBlob->GetBufferSize(), nullptr,
                                             app.psRead.put()))) {
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
    if (FAILED(app.device->CreateBuffer(&bd, nullptr, app.constants.put())) ||
        FAILED(app.device->CreateSamplerState(&sd, app.trilinear.put()))) {
        error = "cannot create pipeline state";
        return false;
    }
    return true;
}

bool uploadImage(const htv::Image& img, std::string& error) {
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
    app.srv = std::move(srv);
    app.textureMips = int(d.MipLevels);
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

void render() {
    if (!app.rtv) return;
    static bool inRender = false;  // WM_PAINT can arrive while a frame is being built
    if (inRender) return;
    inRender = true;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    uiFrame();
    ImGui::Render();

    const UiColors colors = uiColors();
    float clear[4] = {colors.viewport[0], colors.viewport[1], colors.viewport[2], 1.0f};
    app.context->ClearRenderTargetView(app.rtv.get(), clear);
    if (app.hasImage && app.srv) {
        const ViewConstants c = makeConstants(app.originX, app.originY, app.zoom, app.tiling == Tiling::Off ? 1.0f : 3.0f,
                                              std::round(kCheckerSizes[app.checkerSize] * app.scale), colors);
        // SV_Position is in window pixels whatever the viewport; the viewport only clips to the image area.
        D3D11_VIEWPORT vp{app.vpX, app.vpY, app.vpW, app.vpH, 0, 1};
        drawImage(app.rtv.get(), vp, c, app.ps.get());
    }
    ID3D11RenderTargetView* rtv = app.rtv.get();
    app.context->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    app.swapChain->Present(1, 0);
    inRender = false;

    if (!app.firstFrameDone) {
        app.firstFrameDone = true;
        if (!app.benchPath.empty()) {
            writeBench(app.hasImage);
            PostQuitMessage(app.hasImage ? 0 : 1);
        }
    }
}

std::vector<uint8_t> renderThumbnail(int size) {
    std::vector<uint8_t> out;
    static Readback rb;
    if (!app.hasImage || !app.srv || !rb.create(size, DXGI_FORMAT_R8G8B8A8_UNORM)) return out;
    // Cover the square, like the design's thumbnails; rotation and flips as on screen, no tiling.
    const float w = float(app.rotation & 1 ? app.image.height : app.image.width);
    const float h = float(app.rotation & 1 ? app.image.width : app.image.height);
    const float zoom = float(size) / std::min(w, h);
    ViewConstants c = makeConstants((size - w * zoom) * 0.5f, (size - h * zoom) * 0.5f, zoom, 1.0f, 4.0f, uiColors());
    c.clipping = 0;
    c.forcedMip = -1;
    D3D11_VIEWPORT vp{0, 0, float(size), float(size), 0, 1};
    drawImage(rb.rtv.get(), vp, c, app.ps.get());
    out.resize(size_t(size) * size * 4);
    if (!rb.read([&](int y, const uint8_t* row) { std::copy(row, row + size * 4, out.data() + size_t(y) * size * 4); }))
        out.clear();
    return out;
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
