// Settings (Ctrl+,): appearance, browsing, application and keys. Changes apply immediately and are saved.

#include "ui.h"
#include "ui_internal.h"

#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace htv::ui {
namespace {

// The key being reassigned, and a key that is taken by another action and waits for a second press to swap.
Action capturing = Action::Count;
int capturingSlot = 0;
KeyBinding conflictKey;
std::string keyNote;  // warning under the table

void stopCapture() {
    capturing = Action::Count;
    conflictKey = {};
    keyNote.clear();
}

// Group heading (.gh): 11 px semibold, a line under it.
float groupHeading(ImDrawList* dl, float x, float y, float w, const char* title) {
    const Theme& t = T();
    y += S(14);
    drawText(dl, fontSemibold, 11, x, y + S(7), t.tx3, title);
    y += S(14) + S(5);
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + line()), t.bd);
    return y + line() + S(4);
}

void rowLabel(ImDrawList* dl, float x, float cy, const char* label) { drawText(dl, fontRegular, 12, x, cy, T().tx, label); }

void note(ImDrawList* dl, float x, float cy, const char* text) { drawText(dl, fontRegular, 11, x, cy, T().tx3, text); }

std::string scaleLabel(float v) {
    char buf[32];
    if (v <= 0) std::snprintf(buf, sizeof buf, "Auto \xC2\xB7 %d%%", int(std::round(app.dpiScale * 100)));
    else std::snprintf(buf, sizeof buf, "%d%%", int(std::round(v * 100)));
    return buf;
}

uint64_t physicalMemory() {
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);
    return GlobalMemoryStatusEx(&m) ? m.ullTotalPhys : 0;
}

