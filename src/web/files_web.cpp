// Files in the browser: the page hands over File objects (the Open dialog, drag and drop, paste) and reads them when
// asked; decoding happens here, on the page's thread. Settings live in the browser's localStorage.

#include "viewer.h"

#include <emscripten.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <new>

#include "imgui.h"
#include "keys.h"

namespace fs = std::filesystem;

// The page side, in web/shell.html: Module.htvFiles holds the File objects by handle.
EM_JS(void, js_read_file, (int handle, int request), { Module.htvReadFile(handle, request); });
EM_JS(void, js_pick_files, (int purpose), { Module.htvPickFiles(purpose); });
EM_JS(void, js_pick_color, (int purpose, int rgb), { Module.htvPickColor(purpose, rgb); });
EM_JS(void, js_paste, (), { Module.htvPasteFromClipboard(); });
EM_JS(void, js_set_title, (const char* title), { document.title = UTF8ToString(title); });
EM_JS(void, js_save_settings, (const char* text), {
    try { localStorage.setItem('htv.settings', UTF8ToString(text)); } catch (e) {}
});
EM_JS(char*, js_load_settings, (), {
    let text = null;
    try { text = localStorage.getItem('htv.settings'); } catch (e) {}
    return text === null ? 0 : stringToNewUTF8(text);
});

