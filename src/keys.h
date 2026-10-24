// Keyboard shortcuts: the actions keys can trigger, their bindings and how keys are named.
// Settings > Keys reassigns them; Esc and Ctrl+1..9 (recent files) stay fixed.
#pragma once

#include <cstdint>
#include <string>

namespace htv {

enum class Action : int {
    // Files
    Open, PrevFile, NextFile, FirstFile, LastFile, Paste, CopyPath, Reload,
    // View
    Fit, ActualSize, Zoom200, Zoom400, Zoom800, ZoomIn, ZoomOut, RotateLeft, RotateRight, FlipH, FlipV, HidePanels,
    // Channels and modes
    AllChannels, Red, Green, Blue, Alpha, Luminance, SrgbLinear, NormalMode, MipDown, MipUp, AlphaBackground, Tiling,
    // HDR
    ExposureDown, ExposureUp, ResetExposure, AutoRange, Clipping,
    // Analysis
    Grid, PixelOutline, LockPixel, CopyValue, CopyAllFormats, ColorMatch, Histogram, Info, Layers, Compare,
    // Window
    AlwaysOnTop, ClickThrough, Settings, Help, Quit,
    Count
};

enum KeyMods : uint8_t { kModCtrl = 1, kModShift = 2, kModAlt = 4 };

struct KeyBinding {
    uint16_t vk = 0;  // virtual-key code, 0 for none
    uint8_t mods = 0;
    bool empty() const { return vk == 0; }
    bool operator==(const KeyBinding& o) const { return vk == o.vk && mods == o.mods; }
    bool operator!=(const KeyBinding& o) const { return !(*this == o); }
};

struct ActionInfo {
    const char* id;     // stable name in settings.ini
    const char* label;  // as Settings lists it
    KeyBinding defaults[2];
};

const ActionInfo& actionInfo(Action a);
// The primary (slot 0) and secondary (slot 1) key of an action.
KeyBinding binding(Action a, int slot);
void setBinding(Action a, int slot, KeyBinding k);
void resetBindings();
// The action a key triggers, or Action::Count. With Shift held and nothing bound to it, the key without
// Shift counts, so Shift+= still works where = is bound.
Action actionFor(KeyBinding k);
// The action and slot a key is bound to, for conflicts in Settings; false when it is free.
bool findBinding(KeyBinding k, Action& a, int& slot);

// "Ctrl+Shift+T", "PgDn", "Num /". Empty for no key.
std::string keyName(KeyBinding k);
// The primary key's name, for tooltips and the Help screen; empty when the action has no key.
const char* keyText(Action a);

// settings.ini lines: "keys.<id>=<mods>:<vk>,<mods>:<vk>", only for actions that differ from the defaults.
std::string bindingsToSettings();
bool bindingFromSettings(const std::string& key, const std::string& value);  // false: not a key line

}  // namespace htv
