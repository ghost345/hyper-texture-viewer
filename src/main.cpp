// Hyper Texture Viewer: one borderless Win32 window, Direct3D 11, all view math in one pixel shader,
// the interface drawn with Dear ImGui on top (ui.cpp).

#include "app.h"

#include <windowsx.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <d3dcompiler.h>
#include <dwmapi.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <iterator>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "ui.h"
#include "wic_loader.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace fs = std::filesystem;

namespace htv {

App app;

namespace {

// ---- timing ----

double qpcMs() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) * 1000.0 / double(freq.QuadPart);
}

// Milliseconds since the OS created this process, so the benchmark includes loader and CRT start-up.
double msSinceProcessStart() {
    FILETIME created, exited, kernel, user, now;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    GetSystemTimeAsFileTime(&now);
    auto ticks = [](FILETIME f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return double(ticks(now) - ticks(created)) / 10000.0;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool readFile(const fs::path& path, std::vector<uint8_t>& out, std::string& error) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        error = "cannot open file";
        return false;
    }
    LARGE_INTEGER size;
    GetFileSizeEx(f, &size);
    if (size.QuadPart > 0x7fffffff) {
        CloseHandle(f);
        error = "files over 2 GB are not supported yet";
        return false;
    }
    out.resize(size_t(size.QuadPart));
    DWORD read = 0;
    BOOL ok = out.empty() || ReadFile(f, out.data(), DWORD(out.size()), &read, nullptr);
    CloseHandle(f);
    if (!ok || read != out.size()) {
        error = "cannot read file";
        return false;
    }
    return true;
}

std::string lowerExtension(const fs::path& p) {
    std::string e = narrow(p.extension().wstring());
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    for (char& c : e) c = char(tolower(static_cast<unsigned char>(c)));
    return e;
}

bool hasBuiltInDecoder(const std::string& ext) {
    const auto& list = htv::supportedExtensions();
    return std::find(list.begin(), list.end(), ext) != list.end();
}

bool isSupported(const fs::path& p) {
    const std::string ext = lowerExtension(p);
    const auto& wic = htv::wicExtensions();
    return hasBuiltInDecoder(ext) || std::find(wic.begin(), wic.end(), ext) != wic.end();
}

// ---- shaders ----

const char kShader[] = R"(
cbuffer View : register(b0) {
    float2 imageSize;      // source image in pixels
    float2 imageOrigin;    // top-left corner of the shown (rotated, tiled) image in window pixels
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
    float checkerSize;     // window pixels per checker cell
    float3 pad;
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

    // NaN and Inf show as magenta. Written without isnan/isinf so every HLSL compiler accepts it.
    if (any(c != c) || any(abs(c) > 3.402823e38)) return float4(1, 0, 1, 1);

    float3 rgb = c.rgb;
    float a = c.a;
    if (channelMode == 1) rgb = c.rrr;
    else if (channelMode == 2) rgb = c.ggg;
    else if (channelMode == 3) rgb = c.bbb;
    else if (channelMode == 4) rgb = c.aaa;
    else if (channelMode == 6) rgb = dot(c.rgb, float3(0.2126, 0.7152, 0.0722)).xxx;
    if (channelMode != 0) a = 1;

    if (channelMode != 4) rgb *= exposureScale;
    if (transfer >= 1) rgb = srgbEncode(rgb);
    if (transfer >= 2) rgb = srgbEncode(rgb);
    rgb = pow(saturate(rgb), invGamma);

    float2 cell = floor((pos.xy - imageOrigin) / checkerSize);
    float3 under = fmod(cell.x + cell.y, 2) < 1 ? checker1.rgb : checker2.rgb;
    return float4(lerp(under, rgb, saturate(a)), 1);
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
    float pad[3];
    float checker1[4];
    float checker2[4];
    float background[4];
};
static_assert(sizeof(ViewConstants) % 16 == 0, "constant buffer size must be a multiple of 16");

// d3dcompiler_47.dll ships with Windows 8.1 and later; loading it lazily keeps it off the import table.
using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                      UINT, UINT, ID3DBlob**, ID3DBlob**);

