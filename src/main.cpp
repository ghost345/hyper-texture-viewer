// Hyper Texture Viewer prototype: one Win32 window, Direct3D 11, all view math in one pixel shader.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

#include "image.h"

namespace fs = std::filesystem;

namespace {

template <class T>
class Com {
public:
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { reset(); }
    void reset() {
        if (p_) p_->Release();
        p_ = nullptr;
    }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    T** put() {
        reset();
        return &p_;
    }
    explicit operator bool() const { return p_ != nullptr; }

private:
    T* p_ = nullptr;
};

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

bool isSupported(const fs::path& p) {
    const auto& list = htv::supportedExtensions();
    return std::find(list.begin(), list.end(), lowerExtension(p)) != list.end();
}

// ---- shaders ----

const char kShader[] = R"(
cbuffer View : register(b0) {
    float2 imageSize;
    float2 imageOrigin;    // top-left corner of the image in window pixels
    float zoom;
    float exposureScale;   // 2^EV
    uint channelMode;      // 0 RGBA, 1 R, 2 G, 3 B, 4 A, 5 RGB without alpha
    uint channels;         // channels the texture format stores
    uint encodeSrgb;       // sampled values are linear light
    uint snorm;
    float2 pad;
    float4 background;
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
    float2 ip = (pos.xy - imageOrigin) / zoom;
    if (ip.x < 0 || ip.y < 0 || ip.x >= imageSize.x || ip.y >= imageSize.y) return background;

    float4 c;
    if (zoom >= 1.0) {
        // Magnified or 1:1: exact texels, no filtering.
        c = tex.Load(int3(floor(ip), 0));
    } else {
        // Minified: the mip chain averages all source pixels under each screen pixel.
        float2 uv = ip / imageSize;
        float2 g = 1.0 / (zoom * imageSize);
        c = tex.SampleGrad(trilinear, uv, float2(g.x, 0), float2(0, g.y));
    }
    if (snorm) c.rgb = c.rgb * 0.5 + 0.5;
    if (channels == 1) c = float4(c.rrr, 1);
    else if (channels == 2) c = float4(c.rg, 0, 1);

    // NaN and Inf show as magenta. Written without isnan/isinf so every HLSL compiler accepts it.
    if (any(c != c) || any(abs(c) > 3.402823e38)) return float4(1, 0, 1, 1);

    if (channelMode == 4) return float4(c.aaa, 1);
    float3 rgb = c.rgb;
    float a = c.a;
    if (channelMode == 1) rgb = c.rrr;
    else if (channelMode == 2) rgb = c.ggg;
    else if (channelMode == 3) rgb = c.bbb;
    if (channelMode != 0) a = 1;

    rgb *= exposureScale;
    rgb = encodeSrgb ? srgbEncode(rgb) : saturate(rgb);

    float2 cell = floor(pos.xy / 8);
    float checker = fmod(cell.x + cell.y, 2) < 1 ? 0.20 : 0.30;
    return float4(lerp(checker.xxx, rgb, saturate(a)), 1);
}
)";

struct ViewConstants {
    float imageSize[2];
    float imageOrigin[2];
    float zoom;
    float exposureScale;
    uint32_t channelMode;
    uint32_t channels;
    uint32_t encodeSrgb;
    uint32_t snorm;
    float pad[2];
    float background[4];
};
static_assert(sizeof(ViewConstants) % 16 == 0, "constant buffer size must be a multiple of 16");

// d3dcompiler_47.dll ships with Windows 8.1 and later; loading it lazily keeps it off the import table.
using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                      UINT, UINT, ID3DBlob**, ID3DBlob**);

// ---- application ----

struct App {
    HWND hwnd = nullptr;
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    Com<IDXGISwapChain> swapChain;
    Com<ID3D11RenderTargetView> rtv;
    Com<ID3D11VertexShader> vs;
    Com<ID3D11PixelShader> ps;
    Com<ID3D11Buffer> constants;
    Com<ID3D11SamplerState> trilinear;
    Com<ID3D11ShaderResourceView> srv;
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_10_0;
    bool warp = false;
    const char* swapEffect = "";

    htv::Image image;
    bool hasImage = false;
    fs::path path;
    std::string error;

    int viewW = 1, viewH = 1;
    float zoom = 1.0f, originX = 0.0f, originY = 0.0f;
    bool fit = true;
    float exposure = 0.0f;
    int channelMode = 0;
    bool dragging = false;
    POINT dragStart{};
    float dragOriginX = 0.0f, dragOriginY = 0.0f;
    int mouseX = -1, mouseY = -1;

    // Benchmark mode: open the file, draw one frame, write timings and quit.
    std::wstring benchPath;
    double tStart = 0, tDevice = 0, tLoad = 0, tUpload = 0;
    bool firstFrameDone = false;
};

App app;

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

