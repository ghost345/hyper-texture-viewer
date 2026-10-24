// Internals shared by the window (main.cpp), drawing (render.cpp) and file handling (files.cpp).
#pragma once

#include <functional>

#include "app.h"

namespace htv {

constexpr UINT WM_APP_LOADED = WM_APP + 1;  // a background load finished; lParam owns a LoadedFile
constexpr UINT WM_APP_PREFETCHED = WM_APP + 2;  // a neighbour was decoded ahead; lParam owns a LoadedFile
constexpr UINT WM_APP_COMPARE_LOADED = WM_APP + 3;  // a Compare slot's file was decoded; lParam owns a LoadedFile
constexpr UINT_PTR kWatchTimer = 1;         // checks the shown file for changes on disk
constexpr UINT_PTR kLoadingTimer = 2;       // keeps frames coming while a load runs
constexpr UINT_PTR kZebraTimer = 3;         // moves the clipping stripes
constexpr UINT_PTR kFrameTimer = 4;         // advances an animation's frames
constexpr ULONG_PTR kOpenFileMessage = 0x48545631;  // WM_COPYDATA from a second start: a path to open

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
bool createImageTexture(const htv::Image& img, Com<ID3D11ShaderResourceView>& srv, int& mips, std::string& error);
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
Com<ID3D11ShaderResourceView> createTexture(const uint8_t* rgba, int width, int height);

// files.cpp
struct LoadedFile;
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

}  // namespace htv