D3DCompileFn loadCompiler() {
    static D3DCompileFn fn = [] {
        HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
        return compiler ? reinterpret_cast<D3DCompileFn>(reinterpret_cast<void (*)()>(GetProcAddress(compiler, "D3DCompile")))
                        : nullptr;
    }();
    return fn;
}

}  // namespace
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
namespace {

// ---- device ----

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

bool createPipeline(std::string& error) {
    auto compile = loadCompiler();
    if (!compile) {
        error = "d3dcompiler_47.dll not found";
        return false;
    }
    Com<ID3DBlob> vsBlob, psBlob, log;
    const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS;
    if (FAILED(compile(kShader, sizeof(kShader) - 1, "view.hlsl", nullptr, nullptr, "vsMain", "vs_4_0", flags, 0,
                       vsBlob.put(), log.put())) ||
        FAILED(compile(kShader, sizeof(kShader) - 1, "view.hlsl", nullptr, nullptr, "psMain", "ps_4_0", flags, 0,
                       psBlob.put(), log.put()))) {
        error = log ? std::string(static_cast<const char*>(log->GetBufferPointer()), log->GetBufferSize())
                    : "shader compilation failed";
        return false;
    }
    if (FAILED(app.device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
                                              app.vs.put())) ||
        FAILED(app.device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr,
                                             app.ps.put()))) {
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
    app.textureMips = int(d.MipLevels);
    app.srv.reset();
    if (FAILED(app.device->CreateShaderResourceView(texture.get(), nullptr, app.srv.put()))) {
        error = "cannot create texture view";
        return false;
    }
    if (autoMips) app.context->GenerateMips(app.srv.get());
    return true;
}

// ---- folder ----

void listFolder(const fs::path& file) {
    app.folder.clear();
    app.folderIndex = -1;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(file.parent_path(), ec))
        if (e.is_regular_file(ec) && isSupported(e.path())) app.folder.push_back(e.path());
    std::sort(app.folder.begin(), app.folder.end(), [](const fs::path& a, const fs::path& b) {
        return StrCmpLogicalW(a.filename().c_str(), b.filename().c_str()) < 0;
    });
    auto it = std::find(app.folder.begin(), app.folder.end(), file);
    if (it != app.folder.end()) app.folderIndex = int(it - app.folder.begin());
}

void writeBench(bool ok);

bool openFile(const fs::path& path, bool relist) {
    std::string error;
    const double t0 = qpcMs();
    app.path = path;
    if (relist || app.folder.empty()) listFolder(path);
    else {
        auto it = std::find(app.folder.begin(), app.folder.end(), path);
        app.folderIndex = it == app.folder.end() ? -1 : int(it - app.folder.begin());
    }
    const std::string ext = lowerExtension(path);
    htv::LoadResult r;
    if (hasBuiltInDecoder(ext)) {
        std::vector<uint8_t> bytes;
        if (!readFile(path, bytes, error)) {
            app.error = error;
            return false;
        }
        r = htv::loadImage(bytes.data(), bytes.size(), ext);
    }
    // Anything the built-in decoders don't cover goes to the codecs installed in Windows.
    if (!r.ok) {
        htv::LoadResult wic = htv::loadWithWic(path);
        if (wic.ok || r.error.empty()) r = std::move(wic);
    }
    const double t1 = qpcMs();
    if (!r.ok || !uploadImage(r.image, error)) {
        app.error = r.ok ? error : r.error;
        return false;
    }
    app.context->Flush();
    const double t2 = qpcMs();
    app.tLoad = t1 - t0;
    app.tUpload = t2 - t1;
    app.image = std::move(r.image);
    app.hasImage = true;
    app.shownPath = path;
    std::error_code ec;
    app.fileSize = fs::file_size(path, ec);
    app.error.clear();
    app.dataIsLinear = isFloatFormat(htv::formatInfo(app.image.format));
    if (app.forcedMip >= app.textureMips) app.forcedMip = -1;
    if (app.pixelLocked) {
        app.lockedX = std::min(app.lockedX, int(app.image.width) - 1);
        app.lockedY = std::min(app.lockedY, int(app.image.height) - 1);
    }
    if (app.fit) fitToWindow();
    SetWindowTextW(app.hwnd, (path.filename().wstring() + L" - Hyper Texture Viewer").c_str());  // taskbar name
    return true;
}