void leftColumn(ImDrawList* dl, float x, float y, float w) {
    const Theme& t = T();
    const float controlX = x + S(200) + S(12);
    const float rowH = S(32);
    Row r;
    r.dl = dl;

    y = groupHeading(dl, x, y, w, "Appearance");
    {
        float cy = y + rowH * 0.5f;
        rowLabel(dl, x, cy, "Theme");
        static const char* themes[] = {"Dark", "Light", "System"};
        static const char* tips[] = {"Dark theme", "Light theme", "Follow the Windows app theme"};
        if (segmented(dl, "theme", controlX, cy, themes, 3, app.themeMode, false, tips)) {
            applyTheme();
            saveSettings();
        }
        y += rowH;

        cy = y + rowH * 0.5f;
        rowLabel(dl, x, cy, "Interface scale");
        r.x = controlX;
        r.cy = cy;
        ImRect dr;
        const std::string current = scaleLabel(app.interfaceScale);
        toggleMenuOnPress(r.dropdown("scale", current.c_str(), fontRegular, 12, 22, 8, 4,
                                     "Size of the interface; Auto follows the monitor", nullptr, &dr, false,
                                     "Auto \xC2\xB7 200%     "),
                          "scale menu");
        if (beginMenu("scale menu", ImVec2(dr.Min.x, dr.Max.y + S(4)), ImVec2(0, 0), dr.GetWidth())) {
            for (float v : {0.0f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f})
                if (menuItem({scaleLabel(v).c_str(), nullptr, app.interfaceScale == v, true, true}) && app.interfaceScale != v) {
                    app.interfaceScale = v;
                    applyInterfaceScale();
                    saveSettings();
                }
            endMenu();
        }
        note(dl, r.x + S(12), cy, "100\xE2\x80\x93" "200%");
        y += rowH;

        cy = y + rowH * 0.5f;
        rowLabel(dl, x, cy, "Viewport background");
        const UiColors colors = uiColors();
        const ImU32 vp = IM_COL32(int(colors.viewport[0] * 255 + 0.5f), int(colors.viewport[1] * 255 + 0.5f),
                                  int(colors.viewport[2] * 255 + 0.5f), 255);
        const ImVec2 sa(controlX, std::round(cy - S(11))), sb(sa.x + S(22), sa.y + S(22));
        ImGui::SetCursorScreenPos(sa);
        if (ImGui::InvisibleButton("viewport", ImVec2(S(22), S(22)))) {
            float rgb[3] = {colors.viewport[0], colors.viewport[1], colors.viewport[2]};
            if (pickColor(rgb)) {
                std::copy(rgb, rgb + 3, app.viewportColor);
                app.customViewport = true;
                saveSettings();
            }
        }
        requestTip(ImRect(sa, sb), "Colour around the image", nullptr, true);
        dl->AddRectFilled(sa, sb, vp);
        dl->AddRect(sa, sb, ImGui::IsItemHovered() ? t.tx3 : t.bd2, 0, 0, line());
        char hexText[16];
        std::snprintf(hexText, sizeof hexText, "#%02X%02X%02X", int(colors.viewport[0] * 255 + 0.5f),
                      int(colors.viewport[1] * 255 + 0.5f), int(colors.viewport[2] * 255 + 0.5f));
        drawText(dl, fontMono, 12, sb.x + S(12), cy, t.tx2, hexText);
        r.x = sb.x + S(12) + textSize(fontMono, 12, "#000000").x + S(12);
        r.cy = cy;
        if (r.button("viewport reset", Icon::Count, "Reset", kBordered | (app.customViewport ? 0 : kDisabled),
                     "Back to the theme's colour", nullptr, 16, 22)) {
            app.customViewport = false;
            saveSettings();
        }
        y += rowH;

        cy = y + rowH * 0.5f;
        rowLabel(dl, x, cy, "Checkerboard");
        static const char* sizes[] = {"Small", "Medium", "Large"};
        static const char* sizeTips[] = {"8 px cells", "16 px cells", "32 px cells"};
        if (segmented(dl, "checker", controlX, cy, sizes, 3, app.checkerSize, false, sizeTips)) saveSettings();
        y += rowH;
    }

    y = groupHeading(dl, x, y, w, "Browsing");
    {
        auto option = [&](const char* id, const char* label, bool& value, const char* tip) {
            if (checkbox(dl, id, x, y + rowH * 0.5f, label, value, tip)) saveSettings();
            y += rowH;
        };
        option("keep view", "Keep zoom and position when switching files", app.keepView,
               "Off: every file opens fitted to the window");
        option("keep exposure", "Keep exposure and range when switching files", app.keepExposure,
               "Off: exposure and range reset for every file");
        option("reload", "Reload automatically when the file changes on disk", app.autoReload,
               "Zoom, position and exposure stay as they are");
        const float cy = y + rowH * 0.5f;
        rowLabel(dl, x, cy, "Sort folder by");
        static const char* sorts[] = {"Name (natural)", "Name", "Date modified", "Size", "Type"};
        r.x = controlX;
        r.cy = cy;
        ImRect dr;
        toggleMenuOnPress(r.dropdown("sort", sorts[app.folderSort], fontRegular, 12, 22, 8, 4,
                                     "Order of the files that Previous and Next go through", nullptr, &dr, false,
                                     "Name (natural)     "),
                          "sort menu");
        if (beginMenu("sort menu", ImVec2(dr.Min.x, dr.Max.y + S(4)), ImVec2(0, 0), dr.GetWidth())) {
            for (int i = 0; i < 5; ++i)
                if (menuItem({sorts[i], nullptr, app.folderSort == i, true, true}) && app.folderSort != i) {
                    app.folderSort = i;
                    relistFolder();
                    saveSettings();
                }
            endMenu();
        }
        y += rowH;
    }

    y = groupHeading(dl, x, y, w, "Application");
    {
        float cy = y + rowH * 0.5f;
        if (checkbox(dl, "single", x, cy, "Single instance \xE2\x80\x94 open new files in the existing window",
                     app.singleInstance, "Takes effect the next time a file is opened from Explorer"))
            saveSettings();
        y += rowH;

        cy = y + rowH * 0.5f;
        bool portable = portableMode();
        static bool portableFailed = false;
        if (checkbox(dl, "portable", x, cy, "Portable mode", portable, nullptr)) portableFailed = !setPortableMode(portable);
        note(dl, x + S(14) + S(8) + textSize(fontRegular, 12, "Portable mode").x + S(12), cy,
             portableFailed ? "the exe's folder is not writable" : "settings.ini next to the .exe");
        y += rowH;

        cy = y + rowH * 0.5f;
        rowLabel(dl, x, cy, "Image cache");
        const uint64_t ram = physicalMemory();
        const int maxMB = int(std::clamp<uint64_t>(ram / 2 / (1024 * 1024), 512, 32768) / 256 * 256);
        float tpos = float(std::min(app.cacheMB, maxMB)) / float(maxMB);
        const float sw = S(150);
        if (slider(dl, "cache", controlX, cy, 150, tpos, 0.0f, nullptr)) {
            const int mb = int(std::round(tpos * float(maxMB) / 256.0f)) * 256;
            if (mb != app.cacheMB) {
                app.cacheMB = mb;
                trimImageCache();
                saveSettings();
            }
        }
        char buf[64];
        std::snprintf(buf, sizeof buf, app.cacheMB ? "%d MB" : "Off", app.cacheMB);
        drawText(dl, fontMono, 12, controlX + sw + S(12), cy, t.tx, buf);
        std::string tip = "Decoded images kept in memory, so going back and forth is instant";
        if (ram) {
            std::snprintf(buf, sizeof buf, " (this PC has %.0f GB)", double(ram) / (1024.0 * 1024.0 * 1024.0));
            tip += buf;
        }
        requestTip(ImRect(ImVec2(controlX, cy - S(10)), ImVec2(controlX + sw, cy + S(10))), tip.c_str(), nullptr, true);
        y += rowH;

        cy = y + rowH * 0.5f;
        rowLabel(dl, x, cy, "Prefetch neighbours");
        static const char* prefetch[] = {"Off", "\xC2\xB1" "1", "\xC2\xB1" "2"};
        static const char* prefetchTips[] = {"Decode only the file shown", "Decode the next and previous file ahead",
                                             "Decode two files ahead in each direction"};
        if (segmented(dl, "prefetch", controlX, cy, prefetch, 3, app.prefetch, false, prefetchTips, app.cacheMB == 0)) {
            trimImageCache();
            saveSettings();
        }
        y += rowH;

        cy = y + rowH * 0.5f;
        rowLabel(dl, x, cy, "File associations");
        r.x = controlX;
        r.cy = cy;
        if (r.button("assoc", Icon::Count, "Open Windows Default Apps\xE2\x80\xA6", kBordered,
                     "Adds Hyper Texture Viewer to Open with for the image types it reads (for you only), then opens "
                     "Default Apps: Windows lets only you choose which app opens each type",
                     nullptr, 16, 24)) {
            registerFileTypes();
            // Windows 11 opens the app's own page; older versions ignore the parameter and show the list.
            ShellExecuteW(app.hwnd, L"open", L"ms-settings:defaultapps?registeredAppUser=Hyper%20Texture%20Viewer",
                          nullptr, nullptr, SW_SHOWNORMAL);
        }
        y += rowH;
    }
}

