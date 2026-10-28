#!/usr/bin/env python3
"""Make the launcher's logo and header layers from their originals.

    launcher_art.py logo ORIGINAL.png [SIZZLE.png] # assets/launcher/logo.png: the Recharged logo, 1280 wide,
                                                   # and logo_sizzle.png: SIZZLE, the logo with sparks, on it
    launcher_art.py header MSC_Scene.jpg FOLDER   # FOLDER/header_back.png, header_front.png: 1920 x 1067

The header is the lower part of the key art (3500 x 4700): Mario's volley with
Boo and a Bob-omb, the art extended to the left (that side lies under the
launcher's logo) so that Mario stands to the right. The fire and the golden ball
turn the logo's electric blue (hot yellow to icy cyan, orange to sky blue, red
to deep blue) and the grass blue; Mario's face and reds and Boo's tongue keep
their colours, and the scene darkens towards night blue around Mario.

The launcher moves the header's two layers against each other: the
front one is Mario and the ball he kicks, cut out with Meta's SAM 2.1 model
from boxes and points on his parts; the back one is the rest of the scene, the
place behind them filled from its surroundings (OpenCV's inpainting). The
back layer in assets/launcher/ was redone afterwards, so the layers go to a
folder of their own. Needs Pillow and numpy; the header also OpenCV, PyTorch,
torchvision and transformers (the model, facebook/sam2.1-hiera-large, comes
from Hugging Face at first use). assets/launcher/README.md lists the originals.
"""

import sys
from pathlib import Path

import numpy as np
from PIL import Image

ASSETS = Path(__file__).resolve().parent.parent / "assets/launcher"
LOGO_WIDTH = 1280
HEADER_WIDTH = 1920

# Mario's parts and the ball for SAM, in the key art's pixels: a box around each
# and points on it (1) or on things beside it that are not him (0: Boo, the
# Bob-omb, the Koopa below the fist, the grass). One prompt for all of Mario
# leaves holes in his streaked thigh and swinging arm; part by part he is whole.
MARIO_PARTS = (
    ((2380, 2145, 2890, 2805), ((2722, 2207, 1), (2600, 2345, 1), (2620, 2705, 1))),  # raised arm
    ((2020, 2375, 2830, 3185), ((2400, 2860, 1), (2291, 2557, 1), (1509, 2476, 0))),  # head
    ((1880, 2885, 2930, 3565), ((2345, 3204, 1), (2722, 3015, 1), (2500, 3395, 1))),  # body
    ((1220, 2945, 2140, 3625), ((1509, 3177, 1), (1833, 3231, 1), (1348, 3123, 1))),  # kicking leg
    ((1600, 2995, 2160, 3605), ((1750, 3395, 1), (2000, 3145, 1), (1950, 3465, 1), (1700, 3845, 0))),  # thigh
    ((2590, 3205, 3235, 3895), ((2938, 3528, 1), (3150, 3495, 1), (3100, 2908, 0))),  # fist
    ((2650, 3050, 3235, 3380), ((2873, 3196, 1), (2999, 3159, 1), (3100, 2908, 0))),  # the fist's arm
    ((1750, 3195, 2650, 3905), ((2300, 3495, 1), (2450, 3345, 1), (2000, 3695, 1), (2372, 3932, 0))),  # lower arm
    ((1790, 3395, 2250, 3895), ((1860, 3541, 1), (2372, 3932, 0))),  # lower glove
    ((380, 2785, 1380, 4005), ((889, 3393, 1),)),  # the ball
)


def make_logo(source, sizzle=None):
    # The sizzle is the same logo with sparks, its canvas (of the same
    # proportions) stretched over the logo's; both are cut out alike.
    layers = [Image.open(source).convert("RGBA")]
    if sizzle:
        layers.append(Image.open(sizzle).convert("RGBA"))
    canvas = layers[0].size
    left, top, right, bottom = canvas[0], canvas[1], 0, 0
    for layer in layers:
        # A few nearly invisible specks lie further out than the logo.
        box = layer.getchannel("A").point(lambda a: 255 if a > 8 else 0).getbbox()
        sx, sy = canvas[0] / layer.width, canvas[1] / layer.height
        left, top = min(left, box[0] * sx), min(top, box[1] * sy)
        right, bottom = max(right, box[2] * sx), max(bottom, box[3] * sy)
    pad = 8
    box = (max(left - pad, 0), max(top - pad, 0), min(right + pad, canvas[0]), min(bottom + pad, canvas[1]))
    height = round((box[3] - box[1]) * LOGO_WIDTH / (box[2] - box[0]))
    for layer, name in zip(layers, ("logo.png", "logo_sizzle.png")):
        sx, sy = layer.width / canvas[0], layer.height / canvas[1]
        region = (box[0] * sx, box[1] * sy, box[2] * sx, box[3] * sy)
        # Premultiplied resampling: the transparent surroundings add no colour to the edges.
        scaled = layer.convert("RGBa").resize((LOGO_WIDTH, height), Image.LANCZOS, box=region).convert("RGBA")
        visible = scaled.getchannel("A").point(lambda a: 255 if a else 0)
        Image.composite(scaled, Image.new("RGBA", scaled.size), visible).save(ASSETS / name, optimize=True)
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


