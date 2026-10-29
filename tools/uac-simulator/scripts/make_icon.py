#!/usr/bin/env python3
# Generate uacstudio.ico - oscilloscope amber sine trace on deep-space background.
# Design system: bg #171A21 / trace #E8A33D (instrument amber) / LED #4FB573.
# Supersampled at 1024, exported to multi-size ICO (16..256).
import math
from PIL import Image, ImageDraw, ImageFilter

S = 1024
BG = (0x17, 0x1A, 0x21, 255)
BORDER = (0x3A, 0x42, 0x50, 255)
TRACE = (0xE8, 0xA3, 0x3D, 255)
LED = (0x4F, 0xB5, 0x73, 255)


def render(with_led: bool) -> Image.Image:
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([0, 0, S - 1, S - 1], radius=S // 6, fill=BG, outline=BORDER,
                        width=S // 60)

    # sine path: 2 cycles, vertically centered slightly below middle
    amp = S * 0.19
    cy = S * 0.54
    pts = []
    for x in range(S):
        t = x / S
        y = cy + amp * math.sin(2 * math.pi * 2.0 * t)
        pts.append((x, y))

    # phosphor glow: wide soft stroke composited under the crisp trace
    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.line(pts, fill=(TRACE[0], TRACE[1], TRACE[2], 100), width=S // 30)
    glow = glow.filter(ImageFilter.GaussianBlur(S // 36))
    img.alpha_composite(glow)

    d2 = ImageDraw.Draw(img)
    w = S // 76
    d2.line(pts, fill=TRACE, width=w)
    # round caps
    r = w / 2
    for p in (pts[0], pts[-1]):
        d2.ellipse([p[0] - r, p[1] - r, p[0] + r, p[1] + r], fill=TRACE)

    if with_led:
        lr = S * 0.055
        cx, cyc = S * 0.80, S * 0.80
        halo = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        hd = ImageDraw.Draw(halo)
        hd.ellipse([cx - lr * 2, cyc - lr * 2, cx + lr * 2, cyc + lr * 2],
                   fill=(LED[0], LED[1], LED[2], 70))
        halo = halo.filter(ImageFilter.GaussianBlur(S // 40))
        img.alpha_composite(halo)
        d3 = ImageDraw.Draw(img)
        d3.ellipse([cx - lr, cyc - lr, cx + lr, cyc + lr], fill=LED)
    return img


big_led = render(True)
big_plain = render(False)

sizes = [16, 20, 24, 32, 48, 64, 128, 256]
frames = []
for n in sizes:
    src = big_plain if n < 32 else big_led
    frames.append((n, src.resize((n, n), Image.LANCZOS)))

out = "apps/uacstudio/uacstudio.ico"
# Pillow ICO save: pass the LARGE source; the plugin downscales per `sizes` itself
# (append_images is ignored by the ICO plugin).
big_led.save(out, format="ICO", sizes=[(n, n) for n in sizes])
print(f"wrote {out} with sizes {sizes}")
