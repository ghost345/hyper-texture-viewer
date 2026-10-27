// The view and the image on screen, shared by the Windows (main.cpp, files.cpp) and browser (web/) builds:
// layout, zoom and position, pixel lookups, animation frames and layers, settings lines.

#include "viewer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "keys.h"
#include "ui.h"

namespace fs = std::filesystem;

namespace htv {

namespace {

// The image's pixels changed in place (another layer, or the layers composited again): upload them and
// refresh what was worked out from them.
void layerChanged(bool sizeChanged) {
    std::string error;
    if (!uploadImage(app.image, error)) return;
    if (isHighPrecision(app.image) || app.stats.channelsValid) app.stats = htv::computeStats(app.image, app.stats.channelsValid);
    else app.stats = htv::ImageStats();
    app.imageAlpha = htv::hasTranslucentPixels(app.image);
    app.normalHint = app.fromClipboard ? std::string() : htv::missingNormalZ(app.image, narrow(app.shownPath.filename().wstring()));
    if (app.forcedMip >= app.textureMips) app.forcedMip = -1;
    if (app.pixelLocked) {
        app.lockedX = std::min(app.lockedX, int(app.image.width) - 1);
        app.lockedY = std::min(app.lockedY, int(app.image.height) - 1);
    }
    if (sizeChanged && app.fit) fitToWindow();
    requestRedraw();
}

// How long a frame stays, as browsers play it: GIFs saved with 0 or 10 ms mean "as fast as possible",
// which every browser shows as 100 ms, and the animations are made to look right there.
double frameDelayMs(int index) {
    const int delay = app.image.frames[size_t(index)].delayMs;
    return delay <= 10 ? 100.0 : double(delay);
}

}  // namespace

void layout() {
    const float top = app.showBars ? uiTopBarHeight() : 0.0f;
    const float bottom = app.showBars ? uiBottomBarHeight() : 0.0f;
    // The rulers take a strip off the top and left of the image area.
    const float ruler = app.ruler && app.showBars ? std::round(18 * app.scale) : 0.0f;
    app.vpX = ruler;
    app.vpY = top + ruler;
    app.vpW = std::max(1.0f, float(app.winW) - ruler);
    app.vpH = std::max(1.0f, float(app.winH) - top - bottom - ruler);
}

bool inViewport(int x, int y) {
    return x >= app.vpX && y >= app.vpY && x < app.vpX + app.vpW && y < app.vpY + app.vpH;
}

void relayout() {
    layout();
    if (app.fit) fitToWindow();
}

void toggleRuler() {
    app.ruler = !app.ruler;
    relayout();
}

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

void applyInterfaceScale() {
    app.scale = app.interfaceScale > 0 ? app.interfaceScale : app.dpiScale;
    uiSetScale(app.scale);
    layout();
    if (app.fit && app.hasImage) fitToWindow();
    requestRedraw();
}

void toggleBars() {
    app.showBars = !app.showBars;
    const float top = app.vpY;
    layout();
    if (app.fit) fitToWindow();
    else app.originY += app.vpY - top;  // the image stays put on screen
}

bool isAnimation() { return app.hasImage && app.image.frames.size() > 1; }

void showFrame(int index) {
    if (!isAnimation()) return;
    const int n = int(app.image.frames.size());
    index = ((index % n) + n) % n;
    std::string error;
    if (!uploadSurface(app.image.frames[size_t(index)].surface, app.image.format, error)) return;
    app.frameIndex = index;
    app.frameShownMs = nowMs();
    if (app.pixelLocked) app.imageAlpha = app.imageAlpha;  // readouts refresh on the new generation
    requestRedraw();
}

bool hasLayers() {
    if (!app.hasImage) return false;
    const htv::Image& img = app.image;
    return img.layers.size() > 1 || (img.layerKind == htv::LayerKind::PsdLayers && !img.layers.empty());
}

void selectLayer(int index) {
    if (!hasLayers()) return;
    const int n = int(app.image.layers.size());
    index = ((index % n) + n) % n;
    const uint32_t w = app.image.width, h = app.image.height;
    if (!htv::showLayer(app.image, index)) return;
    layerChanged(w != app.image.width || h != app.image.height);
}

void setLayerVisible(int index, bool visible, bool solo) {
    htv::Image& img = app.image;
    if (!hasLayers() || img.layerKind != htv::LayerKind::PsdLayers || index < 0 || index >= int(img.layers.size())) return;
    // Alt+click, as in Photoshop: the layer alone (with the groups around it and what is inside it), and the
    // same again puts every eye back as it was.
    static std::vector<bool> beforeSolo, soloState;
    static int soloIndex = -1;
    static fs::path soloPath;
    auto state = [&] {
        std::vector<bool> v;
        for (const htv::Layer& l : img.layers) v.push_back(l.visible);
        return v;
    };
    auto contains = [&](int outer, int i) {  // `i` is `outer` or inside it
        for (int p = i; p >= 0; p = img.layers[size_t(p)].parent)
            if (p == outer) return true;
        return false;
    };
    if (solo && soloPath == app.shownPath && soloIndex == index && state() == soloState && beforeSolo.size() == img.layers.size()) {
        for (size_t i = 0; i < img.layers.size(); ++i) img.layers[i].visible = beforeSolo[i];
        soloIndex = -1;
    } else if (solo) {
        beforeSolo = state();
        for (int i = 0; i < int(img.layers.size()); ++i) {
            htv::Layer& l = img.layers[size_t(i)];
            if (contains(i, index)) l.visible = true;   // the layer and the groups around it
            else if (!contains(index, i)) l.visible = false;  // inside it: as they were
        }
        soloIndex = index;
        soloPath = app.shownPath;
        soloState = state();
    } else {
        img.layers[size_t(index)].visible = visible;
    }
    if (htv::compositeLayers(img)) layerChanged(false);
}

void advanceFrame() {
    if (!isAnimation() || !app.framePlaying) return;
    // Frames follow a clock of their own: each one is due a delay after the previous one was due, not after
    // it went up, so the timer's coarse ticks don't add up into a slower animation.
    double due = app.frameShownMs + frameDelayMs(app.frameIndex);
    const double now = nowMs();
    if (now + 1.0 < due) return;
    int next = app.frameIndex + 1;
    const int n = int(app.image.frames.size());
    while (now >= due + frameDelayMs(next % n) && next - app.frameIndex < n) due += frameDelayMs(next++ % n);  // skip what was missed
    if (now - due > 1000.0) due = now;  // after a stall, start over instead of racing to catch up
    showFrame(next);
    app.frameShownMs = due;
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

// The decoded file becomes the shown image; its pixels are already on the GPU.
void adoptLoaded(LoadedFile& f, bool sameSize) {
    app.image = std::move(f.image);
    app.stats = std::move(f.stats);
    app.imageAlpha = f.alpha;
    app.hasImage = true;
    ++app.fileGeneration;
    app.frameIndex = 0;
    app.frameShownMs = nowMs();
    if (!isAnimation() && !hasLayers()) app.showFrames = false;
    app.fromClipboard = f.clipboard;
    app.shownPath = f.path;
    app.fileSize = f.fileSize;
    app.error.clear();
    app.normalHint = htv::missingNormalZ(app.image, f.clipboard ? std::string() : narrow(f.path.filename().wstring()));
    if (f.mode != OpenMode::Reload) app.normalHintDismissed = false;
    if (f.mode != OpenMode::Reload) app.dataIsLinear = isFloatFormat(htv::formatInfo(app.image.format));
    if (app.forcedMip >= app.textureMips) app.forcedMip = -1;
    if (app.pixelLocked) {
        app.lockedX = std::min(app.lockedX, int(app.image.width) - 1);
        app.lockedY = std::min(app.lockedY, int(app.image.height) - 1);
    }
    // A reload keeps zoom and position; a new file is fitted when Fit is on, or always when Settings says so.
    if (f.mode != OpenMode::Reload && !app.keepView) app.fit = true;
    if (app.fit && !(f.mode == OpenMode::Reload && sameSize)) fitToWindow();
    if (f.mode != OpenMode::Reload && !app.keepExposure) {
        app.exposure = 0.0f;
        app.blackPoint = 0.0f;
        app.whitePoint = 1.0f;
    }
    if (f.mode == OpenMode::Reload) {
        app.toastUntilMs = nowMs() + 4000.0;
        app.toastName = narrow(f.path.filename().wstring());
    }
}

// One settings.ini line; unknown keys are ignored.
void applySetting(const std::string& key, const std::string& value) {
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
    } else if (key == "ruler_unit") {
        app.rulerUnit = std::clamp(std::atoi(value.c_str()), 0, 2);
    } else if (key == "ruler_dpi") {
        const float v = std::strtof(value.c_str(), nullptr);
        if (v >= 1.0f && v <= 100000.0f) app.rulerDpi = v;
    } else if (key == "cache_mb") {
        app.cacheMB = std::clamp(std::atoi(value.c_str()), 0, 65536);
    } else if (key == "prefetch") {
        app.prefetch = std::clamp(std::atoi(value.c_str()), 0, 2);
    } else if (htv::bindingFromSettings(key, value)) {
    } else if (key == "recent" && app.recent.size() < kMaxRecent) {
        const size_t tab = value.find('\t');
        if (tab == std::string::npos) return;
        RecentFile r;
        r.openedAt = std::strtoll(value.substr(0, tab).c_str(), nullptr, 10);
        r.path = widen(value.substr(tab + 1));
        app.recent.push_back(std::move(r));
    }
}

std::string settingsText() {
    std::string out;
    char buf[256];
    auto add = [&](const char* format, auto... values) {
        std::snprintf(buf, sizeof buf, format, values...);
        out += buf;
    };
    static const char* themes[] = {"dark", "light", "system"};
    add("theme=%s\n", themes[std::clamp(app.themeMode, 0, 2)]);
    if (app.interfaceScale > 0) add("scale=%.2f\n", double(app.interfaceScale));
    if (app.customViewport) {
        int rgb = 0;
        for (int i = 0; i < 3; ++i) rgb = rgb << 8 | int(std::round(std::clamp(app.viewportColor[i], 0.0f, 1.0f) * 255));
        add("viewport=%06X\n", rgb);
    }
    add("checker=%d\nkeep_view=%d\nkeep_exposure=%d\nauto_reload=%d\nsort=%d\nsingle_instance=%d\n", app.checkerSize,
        app.keepView, app.keepExposure, app.autoReload, app.folderSort, app.singleInstance);
    add("cache_mb=%d\nprefetch=%d\n", app.cacheMB, app.prefetch);
    add("ruler_unit=%d\nruler_dpi=%g\n", app.rulerUnit, double(app.rulerDpi));
    out += htv::bindingsToSettings();
    for (const RecentFile& r : app.recent)
        out += "recent=" + std::to_string(r.openedAt) + "\t" + narrow(r.path.wstring()) + "\n";
    return out;
}

}  // namespace htv
