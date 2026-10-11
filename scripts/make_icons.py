"""Regenerates resources/app.ico and resources/archive.ico (needs Pillow).

The drawing mirrors resources/app.svg so the Windows icon, the taskbar and the
in-app SVG look the same. Run: python scripts/make_icons.py
"""
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent / "resources"
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
SS = 4  # supersampling factor


def gradient(size, top, bottom):
    img = Image.new("RGBA", (size, size))
    px = img.load()
    for y in range(size):
        for x in range(size):
            t = (x + y) / (2 * (size - 1))
            px[x, y] = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,)
    return img


def app_icon(size):
    s = size * SS
    k = s / 64.0
    base = gradient(s, (0x3B, 0x82, 0xF6), (0x6D, 0x28, 0xD9))
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle([5 * k, 5 * k, 59 * k, 59 * k], radius=13 * k, fill=255)
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    img.paste(base, (0, 0), mask)
    d = ImageDraw.Draw(img)
    v = [(17, 18), (25, 18), (32, 35), (39, 18), (47, 18), (35.5, 46), (28.5, 46)]
    d.polygon([(x * k, y * k) for x, y in v], fill=(255, 255, 255, 255))
    bolt = [(40, 12), (33, 27), (40, 27), (34, 40)]
    d.line([(x * k, y * k) for x, y in bolt], fill=(0xFA, 0xCC, 0x15, 255), width=max(1, int(3.2 * k)), joint="curve")
    return img.resize((size, size), Image.LANCZOS)


def archive_icon(size):
    s = size * SS
    k = s / 64.0
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([12 * k, 4 * k, 52 * k, 60 * k], radius=5 * k, fill=(248, 250, 255, 255),
                        outline=(0x25, 0x63, 0xEB, 255), width=max(1, int(2.5 * k)))
    for i in range(5):
        y = 8 + i * 6
        d.rectangle([30 * k, y * k, 34 * k, (y + 3) * k], fill=(0x25, 0x63, 0xEB, 255))
    badge = app_icon(size).resize((int(s * 0.55), int(s * 0.55)), Image.LANCZOS)
    img.alpha_composite(badge, (int(s * 0.42), int(s * 0.42)))
    return img.resize((size, size), Image.LANCZOS)


def save(name, painter):
    images = [painter(sz) for sz in SIZES]
    images[-1].save(ROOT / name, format="ICO", sizes=[(sz, sz) for sz in SIZES], append_images=images[:-1])


if __name__ == "__main__":
    save("app.ico", app_icon)
    save("archive.ico", archive_icon)
    print("icons written to", ROOT)