// ---- drawing ----

const float kAlphaBackgrounds[int(AlphaBackground::Count)][3] = {
    {4, 0, 0}, {8, 0, 0}, {16, 0, 0}, {0, 0, 0}, {0.5f, 0, 0}, {1, 0, 0}};

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
        const htv::FormatInfo& info = htv::formatInfo(app.image.format);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(app.context->Map(app.constants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            ViewConstants c{};
            c.imageSize[0] = float(app.image.width);
            c.imageSize[1] = float(app.image.height);
            c.imageOrigin[0] = app.originX;
            c.imageOrigin[1] = app.originY;
            c.zoom = app.zoom;
            c.exposureScale = std::exp2(app.exposure);
            const uint32_t modes[] = {0, 1, 2, 3, 4, 6};
            c.channelMode = modes[int(app.channel)];
            if (c.channelMode == 0 && app.ignoreAlpha) c.channelMode = 5;
            c.channels = uint32_t(info.channels);
            c.transfer = (app.dataIsLinear ? 1u : 0u) + (isSrgbFormat(app.image.format) ? 1u : 0u);
            c.signedValues = info.snorm ? 1u : 0u;
            c.rotation = uint32_t(app.rotation);
            c.flips = (app.flipH ? 1u : 0u) | (app.flipV ? 2u : 0u);
            c.tiles = app.tiling == Tiling::Off ? 1.0f : 3.0f;
            c.normalMap = (app.normalZ ? 1u : 0u) | (app.flipGreen ? 2u : 0u);
            c.forcedMip = app.forcedMip;
            c.invGamma = 1.0f / app.gamma;
            const float* bg = kAlphaBackgrounds[int(app.alphaBackground)];
            if (app.alphaBackground <= AlphaBackground::CheckerLarge) {
                c.checkerSize = std::round(bg[0] * app.scale);
                for (int i = 0; i < 3; ++i) {
                    c.checker1[i] = colors.checker1[i];
                    c.checker2[i] = colors.checker2[i];
                }
            } else {
                c.checkerSize = 1.0f;
                for (int i = 0; i < 3; ++i) c.checker1[i] = c.checker2[i] = bg[0];
            }
            for (int i = 0; i < 3; ++i) c.background[i] = colors.viewport[i];
            c.checker1[3] = c.checker2[3] = c.background[3] = 1.0f;
            *static_cast<ViewConstants*>(m.pData) = c;
            app.context->Unmap(app.constants.get(), 0);
        }
        D3D11_VIEWPORT vp{app.vpX, app.vpY, app.vpW, app.vpH, 0, 1};
        ID3D11RenderTargetView* rtv = app.rtv.get();
        ID3D11Buffer* cb = app.constants.get();
        ID3D11ShaderResourceView* srv = app.srv.get();
        ID3D11SamplerState* sampler = app.trilinear.get();
        app.context->OMSetRenderTargets(1, &rtv, nullptr);
        app.context->RSSetViewports(1, &vp);
        app.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        app.context->IASetInputLayout(nullptr);
        app.context->VSSetShader(app.vs.get(), nullptr, 0);
        app.context->PSSetShader(app.ps.get(), nullptr, 0);
        app.context->PSSetConstantBuffers(0, 1, &cb);
        app.context->PSSetShaderResources(0, 1, &srv);
        app.context->PSSetSamplers(0, 1, &sampler);
        app.context->Draw(3, 0);
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

void writeBench(bool ok) {
    FILE* f = _wfopen(app.benchPath.c_str(), L"a");
    if (!f) return;
    const double now = qpcMs();
    std::fprintf(f,
                 "%s\n  result: %s\n  gpu: %s, feature level %x, swap %s\n"
                 "  process start -> first frame: %.1f ms (coarse clock)\n"
                 "  main -> first frame: %.1f ms (device %.1f, file decode %.1f, gpu upload %.1f)\n",
                 narrow(app.path.filename().wstring()).c_str(), ok ? "ok" : app.error.c_str(),
                 app.warp ? "WARP (software)" : "hardware", unsigned(app.featureLevel), app.swapEffect,
                 msSinceProcessStart(), now - app.tStart, app.tDevice, app.tLoad, app.tUpload);
    if (ok)
        std::fprintf(f, "  image: %ux%u %s\n", app.image.width, app.image.height, app.image.description.c_str());
    std::fclose(f);
}

// ---- window ----

double lastInputMs = 0;  // frames keep coming for a moment after input, for hover states and tooltips

void layout() {
    const float top = app.showBars ? uiTopBarHeight() : 0.0f;
    const float bottom = app.showBars ? uiBottomBarHeight() : 0.0f;
    app.vpX = 0;
    app.vpY = top;
    app.vpW = float(app.winW);
    app.vpH = std::max(1.0f, float(app.winH) - top - bottom);
}

void resizeBuffers() {
    if (!app.swapChain) return;
    app.rtv.reset();
    app.context->OMSetRenderTargets(0, nullptr, nullptr);
    app.swapChain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
    createRenderTarget();
}

bool inViewport(int x, int y) {
    return x >= app.vpX && y >= app.vpY && x < app.vpX + app.vpW && y < app.vpY + app.vpH;
}

void applyDpi(float scale) {
    app.scale = scale;
    uiSetScale(scale);
    layout();
}

int resizeBorder() { return int(std::round(5 * app.scale)); }

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    if ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
        msg == WM_NCMOUSEMOVE || msg == WM_MOUSELEAVE || msg == WM_SIZE || msg == WM_ACTIVATE)
        lastInputMs = qpcMs();
    const bool uiMouse = ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse;

    switch (msg) {
        case WM_NCCALCSIZE:
            // No system frame: the client area is the whole window. A maximised window would hang over the
            // screen edges by the invisible resize border, so it is clipped to the monitor's work area.
            if (wp) {
                if (IsZoomed(hwnd)) {
                    MONITORINFO mi{};
                    mi.cbSize = sizeof(mi);
                    if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi))
                        reinterpret_cast<NCCALCSIZE_PARAMS*>(lp)->rgrc[0] = mi.rcWork;
                }
                return 0;
            }
            break;
        case WM_NCHITTEST: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &p);
            if (!IsZoomed(hwnd)) {
                const int b = resizeBorder();
                const bool l = p.x < b, r = p.x >= app.winW - b, t = p.y < b, bt = p.y >= app.winH - b;
                if (t && l) return HTTOPLEFT;
                if (t && r) return HTTOPRIGHT;
                if (bt && l) return HTBOTTOMLEFT;
                if (bt && r) return HTBOTTOMRIGHT;
                if (l) return HTLEFT;
                if (r) return HTRIGHT;
                if (t) return HTTOP;
                if (bt) return HTBOTTOM;
            }
            if (uiIsCaption(float(p.x), float(p.y))) return HTCAPTION;
            // Alt + drag on the image moves the window, which matters when the bars are hidden.
            if ((GetKeyState(VK_MENU) & 0x8000) && inViewport(p.x, p.y)) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_NCACTIVATE:
            return DefWindowProcW(hwnd, msg, wp, -1);  // keeps Windows from painting a frame over the bars
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize.x = LONG(480 * app.scale);
            mm->ptMinTrackSize.y = LONG(320 * app.scale);
            return 0;
        }
        case WM_SIZE:
            app.winW = std::max(1, int(LOWORD(lp)));
            app.winH = std::max(1, int(HIWORD(lp)));
            layout();
            resizeBuffers();
            if (app.fit) fitToWindow();
            if (wp != SIZE_MINIMIZED) render();
            return 0;
        case WM_DPICHANGED: {
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            applyDpi(float(HIWORD(wp)) / 96.0f);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            render();
            ValidateRect(hwnd, nullptr);
            return 0;
        case WM_MOUSEWHEEL: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &p);
            const int notches = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
            if (GetKeyState(VK_MENU) & 0x8000) {
                setOpacity(app.opacity + 0.05f * notches);  // Alt+wheel anywhere: window opacity
                return 0;
            }
            if (uiMouse || !inViewport(p.x, p.y)) return 0;
            if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) {
                app.exposure = std::clamp(app.exposure + 0.25f * notches, -10.0f, 10.0f);  // quarter stops
            } else if (notches) {
                float z = app.zoom;
                for (int i = 0; i < std::abs(notches); ++i) {
                    const float next = notches > 0 ? z * 1.25f : z / 1.25f;
                    z = ((z < 1.0f && next > 1.0f) || (z > 1.0f && next < 1.0f)) ? 1.0f : next;  // stop at 100%
                }
                zoomAt(z, float(p.x), float(p.y));
            }
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_MBUTTONDOWN:
            if (uiMouse || !inViewport(GET_X_LPARAM(lp), GET_Y_LPARAM(lp))) return 0;
            app.dragging = true;
            app.dragStart = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            app.dragOriginX = app.originX;
            app.dragOriginY = app.originY;
            SetCapture(hwnd);
            return 0;
        case WM_LBUTTONUP:
        case WM_MBUTTONUP:
            if (app.dragging) {
                app.dragging = false;
                ReleaseCapture();
            }
            return 0;
        case WM_LBUTTONDBLCLK:
            if (uiMouse || !inViewport(GET_X_LPARAM(lp), GET_Y_LPARAM(lp))) return 0;
            app.fit = true;
            fitToWindow();
            return 0;
        case WM_MOUSEMOVE:
            app.mouseX = GET_X_LPARAM(lp);
            app.mouseY = GET_Y_LPARAM(lp);
            if (app.dragging) {
                app.originX = app.dragOriginX + float(app.mouseX - app.dragStart.x);
                app.originY = app.dragOriginY + float(app.mouseY - app.dragStart.y);
                app.fit = false;
            }
            return 0;
        case WM_MOUSELEAVE:
            app.mouseX = app.mouseY = -1;
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (uiKeyDown(int(wp))) return 0;
            break;
        case WM_SYSCOMMAND:
            if ((wp & 0xfff0) == SC_KEYMENU) return 0;  // no menu bar: Alt alone must not take the keyboard
            break;
        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wp);
            wchar_t file[MAX_PATH * 4];
            if (DragQueryFileW(drop, 0, file, UINT(std::size(file)))) openFile(file, true);
            DragFinish(drop);
            SetForegroundWindow(hwnd);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void enableDpiAwareness() {
    using SetContextFn = BOOL(WINAPI*)(HANDLE);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    auto setContext = reinterpret_cast<SetContextFn>(
        reinterpret_cast<void (*)()>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
    if (!setContext || !setContext(reinterpret_cast<HANDLE>(-4)))  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        SetProcessDPIAware();
}

}  // namespace

