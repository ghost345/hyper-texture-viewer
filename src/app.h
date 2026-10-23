// Viewer state shared by the window code (main.cpp) and the interface (ui.cpp).
#pragma once

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "image.h"

namespace htv {

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

enum class Channel { RGB, R, G, B, A, Luminance };
enum class AlphaBackground { CheckerSmall, CheckerMedium, CheckerLarge, Black, Gray, White, Count };
enum class Tiling { Off, Seamless, Bordered };
enum class ColorFormat { Html, Hex, DelphiHex, VbHex, Rgb, RgbFloat, Hsv, Hsl, Long, Count };

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
    int textureMips = 1;  // mip levels on the GPU, including generated ones

    // The open file and its folder.
    htv::Image image;
    bool hasImage = false;
    std::filesystem::path path;    // last file asked for, even if it failed to open
    std::filesystem::path shownPath;
    uint64_t fileSize = 0;
    std::string error;
    std::vector<std::filesystem::path> folder;  // supported files next to `path`, in Explorer order
    int folderIndex = -1;

    // Window and layout, in physical pixels.
    float scale = 1.0f;  // DPI scale, 1.0 at 96 dpi
    int winW = 1, winH = 1;
    float vpX = 0, vpY = 0, vpW = 1, vpH = 1;  // image viewport between the bars
    bool showBars = true;
    bool darkTheme = true;
    bool topmost = false;
    float opacity = 1.0f;

    // View: none of this ever changes the file.
    float zoom = 1.0f, originX = 0.0f, originY = 0.0f;  // origin: top-left of the shown image in window pixels
    bool fit = true;
    float exposure = 0.0f;
    float gamma = 1.0f;
    Channel channel = Channel::RGB;
    bool ignoreAlpha = false;
    bool dataIsLinear = false;  // how stored values are interpreted; defaults per format on open
    bool normalZ = false;       // rebuild blue from red and green
    bool flipGreen = false;
    int forcedMip = -1;
    int rotation = 0;  // quarter turns clockwise
    bool flipH = false, flipV = false;
    AlphaBackground alphaBackground = AlphaBackground::CheckerMedium;
    Tiling tiling = Tiling::Off;
    bool grid = false;
    int gridStep = 1;
    bool pixelOutline = false;

    // Pixel readout.
    int mouseX = -1, mouseY = -1;
    bool pixelLocked = false;
    int lockedX = 0, lockedY = 0;
    ColorFormat colorFormat = ColorFormat::Html;
    int averageSize = 1;  // 1, 3 or 5

    bool showInfo = false;
    bool showHelp = false;

    // Panning.
    bool dragging = false;
    POINT dragStart{};
    float dragOriginX = 0.0f, dragOriginY = 0.0f;

    // Benchmark mode: open the file, draw one frame, write timings and quit.
    std::wstring benchPath;
    double tStart = 0, tDevice = 0, tLoad = 0, tUpload = 0;
    bool firstFrameDone = false;
};

extern App app;

// Actions the interface triggers; implemented in main.cpp.
void showOpenDialog();
void step(int delta);
void fitToWindow();
void zoomAt(float newZoom, float x, float y);
void zoomCentered(float newZoom);
void rotate(int quarterTurns);
void setTopmost(bool on);
void setOpacity(float opacity);
void toggleMaximize();
void toggleBars();
void imageSizeOnScreen(float& w, float& h);  // after rotation and tiling, in image pixels
// Image pixel under a window position, honouring rotation, flips and tiling. False outside the image.
bool windowToPixel(float wx, float wy, int& x, int& y);

bool isFloatFormat(const htv::FormatInfo& info);  // linear-light values (half/float, BC6H, sRGB-decoded excluded)
bool isSrgbFormat(htv::GpuFormat format);         // the GPU decodes sRGB when sampling
bool isHighPrecision(const htv::Image& image);     // float or 16-bit: shows the exposure slider

}  // namespace htv
