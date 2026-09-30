#!/usr/bin/env python3
"""Generates the Android adaptive icon (res/mipmap-*/ic_launcher_{background,foreground}.png + the XML) from the game's
title logo (tools/icon_source.jpg): the logo, black keyed to transparent, on a generated starfield."""
import os, random
from PIL import Image, ImageChops, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "../app/src/main/res")
SRC = os.path.join(HERE, "icon_source.jpg")

def layers(size=432):                       # 108 dp at xxxhdpi; only the centre 72 dp is always visible
    rnd = random.Random(2000)
    bg = Image.new("RGB", (size, size), (2, 2, 14)); px = bg.load(); c = size / 2
    for y in range(size):
        for x in range(size):
            v = max(0.0, 1 - (((x - c) ** 2 + (y - c) ** 2) ** .5) / (size * .7))
            px[x, y] = (int(3 + 22 * v), int(3 + 8 * v), int(14 + 60 * v))
    d = ImageDraw.Draw(bg)
    for _ in range(int(size * size / 700)):
        x, y = rnd.uniform(0, size), rnd.uniform(0, size); r = rnd.choice((.6, .8, 1.0, 1.5))
        col = rnd.choice(((255, 255, 255), (150, 190, 255), (255, 180, 130), (170, 255, 190))); v = rnd.uniform(.4, 1)
        d.ellipse((x - r, y - r, x + r, y + r), fill=tuple(int(k * v) for k in col))
    src = Image.open(SRC).convert("RGB"); src = src.crop((6, 4, src.width - 6, src.height - 4))
    w = int(size * 0.62); h = int(w * src.height / src.width)
    logo = src.resize((w, h), Image.LANCZOS)
    r, g, b = logo.split(); lum = ImageChops.lighter(ImageChops.lighter(r, g), b)
    logo = logo.convert("RGBA"); logo.putalpha(lum.point(lambda v: min(255, int(v * 255 / 40))))
    fg = Image.new("RGBA", (size, size), (0, 0, 0, 0)); fg.paste(logo, ((size - w) // 2, (size - h) // 2), logo)
    return bg, fg

def main():
    bg, fg = layers()
    d = os.path.join(RES, "mipmap-xxxhdpi"); os.makedirs(d, exist_ok=True)
    bg.save(os.path.join(d, "ic_launcher_background.png")); fg.save(os.path.join(d, "ic_launcher_foreground.png"))
    a = os.path.join(RES, "mipmap-anydpi-v26"); os.makedirs(a, exist_ok=True)
    open(os.path.join(a, "ic_launcher.xml"), "w").write(
        '<?xml version="1.0" encoding="utf-8"?>\n<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
        '    <background android:drawable="@mipmap/ic_launcher_background"/>\n    <foreground android:drawable="@mipmap/ic_launcher_foreground"/>\n</adaptive-icon>\n')
    Image.alpha_composite(bg.convert("RGBA"), fg).save(os.path.join(HERE, "icon_preview.png"))

if __name__ == "__main__":
    main()