def make_header(source, folder):
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
    size = (HEADER_WIDTH, round(HEADER_WIDTH * height / width))
    scene = Image.fromarray((np.clip(out, 0, 1) * 255 + 0.5).astype(np.uint8)).resize(size, Image.LANCZOS)
    mario = np.zeros((height, width), np.uint8)
    mario[:, -left:] = find_mario(image, (0, top, right, bottom))
    mario = grow_into_outline(mario, value, 28)
    # Averaged down, the cut-out's edge is smooth.
    alpha = Image.fromarray(mario * 255).resize(size, Image.BOX)
    save_layers(np.asarray(scene), np.asarray(alpha), folder)
    return size


def find_mario(art, region):
    """Where Mario and the ball are in `region` of the key art: 1 there, 0 elsewhere."""
    import cv2
    import torch
    from transformers import Sam2Model, Sam2Processor

    name = "facebook/sam2.1-hiera-large"
    model = Sam2Model.from_pretrained(name).eval()
    processor = Sam2Processor.from_pretrained(name)
    left, top, right, bottom = region
    image = art.crop(region)
    found = np.zeros((bottom - top, right - left), bool)
    with torch.no_grad():
        embeddings = model.get_image_embeddings(processor(images=image, return_tensors="pt")["pixel_values"])
        for box, points in MARIO_PARTS:
            inputs = processor(images=image, return_tensors="pt",
                               input_boxes=[[[box[0] - left, box[1] - top, box[2] - left, box[3] - top]]],
                               input_points=[[[[x - left, y - top] for x, y, _ in points]]],
                               input_labels=[[[label for _, _, label in points]]])
            result = model(image_embeddings=embeddings, input_points=inputs["input_points"],
                           input_labels=inputs["input_labels"], input_boxes=inputs["input_boxes"],
                           multimask_output=False)
            logits = processor.post_process_masks(result.pred_masks, inputs["original_sizes"], binarize=False)
            found |= logits[0][0, 0].numpy() > 0
    mask = found.astype(np.uint8)
    # Close the gaps between the parts' outlines, then drop specks of background.
    count, labels, stats, _ = cv2.connectedComponentsWithStats(1 - mask, connectivity=4)
    for i in range(1, count):
        x, y, w, h, area = stats[i]
        if area < 30000 and x > 0 and y > 0 and x + w < mask.shape[1] and y + h < mask.shape[0]:
            mask[labels == i] = 1
    count, labels, stats, _ = cv2.connectedComponentsWithStats(mask, connectivity=8)
    keep = stats[:, cv2.CC_STAT_AREA] >= 4000
    keep[0] = False  # the background
    return keep[labels].astype(np.uint8)


def grow_into_outline(mask, value, reach):
    """The mask grown by up to `reach` pixels into the bright outline drawn around Mario.

    SAM cuts along his body; the white and cyan strokes around him belong to
    him too, and left in the back they would smear into its fill beside him.
    """
    import cv2

    bright = (value > 0.5).astype(np.uint8)
    step = np.ones((3, 3), np.uint8)
    grown = mask.copy()
    for _ in range(reach):
        grown = np.maximum(grown, cv2.dilate(grown, step) & bright)
    return grown


def save_layers(scene, alpha, folder):
    """header_back.png without Mario, header_front.png with only him."""
    import cv2

    shape = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * 4 + 1, 2 * 4 + 1))
    # The back: where Mario was (and the pixels his edge tints), the scene around
    # him flows in. The launcher shows it only beside him, as far as the layers move.
    covered = cv2.dilate((alpha > 0).astype(np.uint8), shape)
    back = cv2.inpaint(np.ascontiguousarray(scene[..., ::-1]), covered, 9, cv2.INPAINT_TELEA)[..., ::-1]
    Image.fromarray(back).save(folder / "header_back.png", optimize=True)
    # The front keeps the scene's colours a little beyond its edge, so that the
    # launcher's filtering blends none of the transparent black into it.
    near = cv2.dilate((alpha > 0).astype(np.uint8), shape)[..., None]
    front = np.dstack((scene * near, alpha))
    Image.fromarray(front.astype(np.uint8), "RGBA").save(folder / "header_front.png", optimize=True)


def main():
    if not ((len(sys.argv) in (3, 4) and sys.argv[1] == "logo") or (len(sys.argv) == 4 and sys.argv[1] == "header")):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    if sys.argv[1] == "logo":
        sizzle = Path(sys.argv[3]) if len(sys.argv) == 4 else None
        width, height = make_logo(Path(sys.argv[2]), sizzle)
        names = (ASSETS / "logo.png",) + ((ASSETS / "logo_sizzle.png",) if sizzle else ())
    else:
        folder = Path(sys.argv[3])
        folder.mkdir(parents=True, exist_ok=True)
        width, height = make_header(Path(sys.argv[2]), folder)
        names = (folder / "header_back.png", folder / "header_front.png")
    for name in names:
        print(f"{name}: {width} x {height}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
