"""Hyper Texture Viewer icon, rebuilt from Oleg's recipe, rendered natively at every size.

Recipe
  1. Background: UV gradient. R = V (1 at the top, 0 at the bottom), G = U (0 left, 1 right), B = 0.
  2. Iris: 6x6 checker (black / white) blended over it in Overlay mode at 50 % opacity.
     Top-left cell is black.
  3. Pupil: black square, inset half a checker cell (from 1.5 to 4.5 cells).
  4. Depth: vertical gradient on the pupil, transparent at the top -> BLUE at the bottom,
     40 % opacity, midpoint pushed towards the blue end (alpha = t ** GRAD_POWER).
  5. Highlight: white square on checker cell (1, 1), straddling the pupil's corner.

All maths is done in 8-bit sRGB values like Photoshop / Illustrator RGB documents.
Every edge lands on a whole pixel, so nothing is resampled.
"""
import os
import numpy as np
from PIL import Image

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
os.makedirs(OUT, exist_ok=True)

BLUE = (0x1F, 0x5F, 0xFF)   # pupil depth colour (was light blue in the draft)
GRAD_OPACITY = 0.40
GRAD_POWER = 3.5            # 50 % of the gradient is reached at t = 0.82
CHECKER_OPACITY = 0.50

# size -> (cells, cell px, padding px). Cell px is even where possible so the
# half-cell pupil inset is a whole pixel. 16 and 20 px use a 4x4 grid: 6x6 is noise there.
LAYOUT = {
    16: (4, 4, 0), 20: (4, 5, 0), 24: (6, 4, 0), 32: (6, 5, 1), 40: (6, 6, 2),
    48: (6, 8, 0), 64: (6, 10, 2), 96: (6, 16, 0), 128: (6, 20, 4), 256: (6, 42, 2),
    512: (6, 84, 4), 1024: (6, 170, 2),
}


def overlay(base, blend):
    return np.where(base < 0.5, 2 * base * blend, 1 - 2 * (1 - base) * (1 - blend))


def render(size):
    n, c, pad = LAYOUT[size]
    side = n * c
    img = np.zeros((size, size, 4), np.float64)
    ys, xs = np.mgrid[0:side, 0:side] + 0.5           # pixel centres
    u = xs / side
    v = 1 - ys / side
    base = np.stack([v, u, np.zeros_like(u)], -1)      # 1. UV gradient
    ci, cj = (xs // c).astype(int), (ys // c).astype(int)
    blend = ((ci + cj) % 2).astype(np.float64)[..., None]   # 0 = black cell, 1 = white cell
    col = base + (overlay(base, blend) - base) * CHECKER_OPACITY   # 2. iris

    # 3. pupil, inset half a cell; at 4x4 the pupil is cells 1..3
    if n == 6:
        p0 = round(1.5 * c)          # odd cells (32 px): rounded symmetrically, pupil stays centred
        p1 = side - p0
    else:
        p0, p1 = c, 3 * c
    t = (np.arange(p0, p1) + 0.5 - p0) / (p1 - p0)
    a = (t ** GRAD_POWER * GRAD_OPACITY)[:, None, None]
    pupil = np.zeros((p1 - p0, p1 - p0, 3)) * (1 - a) + np.array(BLUE) / 255 * a   # 4. depth
    col[p0:p1, p0:p1] = pupil

    # 5. highlight on cell (1, 1) — straddles the pupil corner
    if n == 6:
        h0, h1 = c, 2 * c
    else:
        h0, h1 = c - 1, 2 * c - 1
    col[h0:h1, h0:h1] = 1.0

    img[pad:pad + side, pad:pad + side, :3] = col
    img[pad:pad + side, pad:pad + side, 3] = 1
    return Image.fromarray(np.round(np.clip(img, 0, 1) * 255).astype(np.uint8), "RGBA")


if __name__ == "__main__":
    ico_sizes = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
    imgs = {s: render(s) for s in LAYOUT}
    for s, im in imgs.items():
        im.save(os.path.join(OUT, f"htv_{s}.png"))
    imgs[256].save(os.path.join(OUT, "htv.ico"), sizes=[(s, s) for s in ico_sizes],
                   append_images=[imgs[s] for s in ico_sizes if s != 256])
    print("written", sorted(LAYOUT))