// ---- actions ----

bool isSrgbFormat(htv::GpuFormat f) {
    using G = htv::GpuFormat;
    return f == G::RGBA8Srgb || f == G::BGRA8Srgb || f == G::BC1Srgb || f == G::BC2Srgb || f == G::BC3Srgb ||
           f == G::BC7Srgb;
}

bool isFloatFormat(const htv::FormatInfo& info) { return info.linear && !isSrgbFormat(info.format); }

bool isHighPrecision(const htv::Image& image) {
    using G = htv::GpuFormat;
    const G f = image.format;
    return isFloatFormat(htv::formatInfo(f)) || f == G::RGBA16Unorm || f == G::RG16Unorm || f == G::R16Unorm ||
           f == G::RGB10A2;
}

void showOpenDialog() {
    Com<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put()))))
        return;
    std::wstring pattern;
    auto add = [&](const std::vector<std::string>& exts) {
        for (const std::string& e : exts) pattern += (pattern.empty() ? L"*." : L";*.") + widen(e);
    };
    add(htv::supportedExtensions());
    add(htv::wicExtensions());
    const COMDLG_FILTERSPEC filters[] = {{L"Images", pattern.c_str()}, {L"All files", L"*.*"}};
    dialog->SetFileTypes(UINT(std::size(filters)), filters);
    dialog->SetTitle(L"Open image");
    if (!app.path.empty()) {
        Com<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(app.path.parent_path().c_str(), nullptr,
                                                  IID_PPV_ARGS(folder.put()))))
            dialog->SetFolder(folder.get());
    }
    if (FAILED(dialog->Show(app.hwnd))) return;
    Com<IShellItem> item;
    wchar_t* name = nullptr;
    if (SUCCEEDED(dialog->GetResult(item.put())) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name))) {
        openFile(name, true);
        CoTaskMemFree(name);
    }
}