bool keyMatches(Action a, const std::string& filter) {
    if (filter.empty()) return true;
    auto lower = [](std::string v) {
        for (char& c : v) c = char(std::tolower(static_cast<unsigned char>(c)));
        return v;
    };
    const std::string f = lower(filter);
    if (lower(actionInfo(a).label).find(f) != std::string::npos) return true;
    for (int s = 0; s < 2; ++s)
        if (lower(keyName(binding(a, s))).find(f) != std::string::npos) return true;
    return false;
}

void keysColumn(ImDrawList* dl, float x, float y, float w, float bottom) {
    const Theme& t = T();
    y = groupHeading(dl, x, y, w, "Keys");
    static char filter[64] = "";

    // Filter field and Reset all.
    Row r;
    r.dl = dl;
    r.cy = y + S(12);
    r.measure = true;
    r.button("reset keys", Icon::Count, "Reset all", kBordered, nullptr, nullptr, 16, 24);
    const float resetW = r.x;
    {
        const float fw = w - resetW - S(8), fh = S(24);
        const ImVec2 a(x, y);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(8) + S(14) + S(6), std::round((fh - S(12) * emRegular) * 0.5f)));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(3));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, line());
        ImGui::PushStyleColor(ImGuiCol_FrameBg, t.bar2);
        ImGui::PushStyleColor(ImGuiCol_Border, t.bd2);
        ImGui::PushStyleColor(ImGuiCol_Text, t.tx);
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, t.tx3);
        ImGui::PushFont(fontRegular, 12 * emRegular);
        ImGui::SetNextItemWidth(fw);
        ImGui::InputTextWithHint("##keyfilter", "Filter actions or keys", filter, sizeof filter);
        ImGui::PopFont();
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(3);
        drawIcon(dl, Icon::Search, ImVec2(a.x + S(8), std::round(r.cy - S(7))), S(14), t.tx3);
    }
    r.measure = false;
    r.x = x + w - resetW;
    if (r.button("reset keys", Icon::Count, "Reset all", kBordered, "Every key back to its default", nullptr, 16, 24)) {
        resetBindings();
        stopCapture();
        saveSettings();
    }
    y += S(24) + S(10);

    // Table header.
    const float colP = x + w - S(8) - S(24) - S(120) - S(120), colS = colP + S(120), colX = colS + S(120);
    {
        const float cy = y + S(12);
        drawText(dl, fontSemibold, 11, x + S(8), cy, t.tx3, "Action");
        drawText(dl, fontSemibold, 11, colP, cy, t.tx3, "Primary");
        drawText(dl, fontSemibold, 11, colS, cy, t.tx3, "Secondary");
        y += S(24);
        dl->AddRectFilled(ImVec2(x, y - line()), ImVec2(x + w, y), t.bd);
    }

    // Rows, scrolling inside the rest of the column.
    const float noteH = keyNote.empty() ? 0.0f : S(30);
    const float listH = std::max(S(60), bottom - y - noteH);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, 0);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, S(8));
    ImGui::BeginChild("keys", ImVec2(w, listH), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ImDrawList* cl = ImGui::GetWindowDrawList();
    const std::string f = filter;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    float ry = origin.y;
    bool any = false;
    for (int i = 0; i < int(Action::Count); ++i) {
        const Action a = Action(i);
        if (!keyMatches(a, f)) continue;
        any = true;
        const float rh = S(28), cy = ry + rh * 0.5f;
        const bool selected = capturing == a;
        if (selected) cl->AddRectFilled(ImVec2(x, ry), ImVec2(x + w, ry + rh), t.acs);
        cl->AddRectFilled(ImVec2(x, ry + rh - line()), ImVec2(x + w, ry + rh), t.bd);
        drawText(cl, fontRegular, 12, x + S(8), cy, t.tx, ellipsize(fontRegular, 12, actionInfo(a).label, colP - x - S(16)).c_str());
        ImGui::PushID(i);
        for (int slot = 0; slot < 2; ++slot) {
            const float cx = slot ? colS : colP;
            const bool waiting = selected && capturingSlot == slot;
            const std::string name = waiting ? std::string("Press a key\xE2\x80\xA6") : keyName(binding(a, slot));
            ImGui::SetCursorScreenPos(ImVec2(cx - S(2), ry + S(2)));
            ImGui::PushID(slot);
            const bool clicked = ImGui::InvisibleButton("key", ImVec2(S(116), rh - S(4)));
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            if (clicked) {
                if (waiting) stopCapture();
                else {
                    stopCapture();
                    capturing = a;
                    capturingSlot = slot;
                }
            }
            if (name.empty()) {
                drawText(cl, fontRegular, 12, cx, cy, hovered ? t.tx2 : t.tx3, "\xE2\x80\x94");
            } else {
                const float kw = kbdChip(cl, 0, 0, name.c_str(), false, true);
                kbdChip(cl, cx, cy, name.c_str(), true, true);
                if (waiting) {
                    const ImVec2 ka(cx, std::round(cy - S(9))), kb(cx + kw, ka.y + S(18));
                    cl->AddRect(ka, kb, t.ac, S(3), 0, line());
                } else if (hovered) {
                    const ImVec2 ka(cx, std::round(cy - S(9))), kb(cx + kw, ka.y + S(18));
                    cl->AddRect(ka, kb, t.tx3, S(3), 0, line());
                }
            }
            if (hovered && !waiting) requestTip(ImRect(ImVec2(cx, ry), ImVec2(cx + S(116), ry + rh)), "Click and press a new key", nullptr, true);
        }
        if (selected) {
            if (smallButton(cl, "clear", Icon::Dismiss, ImVec2(colX + S(2), std::round(cy - S(10))), 20, 14,
                            "Remove this key", nullptr)) {
                setBinding(a, capturingSlot, {});
                stopCapture();
                saveSettings();
            }
        }
        ImGui::PopID();
        ry += rh;
    }
    if (!any) drawText(cl, fontRegular, 12, x + S(8), ry + S(14), t.tx3, "No action matches the filter");
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(w - S(10), std::max(ry - origin.y, 1.0f)));
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    if (!keyNote.empty()) {
        const float cy = y + listH + S(16);
        drawIcon(dl, Icon::Warning, ImVec2(x, std::round(cy - S(7))), S(14), t.warn);
        drawText(dl, fontRegular, 11, x + S(14) + S(8), cy, t.warn, ellipsize(fontRegular, 11, keyNote, w - S(22)).c_str());
    }
}

}  // namespace

