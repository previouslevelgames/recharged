#!/usr/bin/env python3
"""Make the launcher's logo and header images in assets/launcher/ from their originals.

    launcher_art.py logo ORIGINAL.png      # logo.png: the Recharged logo, trimmed, 1280 pixels wide
    launcher_art.py header MSC_Scene.jpg   # header.png: Mario from the game's key art, 1920 x 1067

The header is the lower part of the key art (3500 x 4700): Mario's volley with
Boo and a Bob-omb, the art extended to the left (that side lies under the
launcher's logo) so that Mario stands to the right. The fire and the golden ball
turn the logo's electric blue (hot yellow to icy cyan, orange to sky blue, red
to deep blue) and the grass blue; Mario's face and reds and Boo's tongue keep
their colours, and the scene darkens towards night blue around Mario. Needs
Pillow and numpy. assets/launcher/README.md lists the originals.
"""

import sys
from pathlib import Path

import numpy as np
from PIL import Image

ASSETS = Path(__file__).resolve().parent.parent / "assets/launcher"
LOGO_WIDTH = 1280


def make_logo(source):
    image = Image.open(source).convert("RGBA")
    # A few nearly invisible specks lie further out than the logo.
    left, top, right, bottom = image.getchannel("A").point(lambda a: 255 if a > 8 else 0).getbbox()
    pad = 8
    image = image.crop((max(left - pad, 0), max(top - pad, 0), min(right + pad, image.width),
                        min(bottom + pad, image.height)))
    height = round(image.height * LOGO_WIDTH / image.width)
    # Premultiplied resampling: the transparent surroundings add no colour to the edges.
    scaled = image.convert("RGBa").resize((LOGO_WIDTH, height), Image.LANCZOS).convert("RGBA")
    visible = scaled.getchannel("A").point(lambda a: 255 if a else 0)
    Image.composite(scaled, Image.new("RGBA", scaled.size), visible).save(ASSETS / "logo.png", optimize=True)
    return LOGO_WIDTH, height


def smooth(edge0, edge1, x):
    t = np.clip((x - edge0) / (edge1 - edge0), 0, 1)
    return t * t * (3 - 2 * t)


def to_rgb(hue, saturation, value):
    h = (hue % 360) / 60
    c = value * saturation
    x = c * (1 - np.abs(h % 2 - 1))
    z = np.zeros_like(h)
    sector = np.floor(h).astype(int) % 6
    rgb = np.select([sector[..., None] == n for n in range(6)],
                    [np.stack(t, -1) for t in ((c, x, z), (x, c, z), (z, c, x), (z, x, c), (x, z, c), (c, z, x))])
    return rgb + (value - c)[..., None]


def make_header(source):
    image = Image.open(source).convert("RGB")
    if image.size != (3500, 4700):
        raise SystemExit(f"{source}: expected the 3500 x 4700 key art, found {image.width} x {image.height}")
    # Mario's face (2400, 2860) at 76 % across and 37 % down; black beyond the art's left edge.
    left, top, right, bottom = -243, 2145, 3235, 4077
    rgb = np.asarray(image.crop((left, top, right, bottom))).astype(np.float32) / 255
    height, width = rgb.shape[:2]
    ys, xs = np.mgrid[0:height, 0:width].astype(np.float32)
    xs += left  # places below are in the key art's own pixels
    ys += top
    high, low = rgb.max(2), rgb.min(2)
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    c = np.maximum(high - low, 1e-6)
    hue = np.where(high == r, ((g - b) / c) % 6, np.where(high == g, (b - r) / c + 2, (r - g) / c + 4)) * 60
    hue = np.where(hue > 300, hue - 360, hue)  # reds around 0 as small negatives
    saturation = np.where(high > 0, (high - low) / np.maximum(high, 1e-6), 0)
    value = high

    def ellipse(cx, cy, rx, ry, feather):
        distance = np.sqrt(((xs - cx) / rx) ** 2 + ((ys - cy) / ry) ** 2)
        return 1 - smooth(1 - feather, 1 + feather, distance)

    colourful = smooth(0.12, 0.30, saturation) * smooth(0.06, 0.18, value)
    yellow = smooth(10, 20, hue) * (1 - smooth(70, 85, hue))  # orange, gold and yellow
    red = (1 - smooth(8, 18, hue)) * smooth(-40, -25, hue)
    green = smooth(75, 90, hue) * (1 - smooth(150, 170, hue))
    flames = 1 - smooth(1167, 1422, xs)  # the red flames' side, left of Boo
    face = np.maximum(ellipse(2402, 2861, 374, 292, 0.18), ellipse(2661, 2797, 109, 164, 0.25))  # skin and ear
    tongue = ellipse(1549, 2450, 137, 128, 0.2)  # Boo's
    glow = np.maximum(ellipse(3126, 2970, 210, 228, 0.25),  # the red glow behind the Bob-omb
                      ellipse(2950, 3900, 380, 220, 0.25))  # the blast at the bottom right
    fire = colourful * np.maximum(yellow * (1 - face), red * np.maximum(flames * (1 - tongue), glow))
    grass = colourful * green
    fire_rgb = to_rgb(193 + (50 - np.clip(hue, -20, 75)) * 0.36, np.clip(saturation * 0.92, 0, 1), value)
    grass_rgb = to_rgb(np.full_like(hue, 205), saturation * 0.75, value * 0.9)
    out = rgb * (1 - fire - grass)[..., None] + fire_rgb * fire[..., None] + grass_rgb * grass[..., None]
    # Mario in the light: the rest darkens towards the launcher's night blue.
    keep = 1 - 0.62 * smooth(0.45, 1.25, np.sqrt(((xs - 2450) / 1400) ** 2 + ((ys - 2950) / 1050) ** 2))
    out = out * keep[..., None] + np.array([0.027, 0.047, 0.098]) * (1 - keep)[..., None]
    header = Image.fromarray((np.clip(out, 0, 1) * 255 + 0.5).astype(np.uint8))
    header = header.resize((1920, round(1920 * height / width)), Image.LANCZOS)
    header.save(ASSETS / "header.png", optimize=True)
    return header.width, header.height


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("logo", "header"):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    make = make_logo if sys.argv[1] == "logo" else make_header
    width, height = make(Path(sys.argv[2]))
    print(f"{ASSETS / (sys.argv[1] + '.png')}: {width} x {height}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
