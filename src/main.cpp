// Hyper Texture Viewer: one borderless Win32 window, Direct3D 11 (render.cpp), files loaded in the background
// (files.cpp), the interface drawn with Dear ImGui on top (ui*.cpp).

#include "viewer.h"

#include <windowsx.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "keys.h"
#include "ui.h"
#include "wic_loader.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace fs = std::filesystem;

namespace htv {

App app;

double nowMs() {
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

void writeBench(bool ok) {
    FILE* f = _wfopen(app.benchPath.c_str(), L"a");
    if (!f) return;
    const double now = nowMs();
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

namespace {

double lastInputMs = 0;  // frames keep coming for a moment after input, for hover states and tooltips
bool redraw = false;     // something changed outside of input, e.g. a load finished
bool spaceDown = false;  // Space + drag pans, like the left button alone

void layout() {
    const float top = app.showBars ? uiTopBarHeight() : 0.0f;
    const float bottom = app.showBars ? uiBottomBarHeight() : 0.0f;
    app.vpX = 0;
    app.vpY = top;
    app.vpW = float(app.winW);
    app.vpH = std::max(1.0f, float(app.winH) - top - bottom);
}

bool inViewport(int x, int y) {
    return x >= app.vpX && y >= app.vpY && x < app.vpX + app.vpW && y < app.vpY + app.vpH;
}

void applyDpi(float scale) {
    app.dpiScale = scale;
    applyInterfaceScale();
}

int resizeBorder() { return int(std::round(5 * app.scale)); }

void startDrag(LPARAM lp) {
    app.dragging = true;
    app.dragStart = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    app.dragOriginX = app.originX;
    app.dragOriginY = app.originY;
    SetCapture(app.hwnd);
}

// Right drag anywhere moves the window: the top bar leaves little empty space to grab.
bool movingWindow = false;
POINT moveCursorStart{};
RECT moveWindowStart{};

void startWindowMove() {
    if (IsZoomed(app.hwnd)) return;
    movingWindow = true;
    GetCursorPos(&moveCursorStart);
    GetWindowRect(app.hwnd, &moveWindowStart);
    if (GetCapture() != app.hwnd) SetCapture(app.hwnd);  // ImGui may hold it already
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    if ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
        msg == WM_NCMOUSEMOVE || msg == WM_MOUSELEAVE || msg == WM_SIZE || msg == WM_ACTIVATE)
        lastInputMs = nowMs();
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
        case WM_EXITSIZEMOVE:
        case WM_DISPLAYCHANGE:
            queryDisplayPeak();  // the window may now be on another monitor
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            render();
            ValidateRect(hwnd, nullptr);
            return 0;
        case WM_HOTKEY:
            if (wp == kClickThroughHotkey) setClickThrough(false);
            return 0;
        case WM_SETTINGCHANGE:
            // Windows switched between light and dark apps.
            if (app.themeMode == 2 && lp && std::wstring(reinterpret_cast<const wchar_t*>(lp)) == L"ImmersiveColorSet")
                applyTheme();
            break;
        case WM_COPYDATA: {
            // A second start with single instance on hands its file over.
            const auto* cd = reinterpret_cast<const COPYDATASTRUCT*>(lp);
            if (!cd || cd->dwData != kOpenFileMessage) return FALSE;
            const std::wstring path(static_cast<const wchar_t*>(cd->lpData), cd->cbData / sizeof(wchar_t));
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
            if (!path.empty()) openPath(path);
            return TRUE;
        }
        case WM_TIMER:
            if (wp == kWatchTimer) {
                if (app.toastUntilMs > 0 && nowMs() > app.toastUntilMs) {
                    app.toastUntilMs = 0;
                    redraw = true;
                }
                checkFileChanged();
            } else if (wp == kLoadingTimer) {
                checkFileChanged();  // updates the progress
                redraw = true;
            } else if (wp == kZebraTimer) {
                redraw = true;
            } else if (wp == kFrameTimer) {
                advanceFrame();
            }
            return 0;
        case WM_APP_LOADED:
            finishLoad(reinterpret_cast<LoadedFile*>(lp));
            redraw = true;
            return 0;
        case WM_APP_PREFETCHED:
            prefetchDone(reinterpret_cast<LoadedFile*>(lp));
            return 0;
        case WM_APP_COMPARE_LOADED:
            compareSlotLoaded(reinterpret_cast<LoadedFile*>(lp));
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
                if (app.hasImage && isHighPrecision(app.image))
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
            if (uiMouse || !inViewport(GET_X_LPARAM(lp), GET_Y_LPARAM(lp))) return 0;
            if (app.matchPicking || (GetKeyState(VK_MENU) < 0 && app.colorMatch)) {
                // Color match target: the eyedropper, or Alt+click while the panel is open.
                int x, y;
                if (windowToPixel(float(GET_X_LPARAM(lp)), float(GET_Y_LPARAM(lp)), x, y) && setMatchTarget(x, y)) {
                    app.matchPicking = false;
                    app.colorMatch = true;
                }
                return 0;
            }
            if (wp & MK_CONTROL) {
                // Ctrl+click locks the readout to this pixel, or releases it.
                int x, y;
                if (app.pixelLocked) app.pixelLocked = false;
                else if (windowToPixel(float(GET_X_LPARAM(lp)), float(GET_Y_LPARAM(lp)), x, y)) {
                    app.pixelLocked = true;
                    app.lockedX = x;
                    app.lockedY = y;
                }
                return 0;
            }
            startDrag(lp);
            return 0;
        case WM_RBUTTONDOWN:
            startWindowMove();
            return 0;
        case WM_NCRBUTTONDOWN:
            if (wp != HTCAPTION) break;
            startWindowMove();
            return 0;
        case WM_RBUTTONUP:
            if (movingWindow) {
                movingWindow = false;
                ReleaseCapture();
            }
            return 0;
        case WM_CAPTURECHANGED:
            if (reinterpret_cast<HWND>(lp) != hwnd) {  // lost to another window, not re-taken by this one
                movingWindow = false;
                app.dragging = false;
            }
            break;
        case WM_MBUTTONDOWN:
            if (uiMouse || !inViewport(GET_X_LPARAM(lp), GET_Y_LPARAM(lp))) return 0;
            startDrag(lp);
            return 0;
        case WM_LBUTTONUP:
        case WM_MBUTTONUP:
            if (app.dragging) {
                app.dragging = false;
                ReleaseCapture();
            }
            return 0;
        case WM_LBUTTONDBLCLK:
            if (uiMouse || !inViewport(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)) || (wp & MK_CONTROL)) return 0;
            app.fit = true;
            fitToWindow();
            return 0;
        case WM_MOUSEMOVE:
            if (movingWindow) {
                POINT p;
                GetCursorPos(&p);
                SetWindowPos(hwnd, nullptr, moveWindowStart.left + p.x - moveCursorStart.x,
                             moveWindowStart.top + p.y - moveCursorStart.y, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
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
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && app.matchPicking && app.hasImage && !uiMouse) {
                SetCursor(LoadCursorW(nullptr, IDC_CROSS));
                return TRUE;
            }
            if (LOWORD(lp) == HTCLIENT && (app.dragging || spaceDown) && app.hasImage && !uiMouse) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                return TRUE;
            }
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wp == VK_SPACE && !(ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput)) {
                // Space plays or pauses an animation while the Frames panel is open, otherwise it pans.
                if (app.showFrames && isAnimation()) {
                    if (!spaceDown) { app.framePlaying = !app.framePlaying; app.frameShownMs = nowMs(); requestRedraw(); }
                    return 0;
                }
                spaceDown = true;
                return 0;
            }
            if (uiKeyDown(int(wp))) return 0;
            break;
        case WM_KEYUP:
            if (wp == VK_SPACE) spaceDown = false;
            break;
        case WM_SYSCOMMAND:
            if ((wp & 0xfff0) == SC_KEYMENU) return 0;  // no menu bar: Alt alone must not take the keyboard
            break;
        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wp);
            wchar_t file[MAX_PATH * 4];
            if (DragQueryFileW(drop, 0, file, UINT(std::size(file)))) openPath(file);
            DragFinish(drop);
            SetForegroundWindow(hwnd);
            return 0;
        }
        case WM_DESTROY:
            saveOnExit();
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

