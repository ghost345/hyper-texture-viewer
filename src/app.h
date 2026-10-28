// Viewer state shared by the window code (main.cpp, or web/main_web.cpp in the browser) and the interface (ui.cpp).
#pragma once

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#else
#include "web/keycodes.h"
#endif

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "image.h"
#include "stats.h"

namespace htv {

// The browser build has no window of its own: nothing to keep on top, fade, let clicks through or register with
// Explorer, and no folder to look into besides the files the page was given.
#ifdef __EMSCRIPTEN__
constexpr bool kBrowser = true;
#else
constexpr bool kBrowser = false;
#endif

#ifdef _WIN32
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

// A texture the shaders and the interface read.
using GpuView = ID3D11ShaderResourceView;
using TextureRef = Com<ID3D11ShaderResourceView>;
#else
// A WebGL texture (web/render_web.cpp deletes it).
struct GpuView {
    unsigned texture = 0;
    ~GpuView();
};
using TextureRef = std::unique_ptr<GpuView>;
#endif

// An image's texture on the GPU: one, or for an image larger than a texture can be (or than one should be), a grid
// of tiles, each drawn in a pass of its own.
struct GpuTile {
    TextureRef srv;
    uint32_t x = 0, y = 0, width = 0, height = 0;  // the tile's area of the image
};
struct GpuImage {
    std::vector<GpuTile> tiles;
    explicit operator bool() const { return !tiles.empty(); }
    void reset() { tiles.clear(); }
};

enum class Channel { RGB, R, G, B, A, Luminance };
enum class AlphaBackground { Checker, Black, Grey, White, Custom, Count };
enum class Tiling { Off, Seamless, Bordered };
enum class ToneMap { LinearClip, Reinhard, Aces, Agx, Count };
enum class CompareMode { Split, Blend, Difference };

// Compare: an image besides the shown one (A), kept on the GPU.
struct CompareSlot {
    std::filesystem::path path;
    htv::Image image;  // size, format and description only: the pixels live in `srv`
    GpuImage srv;
    TextureRef thumbnail;
    int mips = 1;
    uint64_t fileSize = 0;
    bool loading = false;
    std::string error;
    int generation = 0;  // changes with every upload
};

// Difference statistics of a pair, in the values the files store.
struct CompareStats {
    bool valid = false;
    double maxDelta = 0, meanDelta = 0, mse = 0;
    uint64_t pixels = 0, pixelsOver = 0;  // compared, and differing by more than 2 (8-bit steps)
};

enum class ColorFormat { Html, Hex, DelphiHex, VbHex, Rgb, RgbFloat, Hsv, Hsl, Long, Count };

struct RecentFile {
    std::filesystem::path path;
    int64_t openedAt = 0;  // Unix time
    TextureRef thumbnail;
    bool thumbnailTried = false;
};

struct App {
#ifdef _WIN32
    HWND hwnd = nullptr;
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    Com<IDXGISwapChain> swapChain;
    Com<ID3D11RenderTargetView> rtv;  // the swap chain's back buffer, or `layer` while the window is layered
    // A translucent or click-through window is a layered one, which Present can't draw into: frames go to this
    // GDI-compatible texture and from there to UpdateLayeredWindow.
    Com<ID3D11Texture2D> layer;
    bool rtvOnLayer = false;
    bool layeredStyle = false;  // WS_EX_LAYERED is set
#endif
    std::string layerError;     // why the window couldn't be made translucent, shown under the opacity slider
#ifdef _WIN32
    Com<ID3D11VertexShader> vs;
    Com<ID3D11PixelShader> ps;
    Com<ID3D11PixelShader> psRead;  // copies texels into a float target for the pixel readout
    Com<ID3D11PixelShader> psMatch;  // Color match mask
    Com<ID3D11PixelShader> psDiff;   // Compare: difference of two images through a heat ramp
    Com<ID3D11PixelShader> psDiffRead;  // Compare: per-channel difference for the statistics
    Com<ID3D11BlendState> blendFactor;  // Compare: Blend mode draws the second image at the blend factor
    Com<ID3D11Buffer> constants;
    Com<ID3D11SamplerState> trilinear;
#endif
    GpuImage srv;
#ifdef _WIN32
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_10_0;
    bool warp = false;
    const char* swapEffect = "";
#endif
    int textureMips = 1;  // mip levels on the GPU, including generated ones

