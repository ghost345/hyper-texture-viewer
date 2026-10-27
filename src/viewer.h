// Internals shared by the window (main.cpp), drawing (render.cpp), file handling (files.cpp) and the view
// (view.cpp); the browser build has its own window, drawing and files in web/.
#pragma once

#include <functional>

#include "app.h"

namespace htv {

#ifdef _WIN32
constexpr UINT WM_APP_LOADED = WM_APP + 1;  // a background load finished; lParam owns a LoadedFile
constexpr UINT WM_APP_PREFETCHED = WM_APP + 2;  // a neighbour was decoded ahead; lParam owns a LoadedFile
constexpr UINT WM_APP_COMPARE_LOADED = WM_APP + 3;  // a Compare slot's file was decoded; lParam owns a LoadedFile
constexpr UINT_PTR kWatchTimer = 1;         // checks the shown file for changes on disk
constexpr UINT_PTR kLoadingTimer = 2;       // keeps frames coming while a load runs
constexpr UINT_PTR kZebraTimer = 3;         // moves the clipping stripes
constexpr UINT_PTR kFrameTimer = 4;         // advances an animation's frames
constexpr ULONG_PTR kOpenFileMessage = 0x48545631;  // WM_COPYDATA from a second start: a path to open
#endif
constexpr int kMaxRecent = 10;

enum class OpenMode { New, Browse, Reload };

// When a file was last written, to notice it changing on disk.
struct FileStamp {
    uint64_t writeTime = 0, size = 0;
    bool exists = false;
    bool operator==(const FileStamp& o) const { return writeTime == o.writeTime && size == o.size && exists == o.exists; }
    bool operator!=(const FileStamp& o) const { return !(*this == o); }
};

struct LoadedFile {
    int generation = 0;
    OpenMode mode = OpenMode::New;
    std::filesystem::path path;
    bool clipboard = false;
    bool ok = false;
    std::string error;
    std::string format;  // extension in capitals, for the error card
    htv::Image image;
    htv::ImageStats stats;
    bool channelStats = false;  // the Histogram panel is open: per-channel statistics too
    bool alpha = false;
    uint64_t fileSize = 0;
    FileStamp stamp;
    double decodeMs = 0;
    int compareSlot = -1;  // a Compare slot's file (0 B, 1 C) instead of the shown image
};

std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& w);
double msSinceProcessStart();

// render.cpp
bool createDevice();
bool createSwapChain();
bool createRenderTarget();
bool createPipeline(std::string& error);
bool uploadImage(const htv::Image& img, std::string& error);
// Creates a texture with mips for an image; uploadImage puts the shown image's in app.srv.
bool createImageTexture(const htv::Image& img, GpuImage& srv, int& mips, std::string& error);
// Uploads one surface (an animation frame) as the shown texture, replacing app.srv.
bool uploadSurface(const htv::Surface& surface, htv::GpuFormat format, std::string& error);
void releaseImage();
void resizeBuffers();
void render();
void queryDisplayPeak();
// The shown image fitted into a size x size square, as RGBA8 rows, for the recent files list.
std::vector<uint8_t> renderThumbnail(int size);
// Hands mip 0 to `row` one row at a time as RGBA floats, as the shader samples it: for formats the CPU cannot decode.
bool scanImageGpu(const std::function<void(const float* rgba, int width)>& row);
TextureRef createTexture(const uint8_t* rgba, int width, int height);

// files.cpp
void loadSettings();
void saveOnExit();
void openStartupFile(const std::filesystem::path& path);  // synchronous when benchmarking
void finishLoad(LoadedFile* file);
void prefetchDone(LoadedFile* file);
void compareSlotLoaded(LoadedFile* file);
void checkFileChanged();
bool isSupported(const std::filesystem::path& path);

// main.cpp
void writeBench(bool ok);
void requestRedraw();
// An interface font linked into the program: 101 Inter Regular, 102 Medium, 103 SemiBold, 104 JetBrains Mono.
bool fontResource(int id, const void*& data, size_t& size);

// view.cpp
void layout();  // the image viewport between the bars, after the window or the bars change size
bool inViewport(int x, int y);
void adoptLoaded(LoadedFile& f, bool sameSize);  // the decoded file is on the GPU: show it
void applySetting(const std::string& key, const std::string& value);  // one settings.ini line
std::string settingsText();                                           // the whole settings.ini

}  // namespace htv
