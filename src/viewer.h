// Internals shared by the window (main.cpp), drawing (render.cpp) and file handling (files.cpp).
#pragma once

#include "app.h"

namespace htv {

constexpr UINT WM_APP_LOADED = WM_APP + 1;  // a background load finished; lParam owns a LoadedFile
constexpr UINT_PTR kWatchTimer = 1;         // checks the shown file for changes on disk
constexpr UINT_PTR kLoadingTimer = 2;       // keeps frames coming while a load runs
constexpr UINT_PTR kZebraTimer = 3;         // moves the clipping stripes
constexpr int kClickThroughHotkey = 1;      // Ctrl+Shift+T, registered while the window lets clicks through

std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& w);
double msSinceProcessStart();

// render.cpp
bool createDevice();
bool createSwapChain(bool allowFlip);
bool createRenderTarget();
bool createPipeline(std::string& error);
bool uploadImage(const htv::Image& img, std::string& error);
void releaseImage();
void resizeBuffers();
void render();
void queryDisplayPeak();
// The shown image fitted into a size x size square, as RGBA8 rows, for the recent files list.
std::vector<uint8_t> renderThumbnail(int size);
Com<ID3D11ShaderResourceView> createTexture(const uint8_t* rgba, int width, int height);

// files.cpp
struct LoadedFile;
void loadSettings();
void saveOnExit();
void openStartupFile(const std::filesystem::path& path);  // synchronous when benchmarking
void finishLoad(LoadedFile* file);
void checkFileChanged();
bool isSupported(const std::filesystem::path& path);

// main.cpp
void writeBench(bool ok);
void requestRedraw();

}  // namespace htv