bool createSwapChain() {
    Com<IDXGIDevice> dxgiDevice;
    Com<IDXGIAdapter> adapter;
    Com<IDXGIFactory> factory;
    if (FAILED(app.device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgiDevice.put()))) ||
        FAILED(dxgiDevice->GetAdapter(adapter.put())) ||
        FAILED(adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(factory.put()))))
        return false;

    // Flip model where available (Windows 10, then 8), the classic blt model on Windows 7.
    Com<IDXGIFactory2> factory2;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(factory2.put())))) {
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
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = compiler ? reinterpret_cast<D3DCompileFn>(reinterpret_cast<void (*)()>(GetProcAddress(compiler, "D3DCompile"))) : nullptr;
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
    app.srv.reset();
    if (FAILED(app.device->CreateShaderResourceView(texture.get(), nullptr, app.srv.put()))) {
        error = "cannot create texture view";
        return false;
    }
    if (autoMips) app.context->GenerateMips(app.srv.get());
    return true;
}

void fitToWindow() {
    if (!app.hasImage) return;
    float z = std::min(float(app.viewW) / app.image.width, float(app.viewH) / app.image.height);
    if (z > 1.0f) z = std::floor(z);  // small textures grow in whole steps so pixels stay square
    app.zoom = std::max(z, 1.0f / 256.0f);
    app.originX = std::round((app.viewW - app.image.width * app.zoom) * 0.5f);
    app.originY = std::round((app.viewH - app.image.height * app.zoom) * 0.5f);
}

void updateTitle() {
    std::wstring t;
    if (!app.path.empty()) t = app.path.filename().wstring();
    if (app.hasImage) {
        t += L"  |  " + std::to_wstring(app.image.width) + L"x" + std::to_wstring(app.image.height) + L"  " +
             widen(app.image.description);
        if (!app.image.note.empty()) t += L" (" + widen(app.image.note) + L")";
        wchar_t buf[256];
        swprintf(buf, 256, L"  |  %g%%  EV %+.2f", double(app.zoom * 100.0f), double(app.exposure));
        t += buf;
        if (app.image.maxValue > 0) {
            swprintf(buf, 256, L"  max %.3g", double(app.image.maxValue));
            t += buf;
        }
        const wchar_t* channelNames[] = {L"", L"  [R]", L"  [G]", L"  [B]", L"  [A]", L"  [RGB]"};
        t += channelNames[app.channelMode];
        if (app.mouseX >= 0) {
            const float fx = (app.mouseX - app.originX) / app.zoom, fy = (app.mouseY - app.originY) / app.zoom;
            if (fx >= 0 && fy >= 0 && fx < app.image.width && fy < app.image.height) {
                const uint32_t x = uint32_t(fx), y = uint32_t(fy);
                float px[4];
                if (htv::readPixel(app.image, x, y, px))
                    swprintf(buf, 256, L"  |  %u,%u  %.4g %.4g %.4g %.4g", x, y, double(px[0]), double(px[1]),
                             double(px[2]), double(px[3]));
                else
                    swprintf(buf, 256, L"  |  %u,%u  (compressed)", x, y);
                t += buf;
            }
        }
    }
    if (!app.error.empty()) t += L"  |  " + widen(app.error);
    if (t.empty()) t = L"Hyper Texture Viewer - drop an image here";
    SetWindowTextW(app.hwnd, t.c_str());
}

void writeBench(bool ok);

bool openFile(const fs::path& path) {
    std::vector<uint8_t> bytes;
    std::string error;
    const double t0 = qpcMs();
    app.path = path;
    if (!readFile(path, bytes, error)) {
        app.error = error;
        updateTitle();
        return false;
    }
    htv::LoadResult r = htv::loadImage(bytes.data(), bytes.size(), lowerExtension(path));
    bytes = {};
    const double t1 = qpcMs();
    if (!r.ok || !uploadImage(r.image, error)) {
        app.error = r.ok ? error : r.error;
        updateTitle();
        return false;
    }
    app.context->Flush();
    const double t2 = qpcMs();
    app.tLoad = t1 - t0;
    app.tUpload = t2 - t1;
    app.image = std::move(r.image);
    app.hasImage = true;
    app.error.clear();
    if (app.fit) fitToWindow();
    updateTitle();
    InvalidateRect(app.hwnd, nullptr, FALSE);
    return true;
}

void step(int delta) {
    if (app.path.empty()) return;
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(app.path.parent_path(), ec))
        if (e.is_regular_file(ec) && isSupported(e.path())) files.push_back(e.path());
    if (files.empty()) return;
    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
        return StrCmpLogicalW(a.filename().c_str(), b.filename().c_str()) < 0;
    });
    auto it = std::find(files.begin(), files.end(), app.path);
    long i = it == files.end() ? 0 : long(it - files.begin());
    const long n = long(files.size());
    i = ((i + delta) % n + n) % n;
    openFile(files[size_t(i)]);
}

void render() {
    if (!app.rtv) return;
    const float bg[4] = {0.12f, 0.12f, 0.12f, 1.0f};
    app.context->ClearRenderTargetView(app.rtv.get(), bg);
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
            c.channelMode = uint32_t(app.channelMode);
            c.channels = uint32_t(info.channels);
            c.encodeSrgb = info.linear ? 1u : 0u;
            c.snorm = info.snorm ? 1u : 0u;
            for (int i = 0; i < 4; ++i) c.background[i] = bg[i];
            *static_cast<ViewConstants*>(m.pData) = c;
            app.context->Unmap(app.constants.get(), 0);
        }
        D3D11_VIEWPORT vp{0, 0, float(app.viewW), float(app.viewH), 0, 1};
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
    app.swapChain->Present(1, 0);
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

