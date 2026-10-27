"""Hyper Texture Viewer icon v3: UV-gradient checker tile + white 'eye / shutter' mark.
The eye contour is traced from Oleg's 521 px drawing (eye_contours.json, normalised 0..1).
Everything else is procedural, so each size is rendered natively (8x supersampled)."""
import os, json
import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out"); os.makedirs(OUT, exist_ok=True)
EYE = json.load(open(os.path.join(HERE, "eye_contours.json")))

# ---- look ------------------------------------------------------------------
BLUE_BOTTOM = 0.62      # B = V^1.8 * this: navy bottom-left, teal bottom-right
CHECKER_OPACITY = 0.30  # 6x6 checker, Overlay
SATURATION = 0.80       # 1.0 = as drawn; <1 calmer
BRIGHTNESS = 0.88       # multiplier on the background (the white eye stays 100 %)
CORNER = 0.155          # corner radius / side, as drawn
EYE_SCALE = 1.0         # eye size relative to the tile
SS = 8

# size -> padding px (transparent margin around the tile)
PAD = {16: 0, 20: 0, 24: 0, 32: 1, 40: 1, 48: 1, 64: 2, 96: 3, 128: 4, 256: 8, 512: 16, 1024: 32}


def overlay(b, l):
    return np.where(b < 0.5, 2 * b * l, 1 - 2 * (1 - b) * (1 - l))


def background(px, cells):
    ys, xs = np.mgrid[0:px, 0:px].astype(np.float32) + 0.5
    u, v = xs / px, ys / px                                  # v grows downwards
    base = np.stack([(1 - v) ** 1.6, u, v ** 1.8 * BLUE_BOTTOM], -1)   # curves keep the left edge dark instead of purple
    chk = ((np.floor(xs / (px / cells)) + np.floor(ys / (px / cells))) % 2)[..., None]
    col = base + (overlay(base, chk) - base) * CHECKER_OPACITY
    grey = col.mean(-1, keepdims=True)
    col = grey + (col - grey) * SATURATION
    return np.clip(col * BRIGHTNESS, 0, 1)


def render(size, simple=False, pad=None):
    SS = 8 if size <= 256 else (4 if size <= 512 else 2)
    pad = PAD[size] if pad is None else pad
    side = size - 2 * pad
    S = size * SS
    cells = 4 if size <= 20 else 6
    tile = background(side * SS, cells)
    canvas = np.zeros((S, S, 4), np.float32)
    o = pad * SS
    canvas[o:o + side * SS, o:o + side * SS, :3] = tile
    img = Image.fromarray((canvas * 255).astype(np.uint8), "RGBA")
    # rounded tile mask
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([o, o, o + side * SS - 1, o + side * SS - 1],
                                           radius=round(CORNER * side * SS), fill=255)
    # white eye
    eye = Image.new("L", (S, S), 0)
    d = ImageDraw.Draw(eye)
    c0 = 0.5
    for poly in EYE:
        pts = [(o + (c0 + (x - c0) * EYE_SCALE) * side * SS, o + (c0 + (y - c0) * EYE_SCALE) * side * SS) for x, y in poly]
        d.polygon(pts, fill=255)
    if simple:
        eye = simple_eye(S, o, side * SS)
    white = Image.new("RGBA", (S, S), (255, 255, 255, 255))
    img = Image.composite(white, img, eye)
    img.putalpha(mask)
    return img.resize((size, size), Image.BOX)


def simple_eye(S, o, side):
    """16-24 px: same lens outline, one solid iris hole instead of the two-blade shutter."""
    eye = Image.new("L", (S, S), 0)
    d = ImageDraw.Draw(eye)
    # outer lens = union of the traced pieces, filled solid
    for poly in EYE:
        d.polygon([(o + x * side, o + y * side) for x, y in poly], fill=255)
    lens = np.array(eye)
    import cv2
    k = max(3, int(side * 0.09)) | 1
    lens = cv2.morphologyEx(lens, cv2.MORPH_CLOSE, np.ones((k, k), np.uint8))
    eye = Image.fromarray(lens)
    d = ImageDraw.Draw(eye)
    r = side * 0.19
    cx = cy = o + side / 2
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=0)          # iris hole shows the tile
    return eye


if __name__ == "__main__":
    ico_sizes = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
    imgs = {s: render(s, simple=(s <= 24)) for s in PAD}
    for s, im in imgs.items():
        im.save(os.path.join(OUT, f"htv_{s}.png"))
    imgs[256].save(os.path.join(OUT, "htv.ico"), sizes=[(s, s) for s in ico_sizes],
                   append_images=[imgs[s] for s in ico_sizes if s != 256])
    print("written", sorted(PAD))