bool settingsKeyDown(KeyBinding k) {
    if (capturing == Action::Count || !app.showSettings) {
        capturing = Action::Count;
        return false;
    }
    if (k.vk == VK_ESCAPE && k.mods == 0) {
        stopCapture();
        return true;
    }
    if (k.mods == kModCtrl && k.vk >= '1' && k.vk <= '9') {
        keyNote = "Ctrl+1\xE2\x80\xA6" "9 open recent files and cannot be reassigned";
        return true;
    }
    if (binding(capturing, capturingSlot) == k) {
        stopCapture();
        return true;
    }
    Action other;
    int otherSlot;
    if (findBinding(k, other, otherSlot)) {
        if (conflictKey != k) {
            conflictKey = k;
            const std::string name = keyName(k);
            keyNote = name + " is already used by \xE2\x80\x9C" + actionInfo(other).label + "\xE2\x80\x9D \xE2\x80\x94 press " +
                      name + " again to swap the two bindings, Esc to cancel";
            return true;
        }
        setBinding(other, otherSlot, binding(capturing, capturingSlot));  // the other action gets our old key
    }
    setBinding(capturing, capturingSlot, k);
    stopCapture();
    saveSettings();
    return true;
}

void settingsPanel() {
    if (!app.showSettings) {
        if (capturing != Action::Count) stopCapture();
        return;
    }
    const Theme& t = T();
    const float width = std::min(S(1160), app.vpW - S(32)), height = std::min(S(820), app.vpH - S(32));
    const ImVec2 def(std::round(app.vpX + (app.vpW - width) * 0.5f), std::round(app.vpY + (app.vpH - height) * 0.5f));
    if (!beginPanel("##settings", def, width, height)) {
        ImGui::End();
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetWindowPos();
    const float headerH = S(30);
    panelHeader(dl, p, width, headerH, "Settings", 13, 12);
    const float hcy = p.y + (headerH - line()) * 0.5f;
    const char* hint = "Changes apply immediately";
    drawText(dl, fontRegular, 11, p.x + width - S(4) - S(20) - S(8) - textSize(fontRegular, 11, hint).x, hcy, t.tx3, hint);
    if (smallButton(dl, "close", Icon::Dismiss, ImVec2(p.x + width - S(4) - S(20), std::round(hcy - S(10))), 20, 14, "Close",
                    keyText(Action::Settings)))
        app.showSettings = false;

    // Two columns: options on the left, keys on the right.
    const float leftW = std::min(S(500), width * 0.45f);
    const float top = p.y + headerH;
    dl->AddRectFilled(ImVec2(p.x + leftW, top), ImVec2(p.x + leftW + line(), p.y + height), t.bd);
    leftColumn(dl, p.x + S(22), top + S(4), leftW - 2 * S(22));
    keysColumn(dl, p.x + leftW + line() + S(22), top, width - leftW - line() - 2 * S(22), p.y + height - S(20));
    ImGui::End();
}

}  // namespace htv::ui
