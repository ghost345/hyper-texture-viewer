#include "keys.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>

namespace htv {
namespace {

constexpr KeyBinding key(int vk, int mods = 0) { return KeyBinding{uint16_t(vk), uint8_t(mods)}; }
constexpr int C = kModCtrl, S = kModShift, A = kModAlt;

const ActionInfo kActions[int(Action::Count)] = {
    {"open", "Open", {key('O', C)}},
    {"prev", "Previous file", {key(VK_LEFT), key(VK_PRIOR)}},
    {"next", "Next file", {key(VK_RIGHT), key(VK_NEXT)}},
    {"first", "First file in the folder", {key(VK_HOME)}},
    {"last", "Last file in the folder", {key(VK_END)}},
    {"paste", "Paste image from the clipboard", {key('V', C)}},
    {"copy_path", "Copy file path", {key('C', C | A)}},
    {"reload", "Reload", {key(VK_F5)}},

    {"fit", "Fit", {key('F')}},
    {"actual_size", "Actual size", {key('1'), key(VK_DIVIDE)}},
    {"zoom_200", "Zoom 200%", {key('2')}},
    {"zoom_400", "Zoom 400%", {key('4')}},
    {"zoom_800", "Zoom 800%", {key('8')}},
    {"zoom_in", "Zoom in", {key(VK_ADD)}},
    {"zoom_out", "Zoom out", {key(VK_SUBTRACT)}},
    {"rotate_left", "Rotate left", {key('L', C)}},
    {"rotate_right", "Rotate right", {key('R', C)}},
    {"flip_h", "Flip horizontal", {key('H')}},
    {"flip_v", "Flip vertical", {key('V')}},
    {"hide_panels", "Hide panels", {key(VK_TAB)}},

    {"all_channels", "Show all channels", {key('C')}},
    {"red", "Show red channel", {key('R')}},
    {"green", "Show green channel", {key('G')}},
    {"blue", "Show blue channel", {key('B')}},
    {"alpha", "Show alpha channel", {key('A')}},
    {"luminance", "Show luminance", {key('Y')}},
    {"srgb_linear", "sRGB / Linear", {key('L', S)}},
    {"normal_mode", "Normal map mode", {key('N')}},
    {"mip_down", "Mip level down", {key(VK_OEM_4)}},
    {"mip_up", "Mip level up", {key(VK_OEM_6)}},
    {"alpha_background", "Alpha background", {key('A', S)}},
    {"tiling", "Tiling", {key('T')}},

    {"exposure_down", "Exposure \xE2\x88\x92" "0.5 EV", {key(VK_OEM_MINUS)}},
    {"exposure_up", "Exposure +0.5 EV", {key(VK_OEM_PLUS)}},
    {"reset_exposure", "Reset exposure", {key('0'), key(VK_NUMPAD0)}},
    {"auto_range", "Auto range", {key('A', C)}},
    {"clipping", "Show clipping", {key('Z')}},

    {"grid", "Pixel grid", {key('G', C)}},
    {"pixel_outline", "Pixel outline", {key('P')}},
    {"lock_pixel", "Lock pixel under the cursor", {}},
    {"copy_value", "Copy pixel value", {key('C', C)}},
    {"copy_all_formats", "Copy pixel value in all formats", {key('C', C | S)}},
    {"color_match", "Color match", {key('M')}},
    {"match_prev_region", "Color match: previous region", {key(VK_OEM_COMMA)}},
    {"match_next_region", "Color match: next region", {key(VK_OEM_PERIOD)}},
    {"histogram", "Histogram", {key('H', C)}},
    {"info", "Info", {key('I')}},
    {"layers", "Layers", {key('L')}},
    {"compare", "Compare", {key('K', C)}},

    {"always_on_top", "Always on top", {key('T', C | S)}},
    {"click_through", "Transparent to mouse", {key('T', C)}},
    {"settings", "Settings", {key(VK_OEM_COMMA, C)}},
    {"help", "Keyboard shortcuts", {key(VK_F1)}},
    {"quit", "Close", {key('W', C)}},
};

KeyBinding bindings[int(Action::Count)][2];
std::string texts[int(Action::Count)];
bool initialised = false;

void refreshTexts() {
    for (int i = 0; i < int(Action::Count); ++i) texts[i] = keyName(bindings[i][0]);
}

void init() {
    if (initialised) return;
    initialised = true;
    for (int i = 0; i < int(Action::Count); ++i)
        for (int s = 0; s < 2; ++s) bindings[i][s] = kActions[i].defaults[s];
    refreshTexts();
}

std::string bindingText(KeyBinding k) {
    if (k.empty()) return "";
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d:%d", int(k.mods), int(k.vk));
    return buf;
}

KeyBinding parseBinding(const std::string& s) {
    const size_t colon = s.find(':');
    if (colon == std::string::npos) return {};
    const int mods = std::atoi(s.substr(0, colon).c_str()), vk = std::atoi(s.substr(colon + 1).c_str());
    if (vk <= 0 || vk > 255 || mods < 0 || mods > 7) return {};
    return key(vk, mods);
}

}  // namespace

const ActionInfo& actionInfo(Action a) { return kActions[int(a)]; }

KeyBinding binding(Action a, int slot) {
    init();
    return bindings[int(a)][slot & 1];
}

void setBinding(Action a, int slot, KeyBinding k) {
    init();
    bindings[int(a)][slot & 1] = k;
    // A primary slot left empty takes the secondary key.
    if (bindings[int(a)][0].empty() && !bindings[int(a)][1].empty()) std::swap(bindings[int(a)][0], bindings[int(a)][1]);
    refreshTexts();
}

void resetBindings() {
    initialised = false;
    init();
}

bool findBinding(KeyBinding k, Action& a, int& slot) {
    init();
    if (k.empty()) return false;
    for (int i = 0; i < int(Action::Count); ++i)
        for (int s = 0; s < 2; ++s)
            if (bindings[i][s] == k) {
                a = Action(i);
                slot = s;
                return true;
            }
    return false;
}

Action actionFor(KeyBinding k) {
    Action a;
    int slot;
    if (findBinding(k, a, slot)) return a;
    if ((k.mods & kModShift) && findBinding(key(k.vk, k.mods & ~kModShift), a, slot)) return a;
    return Action::Count;
}

std::string keyName(KeyBinding k) {
    if (k.empty()) return "";
    std::string s;
    if (k.mods & kModCtrl) s += "Ctrl+";
    if (k.mods & kModShift) s += "Shift+";
    if (k.mods & kModAlt) s += "Alt+";
    const int vk = k.vk;
    char buf[16];
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        s += char(vk);
    } else if (vk >= VK_F1 && vk <= VK_F24) {
        std::snprintf(buf, sizeof buf, "F%d", vk - VK_F1 + 1);
        s += buf;
    } else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        std::snprintf(buf, sizeof buf, "Num %d", vk - VK_NUMPAD0);
        s += buf;
    } else {
        switch (vk) {
            case VK_LEFT: s += "\xE2\x86\x90"; break;
            case VK_RIGHT: s += "\xE2\x86\x92"; break;
            case VK_UP: s += "\xE2\x86\x91"; break;
            case VK_DOWN: s += "\xE2\x86\x93"; break;
            case VK_PRIOR: s += "PgUp"; break;
            case VK_NEXT: s += "PgDn"; break;
            case VK_HOME: s += "Home"; break;
            case VK_END: s += "End"; break;
            case VK_INSERT: s += "Ins"; break;
            case VK_DELETE: s += "Del"; break;
            case VK_BACK: s += "Backspace"; break;
            case VK_TAB: s += "Tab"; break;
            case VK_RETURN: s += "Enter"; break;
            case VK_SPACE: s += "Space"; break;
            case VK_PAUSE: s += "Pause"; break;
            case VK_ADD: s += "Num +"; break;
            case VK_SUBTRACT: s += "Num \xE2\x88\x92"; break;
            case VK_MULTIPLY: s += "Num *"; break;
            case VK_DIVIDE: s += "Num /"; break;
            case VK_DECIMAL: s += "Num ."; break;
            case VK_OEM_MINUS: s += "-"; break;
            case VK_OEM_PLUS: s += "="; break;
            case VK_OEM_4: s += "["; break;
            case VK_OEM_6: s += "]"; break;
            case VK_OEM_1: s += ";"; break;
            case VK_OEM_7: s += "'"; break;
            case VK_OEM_COMMA: s += ","; break;
            case VK_OEM_PERIOD: s += "."; break;
            case VK_OEM_2: s += "/"; break;
            case VK_OEM_3: s += "`"; break;
            case VK_OEM_5: s += "\\"; break;
            default:
                std::snprintf(buf, sizeof buf, "Key %d", vk);
                s += buf;
        }
    }
    return s;
}

const char* keyText(Action a) {
    init();
    return texts[int(a)].empty() ? nullptr : texts[int(a)].c_str();
}

std::string bindingsToSettings() {
    init();
    std::string out;
    for (int i = 0; i < int(Action::Count); ++i) {
        if (bindings[i][0] == kActions[i].defaults[0] && bindings[i][1] == kActions[i].defaults[1]) continue;
        out += std::string("keys.") + kActions[i].id + "=" + bindingText(bindings[i][0]) + "," + bindingText(bindings[i][1]) +
               "\n";
    }
    return out;
}

bool bindingFromSettings(const std::string& k, const std::string& value) {
    if (k.compare(0, 5, "keys.") != 0) return false;
    init();
    const std::string id = k.substr(5);
    for (int i = 0; i < int(Action::Count); ++i) {
        if (id != kActions[i].id) continue;
        const size_t comma = value.find(',');
        bindings[i][0] = parseBinding(value.substr(0, comma));
        bindings[i][1] = comma == std::string::npos ? KeyBinding{} : parseBinding(value.substr(comma + 1));
        refreshTexts();
        break;
    }
    return true;
}

}  // namespace htv