void step(int delta) {
    if (app.folder.empty()) return;
    const int n = int(app.folder.size());
    int i = app.folderIndex < 0 ? 0 : app.folderIndex;
    // Skip files that fail to open, but stop after one full round.
    for (int tries = 0; tries < n; ++tries) {
        i = ((i + delta) % n + n) % n;
        if (openFile(app.folder[size_t(i)], false)) return;
    }
}

void imageSizeOnScreen(float& w, float& h) {
    w = float(app.rotation & 1 ? app.image.height : app.image.width);
    h = float(app.rotation & 1 ? app.image.width : app.image.height);
    if (app.tiling != Tiling::Off) {
        w *= 3;
        h *= 3;
    }
}

void fitToWindow() {
    if (!app.hasImage) return;
    float w, h;
    imageSizeOnScreen(w, h);
    // The design leaves a 16 px margin around a fitted image.
    const float margin = app.showBars ? std::round(16 * app.scale) : 0.0f;
    float z = std::min((app.vpW - 2 * margin) / w, (app.vpH - 2 * margin) / h);
    if (z > 1.0f) z = std::floor(z);  // small textures grow in whole steps so pixels stay square
    app.zoom = std::max(z, 1.0f / 256.0f);
    app.originX = app.vpX + std::round((app.vpW - w * app.zoom) * 0.5f);
    app.originY = app.vpY + std::round((app.vpH - h * app.zoom) * 0.5f);
}

