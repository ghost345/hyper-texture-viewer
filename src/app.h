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
#include "stats.h"

namespace htv {

template <class T>
class Com {
public:
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    Com& operator=(Com&& o) noexcept {
        if (this != &o) {
            reset();
            p_ = o.p_;
            o.p_ = nullptr;
        }
        return *this;
    }
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
enum class AlphaBackground { Checker, Black, Grey, White, Custom, Count };
enum class Tiling { Off, Seamless, Bordered };
enum class ToneMap { LinearClip, Reinhard, Aces, Agx, Count };
enum class ColorFormat { Html, Hex, DelphiHex, VbHex, Rgb, RgbFloat, Hsv, Hsl, Long, Count };

struct RecentFile {
    std::filesystem::path path;
    int64_t openedAt = 0;  // Unix time
    Com<ID3D11ShaderResourceView> thumbnail;
    bool thumbnailTried = false;
};

struct App {
    HWND hwnd = nullptr;
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    Com<IDXGISwapChain> swapChain;
    Com<ID3D11RenderTargetView> rtv;
    Com<ID3D11VertexShader> vs;
    Com<ID3D11PixelShader> ps;
    Com<ID3D11PixelShader> psRead;  // copies texels into a float target for the pixel readout
    Com<ID3D11Buffer> constants;
    Com<ID3D11SamplerState> trilinear;
    Com<ID3D11ShaderResourceView> srv;
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_10_0;
    bool warp = false;
    const char* swapEffect = "";
    int textureMips = 1;  // mip levels on the GPU, including generated ones

    // The open file and its folder.
    htv::Image image;
    htv::ImageStats stats;
    bool imageAlpha = false;       // some pixel is not fully opaque
    int imageGeneration = 0;       // changes with every upload, for caches of image data
    bool hasImage = false;         // an image is on screen (a failed file clears it)
    std::filesystem::path path;    // last file asked for: shown, loading or failed
    std::filesystem::path shownPath;
    bool fromClipboard = false;    // the shown image was pasted
    uint64_t fileSize = 0;
    std::string error;             // why `path` failed to open; the viewport shows it instead of an image
    std::string errorFormat;       // what is known about the failed file, e.g. "PSD"

    // Background loading: the previous image stays on screen until the next one is ready.
    bool loading = false;
    double loadingSinceMs = 0;
    uint64_t loadingSize = 0;
    float loadingProgress = -1.0f;  // share of the file read, or -1 while decoding

    // "Reloaded" notice after the file changed on disk.
    double toastUntilMs = 0;
    std::string toastName;

    std::vector<RecentFile> recent;  // most recent first
    std::vector<std::filesystem::path> folder;  // supported files next to `path`, in Explorer order
    int folderIndex = -1;

    // Window and layout, in physical pixels.
    float scale = 1.0f;  // DPI scale, 1.0 at 96 dpi
    int winW = 1, winH = 1;
    float vpX = 0, vpY = 0, vpW = 1, vpH = 1;  // image viewport between the bars
    bool showBars = true;
    bool darkTheme = true;
    bool topmost = false;
    bool clickThrough = false;  // "Transparent to mouse": clicks reach the windows below
    float opacity = 1.0f;

    // View: none of this ever changes the file.
    float zoom = 1.0f, originX = 0.0f, originY = 0.0f;  // origin: top-left of the shown image in window pixels
    bool fit = true;
    float exposure = 0.0f;
    float gamma = 1.0f;
    float blackPoint = 0.0f, whitePoint = 1.0f;  // HDR range in image values
    ToneMap toneMap = ToneMap::LinearClip;
    bool showClipping = false;  // zebra above the white point, magenta NaN and Inf
    float displayPeak = 0.0f;   // HDR monitor peak relative to SDR white, 0 on an SDR monitor
    Channel channel = Channel::RGB;
    bool ignoreAlpha = false;
    bool dataIsLinear = false;  // how stored values are interpreted; defaults per format on open
    bool normalZ = false;       // rebuild blue from red and green
    bool flipGreen = false;
    int forcedMip = -1;
    int rotation = 0;  // quarter turns clockwise
    bool flipH = false, flipV = false;
    AlphaBackground alphaBackground = AlphaBackground::Checker;
    int checkerSize = 0;  // 0, 1, 2: 8, 16 or 32 px cells
    float customBackground[3] = {1.0f, 0.0f, 1.0f};  // magenta: stands out from any texture
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
void openPath(const std::filesystem::path& path);  // a new file: lists its folder, fits if Fit is on
void step(int delta);
void stepTo(int index);  // index into `folder`; negative counts from the end
void reloadFile();
void cancelLoading();
void pasteFromClipboard();
void copyFilePath();
void showInExplorer();
void openRecent(int index);
ID3D11ShaderResourceView* recentThumbnail(int index);
void chooseCustomBackground();
void autoRange();
void saveSettings();
void relayout();  // after the bars change height
void fitToWindow();
void zoomAt(float newZoom, float x, float y);
void zoomCentered(float newZoom);
void rotate(int quarterTurns);
void setTopmost(bool on);
void setOpacity(float opacity);
// Clicks go through the window to whatever is below; Ctrl+Shift+T anywhere turns it off again.
void setClickThrough(bool on);
void toggleMaximize();
void toggleBars();
void imageSizeOnScreen(float& w, float& h);  // after rotation, in image pixels; one copy when tiling
// The area the image covers in window pixels: the image itself, or the 3x3 tiling preview around it.
void shownArea(float& x0, float& y0, float& x1, float& y1);
// Image pixel under a window position, honouring rotation, flips and tiling. False outside the image.
bool windowToPixel(float wx, float wy, int& x, int& y);

bool isFloatFormat(const htv::FormatInfo& info);  // linear-light values (half/float, BC6H, sRGB-decoded excluded)
bool isSrgbFormat(htv::GpuFormat format);         // the GPU decodes sRGB when sampling
bool isHighPrecision(const htv::Image& image);     // float or 16-bit: shows the HDR slider row
// Reads an n x n block of mip 0 around (x, y) through the GPU, for formats the CPU cannot decode.
// Values are as the shader samples them (sRGB formats decoded to linear). False when unavailable.
bool readPixelsGpu(int x, int y, int n, float* rgba);
double nowMs();

}  // namespace htv