    // The open file and its folder.
    htv::Image image;
    htv::ImageStats stats;
    bool imageAlpha = false;       // some pixel is not fully opaque
    int imageGeneration = 0;       // changes with every upload, for caches of image data
    int fileGeneration = 0;        // changes with every file shown (not with its layers or frames)
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
    std::string normalHint;      // why the image looks like a normal map with Z left out; empty when it doesn't
    bool normalHintDismissed = false;
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
    int forcedMip = -1;
    int rotation = 0;  // quarter turns clockwise
    bool flipH = false, flipV = false;
    AlphaBackground alphaBackground = AlphaBackground::Checker;
    int checkerSize = 0;  // 0, 1, 2: 8, 16 or 32 px cells
    float customBackground[3] = {1.0f, 0.0f, 1.0f};  // magenta: stands out from any texture
    Tiling tiling = Tiling::Off;
    bool grid = false;
    int gridStep = 1;
    bool ruler = false;     // rulers along the top and left of the image area
    int rulerUnit = 0;      // 0 pixels, 1 centimetres, 2 inches
    float rulerDpi = 72.0f;  // for files that state no resolution

    // Pixel readout.
    int mouseX = -1, mouseY = -1;
    bool pixelLocked = false;
    int lockedX = 0, lockedY = 0;
    ColorFormat colorFormat = ColorFormat::Html;
    int averageSize = 1;  // 1, 3 or 5
    int averageBeforeMatch = 0;  // Avg chosen before Color match forced 1x1; 0 when not forced

    // Color match (M): outlines the pixels within a tolerance of a target colour.
    bool colorMatch = false;
    bool matchHasTarget = false;
    float matchTarget[4] = {};    // as psMatch compares: sRGB formats re-encoded, one- and two-channel data expanded
    int matchMode = 0;            // whole colour (RGB), RGBA, R, G, B, A, luminance, hue, saturation
    int matchTolerance = 2;       // in 8-bit steps: values within N/255 match
    bool matchValid = false;      // matchMask and matchRegions are up to date
    GpuImage matchMask;  // in the shown image's tiles
    htv::MaskRegions matchRegions;
    int matchRegion = -1;         // the region the view went to last

    // Compare (Ctrl+K): A is the shown image, B and C are other files.
    bool compare = false;
    CompareMode compareMode = CompareMode::Split;
    CompareSlot compareSlots[2];      // B and C
    int comparePair = 0;              // Blend and Difference: A and B, A and C, B and C
    float compareBlend = 0.5f;        // how much of the pair's second image shows
    float compareGain = 8.0f;         // Difference: amplification
    bool compareSameSize = true;      // B and C stretched over A when their sizes differ
    float compareShutters[2] = {0.5f, 0.75f};  // Split: where B and C begin, across A's width
    CompareStats compareStats;
    std::filesystem::path previousPath;  // the image shown before this one: the first guess for B

    // Layers / Frames (L).
    bool showFrames = false;      // the Frames panel for animations, or the Layers panel for files with layers
    int frameIndex = 0;
    bool framePlaying = true;
    double frameShownMs = 0;      // when the current frame went up

    bool showInfo = false;
    bool showHelp = false;
    bool showHistogram = false;
    bool showSettings = false;
    int histogramMode = 0;       // RGB, R, G, B, luminance, parade
    bool histogramLog = false;   // pixel counts on a log scale

    // Settings (Ctrl+,), kept in settings.ini.
    int themeMode = 0;            // 0 dark, 1 light, 2 as Windows apps
    float interfaceScale = 0.0f;  // 0 follows the monitor's DPI
    float dpiScale = 1.0f;        // the monitor's
    bool customViewport = false;  // the viewport colour differs from the theme's
    float viewportColor[3] = {0.106f, 0.106f, 0.106f};
    bool keepView = true;         // keep zoom and position when switching files (else fit each file)
    bool keepExposure = true;     // keep exposure and range when switching files
    bool autoReload = true;       // reload the shown file when it changes on disk
    int folderSort = 0;           // name (natural), name, date modified, size, type
    bool singleInstance = true;   // files opened from Explorer go to the running window
    int cacheMB = 1024;           // decoded images kept for going back and forth
    int prefetch = 1;             // neighbours decoded ahead on each side: 0, 1 or 2

    // Panning.
    bool dragging = false;
    bool dragClick = false;  // a left press that is still a click: it has not moved far enough to pan
    float dragStartX = 0.0f, dragStartY = 0.0f;
    float dragOriginX = 0.0f, dragOriginY = 0.0f;