void zoomAt(float newZoom, float x, float y) {
    newZoom = std::clamp(newZoom, 1.0f / 256.0f, 256.0f);
    app.originX = std::round(x - (x - app.originX) * newZoom / app.zoom);
    app.originY = std::round(y - (y - app.originY) * newZoom / app.zoom);
    app.zoom = newZoom;
    app.fit = false;
}

void zoomCentered(float newZoom) { zoomAt(newZoom, app.vpX + app.vpW * 0.5f, app.vpY + app.vpH * 0.5f); }

void rotate(int quarterTurns) {
    // Keep the image centre where it is on screen.
    float w, h;
    imageSizeOnScreen(w, h);
    const float cx = app.originX + w * app.zoom * 0.5f, cy = app.originY + h * app.zoom * 0.5f;
    app.rotation = (app.rotation + quarterTurns + 4) % 4;
    if (app.fit) {
        fitToWindow();
        return;
    }
    imageSizeOnScreen(w, h);
    app.originX = std::round(cx - w * app.zoom * 0.5f);
    app.originY = std::round(cy - h * app.zoom * 0.5f);
}

bool windowToPixel(float wx, float wy, int& x, int& y) {
    if (!app.hasImage) return false;
    float tw, th;
    imageSizeOnScreen(tw, th);
    float u = std::floor((wx - app.originX) / app.zoom), v = std::floor((wy - app.originY) / app.zoom);
    if (u < 0 || v < 0 || u >= tw || v >= th) return false;
    const int sw = int(app.rotation & 1 ? app.image.height : app.image.width);
    const int sh = int(app.rotation & 1 ? app.image.width : app.image.height);
    int du = int(u) % sw, dv = int(v) % sh;
    if (app.flipH) du = sw - 1 - du;
    if (app.flipV) dv = sh - 1 - dv;
    switch (app.rotation) {
        case 0: x = du; y = dv; break;
        case 1: x = dv; y = sw - 1 - du; break;
        case 2: x = sw - 1 - du; y = sh - 1 - dv; break;
        default: x = sh - 1 - dv; y = du; break;
    }
    return true;
}