namespace htv {

namespace {

// A file the page was given. Paths are made up of the folder the file was dropped with, if any, and its name.
struct WebFile {
    fs::path path;
    int handle = -1;
    uint64_t size = 0;
    uint64_t modified = 0;  // milliseconds since 1970
};
std::vector<WebFile> files;

const WebFile* findFile(const fs::path& path) {
    for (const WebFile& f : files)
        if (f.path == path) return &f;
    return nullptr;
}

std::string lowerExtension(const fs::path& p) {
    std::string e = p.extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    for (char& c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

// ---- reading and decoding ----

// A read the page is doing; its bytes come back through htv_file_data.
struct Request {
    int id = 0;
    std::unique_ptr<LoadedFile> job;
};
std::vector<Request> requests;
int lastRequest = 0;
int latestGeneration = 0;
int compareGeneration[2] = {};

// Bytes that arrived, decoded once a frame showed the loading state.
std::vector<std::pair<std::unique_ptr<LoadedFile>, std::vector<uint8_t>>> arrived;

void decode(LoadedFile& r, const std::vector<uint8_t>& bytes) {
    const double t0 = nowMs();
    const std::string ext = lowerExtension(r.path);
    try {
        htv::LoadResult result = htv::loadImage(bytes.data(), bytes.size(), ext);
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
    } catch (const std::bad_alloc&) {
        r.image = htv::Image();
        r.ok = false;
        r.error = "Not enough memory for this image (the browser gives a page up to 4 GB).";
    }
    r.decodeMs = nowMs() - t0;
}

void startRead(std::unique_ptr<LoadedFile> job) {
    const WebFile* f = findFile(job->path);
    if (!f) {
        job->error = "The file is no longer available; open it again.";
        arrived.emplace_back(std::move(job), std::vector<uint8_t>());
        return;
    }
    job->fileSize = f->size;
    job->stamp = {f->modified, f->size, true};
    Request r;
    r.id = ++lastRequest;
    r.job = std::move(job);
    const int handle = f->handle;
    requests.push_back(std::move(r));
    js_read_file(handle, lastRequest);
}

// ---- cache of decoded images, as in files.cpp ----

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

std::unique_ptr<LoadedFile> cacheTake(const fs::path& path) {
    for (auto it = cache.begin(); it != cache.end(); ++it) {
        if (it->file->path != path) continue;
        std::unique_ptr<LoadedFile> f = std::move(it->file);
        cache.erase(it);
        return f;
    }
    return nullptr;
}

// ---- the folder: the files the page was given last ----

// Explorer's order: runs of digits compare by value.
bool naturalLess(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const unsigned char ca = static_cast<unsigned char>(a[i]), cb = static_cast<unsigned char>(b[j]);
        if (std::isdigit(ca) && std::isdigit(cb)) {
            size_t ei = i, ej = j;
            while (ei < a.size() && std::isdigit(static_cast<unsigned char>(a[ei]))) ++ei;
            while (ej < b.size() && std::isdigit(static_cast<unsigned char>(b[ej]))) ++ej;
            const std::string na = a.substr(i, ei - i), nb = b.substr(j, ej - j);
            const size_t za = na.find_first_not_of('0'), zb = nb.find_first_not_of('0');
            const std::string va = za == std::string::npos ? "" : na.substr(za), vb = zb == std::string::npos ? "" : nb.substr(zb);
            if (va.size() != vb.size()) return va.size() < vb.size();
            if (va != vb) return va < vb;
            i = ei;
            j = ej;
            continue;
        }
        const int la = std::tolower(ca), lb = std::tolower(cb);
        if (la != lb) return la < lb;
        ++i;
        ++j;
    }
    return a.size() - i < b.size() - j;
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void listFolder() {
    std::vector<const WebFile*> sorted;
    for (const WebFile& f : files) sorted.push_back(&f);
    std::sort(sorted.begin(), sorted.end(),
              [](const WebFile* a, const WebFile* b) { return naturalLess(a->path.string(), b->path.string()); });
    if (app.folderSort == 1)
        std::stable_sort(sorted.begin(), sorted.end(),
                         [](const WebFile* a, const WebFile* b) { return lower(a->path.string()) < lower(b->path.string()); });
    else if (app.folderSort == 2)
        std::stable_sort(sorted.begin(), sorted.end(), [](const WebFile* a, const WebFile* b) { return a->modified > b->modified; });
    else if (app.folderSort == 3)
        std::stable_sort(sorted.begin(), sorted.end(), [](const WebFile* a, const WebFile* b) { return a->size > b->size; });
    else if (app.folderSort == 4)
        std::stable_sort(sorted.begin(), sorted.end(), [](const WebFile* a, const WebFile* b) {
            return lowerExtension(a->path) < lowerExtension(b->path);
        });
    app.folder.clear();
    for (const WebFile* f : sorted) app.folder.push_back(f->path);
    auto it = std::find(app.folder.begin(), app.folder.end(), app.path);
    app.folderIndex = it == app.folder.end() ? -1 : int(it - app.folder.begin());
}

fs::path pathBeforeLoad;

void requestOpen(const fs::path& path, OpenMode mode) {
    if (!app.loading) pathBeforeLoad = app.path;
    app.path = path;
    auto it = std::find(app.folder.begin(), app.folder.end(), path);
    app.folderIndex = it == app.folder.end() ? -1 : int(it - app.folder.begin());
    auto job = std::make_unique<LoadedFile>();
    job->path = path;
    job->mode = mode;
    job->generation = ++latestGeneration;
    job->channelStats = app.showHistogram;
    app.loading = true;
    app.loadingSinceMs = nowMs();
    app.loadingProgress = -1.0f;
    if (const WebFile* f = findFile(path)) app.loadingSize = f->size;
    if (mode != OpenMode::Reload)
        if (std::unique_ptr<LoadedFile> hit = cacheTake(path)) {
            hit->mode = mode;
            hit->generation = job->generation;
            finishLoad(hit.release());
            return;
        }
    startRead(std::move(job));
}

// What the next files the page hands over are for: 0 opening, 1 and 2 Compare's B and C.
int pickPurpose = 0;
std::vector<WebFile> incoming;

void showResult(const std::string& error) {
    app.error = error;
    releaseImage();
    js_set_title((narrow(app.path.filename().wstring()) + " - Hyper Texture Viewer").c_str());
}

}  // namespace

bool isSupported(const fs::path& p) {
    const std::string ext = lowerExtension(p);
    const auto& list = htv::supportedExtensions();
    return std::find(list.begin(), list.end(), ext) != list.end();
}

// Decodes what arrived, one file a frame; called by the main loop after a frame showed the loading badge.
void decodeArrived() {
    if (arrived.empty()) return;
    auto [job, bytes] = std::move(arrived.front());
    arrived.erase(arrived.begin());
    if (job->error.empty()) decode(*job, bytes);
    bytes = {};
    if (job->compareSlot >= 0) compareSlotLoaded(job.release());
    else finishLoad(job.release());
}

bool decodePending() { return !arrived.empty(); }

void finishLoad(LoadedFile* raw) {
    std::unique_ptr<LoadedFile> f(raw);
    if (f->generation != latestGeneration) {
        if (f->path != app.shownPath) cacheInsert(std::move(f));
        return;
    }
    app.loading = false;
    app.errorFormat = lowerExtension(f->path);
    for (char& c : app.errorFormat) c = char(std::toupper(static_cast<unsigned char>(c)));
    if (!f->ok) {
        showResult(f->error);
        return;
    }
    std::string error;
    if (!uploadImage(f->image, error)) {
        showResult(error);
        return;
    }
    const bool sameSize = app.hasImage && app.image.width == f->image.width && app.image.height == f->image.height;
    if (app.hasImage && !app.fromClipboard && app.shownPath != f->path) app.previousPath = app.shownPath;
    if (app.hasImage && !app.fromClipboard && app.shownPath != f->path && app.cacheMB > 0) {
        auto old = std::make_unique<LoadedFile>();
        old->path = app.shownPath;
        old->ok = true;
        old->image = std::move(app.image);
        old->stats = std::move(app.stats);
        old->alpha = app.imageAlpha;
        old->fileSize = app.fileSize;
        cacheInsert(std::move(old));
    }
    adoptLoaded(*f, sameSize);
    js_set_title(((f->clipboard ? std::string("Clipboard") : narrow(f->path.filename().wstring())) + " - Hyper Texture Viewer").c_str());
    requestRedraw();
}

void loadCompareSlot(int slot, const fs::path& path) {
    CompareSlot& s = app.compareSlots[slot];
    const int generation = ++compareGeneration[slot];
    s = CompareSlot();
    s.generation = generation;
    if (path.empty()) return;
    s.path = path;
    s.loading = true;
    if (std::unique_ptr<LoadedFile> f = cacheTake(path)) {
        f->compareSlot = slot;
        f->generation = generation;
        compareSlotLoaded(f.release());
        return;
    }
    auto job = std::make_unique<LoadedFile>();
    job->path = path;
    job->generation = generation;
    job->compareSlot = slot;
    startRead(std::move(job));
}

void compareSlotLoaded(LoadedFile* raw) {
    std::unique_ptr<LoadedFile> f(raw);
    const int slot = f->compareSlot;
    CompareSlot& s = app.compareSlots[slot];
    if (f->generation != compareGeneration[slot]) {
        if (f->ok) cacheInsert(std::move(f));
        return;
    }
    s.loading = false;
    if (!f->ok) {
        s.error = f->error;
        return;
    }
    std::string error;
    if (!createImageTexture(f->image, s.srv, s.mips, error)) {
        s.error = error;
        return;
    }
    s.fileSize = f->fileSize;
    s.image.width = f->image.width;
    s.image.height = f->image.height;
    s.image.format = f->image.format;
    s.image.description = f->image.description;
    s.generation = ++compareGeneration[slot];
    if (f->path != app.shownPath) cacheInsert(std::move(f));
    requestRedraw();
}

void openStartupFile(const fs::path& path) { requestOpen(path, OpenMode::New); }

void openPath(const fs::path& path) { requestOpen(path, OpenMode::New); }

void showOpenDialog() { js_pick_files(0); }

bool pickImageFile(fs::path&) { return false; }  // the browser's dialog answers later: see pickCompareFile

void pickCompareFile(int slot) { js_pick_files(1 + slot); }

void step(int delta) {
    if (app.folder.empty()) return;
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
    ++latestGeneration;
    app.loading = false;
    app.path = pathBeforeLoad;
    auto it = std::find(app.folder.begin(), app.folder.end(), app.path);
    app.folderIndex = it == app.folder.end() ? -1 : int(it - app.folder.begin());
}

void checkFileChanged() {}

void pasteFromClipboard() { js_paste(); }

void copyFilePath() {
    const fs::path& p = app.hasImage ? app.shownPath : app.path;
    if (!p.empty() && !app.fromClipboard) ImGui::SetClipboardText(narrow(p.wstring()).c_str());
}

std::string fileTime(const fs::path& path, bool created) {
    const WebFile* f = findFile(path);
    if (created || !f || !f->modified) return "-";
    const std::time_t t = std::time_t(f->modified / 1000);
    const std::tm* local = std::localtime(&t);
    char buf[32];
    if (!local || !std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", local)) return "-";
    return buf;
}

void showInExplorer() {}
void openRecent(int) {}
GpuView* recentThumbnail(int) { return nullptr; }
bool pickColor(float[3]) { return false; }  // see chooseCustomBackground and chooseViewportColor

static int packRgb(const float rgb[3]) {
    int v = 0;
    for (int i = 0; i < 3; ++i) v = v << 8 | int(std::clamp(rgb[i], 0.0f, 1.0f) * 255.0f + 0.5f);
    return v;
}

void chooseCustomBackground() { js_pick_color(0, packRgb(app.customBackground)); }
void chooseViewportColor() { js_pick_color(1, packRgb(app.viewportColor)); }

void saveOnExit() { saveSettings(); }

void loadSettings() {
    char* text = js_load_settings();
    if (!text) return;
    std::string s = text;
    std::free(text);
    size_t start = 0;
    while (start < s.size()) {
        size_t end = s.find('\n', start);
        if (end == std::string::npos) end = s.size();
        const std::string line = s.substr(start, end - start);
        start = end + 1;
        const size_t eq = line.find('=');
        if (eq != std::string::npos) applySetting(line.substr(0, eq), line.substr(eq + 1));
    }
}

void saveSettings() { js_save_settings(settingsText().c_str()); }

bool portableMode() { return false; }
bool setPortableMode(bool) { return false; }
bool registerFileTypes() { return false; }
void prefetchDone(LoadedFile*) {}
void trimImageCache() { trimCache(cacheBudget()); }

void relistFolder() {
    if (!files.empty()) listFolder();
}

}  // namespace htv

using namespace htv;

// ---- called by the page ----

extern "C" {

EMSCRIPTEN_KEEPALIVE void htv_files_begin(int purpose) {
    pickPurpose = purpose;
    incoming.clear();
}

EMSCRIPTEN_KEEPALIVE void htv_files_add(int handle, const char* path, double size, double modified) {
    WebFile f;
    f.path = fs::path(path);
    f.handle = handle;
    f.size = uint64_t(size);
    f.modified = uint64_t(modified);
    if (isSupported(f.path)) incoming.push_back(std::move(f));
}

// The files handed over since htv_files_begin: a new folder to browse, or the file for a Compare slot. `first` is
// the path to open first, empty for the first in folder order.
EMSCRIPTEN_KEEPALIVE void htv_files_end(const char* first) {
    if (incoming.empty()) {
        app.error = "None of these files is an image type the viewer reads.";
        requestRedraw();
        return;
    }
    if (pickPurpose == 1 || pickPurpose == 2) {
        // Compare keeps the folder; the new file joins the known ones.
        const WebFile f = incoming.front();
        if (!findFile(f.path)) files.push_back(f);
        loadCompareSlot(pickPurpose - 1, f.path);
        incoming.clear();
        return;
    }
    // A new set of files replaces the old ones, except those Compare is showing.
    std::vector<WebFile> kept;
    for (const WebFile& f : files)
        for (const CompareSlot& s : app.compareSlots)
            if (s.path == f.path) kept.push_back(f);
    files = std::move(incoming);
    for (WebFile& k : kept)
        if (!findFile(k.path)) files.push_back(std::move(k));
    incoming.clear();
    cache.clear();
    std::string firstPath = first ? first : "";
    app.path = firstPath;
    listFolder();
    if (firstPath.empty() || !findFile(firstPath)) firstPath = app.folder.empty() ? std::string() : app.folder.front().string();
    if (!firstPath.empty()) openPath(firstPath);
}

// The page read a file: `data` is malloc'd for this side to keep, or null with `error` set.
EMSCRIPTEN_KEEPALIVE void htv_file_data(int request, uint8_t* data, double size, const char* error) {
    auto it = std::find_if(requests.begin(), requests.end(), [&](const Request& r) { return r.id == request; });
    if (it == requests.end()) {
        std::free(data);
        return;
    }
    std::unique_ptr<LoadedFile> job = std::move(it->job);
    requests.erase(it);
    std::vector<uint8_t> bytes;
    if (data) {
        try {
            bytes.assign(data, data + size_t(size));
        } catch (const std::bad_alloc&) {
            job->error = "Not enough memory for this file.";
        }
        std::free(data);
    } else {
        job->error = error && *error ? error : "The file cannot be read.";
    }
    arrived.emplace_back(std::move(job), std::move(bytes));
    requestRedraw();
}

// An image from the clipboard (the page's paste event).
EMSCRIPTEN_KEEPALIVE void htv_paste(uint8_t* data, double size, const char* extension) {
    std::vector<uint8_t> bytes(data, data + size_t(size));
    std::free(data);
    auto r = std::make_unique<LoadedFile>();
    r->generation = ++latestGeneration;
    r->clipboard = true;
    r->fileSize = bytes.size();
    const std::string ext = extension;
    htv::LoadResult result = htv::loadImage(bytes.data(), bytes.size(), ext);
    if (!result.ok) return;
    r->image = std::move(result.image);
    if (isHighPrecision(r->image)) r->stats = htv::computeStats(r->image, false);
    r->alpha = htv::hasTranslucentPixels(r->image);
    r->ok = true;
    app.path.clear();
    app.folder.clear();
    app.folderIndex = -1;
    finishLoad(r.release());
}

EMSCRIPTEN_KEEPALIVE void htv_color_picked(int purpose, int rgb) {
    float* out = purpose == 0 ? app.customBackground : app.viewportColor;
    for (int i = 0; i < 3; ++i) out[i] = float((rgb >> (16 - 8 * i)) & 0xff) / 255.0f;
    if (purpose == 0) {
        app.alphaBackground = AlphaBackground::Custom;
        app.ignoreAlpha = false;
    } else {
        app.customViewport = true;
        saveSettings();
    }
    requestRedraw();
}

}  // extern "C"
