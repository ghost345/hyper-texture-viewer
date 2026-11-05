// The main window's interface, drawn with Dear ImGui after the image: top bar, bottom bar, menus and panels.
#pragma once

namespace htv {

struct UiColors {
    float viewport[3];
    float checker1[3];
    float checker2[3];
};

void uiInit();                    // fonts and style; after ImGui's platform and renderer backends
void uiSetScale(float dpiScale);  // 1.0 at 96 dpi
void uiFrame();                   // between ImGui::NewFrame and ImGui::Render
UiColors uiColors();              // theme colours the image shader needs

float uiTopBarHeight();
float uiBottomBarHeight();
float uiMinWidth();  // the narrowest the window can be with nothing in the bars overlapping; 0 if unknown yet
bool uiIsCaption(float x, float y);  // empty top bar: drags the window like a title bar
bool uiKeyDown(int virtualKey);      // keyboard shortcuts; true when handled
// Files were dropped at (x, y), in the same pixels as the mouse. `name` is the first file's (UTF-8); `single` is
// false for a folder or several files, which always open. Ends in finishDrop, at once or after asking.
void uiFilesDropped(float x, float y, const char* name, bool single);

}  // namespace htv