void setTopmost(bool on) {
    app.topmost = on;
    SetWindowPos(app.hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void setOpacity(float opacity) {
    opacity = std::clamp(std::round(opacity * 20.0f) / 20.0f, 0.2f, 1.0f);
    const bool wasLayered = app.opacity < 1.0f;
    const bool layered = opacity < 1.0f;
    app.opacity = opacity;
    if (layered != wasLayered) {
        // Flip-model swap chains do not compose with layered windows; switch presentation models.
        app.rtv.reset();
        app.context->ClearState();
        app.context->Flush();
        app.swapChain.reset();
        LONG_PTR ex = GetWindowLongPtrW(app.hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(app.hwnd, GWL_EXSTYLE, layered ? (ex | WS_EX_LAYERED) : (ex & ~LONG_PTR(WS_EX_LAYERED)));
        createSwapChain(!layered);
        createRenderTarget();
    }
    if (layered) SetLayeredWindowAttributes(app.hwnd, 0, BYTE(std::lround(opacity * 255)), LWA_ALPHA);
}

void toggleMaximize() { ShowWindow(app.hwnd, IsZoomed(app.hwnd) ? SW_RESTORE : SW_MAXIMIZE); }

void toggleBars() {
    app.showBars = !app.showBars;
    const float top = app.vpY;
    layout();
    if (app.fit) fitToWindow();
    else app.originY += app.vpY - top;  // the image stays put on screen
}

}  // namespace htv

using namespace htv;

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show) {
    app.tStart = qpcMs();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);  // for Windows codecs and dialogs
    enableDpiAwareness();

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    fs::path file;
    for (int i = 1; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--bench" && i + 1 < argc)
            app.benchPath = argv[++i];
        else
            file = argv[i];
    }
    LocalFree(argv);
    app.path = file;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"HyperTextureViewer";
    RegisterClassExW(&wc);

    // A normal overlapped window keeps snapping, the taskbar and minimise animations; WM_NCCALCSIZE removes its frame.
    app.hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"Hyper Texture Viewer", WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800, nullptr, nullptr, instance, nullptr);
    // A start-up failure is reported in a message box, or in the benchmark file when benchmarking.
    auto failStartup = [](const std::string& reason) {
        app.error = reason;
        if (!app.benchPath.empty()) {
            writeBench(false);
        } else {
            const std::wstring msg = L"Cannot start Direct3D 11.\n" + widen(reason);
            MessageBoxW(app.hwnd, msg.c_str(), L"Hyper Texture Viewer", MB_ICONERROR);
        }
        return 1;
    };
    if (!app.hwnd) return failStartup("cannot create the window");

    // The default size is 1280x800 at 100%, scaled for the monitor and kept on its work area.
    app.scale = ImGui_ImplWin32_GetDpiScaleForHwnd(app.hwnd);
    {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(app.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        const RECT& wa = mi.rcWork;
        const int w = std::min(int(1280 * app.scale), int(wa.right - wa.left));
        const int h = std::min(int(800 * app.scale), int(wa.bottom - wa.top));
        SetWindowPos(app.hwnd, nullptr, wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2,
                     w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    // A one-pixel sliver of DWM frame brings back the window shadow without drawing anything visible.
    const MARGINS shadow{0, 0, 1, 0};
    DwmExtendFrameIntoClientArea(app.hwnd, &shadow);

    std::string error;
    const double tDevice0 = qpcMs();
    if (!createDevice()) return failStartup("no Direct3D 11 device, not even WARP");
    if (!createSwapChain(true)) return failStartup("cannot create the swap chain");
    if (!createRenderTarget()) return failStartup("cannot create the render target");
    if (!createPipeline(error)) return failStartup(error);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplWin32_Init(app.hwnd);
    if (!ImGui_ImplDX11_Init(app.device.get(), app.context.get())) return failStartup("cannot start the interface");
    uiInit();
    app.tDevice = qpcMs() - tDevice0;

    RECT rc;
    GetClientRect(app.hwnd, &rc);
    app.winW = std::max(1, int(rc.right));
    app.winH = std::max(1, int(rc.bottom));
    applyDpi(app.scale);
    if (!file.empty()) openFile(fs::absolute(file), true);
    ShowWindow(app.hwnd, show);
    UpdateWindow(app.hwnd);

    // Frames are drawn while something changes and for a moment after the last input, then the loop sleeps.
    MSG msg{};
    for (;;) {
        const bool idle = qpcMs() - lastInputMs > 1500.0 && !app.dragging;
        if (idle || IsIconic(app.hwnd)) WaitMessage();
        bool quit = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) quit = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
        if (!IsIconic(app.hwnd)) render();
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    return int(msg.wParam);
}