void zoomAt(float newZoom, int x, int y) {
    newZoom = std::clamp(newZoom, 1.0f / 256.0f, 256.0f);
    app.originX = std::round(x - (x - app.originX) * newZoom / app.zoom);
    app.originY = std::round(y - (y - app.originY) * newZoom / app.zoom);
    app.zoom = newZoom;
    app.fit = false;
}

void redraw() {
    updateTitle();
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            app.viewW = std::max(1, int(LOWORD(lp)));
            app.viewH = std::max(1, int(HIWORD(lp)));
            if (app.swapChain) {
                app.rtv.reset();
                app.context->OMSetRenderTargets(0, nullptr, nullptr);
                app.swapChain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
                createRenderTarget();
                if (app.fit) fitToWindow();
                redraw();
            }
            return 0;
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
            if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) {
                app.exposure += 0.25f * notches;  // Ctrl+wheel: exposure in quarter stops
            } else if (notches) {
                float z = app.zoom;
                for (int i = 0; i < std::abs(notches); ++i) {
                    const float next = notches > 0 ? z * 1.25f : z / 1.25f;
                    z = ((z < 1.0f && next > 1.0f) || (z > 1.0f && next < 1.0f)) ? 1.0f : next;  // stop at 100%
                }
                zoomAt(z, p.x, p.y);
            }
            redraw();
            return 0;
        }
        case WM_LBUTTONDOWN:
            app.dragging = true;
            app.dragStart = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            app.dragOriginX = app.originX;
            app.dragOriginY = app.originY;
            SetCapture(hwnd);
            return 0;
        case WM_LBUTTONUP:
            app.dragging = false;
            ReleaseCapture();
            return 0;
        case WM_LBUTTONDBLCLK:
            app.fit = true;
            fitToWindow();
            redraw();
            return 0;
        case WM_MOUSEMOVE:
            app.mouseX = GET_X_LPARAM(lp);
            app.mouseY = GET_Y_LPARAM(lp);
            if (app.dragging) {
                app.originX = app.dragOriginX + float(app.mouseX - app.dragStart.x);
                app.originY = app.dragOriginY + float(app.mouseY - app.dragStart.y);
                app.fit = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            updateTitle();
            return 0;
        case WM_KEYDOWN:
            switch (wp) {
                case VK_ESCAPE: DestroyWindow(hwnd); break;
                case VK_RIGHT:
                case VK_NEXT:
                case VK_SPACE: step(+1); break;
                case VK_LEFT:
                case VK_PRIOR:
                case VK_BACK: step(-1); break;
                case 'F': app.fit = true; fitToWindow(); break;
                case '1': zoomAt(1.0f, app.viewW / 2, app.viewH / 2); break;
                case '2': zoomAt(2.0f, app.viewW / 2, app.viewH / 2); break;
                case '4': zoomAt(4.0f, app.viewW / 2, app.viewH / 2); break;
                case '8': zoomAt(8.0f, app.viewW / 2, app.viewH / 2); break;
                case VK_OEM_PLUS:
                case VK_ADD: app.exposure += 0.5f; break;
                case VK_OEM_MINUS:
                case VK_SUBTRACT: app.exposure -= 0.5f; break;
                case '0': app.exposure = 0.0f; break;
                case 'C': app.channelMode = 0; break;
                case 'R': app.channelMode = app.channelMode == 1 ? 0 : 1; break;
                case 'G': app.channelMode = app.channelMode == 2 ? 0 : 2; break;
                case 'B': app.channelMode = app.channelMode == 3 ? 0 : 3; break;
                case 'A': app.channelMode = app.channelMode == 4 ? 0 : 4; break;
                case 'X': app.channelMode = app.channelMode == 5 ? 0 : 5; break;
                default: return 0;
            }
            redraw();
            return 0;
        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wp);
            wchar_t file[MAX_PATH * 4];
            if (DragQueryFileW(drop, 0, file, UINT(std::size(file)))) openFile(file);
            DragFinish(drop);
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

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show) {
    app.tStart = qpcMs();
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

    std::string error;
    const double tDevice0 = qpcMs();
    if (!createDevice()) return failStartup("no Direct3D 11 device, not even WARP");
    if (!createSwapChain()) return failStartup("cannot create the swap chain");
    if (!createRenderTarget()) return failStartup("cannot create the render target");
    if (!createPipeline(error)) return failStartup(error);
    app.tDevice = qpcMs() - tDevice0;

    RECT rc;
    GetClientRect(app.hwnd, &rc);
    app.viewW = std::max(1, int(rc.right));
    app.viewH = std::max(1, int(rc.bottom));
    if (!file.empty()) openFile(fs::absolute(file));
    updateTitle();
    ShowWindow(app.hwnd, show);
    UpdateWindow(app.hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return int(msg.wParam);
}
