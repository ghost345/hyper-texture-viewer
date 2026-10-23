# Hyper Texture Viewer

Fast image viewer for texture artists on Windows. Early prototype.

## What works

- Formats: EXR (half and float, scanline, ZIP/PIZ/RLE), DDS (BC1-BC7 including BC6H, uncompressed formats, mip chains), Radiance HDR, PNG (8 and 16 bit), JPEG, TGA, BMP, PSD (flattened image), GIF (first frame).
- Compressed DDS goes to the GPU as is, so BC textures look exactly as in a game.
- Pixels stay sharp when zoomed in (no filtering at 100% and above). Below 100% the mip chain averages the source pixels, so detail does not flicker or alias.
- Exposure for any image, values above 1.0 in HDR images, NaN/Inf shown in magenta.
- Single channels and alpha, alpha shown over a checkerboard.
- Pixel coordinates and values under the cursor (in the title bar for now).
- One exe, static runtime, no installer. Windows 10/11 x64; Direct3D 11 on any GPU, with a software fallback (WARP).

## Controls

| Input | Action |
| --- | --- |
| Mouse wheel | Zoom around the cursor (stops at 100%) |
| Ctrl + wheel, `+` / `-` | Exposure in 1/4 or 1/2 stops |
| `0` | Reset exposure |
| Left drag | Pan |
| Double click, `F` | Fit to window |
| `1` `2` `4` `8` | Zoom 100%, 200%, 400%, 800% |
| `R` `G` `B` `A` | Show one channel (press again to go back) |
| `X` | RGB without alpha |
| `C` | All channels |
| Left / Right, PgUp / PgDn, Space / Backspace | Previous / next image in the folder |
| Esc | Quit |

Drop a file on the window or pass it on the command line.

## Building

Visual Studio 2022 and CMake 3.20 or newer:

```
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
```

MinGW-w64 works too and produces a static exe. GitHub Actions builds every push, runs the tests and measures start-up time on sample images (`--bench <file>` writes timings and exits after the first frame).

## Third-party code

| Library | Use | License |
| --- | --- | --- |
| [stb_image, stb_image_write](https://github.com/nothings/stb) | PNG, JPEG, TGA, BMP, PSD, GIF, HDR; zlib for EXR | Public domain / MIT |
| [tinyexr 1.0.9](https://github.com/syoyo/tinyexr) | EXR in the prototype; to be replaced by OpenEXR for DWAA/DWAB, layers and multipart | BSD-3-Clause |

Licenses are in the header files under `third_party/`.
