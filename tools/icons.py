#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Draws icon.png (52 px) and icon-40.png, a white cassette on a flat purple rounded square, and
pane.png (96 px), the cassette alone in white on clear, which Q2 Pod's Home pane draws in its
theme's own colours. Each is drawn 8 times over and boxed down.

    python3 tools/icons.py"""
import pathlib
from PIL import Image, ImageDraw

S = 8  # drawn this many times over, then boxed down
FILL = (124, 77, 214, 255)
WHITE = (255, 255, 255, 255)
CLEAR = (255, 255, 255, 0)  # white, so a boxed edge stays white


def cassette(d, x0, y0, w, h, hole):
    """A white cassette w x h at x0, y0; hole is what shows through its window, reels' hubs and foot."""
    d.rounded_rectangle((x0, y0, x0 + w, y0 + h), radius=h * .12, fill=WHITE)
    wx0, wy0, wx1, wy1 = x0 + w * .14, y0 + h * .17, x0 + w * .86, y0 + h * .58
    d.rounded_rectangle((wx0, wy0, wx1, wy1), radius=h * .09, fill=hole)
    cy, rr = (wy0 + wy1) / 2, (wy1 - wy0) * .34
    for cx in (x0 + w * .33, x0 + w * .67):
        d.ellipse((cx - rr, cy - rr, cx + rr, cy + rr), fill=WHITE)
        k = rr * .45
        d.ellipse((cx - k, cy - k, cx + k, cy + k), fill=hole)
    ty = y0 + h * .72
    d.polygon([(x0 + w * .24, y0 + h), (x0 + w * .3, ty), (x0 + w * .7, ty), (x0 + w * .76, y0 + h)], fill=hole)
    d.polygon([(x0 + w * .27, y0 + h + 1), (x0 + w * .32, ty + h * .06), (x0 + w * .68, ty + h * .06),
               (x0 + w * .73, y0 + h + 1)], fill=WHITE)


def icon(size):
    im = Image.new('RGBA', (size * S, size * S), FILL[:3] + (0,))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((0, 0, size * S - 1, size * S - 1), radius=round(size * .22) * S, fill=FILL)
    w, h = size * .72 * S, size * .48 * S
    cassette(d, (size * S - w) / 2, (size * S - h) / 2, w, h, FILL)
    return im.resize((size, size), Image.Resampling.BOX)


def pane(size=96, fit=92):
    im = Image.new('RGBA', (size * S, size * S), CLEAR)
    d = ImageDraw.Draw(im)
    w, h = fit * S, fit * 2 / 3 * S
    cassette(d, (size * S - w) / 2, (size * S - h) / 2, w, h, CLEAR)
    return im.resize((size, size), Image.Resampling.BOX)


root = pathlib.Path(__file__).resolve().parent.parent
icon(52).save(root / 'icon.png', optimize=True)
icon(40).save(root / 'icon-40.png', optimize=True)
pane().save(root / 'pane.png', optimize=True)
