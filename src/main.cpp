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
#include "resource.h"
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

// The app icon at the two sizes Windows shows it in at this DPI. Each size in htv.ico is drawn on its own pixel
// grid, so the small one is loaded at its size rather than shrunk from the big one.
HICON loadAppIcon(int metric, UINT dpi) {
    using MetricsFn = int(WINAPI*)(int, UINT);
    using ScaleDownFn = HRESULT(WINAPI*)(HINSTANCE, PCWSTR, int, int, HICON*);
    static const auto metricsForDpi = reinterpret_cast<MetricsFn>(
        reinterpret_cast<void (*)()>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi")));
    static const auto scaleDown = [] {
        HMODULE comctl = LoadLibraryW(L"comctl32.dll");
        return comctl ? reinterpret_cast<ScaleDownFn>(reinterpret_cast<void (*)()>(GetProcAddress(comctl, "LoadIconWithScaleDown")))
                      : nullptr;
    }();
    const int size = metricsForDpi ? metricsForDpi(metric, dpi) : MulDiv(GetSystemMetrics(metric), int(dpi), 96);
    HINSTANCE instance = GetModuleHandleW(nullptr);
    HICON icon = nullptr;
    if (scaleDown && SUCCEEDED(scaleDown(instance, MAKEINTRESOURCEW(IDI_APPICON), size, size, &icon))) return icon;
    return static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
}

void setWindowIcons(HWND hwnd, UINT dpi) {
    static HICON bigIcon = nullptr, smallIcon = nullptr;  // `small` is a macro in the Windows headers
    HICON newBig = loadAppIcon(SM_CXICON, dpi), newSmall = loadAppIcon(SM_CXSMICON, dpi);
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(newBig));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(newSmall));
    if (bigIcon) DestroyIcon(bigIcon);
    if (smallIcon) DestroyIcon(smallIcon);
    bigIcon = newBig;
    smallIcon = newSmall;
}

void applyDpi(float scale) {
    app.dpiScale = scale;
    applyInterfaceScale();
}

int resizeBorder() { return int(std::round(5 * app.scale)); }

void startDrag(LPARAM lp, bool click) {
    startPan(float(GET_X_LPARAM(lp)), float(GET_Y_LPARAM(lp)), click);
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
            // Alt + drag on the image moves the window, which matters when the bars are hidden. Panels over the
            // image keep their Alt+clicks (a layer's eye).
            if ((GetKeyState(VK_MENU) & 0x8000) && inViewport(p.x, p.y) && !uiMouse) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_NCACTIVATE:
            return DefWindowProcW(hwnd, msg, wp, -1);  // keeps Windows from painting a frame over the bars
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            // Wide enough for the top bar with everything it can fold away in "...".
            mm->ptMinTrackSize.x = std::max(LONG(480 * app.scale), LONG(std::ceil(uiMinWidth())));
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
            setWindowIcons(hwnd, HIWORD(wp));
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
            startDrag(lp, !spaceDown);
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
            startDrag(lp, false);
            return 0;
        case WM_LBUTTONUP:
        case WM_MBUTTONUP:
            if (app.dragging) {
                endPan();
                ReleaseCapture();
            }
            return 0;
        case WM_LBUTTONDBLCLK:
            if (uiMouse || !inViewport(GET_X_LPARAM(lp), GET_Y_LPARAM(lp))) return 0;
            imageDoubleClicked();
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
            movePan(float(app.mouseX), float(app.mouseY));
            return 0;
        case WM_MOUSELEAVE:
            app.mouseX = app.mouseY = -1;
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && app.colorMatch && app.hasImage && !uiMouse && !app.dragging && !spaceDown) {
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

bool fontResource(int id, const void*& data, size_t& size) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));  // RT_RCDATA
    HGLOBAL mem = res ? LoadResource(nullptr, res) : nullptr;
    data = mem ? LockResource(mem) : nullptr;
    size = data ? SizeofResource(nullptr, res) : 0;
    return data != nullptr;
}

void quitApp() { DestroyWindow(app.hwnd); }

void minimizeWindow() { ShowWindow(app.hwnd, SW_MINIMIZE); }

bool windowMaximized() { return IsZoomed(app.hwnd) != FALSE; }

void openDefaultApps() {
    // Windows 11 opens the app's own page; older versions ignore the parameter and show the list.
    ShellExecuteW(app.hwnd, L"open", L"ms-settings:defaultapps?registeredAppUser=Hyper%20Texture%20Viewer", nullptr,
                  nullptr, SW_SHOWNORMAL);
}

// ---- actions ----

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

void pickCompareFile(int slot) {
    std::filesystem::path path;
    if (pickImageFile(path)) loadCompareSlot(slot, path);
}

void setTopmost(bool on) {
    app.topmost = on;
    SetWindowPos(app.hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Opacity and click-through both need a layered window, which the next frame sets up (see render); clicks
// pass through only with WS_EX_TRANSPARENT on top of that.
static void applyLayering() {
    const LONG_PTR ex = GetWindowLongPtrW(app.hwnd, GWL_EXSTYLE);
    const LONG_PTR wanted = app.clickThrough ? (ex | WS_EX_TRANSPARENT) : (ex & ~LONG_PTR(WS_EX_TRANSPARENT));
    if (wanted != ex) SetWindowLongPtrW(app.hwnd, GWL_EXSTYLE, wanted);
    redraw = true;
}

void setOpacity(float opacity) {
    app.opacity = std::clamp(std::round(opacity * 20.0f) / 20.0f, 0.1f, 1.0f);
    applyLayering();
}

void setClickThrough(bool on) {
    if (on == app.clickThrough) return;
    // Only for a window that stays in sight over the one being clicked: see-through or always on top.
    // Its key works while the window has the focus, which Alt+Tab or the taskbar gives back.
    if (on && app.opacity >= 1.0f && !app.topmost) return;
    app.clickThrough = on;
    applyLayering();
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
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON));  // until the window has its DPI's sizes
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
    setWindowIcons(app.hwnd, UINT(std::lround(app.scale * 96.0f)));
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
    if (!createSwapChain()) return failStartup("cannot create the swap chain");
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
