// Resource IDs shared by resources.rc and the code.
#pragma once

// The app icon (res/htv.ico): the exe's icon in Explorer, the window's on the taskbar and in Alt+Tab. The lowest
// ICON ID, so Explorer picks it for the exe.
#define IDI_APPICON 101

// "No preview" icons for the image types the app registers for (res/filetypes), after the app icon. A ProgID's
// DefaultIcon names them as "<exe>",-<ID>. JPEG files share the JPG icon.
#define IDI_FT_EXR 201
#define IDI_FT_HDR 202
#define IDI_FT_DDS 203
#define IDI_FT_KTX 204
#define IDI_FT_KTX2 205
#define IDI_FT_PSD 206
#define IDI_FT_TIF 207
#define IDI_FT_TIFF 208
#define IDI_FT_TGA 209
#define IDI_FT_PNG 210
#define IDI_FT_JPG 211
#define IDI_FT_BMP 212
#define IDI_FT_GIF 213
#define IDI_FT_WEBP 214
#define IDI_FT_AVIF 215
#define IDI_FT_HEIC 216