void requestRedraw() { redraw = true; }

void relayout() {
    layout();
    if (app.fit) fitToWindow();
}

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

bool pickImageFile(std::filesystem::path& out) {
    Com<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put()))))
        return false;
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
    if (FAILED(dialog->Show(app.hwnd))) return false;
    Com<IShellItem> item;
    wchar_t* name = nullptr;
    if (FAILED(dialog->GetResult(item.put())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &name))) return false;
    out = name;
    CoTaskMemFree(name);
    return true;
}

void showOpenDialog() {
    std::filesystem::path path;
    if (pickImageFile(path)) openPath(path);
}

void imageSizeOnScreen(float& w, float& h) {
    w = float(app.rotation & 1 ? app.image.height : app.image.width);
    h = float(app.rotation & 1 ? app.image.width : app.image.height);
}

// The origin is always the original image; tiling adds a copy on every side of it.
void shownArea(float& x0, float& y0, float& x1, float& y1) {
    float w, h;
    imageSizeOnScreen(w, h);
    const int around = app.tiling != Tiling::Off ? 1 : 0;
    x0 = app.originX - around * w * app.zoom;
    y0 = app.originY - around * h * app.zoom;
    x1 = app.originX + (1 + around) * w * app.zoom;
    y1 = app.originY + (1 + around) * h * app.zoom;
}

