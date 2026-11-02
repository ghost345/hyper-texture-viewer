"""'No preview' file-type icons for every format Hyper Texture Viewer opens.

A light document page with a folded corner, the app tile on it, and a format label.
The label colour groups formats by family; at 16-24 px the label is a plain colour bar
(text is unreadable there), from 32 px it carries the extension.
Uses make_icon.render() for the app tile, so the tile always matches htv.ico.
"""
import os
from PIL import Image, ImageDraw, ImageFont
import make_icon

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out", "filetypes"); os.makedirs(OUT, exist_ok=True)
FONT = "/usr/share/fonts/opentype/inter/Inter-Bold.otf"

FAMILY = {                      # label colours, white text >= 3:1 (bold, large)
    "hdr":     (0xB9, 0x6A, 0x1E),   # HDR / float
    "gpu":     (0x2F, 0x6F, 0xD0),   # GPU texture containers
    "layered": (0x7B, 0x4F, 0xC9),   # layers / pages
    "image":   (0x2E, 0x8A, 0x5C),   # ordinary images
}
FORMATS = {
    "exr": "hdr", "hdr": "hdr",
    "dds": "gpu", "ktx": "gpu", "ktx2": "gpu",
    "psd": "layered", "tif": "layered", "tiff": "layered",
    "tga": "image", "png": "image", "jpg": "image", "jpeg": "image", "bmp": "image",
    "gif": "image", "webp": "image", "avif": "image", "heic": "image",
}
LABEL = {"tif": "TIF", "tiff": "TIFF", "jpeg": "JPG"}
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
SS = 8

PAGE = (0xF4, 0xF4, 0xF4)
PAGE_EDGE = (0x9A, 0x9A, 0x9A)
FOLD = (0xD6, 0xD6, 0xD6)


def page_geometry(size):
    # page box in target pixels: portrait, centred
    w = round(size * (0.78 if size <= 24 else 0.72))
    h = size if size <= 24 else size - max(1, round(size / 32))
    x0 = (size - w) // 2
    y0 = (size - h) // 2
    fold = max(3, round(w * 0.26))
    return x0, y0, w, h, fold


def render(ext, size):
    fam = FORMATS[ext]
    col = FAMILY[fam]
    S = size * SS
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    x0, y0, w, h, fold = page_geometry(size)
    X0, Y0, X1, Y1, F = x0 * SS, y0 * SS, (x0 + w) * SS, (y0 + h) * SS, fold * SS
    r = max(1, round(size * 0.04)) * SS
    page = [(X0 + r, Y0), (X1 - F, Y0), (X1, Y0 + F), (X1, Y1 - r), (X1 - r, Y1), (X0 + r, Y1), (X0, Y1 - r), (X0, Y0 + r)]
    d.polygon(page, fill=PAGE + (255,), outline=PAGE_EDGE + (255,), width=SS)
    d.polygon([(X1 - F, Y0), (X1 - F, Y0 + F), (X1, Y0 + F)], fill=FOLD + (255,), outline=PAGE_EDGE + (255,), width=SS)

    small = size <= 24
    # format label
    if small:
        bh = max(2, round(size * 0.14))
        by = y0 + h - bh - max(1, round(size * 0.06))
        d.rectangle([X0 + SS, by * SS, X1 - SS, (by + bh) * SS - 1], fill=col + (255,))
    else:
        bh = round(h * 0.25)
        by = y0 + h - bh - round(h * 0.07)
        bx0, bx1 = x0 - round(size * 0.06), x0 + w - round(w * 0.12)   # label pokes out on the left
        d.rounded_rectangle([bx0 * SS, by * SS, bx1 * SS, (by + bh) * SS], radius=max(1, round(size * 0.025)) * SS,
                            fill=col + (255,))
        text = LABEL.get(ext, ext.upper())
        fs = bh * 0.68 * SS
        font = ImageFont.truetype(FONT, int(fs))
        maxw = (bx1 - bx0) * SS * 0.84
        while d.textlength(text, font=font) > maxw:
            fs *= 0.94
            font = ImageFont.truetype(FONT, int(fs))
        cx = (bx0 + bx1) / 2 * SS
        cy = (by + bh / 2) * SS
        d.text((cx, cy), text, font=font, fill=(255, 255, 255, 255), anchor="mm")

    # app tile, centred in the space above the label
    ts = round(w * (0.70 if small else 0.62))
    top = y0 + (fold if small else round(h * 0.16))
    bottom = by
    ty = top + max(0, (bottom - top - ts) // 2)
    tx = x0 + (w - ts) // 2
    tile = make_icon.render(ts, simple=ts <= 24, pad=0) if ts >= 8 else None
    out = img.resize((size, size), Image.BOX)
    if tile is not None:
        out.alpha_composite(tile, (tx, ty))
    return out


if __name__ == "__main__":
    sheet = []
    for ext in FORMATS:
        imgs = {s: render(ext, s) for s in SIZES}
        for s in (32, 48, 256):
            imgs[s].save(os.path.join(OUT, f"{ext}_{s}.png"))
        imgs[256].save(os.path.join(OUT, f"htv_{ext}.ico"), sizes=[(s, s) for s in SIZES],
                       append_images=[imgs[s] for s in SIZES if s != 256])
        sheet.append((ext, imgs))
    # overview sheet
    cols = [256, 96, 48, 32, 24, 16]
    W = sum(cols) + 20 * len(cols) + 80
    H = len(sheet) * 0 + 300
    rows = [sheet[i:i + 1] for i in range(len(sheet))]
    ov = Image.new("RGB", (1600, 700), (32, 32, 32))
    # grid of 256 previews (scaled to 128) + small sizes, two rows per format family
    x = y = 20
    for i, (ext, imgs) in enumerate(sheet):
        X = 20 + (i % 6) * 260
        Y = 20 + (i // 6) * 220
        big = imgs[256].resize((128, 128), Image.LANCZOS)
        ov.paste(big, (X, Y), big)
        xx = X
        for s in (48, 32, 24, 16):
            ov.paste(imgs[s], (xx, Y + 140), imgs[s]); xx += s + 10
        light = Image.new("RGB", (120, 40), (245, 245, 245)); ov.paste(light, (X + 130, Y + 10))
        ov.paste(imgs[32], (X + 136, Y + 14), imgs[32]); ov.paste(imgs[16], (X + 176, Y + 22), imgs[16])
    ov.save(os.path.join(HERE, "out", "filetypes_overview.png"))
    print("written", len(sheet), "formats")
