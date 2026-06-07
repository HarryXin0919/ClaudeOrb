#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Generate firmware/claude_orb/claude_logo.h — the on-device Claude sunburst icon.

It takes the orange Claude mark (downloaded from the lobehub icon set, or a local
PNG via --src), recolours it, and emits an anti-aliased RGB565 bitmap that is
pre-blended over the dashboard's dark background (so the transparent edges stay
smooth on the OLED-like panel).

The Claude logo is a trademark of Anthropic. This only renders it as a small
icon for interface identification. Requires Pillow:  pip install pillow

Examples:
    python gen_logo.py                       # download + default orange-yellow
    python gen_logo.py --src claude.png      # use your own PNG
    python gen_logo.py --color 217,119,87    # Claude coral instead of orange-yellow
"""
import argparse
import io
import os
import urllib.request
from PIL import Image

LOGO_URL = "https://unpkg.com/@lobehub/icons-static-png/dark/claude-color.png"
OUT = os.path.join(os.path.dirname(__file__), "..", "firmware", "claude_orb", "claude_logo.h")


def main():
    ap = argparse.ArgumentParser(description="Make claude_logo.h for ClaudeOrb")
    ap.add_argument("--src", help="local PNG of the Claude mark (transparent bg); default: download")
    ap.add_argument("--size", type=int, default=44)
    ap.add_argument("--color", default="244,168,54", help="logo R,G,B (default orange-yellow #F4A836)")
    ap.add_argument("--bg", default="8,8,10", help="dashboard background R,G,B (must match firmware C_BG)")
    a = ap.parse_args()

    if a.src:
        im = Image.open(a.src)
    else:
        print("downloading Claude mark from lobehub ...")
        im = Image.open(io.BytesIO(urllib.request.urlopen(LOGO_URL, timeout=30).read()))

    W = H = a.size
    TARGET = tuple(int(x) for x in a.color.split(","))
    BG = tuple(int(x) for x in a.bg.split(","))
    im = im.convert("RGBA").resize((W, H), Image.LANCZOS)

    def c565(r, g, b):
        return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

    vals = []
    for y in range(H):
        for x in range(W):
            _, _, _, al = im.getpixel((x, y))
            r = (TARGET[0] * al + BG[0] * (255 - al)) // 255
            g = (TARGET[1] * al + BG[1] * (255 - al)) // 255
            b = (TARGET[2] * al + BG[2] * (255 - al)) // 255
            vals.append(c565(r, g, b))

    with open(OUT, "w", encoding="utf-8") as f:
        f.write("#pragma once\n")
        f.write(f"#define CLAUDE_LOGO_W {W}\n#define CLAUDE_LOGO_H {H}\n")
        f.write(f"const uint16_t claude_logo[{W * H}] = {{\n")
        for i in range(0, len(vals), 12):
            f.write("  " + ",".join("0x%04X" % v for v in vals[i:i + 12]) + ",\n")
        f.write("};\n")
    print("wrote", os.path.abspath(OUT), f"{W}x{H}")


if __name__ == "__main__":
    main()