void fitToWindow() {
    if (!app.hasImage) return;
    float w, h;
    imageSizeOnScreen(w, h);
    // The design leaves a 16 px margin around a fitted image; the clean view fills the window.
    const float margin = app.showBars ? std::round(16 * app.scale) : 0.0f;
    float z = std::min((app.vpW - 2 * margin) / w, (app.vpH - 2 * margin) / h);
    if (z > 1.0f) z = std::floor(z);  // small textures grow in whole steps so pixels stay square
    app.zoom = std::max(z, 1.0f / 256.0f);
    app.originX = app.vpX + std::round((app.vpW - w * app.zoom) * 0.5f);
    app.originY = app.vpY + std::round((app.vpH - h * app.zoom) * 0.5f);
}

void zoomAt(float newZoom, float x, float y) {
    if (!app.hasImage) return;
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
    float x0, y0, x1, y1;
    shownArea(x0, y0, x1, y1);
    if (wx < x0 || wy < y0 || wx >= x1 || wy >= y1) return false;
    const int sw = int(app.rotation & 1 ? app.image.height : app.image.width);
    const int sh = int(app.rotation & 1 ? app.image.width : app.image.height);
    const int u = int(std::floor((wx - x0) / app.zoom)), v = int(std::floor((wy - y0) / app.zoom));
    int du = std::min(u, 3 * sw - 1) % sw, dv = std::min(v, 3 * sh - 1) % sh;
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

void imageRectToWindow(float x0, float y0, float x1, float y1, float& wx0, float& wy0, float& wx1, float& wy1) {
    const float sw = float(app.rotation & 1 ? app.image.height : app.image.width);
    const float sh = float(app.rotation & 1 ? app.image.width : app.image.height);
    // The inverse of windowToPixel for each corner, on continuous coordinates.
    auto toShown = [&](float x, float y, float& u, float& v) {
        switch (app.rotation) {
            case 0: u = x; v = y; break;
            case 1: u = sw - y; v = x; break;
            case 2: u = sw - x; v = sh - y; break;
            default: u = y; v = sh - x; break;
        }
        if (app.flipH) u = sw - u;
        if (app.flipV) v = sh - v;
    };
    float u0, v0, u1, v1;
    toShown(x0, y0, u0, v0);
    toShown(x1, y1, u1, v1);
    wx0 = app.originX + std::min(u0, u1) * app.zoom;
    wx1 = app.originX + std::max(u0, u1) * app.zoom;
    wy0 = app.originY + std::min(v0, v1) * app.zoom;
    wy1 = app.originY + std::max(v0, v1) * app.zoom;
}

void showMatchRegion(int index) {
    if (index < 0 || index >= int(app.matchRegions.boxes.size())) return;
    app.matchRegion = index;
    const htv::MaskRegion& r = app.matchRegions.boxes[size_t(index)];
    float x0, y0, x1, y1;
    imageRectToWindow(float(r.x0), float(r.y0), float(r.x1), float(r.y1), x0, y0, x1, y1);
    // Zoom out until the region takes at most 80% of the viewport; never zoom in.
    const float room = std::min(app.vpW * 0.8f / std::max(x1 - x0, 1.0f), app.vpH * 0.8f / std::max(y1 - y0, 1.0f));
    if (room < 1.0f) {
        zoomAt(app.zoom * room, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f);
        imageRectToWindow(float(r.x0), float(r.y0), float(r.x1), float(r.y1), x0, y0, x1, y1);
    }
    app.originX += std::round(app.vpX + app.vpW * 0.5f - (x0 + x1) * 0.5f);
    app.originY += std::round(app.vpY + app.vpH * 0.5f - (y0 + y1) * 0.5f);
    app.fit = false;
}

void setTopmost(bool on) {
    app.topmost = on;
    SetWindowPos(app.hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Opacity and click-through both need a layered window.
static void applyLayering(bool wasLayered) {
    const bool layered = app.opacity < 1.0f || app.clickThrough;
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
    const LONG_PTR ex = GetWindowLongPtrW(app.hwnd, GWL_EXSTYLE);
    const LONG_PTR wanted = app.clickThrough ? (ex | WS_EX_TRANSPARENT) : (ex & ~LONG_PTR(WS_EX_TRANSPARENT));
    if (wanted != ex) SetWindowLongPtrW(app.hwnd, GWL_EXSTYLE, wanted);
    if (layered) SetLayeredWindowAttributes(app.hwnd, 0, BYTE(std::lround(app.opacity * 255)), LWA_ALPHA);
}

void setOpacity(float opacity) {
    const bool wasLayered = app.opacity < 1.0f || app.clickThrough;
    app.opacity = std::clamp(std::round(opacity * 20.0f) / 20.0f, 0.1f, 1.0f);
    applyLayering(wasLayered);
}

void setClickThrough(bool on) {
    if (on == app.clickThrough) return;
    // Only a see-through window may let clicks pass: an opaque one would just stop answering.
    if (on && app.opacity >= 1.0f) return;
    // The window gets no mouse input while on, so a system-wide hotkey turns it off; without one, stay off.
    if (on) {
        // Its key in Settings, or Ctrl+T when it has none: there has to be a way back.
        KeyBinding k = binding(Action::ClickThrough, 0);
        if (k.empty()) k = KeyBinding{'T', kModCtrl};
        const UINT mods = (k.mods & kModCtrl ? MOD_CONTROL : 0) | (k.mods & kModShift ? MOD_SHIFT : 0) |
                          (k.mods & kModAlt ? MOD_ALT : 0) | MOD_NOREPEAT;
        if (!RegisterHotKey(app.hwnd, kClickThroughHotkey, mods, k.vk)) return;
    } else {
        UnregisterHotKey(app.hwnd, kClickThroughHotkey);
    }
    const bool wasLayered = app.opacity < 1.0f || app.clickThrough;
    app.clickThrough = on;
    applyLayering(wasLayered);
    redraw = true;
}

void applyInterfaceScale() {
    app.scale = app.interfaceScale > 0 ? app.interfaceScale : app.dpiScale;
    uiSetScale(app.scale);
    layout();
    if (app.fit && app.hasImage) fitToWindow();
    redraw = true;
}

void applyTheme() {
    bool dark = app.themeMode != 1;
    if (app.themeMode == 2) {
        DWORD light = 0, size = sizeof(light);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                         L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) == ERROR_SUCCESS)
            dark = light == 0;
    }
    app.darkTheme = dark;
    redraw = true;
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
    app.tStart = nowMs();
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
    if (app.benchPath.empty()) loadSettings();
    applyTheme();

    // Single instance: a file opened while a viewer runs goes to that window, which comes to the front.
    HANDLE instanceMutex = nullptr;
    if (app.benchPath.empty() && app.singleInstance) {
        instanceMutex = CreateMutexW(nullptr, FALSE, L"HyperTextureViewer.SingleInstance");
        if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            if (HWND other = FindWindowW(L"HyperTextureViewer", nullptr)) {
                std::error_code ec;
                const std::wstring path = file.empty() ? std::wstring() : fs::absolute(file, ec).wstring();
                COPYDATASTRUCT cd{kOpenFileMessage, DWORD(path.size() * sizeof(wchar_t)), const_cast<wchar_t*>(path.c_str())};
                DWORD_PTR result = 0;
                AllowSetForegroundWindow(ASFW_ANY);
                if (SendMessageTimeoutW(other, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cd), SMTO_ABORTIFHUNG, 3000,
                                        &result) &&
                    result)
                    return 0;
            }
        }
    }

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
    // Windows 11 draws a 1 px border around the frame; the bars already end the window. Older versions ignore it.
    const DWORD noBorder = 0xFFFFFFFE;  // DWMWA_COLOR_NONE
    DwmSetWindowAttribute(app.hwnd, 34 /* DWMWA_BORDER_COLOR */, &noBorder, sizeof(noBorder));

    std::string error;
    const double tDevice0 = nowMs();
    if (!createDevice()) return failStartup("no Direct3D 11 device, not even WARP");
    if (!createSwapChain(true)) return failStartup("cannot create the swap chain");
    if (!createRenderTarget()) return failStartup("cannot create the render target");
    if (!createPipeline(error)) return failStartup(error);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplWin32_Init(app.hwnd);
    if (!ImGui_ImplDX11_Init(app.device.get(), app.context.get())) return failStartup("cannot start the interface");
    uiInit();
    app.tDevice = nowMs() - tDevice0;

    RECT rc;
    GetClientRect(app.hwnd, &rc);
    app.winW = std::max(1, int(rc.right));
    app.winH = std::max(1, int(rc.bottom));
    applyDpi(app.scale);
    if (!file.empty()) openStartupFile(fs::absolute(file));
    ShowWindow(app.hwnd, show);
    UpdateWindow(app.hwnd);
    if (app.benchPath.empty()) {
        queryDisplayPeak();
        SetTimer(app.hwnd, kWatchTimer, 500, nullptr);
    }

    // Frames are drawn while something changes and for a moment after the last input, then the loop sleeps.
    MSG msg{};
    int exitCode = 0;
    bool zebraTimer = false, frameTimer = false;
    for (;;) {
        // The clipping stripes move, so they need frames while they are on screen (15 a second is plenty).
        const bool zebra = app.showClipping && app.hasImage && isHighPrecision(app.image) && app.benchPath.empty();
        if (zebra != zebraTimer) {
            if (zebra) SetTimer(app.hwnd, kZebraTimer, 66, nullptr);
            else KillTimer(app.hwnd, kZebraTimer);
            zebraTimer = zebra;
        }
        // An animation playing needs a steady beat; the frame's own delay decides when the next goes up.
        const bool playing = isAnimation() && app.framePlaying && app.benchPath.empty();
        if (playing != frameTimer) {
            if (playing) SetTimer(app.hwnd, kFrameTimer, 15, nullptr);
            else KillTimer(app.hwnd, kFrameTimer);
            frameTimer = playing;
        }
        const bool active = nowMs() - lastInputMs < 1500.0 || app.dragging || app.loading;
        if (!active || IsIconic(app.hwnd)) WaitMessage();
        bool quit = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit = true;
                exitCode = int(msg.wParam);  // later messages in this batch reuse `msg`
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
        // A timer tick that changed nothing does not cost a frame.
        const bool input = nowMs() - lastInputMs < 1500.0;
        if (!IsIconic(app.hwnd) && (input || app.dragging || app.loading || redraw)) render();
        redraw = false;
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    return exitCode;
}