    // Benchmark mode: open the file, draw one frame, write timings and quit.
    std::filesystem::path benchPath;
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
// When a file was created or last written, in local time as "2026-10-27 14:05:09"; "-" when unknown.
std::string fileTime(const std::filesystem::path& path, bool created);
void showInExplorer();
void openRecent(int index);
GpuView* recentThumbnail(int index);
void chooseCustomBackground();
void chooseViewportColor();  // Settings: the colour around the image
bool pickColor(float rgb[3]);  // the Windows colour dialog; false when cancelled
void autoRange();
// Per-channel statistics of the shown image for the Histogram panel, computed once when first needed.
void ensureChannelStats();
void saveSettings();
// settings.ini next to the exe instead of in %APPDATA%.
bool portableMode();
bool setPortableMode(bool on);  // false when the exe's folder is not writable
bool registerFileTypes();  // Open with and Default Apps entries for the current user; false if any write failed
void applyTheme();              // after themeMode changes, or Windows switches its theme
void applyInterfaceScale();     // after interfaceScale changes
void relistFolder();            // after folderSort changes
void trimImageCache();          // after cacheMB or prefetch change
void relayout();  // after the bars change height
void fitToWindow();
void zoomAt(float newZoom, float x, float y);
void zoomCentered(float newZoom);
void rotate(int quarterTurns);
void setTopmost(bool on);
void toggleRuler();
void setOpacity(float opacity);
// Clicks go through the window to whatever is below; Ctrl+Shift+T anywhere turns it off again.
void setClickThrough(bool on);
void toggleMaximize();
void minimizeWindow();
bool windowMaximized();
void quitApp();
void openDefaultApps();  // Windows Settings > Default Apps, after registerFileTypes
void toggleBars();
void imageSizeOnScreen(float& w, float& h);  // after rotation, in image pixels; one copy when tiling
// The area the image covers in window pixels: the image itself, or the 3x3 tiling preview around it.
void shownArea(float& x0, float& y0, float& x1, float& y1);
// Image pixel under a window position, honouring rotation, flips and tiling. False outside the image.
bool windowToPixel(float wx, float wy, int& x, int& y);
// Mouse on the image. A left press that ends without panning is a click: with Color match on it picks the
// target, otherwise it locks the readout to that pixel. Double-click unlocks, or fits the image to the window.
void startPan(float wx, float wy, bool left);
void movePan(float wx, float wy);
void endPan();
void imageDoubleClicked();
// The window rectangle an image rectangle (in image pixels, x1 and y1 exclusive) covers on the original copy.
void imageRectToWindow(float x0, float y0, float x1, float y1, float& wx0, float& wy0, float& wx1, float& wy1);

bool isFloatFormat(const htv::FormatInfo& info);  // linear-light values (half/float, BC6H, sRGB-decoded excluded)
bool isSrgbFormat(htv::GpuFormat format);         // the GPU decodes sRGB when sampling
bool isHighPrecision(const htv::Image& image);     // float or 16-bit: shows the HDR slider row
// Reads an n x n block of mip 0 around (x, y) through the GPU, for formats the CPU cannot decode.
// Values are as the shader samples them (sRGB formats decoded to linear). False when unavailable.
bool readPixelsGpu(int x, int y, int n, float* rgba);
// Color match: the target becomes the pixel at (x, y); the mask and its regions follow on the next frame.
bool setMatchTarget(int x, int y);
// Avg is 1x1 while Color match is on, and goes back to the chosen size when it is off. Once a frame.
void syncMatchAverage();
void updateColorMatch();  // rebuilds the mask when the image, target, mode or tolerance changed
void showMatchRegion(int index);  // centres the view on a region, zooming out when it does not fit
// Animations: the current image has more than one frame.
bool isAnimation();
void showFrame(int index);  // uploads a frame of the animation; wraps around
void advanceFrame();        // the next frame, for the playback timer
// Layers: the file has cube faces, array or volume slices, pages, EXR or PSD layers to list.
bool hasLayers();
void selectLayer(int index);  // one-at-a-time kinds: shows that layer instead
// PSD: shows or hides a layer (or group) and composites again. `solo` shows it alone, or everything again
// when it already was alone.
void setLayerVisible(int index, bool visible, bool solo = false);
// A 64 px thumbnail of a layer, made on first use.
GpuView* layerThumbnail(int index);
// Compare: slot 0 is B, 1 is C. Loads in the background; an empty path clears the slot.
void loadCompareSlot(int slot, const std::filesystem::path& path);
bool pickImageFile(std::filesystem::path& out);  // the Open dialog; false when cancelled
void pickCompareFile(int slot);  // the Open dialog for a Compare slot; loads the file chosen
void updateCompareStats();  // recomputes the pair's difference statistics when the images or the pair changed
// The images Split shows, left to right: 0 for A, 1 for B, 2 for C. Returns how many.
int compareLayers(int* out);
// A thumbnail of A (slot -1), B (0) or C (1), as the view shows it.
GpuView* compareThumbnail(int slot);
double nowMs();

}  // namespace htv
