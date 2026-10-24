// Files: background decoding, the folder list, reloading on change, recent files, the clipboard and settings.

#include "viewer.h"

#include <commdlg.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>

#include "imgui.h"
#include "keys.h"
#include "wic_loader.h"

namespace fs = std::filesystem;

namespace htv {

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
    fs::path path;
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
};

namespace {

FileStamp stampOf(const fs::path& path) {
    FileStamp s;
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return s;
    s.exists = true;
    s.writeTime = (uint64_t(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
    s.size = (uint64_t(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    return s;
}

std::string lowerExtension(const fs::path& p) {
    std::string e = narrow(p.extension().wstring());
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    for (char& c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

bool hasBuiltInDecoder(const std::string& ext) {
    const auto& list = htv::supportedExtensions();
    return std::find(list.begin(), list.end(), ext) != list.end();
}

// ---- background loading ----

// One worker decodes the latest request; anything asked for while it works replaces the waiting request,
// so holding an arrow key skips files instead of queuing them.
std::mutex loaderMutex;
std::condition_variable loaderWake;
std::unique_ptr<LoadedFile> waiting;  // the request: path, mode and generation filled in
std::atomic<int> latestGeneration{0};
std::atomic<uint64_t> bytesRead{0};
std::atomic<bool> decoding{false};
bool workerStarted = false;

// Reads a whole file in chunks; `cancelled` is asked between chunks and `progress` (when given) counts the bytes.
bool readFile(const fs::path& path, const std::function<bool()>& cancelled, std::atomic<uint64_t>* progress,
              std::vector<uint8_t>& out, std::string& error) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        error = GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND
                    ? "The file no longer exists."
                    : "The file cannot be opened; another program may be holding it.";
        return false;
    }
    LARGE_INTEGER size;
    GetFileSizeEx(f, &size);
    if (size.QuadPart > 0x7fffffff) {
        CloseHandle(f);
        error = "Files over 2 GB are not supported yet.";
        return false;
    }
    out.resize(size_t(size.QuadPart));
    // Chunks, so the loading badge can show progress and a superseded load stops early.
    size_t done = 0;
    bool ok = true;
    while (done < out.size()) {
        if (cancelled()) {
            ok = false;
            break;
        }
        const DWORD chunk = DWORD(std::min<size_t>(out.size() - done, 8u << 20));
        DWORD read = 0;
        if (!ReadFile(f, out.data() + done, chunk, &read, nullptr) || read == 0) {
            error = "Unexpected end of file.";
            ok = false;
            break;
        }
        done += read;
        if (progress) *progress = done;
    }
    CloseHandle(f);
    return ok;
}

// Prefetching decodes the folder's neighbours on a second, lower-priority thread; the user's request never
// waits for it. A request for the file it is decoding takes that decode over instead of starting again.
std::mutex prefetchMutex;
std::condition_variable prefetchWake;
std::vector<std::unique_ptr<LoadedFile>> prefetchQueue;  // most wanted first
std::atomic<int> prefetchGeneration{0};                  // a new queue cancels reads of the old one
fs::path prefetchInFlight;
std::atomic<int> promotedGeneration{0};                  // the user asked for the file in flight
OpenMode promotedMode = OpenMode::Browse;
bool prefetchStarted = false;

// `foreground`: the user's request, which shows progress; otherwise a prefetch.
void decode(LoadedFile& r, bool foreground) {
    const double t0 = nowMs();
    r.stamp = stampOf(r.path);
    r.fileSize = r.stamp.size;
    const std::string ext = lowerExtension(r.path);
    r.format = ext;
    for (char& c : r.format) c = char(std::toupper(static_cast<unsigned char>(c)));
    if (foreground) {
        bytesRead = 0;
        decoding = false;
    }
    const int generation = r.generation;
    const std::function<bool()> cancelled = [&] {
        return foreground ? generation != latestGeneration
                          : generation != prefetchGeneration && promotedGeneration == 0;
    };
    htv::LoadResult result;
    if (hasBuiltInDecoder(ext)) {
        std::vector<uint8_t> bytes;
        if (!readFile(r.path, cancelled, foreground ? &bytesRead : nullptr, bytes, r.error)) return;
        if (foreground) decoding = true;
        result = htv::loadImage(bytes.data(), bytes.size(), ext);
    }
    // Anything the built-in decoders don't cover goes to the codecs installed in Windows.
    if (!result.ok) {
        if (foreground) decoding = true;
        htv::LoadResult wic = htv::loadWithWic(r.path);
        if (wic.ok || result.error.empty()) result = std::move(wic);
    }
    if (!result.ok) {
        r.error = result.error.empty() ? "Unknown format." : result.error;
        if (!r.error.empty()) r.error[0] = char(std::toupper(static_cast<unsigned char>(r.error[0])));
        if (r.error.back() != '.') r.error += '.';
        return;
    }
    r.image = std::move(result.image);
    if (isHighPrecision(r.image) || r.channelStats) r.stats = htv::computeStats(r.image, r.channelStats);
    r.alpha = htv::hasTranslucentPixels(r.image);
    r.ok = true;
    r.decodeMs = nowMs() - t0;
}

void workerLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // for the Windows codecs
    for (;;) {
        std::unique_ptr<LoadedFile> job;
        {
            std::unique_lock<std::mutex> lock(loaderMutex);
            loaderWake.wait(lock, [] { return waiting != nullptr; });
            job = std::move(waiting);
        }
        decode(*job, true);
        // Superseded results still go back: the main thread keeps them in the cache.
        PostMessageW(app.hwnd, WM_APP_LOADED, 0, reinterpret_cast<LPARAM>(job.release()));
    }
}

void prefetchLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        std::unique_ptr<LoadedFile> job;
        {
            std::unique_lock<std::mutex> lock(prefetchMutex);
            prefetchWake.wait(lock, [] { return !prefetchQueue.empty(); });
            job = std::move(prefetchQueue.front());
            prefetchQueue.erase(prefetchQueue.begin());
            prefetchInFlight = job->path;
        }
        decode(*job, false);
        bool promoted = false;
        {
            std::lock_guard<std::mutex> lock(prefetchMutex);
            prefetchInFlight.clear();
            if (const int g = promotedGeneration.exchange(0)) {
                job->generation = g;
                job->mode = promotedMode;
                promoted = true;
            }
        }
        PostMessageW(app.hwnd, promoted ? WM_APP_LOADED : WM_APP_PREFETCHED, 0, reinterpret_cast<LPARAM>(job.release()));
    }
}

// ---- cache of decoded images ----

// Images the user moved away from and prefetched neighbours, least recently used first out. Main thread only.
struct CacheEntry {
    std::unique_ptr<LoadedFile> file;
    uint64_t bytes = 0;
    uint64_t lastUse = 0;
};
std::vector<CacheEntry> cache;
uint64_t cacheClock = 0;

uint64_t imageBytes(const htv::Image& image) {
    uint64_t n = 0;
    for (const htv::Surface& s : image.mips) n += s.data.size();
    return n;
}

uint64_t cacheBudget() { return uint64_t(std::max(app.cacheMB, 0)) << 20; }

void trimCache(uint64_t budget) {
    uint64_t total = 0;
    for (const CacheEntry& e : cache) total += e.bytes;
    while (total > budget && !cache.empty()) {
        auto oldest = std::min_element(cache.begin(), cache.end(),
                                       [](const CacheEntry& a, const CacheEntry& b) { return a.lastUse < b.lastUse; });
        total -= oldest->bytes;
        cache.erase(oldest);
    }
}

void cacheInsert(std::unique_ptr<LoadedFile> f) {
    if (!f || !f->ok || f->clipboard || f->path.empty()) return;
    const uint64_t bytes = imageBytes(f->image);
    if (bytes > cacheBudget()) return;
    cache.erase(std::remove_if(cache.begin(), cache.end(), [&](const CacheEntry& e) { return e.file->path == f->path; }),
                cache.end());
    cache.push_back({std::move(f), bytes, ++cacheClock});
    trimCache(cacheBudget());
}

// The cached decode of `path` if the file has not changed since; it leaves the cache.
std::unique_ptr<LoadedFile> cacheTake(const fs::path& path) {
    for (auto it = cache.begin(); it != cache.end(); ++it) {
        if (it->file->path != path) continue;
        std::unique_ptr<LoadedFile> f = std::move(it->file);
        cache.erase(it);
        if (f->stamp != stampOf(path)) return nullptr;
        return f;
    }
    return nullptr;
}

bool cached(const fs::path& path) {
    for (CacheEntry& e : cache)
        if (e.file->path == path) {
            e.lastUse = ++cacheClock;  // still wanted: a neighbour again
            return true;
        }
    return false;
}

int lastStep = 1;  // direction of the last Previous/Next, prefetched first

// Queues the shown file's neighbours that are not decoded yet; the queue of the previous file is dropped.
void schedulePrefetch() {
    if (app.cacheMB <= 0 || app.prefetch <= 0 || app.folderIndex < 0 || app.folder.size() < 2 || !app.benchPath.empty())
        return;
    const int n = int(app.folder.size());
    std::vector<fs::path> wanted;
    for (int d = 1; d <= app.prefetch; ++d)
        for (int dir : {lastStep, -lastStep}) {
            const fs::path& p = app.folder[size_t(((app.folderIndex + dir * d) % n + n) % n)];
            if (p != app.shownPath && !cached(p) && std::find(wanted.begin(), wanted.end(), p) == wanted.end())
                wanted.push_back(p);
        }
    std::lock_guard<std::mutex> lock(prefetchMutex);
    prefetchQueue.clear();
    const int generation = ++prefetchGeneration;
    for (const fs::path& p : wanted) {
        if (p == prefetchInFlight) continue;
        auto job = std::make_unique<LoadedFile>();
        job->path = p;
        job->mode = OpenMode::Browse;
        job->generation = generation;
        job->channelStats = app.showHistogram;
        prefetchQueue.push_back(std::move(job));
    }
    if (!prefetchStarted) {
        std::thread(prefetchLoop).detach();
        prefetchStarted = true;
    }
    prefetchWake.notify_one();
}

// ---- folder ----

void listFolder(const fs::path& file) {
    app.folder.clear();
    app.folderIndex = -1;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(file.parent_path(), ec))
        if (e.is_regular_file(ec) && isSupported(e.path())) app.folder.push_back(e.path());
    // Explorer's order (numbers in names by value) unless Settings asks for another; ties by that order.
    auto natural = [](const fs::path& a, const fs::path& b) {
        return StrCmpLogicalW(a.filename().c_str(), b.filename().c_str()) < 0;
    };
    std::sort(app.folder.begin(), app.folder.end(), natural);
    if (app.folderSort == 1) {
        std::stable_sort(app.folder.begin(), app.folder.end(), [](const fs::path& a, const fs::path& b) {
            return _wcsicmp(a.filename().c_str(), b.filename().c_str()) < 0;
        });
    } else if (app.folderSort == 2 || app.folderSort == 3) {
        std::vector<std::pair<uint64_t, fs::path>> keyed;
        for (const fs::path& f : app.folder) {
            const FileStamp st = stampOf(f);
            keyed.emplace_back(app.folderSort == 2 ? st.writeTime : st.size, f);
        }
        // Newest and largest first, as Explorer shows them after one click on the column.
        std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (size_t i = 0; i < keyed.size(); ++i) app.folder[i] = std::move(keyed[i].second);
    } else if (app.folderSort == 4) {
        std::stable_sort(app.folder.begin(), app.folder.end(), [](const fs::path& a, const fs::path& b) {
            return _wcsicmp(a.extension().c_str(), b.extension().c_str()) < 0;
        });
    }
    auto it = std::find(app.folder.begin(), app.folder.end(), file);
    if (it != app.folder.end()) app.folderIndex = int(it - app.folder.begin());
}

FileStamp watchedStamp, pendingStamp;
FileStamp shownStamp;     // of the image on screen, for the cache
std::string shownFormat;
bool reloadPending = false;
fs::path pathBeforeLoad;  // restored when a load is cancelled

void requestOpen(const fs::path& path, OpenMode mode) {
    if (!app.loading) pathBeforeLoad = app.path;
    app.path = path;
    if (mode == OpenMode::New || app.folder.empty()) {
        listFolder(path);
    } else {
        auto it = std::find(app.folder.begin(), app.folder.end(), path);
        app.folderIndex = it == app.folder.end() ? -1 : int(it - app.folder.begin());
    }
    auto job = std::make_unique<LoadedFile>();
    job->path = path;
    job->mode = mode;
    job->generation = ++latestGeneration;
    job->channelStats = app.showHistogram;
    reloadPending = false;
    app.loading = true;
    app.loadingSinceMs = nowMs();
    app.loadingSize = stampOf(path).size;
    app.loadingProgress = 0.0f;
    if (!app.benchPath.empty()) {
        // Benchmarks time the whole open on the main thread.
        decode(*job, true);
        finishLoad(job.release());
        return;
    }
    // Decoded before: shown at once.
    if (mode != OpenMode::Reload) {
        if (std::unique_ptr<LoadedFile> hit = cacheTake(path)) {
            hit->mode = mode;
            hit->generation = job->generation;
            finishLoad(hit.release());
            return;
        }
    }
    SetTimer(app.hwnd, kLoadingTimer, 50, nullptr);
    {
        // Being prefetched right now: that decode becomes this request.
        std::lock_guard<std::mutex> lock(prefetchMutex);
        if (mode != OpenMode::Reload && prefetchInFlight == path) {
            promotedGeneration = job->generation;
            promotedMode = mode;
            return;
        }
        promotedGeneration = 0;
        prefetchQueue.erase(std::remove_if(prefetchQueue.begin(), prefetchQueue.end(),
                                           [&](const std::unique_ptr<LoadedFile>& q) { return q->path == path; }),
                            prefetchQueue.end());
    }
    std::lock_guard<std::mutex> lock(loaderMutex);
    waiting = std::move(job);
    if (!workerStarted) {
        std::thread(workerLoop).detach();
        workerStarted = true;
    }
    loaderWake.notify_one();
}

// ---- settings and recent files ----

fs::path folderIn(const wchar_t* variable) {
    const wchar_t* base = _wgetenv(variable);
    if (!base || !*base) return {};
    fs::path dir = fs::path(base) / L"HyperTextureViewer";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

fs::path exeFolder() {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return n ? fs::path(std::wstring(buf, n)).parent_path() : fs::path();
}

fs::path portableSettingsFile() {
    const fs::path dir = exeFolder();
    return dir.empty() ? fs::path() : dir / L"settings.ini";
}

// Portable mode is a settings.ini next to the exe; otherwise the settings live in %APPDATA%.
fs::path settingsFile() {
    std::error_code ec;
    const fs::path portable = portableSettingsFile();
    if (!portable.empty() && fs::exists(portable, ec)) return portable;
    const fs::path dir = folderIn(L"APPDATA");
    return dir.empty() ? fs::path() : dir / L"settings.ini";
}

constexpr int kThumbnailSize = 64;
constexpr int kMaxRecent = 10;

fs::path thumbnailFile(const fs::path& image) {
    const fs::path dir = folderIn(L"LOCALAPPDATA");
    if (dir.empty()) return {};
    std::wstring key = image.wstring();
    for (wchar_t& c : key) c = wchar_t(towlower(c));
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    for (wchar_t c : key) h = (h ^ uint64_t(c)) * 1099511628211ull;
    wchar_t name[32];
    swprintf(name, 32, L"%016llx.rgba", static_cast<unsigned long long>(h));
    std::error_code ec;
    fs::create_directories(dir / L"thumbs", ec);
    return dir / L"thumbs" / name;
}

void saveThumbnail(const fs::path& image) {
    const std::vector<uint8_t> px = renderThumbnail(kThumbnailSize);
    const fs::path file = thumbnailFile(image);
    if (px.empty() || file.empty()) return;
    if (FILE* f = _wfopen(file.c_str(), L"wb")) {
        std::fwrite(px.data(), 1, px.size(), f);
        std::fclose(f);
    }
    for (RecentFile& r : app.recent)
        if (r.path == image) {
            r.thumbnail.reset();
            r.thumbnailTried = false;
        }
}

void addRecent(const fs::path& path, OpenMode mode) {
    if (mode == OpenMode::Reload) return;
    // Browsing a folder moves its entry along instead of filling the list with neighbours.
    if (mode == OpenMode::Browse && !app.recent.empty() && app.recent[0].path.parent_path() == path.parent_path()) {
        app.recent[0].path = path;
        app.recent[0].openedAt = int64_t(std::time(nullptr));
        app.recent[0].thumbnail.reset();
        app.recent[0].thumbnailTried = false;
        return;
    }
    app.recent.erase(std::remove_if(app.recent.begin(), app.recent.end(), [&](const RecentFile& r) { return r.path == path; }),
                     app.recent.end());
    RecentFile r;
    r.path = path;
    r.openedAt = int64_t(std::time(nullptr));
    app.recent.insert(app.recent.begin(), std::move(r));
    if (app.recent.size() > kMaxRecent) app.recent.resize(kMaxRecent);
}

void showResult(const std::string& error) {
    app.error = error;
    releaseImage();
    SetWindowTextW(app.hwnd, (app.path.filename().wstring() + L" - Hyper Texture Viewer").c_str());
}

}  // namespace

bool isSupported(const fs::path& p) {
    const std::string ext = lowerExtension(p);
    const auto& wic = htv::wicExtensions();
    return hasBuiltInDecoder(ext) || std::find(wic.begin(), wic.end(), ext) != wic.end();
}

void finishLoad(LoadedFile* raw) {
    std::unique_ptr<LoadedFile> f(raw);
    if (f->generation != latestGeneration) {  // superseded by a newer request: kept for later
        if (f->path != app.shownPath) cacheInsert(std::move(f));
        return;
    }
    app.loading = false;
    KillTimer(app.hwnd, kLoadingTimer);
    if (!f->clipboard) watchedStamp = f->stamp;
    app.errorFormat = f->format;
    if (!f->ok) {
        showResult(f->error);
        return;
    }
    std::string error;
    const double t0 = nowMs();
    if (!uploadImage(f->image, error)) {
        showResult(error);
        return;
    }
    app.context->Flush();
    app.tLoad = f->decodeMs;
    app.tUpload = nowMs() - t0;

    const bool sameSize = app.hasImage && app.image.width == f->image.width && app.image.height == f->image.height;
    // The image on screen goes to the cache for going back to it, unless this is a new version of it.
    if (app.hasImage && !app.fromClipboard && app.shownPath != f->path && app.cacheMB > 0) {
        auto old = std::make_unique<LoadedFile>();
        old->path = app.shownPath;
        old->ok = true;
        old->image = std::move(app.image);
        old->stats = std::move(app.stats);
        old->alpha = app.imageAlpha;
        old->fileSize = app.fileSize;
        old->stamp = shownStamp;
        old->format = shownFormat;
        cacheInsert(std::move(old));
    }
    shownStamp = f->stamp;
    shownFormat = f->format;
    app.image = std::move(f->image);
    app.stats = std::move(f->stats);
    app.imageAlpha = f->alpha;
    app.hasImage = true;
    app.fromClipboard = f->clipboard;
    app.shownPath = f->path;
    app.fileSize = f->fileSize;
    app.error.clear();
    if (f->mode != OpenMode::Reload) app.dataIsLinear = isFloatFormat(htv::formatInfo(app.image.format));
    if (app.forcedMip >= app.textureMips) app.forcedMip = -1;
    if (app.pixelLocked) {
        app.lockedX = std::min(app.lockedX, int(app.image.width) - 1);
        app.lockedY = std::min(app.lockedY, int(app.image.height) - 1);
    }
    // A reload keeps zoom and position; a new file is fitted when Fit is on, or always when Settings says so.
    if (f->mode != OpenMode::Reload && !app.keepView) app.fit = true;
    if (app.fit && !(f->mode == OpenMode::Reload && sameSize)) fitToWindow();
    if (f->mode != OpenMode::Reload && !app.keepExposure) {
        app.exposure = 0.0f;
        app.blackPoint = 0.0f;
        app.whitePoint = 1.0f;
    }
    if (f->mode == OpenMode::Reload) {
        app.toastUntilMs = nowMs() + 4000.0;
        app.toastName = narrow(f->path.filename().wstring());
    }
    const std::wstring title = f->clipboard ? std::wstring(L"Clipboard") : f->path.filename().wstring();
    SetWindowTextW(app.hwnd, (title + L" - Hyper Texture Viewer").c_str());  // taskbar name
    if (!f->clipboard && app.benchPath.empty()) {
        addRecent(f->path, f->mode);
        if (f->mode == OpenMode::New) {
            saveThumbnail(f->path);
            saveSettings();
        }
    }
    if (!f->clipboard) schedulePrefetch();
    requestRedraw();
}

void openStartupFile(const fs::path& path) { requestOpen(path, OpenMode::New); }

void openPath(const fs::path& path) {
    std::error_code ec;
    const fs::path absolute = fs::absolute(path, ec);
    requestOpen(ec ? path : absolute, OpenMode::New);
}

void step(int delta) {
    if (app.folder.empty()) return;
    lastStep = delta < 0 ? -1 : 1;
    const int n = int(app.folder.size());
    const int from = app.folderIndex < 0 ? (delta > 0 ? -1 : 0) : app.folderIndex;
    requestOpen(app.folder[size_t(((from + delta) % n + n) % n)], OpenMode::Browse);
}

void stepTo(int index) {
    if (app.folder.empty()) return;
    const int n = int(app.folder.size());
    requestOpen(app.folder[size_t(std::clamp(index < 0 ? n + index : index, 0, n - 1))], OpenMode::Browse);
}

void reloadFile() {
    if (!app.path.empty()) requestOpen(app.path, OpenMode::Reload);
}

void cancelLoading() {
    if (!app.loading) return;
    ++latestGeneration;  // the worker's result will be dropped
    app.loading = false;
    KillTimer(app.hwnd, kLoadingTimer);
    // Back to what is on screen.
    {
        app.path = pathBeforeLoad;
        auto it = std::find(app.folder.begin(), app.folder.end(), app.path);
        app.folderIndex = it == app.folder.end() ? -1 : int(it - app.folder.begin());
    }
}

// Called by a timer: reloads the shown (or failed) file once it changes and then stays unchanged for a tick,
// so a file that is still being written is not read half way.
void checkFileChanged() {
    if (app.loading) {
        app.loadingProgress = decoding || !app.loadingSize ? -1.0f : float(bytesRead) / float(app.loadingSize);
        return;
    }
    if (app.fromClipboard || app.path.empty() || !app.autoReload) return;
    const FileStamp now = stampOf(app.path);
    if (!now.exists || now == watchedStamp) {
        reloadPending = false;
        return;
    }
    if (!reloadPending || now != pendingStamp) {
        reloadPending = true;
        pendingStamp = now;
        return;
    }
    reloadFile();
}

void pasteFromClipboard() {
    if (!OpenClipboard(app.hwnd)) return;
    fs::path file;
    std::vector<uint8_t> bytes;
    std::string ext;
    if (HANDLE h = GetClipboardData(CF_HDROP)) {
        wchar_t name[MAX_PATH * 4];
        if (DragQueryFileW(static_cast<HDROP>(h), 0, name, UINT(std::size(name)))) file = name;
    } else if (HANDLE png = GetClipboardData(RegisterClipboardFormatW(L"PNG"))) {
        const auto* p = static_cast<const uint8_t*>(GlobalLock(png));
        if (p) bytes.assign(p, p + GlobalSize(png));
        GlobalUnlock(png);
        ext = "png";
    } else if (HANDLE dib = GetClipboardData(CF_DIB)) {
        // A DIB is a BMP file without its 14-byte file header.
        const auto* p = static_cast<const uint8_t*>(GlobalLock(dib));
        const size_t size = GlobalSize(dib);
        if (p && size >= sizeof(BITMAPINFOHEADER)) {
            BITMAPINFOHEADER bih;
            std::memcpy(&bih, p, sizeof bih);
            uint32_t offset = 14 + bih.biSize;
            if (bih.biSize == sizeof(BITMAPINFOHEADER) && bih.biCompression == BI_BITFIELDS) offset += 12;
            if (bih.biBitCount <= 8) offset += 4 * (bih.biClrUsed ? bih.biClrUsed : 1u << bih.biBitCount);
            const uint32_t total = uint32_t(14 + size);
            const uint8_t header[14] = {'B', 'M', uint8_t(total), uint8_t(total >> 8), uint8_t(total >> 16),
                                        uint8_t(total >> 24), 0, 0, 0, 0, uint8_t(offset), uint8_t(offset >> 8),
                                        uint8_t(offset >> 16), uint8_t(offset >> 24)};
            bytes.assign(header, header + 14);
            bytes.insert(bytes.end(), p, p + size);
            ext = "bmp";
        }
        GlobalUnlock(dib);
    }
    CloseClipboard();
    if (!file.empty()) {
        openPath(file);
        return;
    }
    if (bytes.empty()) return;
    auto r = std::make_unique<LoadedFile>();
    r->generation = ++latestGeneration;
    r->clipboard = true;
    r->fileSize = bytes.size();
    r->format = ext == "png" ? "PNG" : "BMP";
    htv::LoadResult result = htv::loadImage(bytes.data(), bytes.size(), ext);
    if (!result.ok) return;  // nothing usable: leave the current image alone
    r->image = std::move(result.image);
    r->alpha = htv::hasTranslucentPixels(r->image);
    r->ok = true;
    app.path.clear();
    app.folder.clear();
    app.folderIndex = -1;
    finishLoad(r.release());
}

void copyFilePath() {
    const fs::path& p = app.hasImage ? app.shownPath : app.path;
    if (!p.empty() && !app.fromClipboard) ImGui::SetClipboardText(narrow(p.wstring()).c_str());
}

void showInExplorer() {
    if (app.path.empty()) return;
    const std::wstring args = L"/select,\"" + app.path.wstring() + L"\"";
    ShellExecuteW(app.hwnd, nullptr, L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

void openRecent(int index) {
    if (index >= 0 && index < int(app.recent.size())) openPath(app.recent[size_t(index)].path);
}

ID3D11ShaderResourceView* recentThumbnail(int index) {
    if (index < 0 || index >= int(app.recent.size())) return nullptr;
    RecentFile& r = app.recent[size_t(index)];
    if (!r.thumbnailTried) {
        r.thumbnailTried = true;
        const fs::path file = thumbnailFile(r.path);
        std::vector<uint8_t> px(size_t(kThumbnailSize) * kThumbnailSize * 4);
        if (FILE* f = file.empty() ? nullptr : _wfopen(file.c_str(), L"rb")) {
            if (std::fread(px.data(), 1, px.size(), f) == px.size()) r.thumbnail = createTexture(px.data(), kThumbnailSize, kThumbnailSize);
            std::fclose(f);
        }
    }
    return r.thumbnail.get();
}

bool pickColor(float rgb[3]) {
    static COLORREF custom[16] = {};
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = app.hwnd;
    cc.lpCustColors = custom;
    cc.rgbResult = RGB(int(std::round(rgb[0] * 255)), int(std::round(rgb[1] * 255)), int(std::round(rgb[2] * 255)));
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) return false;
    rgb[0] = GetRValue(cc.rgbResult) / 255.0f;
    rgb[1] = GetGValue(cc.rgbResult) / 255.0f;
    rgb[2] = GetBValue(cc.rgbResult) / 255.0f;
    return true;
}

void chooseCustomBackground() {
    if (!pickColor(app.customBackground)) return;
    app.alphaBackground = AlphaBackground::Custom;
    app.ignoreAlpha = false;
}

// Black and white points from the luminance percentiles, ignoring the darkest and brightest few pixels.
void autoRange() {
    if (!app.stats.valid) return;
    app.blackPoint = app.stats.percentile(0.001);
    app.whitePoint = std::max(app.stats.percentile(0.995), app.blackPoint * 1.01f + 1e-6f);
}

void ensureChannelStats() {
    if (!app.hasImage || app.stats.channelsValid) return;
    static int triedGeneration = -1;  // a failed GPU read is not retried every frame
    if (triedGeneration == app.imageGeneration) return;
    triedGeneration = app.imageGeneration;
    const htv::FormatInfo& info = htv::formatInfo(app.image.format);
    if (!info.compressed) {
        app.stats = htv::computeStats(app.image, true);
        return;
    }
    // The GPU decodes sRGB formats to linear; the histogram shows stored values like for every other format.
    const bool srgb = isSrgbFormat(app.image.format);
    auto stored = [srgb](float v) {
        if (!srgb) return v;
        v = std::clamp(v, 0.0f, 1.0f);
        return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    };
    htv::StatsBuilder builder(true);
    const bool ok = scanImageGpu([&](const float* p, int width) {
        for (int x = 0; x < width; ++x, p += 4) {
            const float r = stored(p[0]);
            if (info.channels == 1) builder.add(r, r, r);
            else builder.add(r, stored(p[1]), info.channels == 2 ? 0.0f : stored(p[2]));
        }
    });
    if (ok) app.stats = builder.finish();
}

void saveOnExit() {
    if (app.hasImage && !app.fromClipboard && !app.recent.empty() && app.recent[0].path == app.shownPath)
        saveThumbnail(app.shownPath);
    saveSettings();
}

void loadSettings() {
    const fs::path file = settingsFile();
    FILE* f = file.empty() ? nullptr : _wfopen(file.c_str(), L"rb");
    if (!f) return;
    char line[4096];
    while (std::fgets(line, sizeof line, f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        const size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = s.substr(0, eq), value = s.substr(eq + 1);
        auto flag = [&](const char* name, bool& out) {
            if (key == name) out = value == "1";
            return key == name;
        };
        if (key == "theme") {
            app.themeMode = value == "light" ? 1 : value == "system" ? 2 : 0;
        } else if (key == "scale") {
            const float v = std::strtof(value.c_str(), nullptr);
            app.interfaceScale = v >= 1.0f && v <= 2.0f ? v : 0.0f;
        } else if (key == "viewport") {
            unsigned rgb = 0;
            app.customViewport = std::sscanf(value.c_str(), "%x", &rgb) == 1;
            for (int i = 0; i < 3; ++i) app.viewportColor[i] = float((rgb >> (16 - 8 * i)) & 0xff) / 255.0f;
        } else if (key == "checker") {
            app.checkerSize = std::clamp(std::atoi(value.c_str()), 0, 2);
        } else if (flag("keep_view", app.keepView) || flag("keep_exposure", app.keepExposure) ||
                   flag("auto_reload", app.autoReload) || flag("single_instance", app.singleInstance)) {
        } else if (key == "sort") {
            app.folderSort = std::clamp(std::atoi(value.c_str()), 0, 4);
        } else if (key == "cache_mb") {
            app.cacheMB = std::clamp(std::atoi(value.c_str()), 0, 65536);
        } else if (key == "prefetch") {
            app.prefetch = std::clamp(std::atoi(value.c_str()), 0, 2);
        } else if (htv::bindingFromSettings(key, value)) {
        } else if (key == "recent" && app.recent.size() < kMaxRecent) {
            const size_t tab = value.find('\t');
            if (tab == std::string::npos) continue;
            RecentFile r;
            r.openedAt = std::strtoll(value.substr(0, tab).c_str(), nullptr, 10);
            r.path = widen(value.substr(tab + 1));
            app.recent.push_back(std::move(r));
        }
    }
    std::fclose(f);
}

bool portableMode() {
    std::error_code ec;
    const fs::path portable = portableSettingsFile();
    return !portable.empty() && fs::exists(portable, ec);
}

bool setPortableMode(bool on) {
    if (on == portableMode()) return true;
    const fs::path portable = portableSettingsFile();
    std::error_code ec;
    if (on) {
        // Creating the file is what makes the mode; Program Files is not writable without elevation.
        FILE* f = portable.empty() ? nullptr : _wfopen(portable.c_str(), L"wb");
        if (!f) return false;
        std::fclose(f);
    } else if (!fs::remove(portable, ec)) {
        return false;
    }
    saveSettings();
    return true;
}

void prefetchDone(LoadedFile* raw) {
    std::unique_ptr<LoadedFile> f(raw);
    if (f->generation != prefetchGeneration || f->path == app.shownPath) return;
    cacheInsert(std::move(f));
}

void trimImageCache() {
    trimCache(cacheBudget());
    if (app.cacheMB <= 0 || app.prefetch <= 0) {
        std::lock_guard<std::mutex> lock(prefetchMutex);
        prefetchQueue.clear();
        ++prefetchGeneration;
    } else {
        schedulePrefetch();
    }
}

void relistFolder() {
    if (app.path.empty() || app.fromClipboard) return;
    listFolder(app.path);
}

void saveSettings() {
    if (!app.benchPath.empty()) return;
    const fs::path file = settingsFile();
    FILE* f = file.empty() ? nullptr : _wfopen(file.c_str(), L"wb");
    if (!f) return;
    static const char* themes[] = {"dark", "light", "system"};
    std::fprintf(f, "theme=%s\n", themes[std::clamp(app.themeMode, 0, 2)]);
    if (app.interfaceScale > 0) std::fprintf(f, "scale=%.2f\n", double(app.interfaceScale));
    if (app.customViewport) {
        int rgb = 0;
        for (int i = 0; i < 3; ++i) rgb = rgb << 8 | int(std::round(std::clamp(app.viewportColor[i], 0.0f, 1.0f) * 255));
        std::fprintf(f, "viewport=%06X\n", rgb);
    }
    std::fprintf(f, "checker=%d\nkeep_view=%d\nkeep_exposure=%d\nauto_reload=%d\nsort=%d\nsingle_instance=%d\n",
                 app.checkerSize, app.keepView, app.keepExposure, app.autoReload, app.folderSort, app.singleInstance);
    std::fprintf(f, "cache_mb=%d\nprefetch=%d\n", app.cacheMB, app.prefetch);
    std::fputs(htv::bindingsToSettings().c_str(), f);
    for (const RecentFile& r : app.recent)
        std::fprintf(f, "recent=%lld\t%s\n", static_cast<long long>(r.openedAt), narrow(r.path.wstring()).c_str());
    std::fclose(f);
}

}  // namespace htv
