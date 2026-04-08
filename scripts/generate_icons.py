"""
Generate Material-Design-style file-type icon PNGs (48x48, on transparent).
Uses only Pillow drawing primitives — no external resources needed.
Output: romfs/icons/*.png

Icons: folder, file, image, video, audio, archive, text, code, settings, download, back, network, add, sdcard, usb, about, menu
"""

import os
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    print("Installing Pillow...", file=sys.stderr)
    import subprocess
    subprocess.check_call([sys.executable, "-m", "pip", "install", "Pillow"])
    from PIL import Image, ImageDraw

SIZE = 48
PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(PROJECT_ROOT, "romfs", "icons")

WHITE = (224, 224, 224, 255)
ACCENT = (96, 165, 250, 255)
FOLDER_CLR = (251, 191, 36, 255)
TRANSPARENT = (0, 0, 0, 0)


def new_img():
    return Image.new("RGBA", (SIZE, SIZE), TRANSPARENT)


def draw_base_file(draw: ImageDraw.ImageDraw, color=WHITE):
    """Draw a generic file shape (rectangle with folded corner)."""
    draw.rectangle([10, 4, 38, 44], outline=color, width=2)
    draw.polygon([(28, 4), (38, 14), (28, 14)], fill=color)
    draw.line([(28, 4), (28, 14), (38, 14)], fill=color, width=2)


def icon_folder():
    img = new_img()
    d = ImageDraw.Draw(img)
    d.rectangle([6, 10, 20, 16], fill=FOLDER_CLR)
    d.rounded_rectangle([6, 16, 42, 40], radius=2, fill=FOLDER_CLR)
    return img


def icon_file():
    img = new_img()
    d = ImageDraw.Draw(img)
    draw_base_file(d)
    return img


def icon_image():
    img = new_img()
    d = ImageDraw.Draw(img)
    draw_base_file(d, color=(100, 200, 130, 255))
    d.ellipse([15, 18, 21, 24], fill=(100, 200, 130, 255))
    d.polygon([(14, 38), (24, 26), (30, 32), (36, 24), (36, 38)], fill=(100, 200, 130, 255))
    return img


def icon_video():
    img = new_img()
    d = ImageDraw.Draw(img)
    draw_base_file(d, color=(239, 68, 68, 255))
    d.polygon([(19, 22), (19, 38), (33, 30)], fill=(239, 68, 68, 255))
    return img


def icon_audio():
    img = new_img()
    d = ImageDraw.Draw(img)
    draw_base_file(d, color=(168, 85, 247, 255))
    d.ellipse([17, 32, 24, 38], fill=(168, 85, 247, 255))
    d.line([(24, 35), (24, 20)], fill=(168, 85, 247, 255), width=2)
    d.line([(24, 20), (32, 18)], fill=(168, 85, 247, 255), width=2)
    d.ellipse([28, 28, 35, 34], fill=(168, 85, 247, 255))
    d.line([(35, 31), (35, 18)], fill=(168, 85, 247, 255), width=2)
    return img


def icon_archive():
    img = new_img()
    d = ImageDraw.Draw(img)
    draw_base_file(d, color=(245, 158, 11, 255))
    for y in range(20, 40, 4):
        d.rectangle([22, y, 26, y + 2], fill=(245, 158, 11, 255))
    return img


def icon_text():
    img = new_img()
    d = ImageDraw.Draw(img)
    draw_base_file(d)
    for y in range(22, 38, 5):
        w = 20 if y < 36 else 14
        d.line([(16, y), (16 + w, y)], fill=WHITE, width=2)
    return img


def icon_code():
    img = new_img()
    d = ImageDraw.Draw(img)
    draw_base_file(d, color=ACCENT)
    d.line([(20, 24), (15, 30), (20, 36)], fill=ACCENT, width=2)
    d.line([(28, 24), (33, 30), (28, 36)], fill=ACCENT, width=2)
    return img


def icon_settings():
    import math
    img = new_img()
    d = ImageDraw.Draw(img)
    cx, cy = 24, 24
    d.ellipse([12, 12, 36, 36], outline=WHITE, width=2)
    d.ellipse([18, 18, 30, 30], fill=TRANSPARENT, outline=WHITE, width=2)
    for i in range(8):
        angle = i * math.pi / 4
        x1 = cx + int(15 * math.cos(angle))
        y1 = cy + int(15 * math.sin(angle))
        x2 = cx + int(19 * math.cos(angle))
        y2 = cy + int(19 * math.sin(angle))
        d.line([(x1, y1), (x2, y2)], fill=WHITE, width=4)
    return img


def icon_download():
    img = new_img()
    d = ImageDraw.Draw(img)
    color = ACCENT
    d.rounded_rectangle([10, 30, 38, 38], radius=4, outline=color, width=3)
    d.line([(24, 10), (24, 27)], fill=color, width=4)
    d.polygon([(16, 22), (24, 31), (32, 22)], fill=color)
    return img


def icon_back():
    img = new_img()
    d = ImageDraw.Draw(img)
    # Bent arrow: go up, then turn left.
    d.line([(34, 38), (34, 18), (16, 18)], fill=WHITE, width=4)
    d.polygon([(16, 18), (24, 10), (24, 26)], fill=WHITE)
    return img


def icon_network():
    """Globe / network icon for network drives."""
    img = new_img()
    d = ImageDraw.Draw(img)
    color = ACCENT
    # Outer circle
    d.ellipse([8, 8, 40, 40], outline=color, width=2)
    # Horizontal lines
    d.line([(8, 24), (40, 24)], fill=color, width=2)
    d.line([(12, 16), (36, 16)], fill=color, width=1)
    d.line([(12, 32), (36, 32)], fill=color, width=1)
    # Vertical ellipse (meridian)
    d.ellipse([18, 8, 30, 40], outline=color, width=2)
    return img


def icon_add():
    """Plus sign icon for 'add network address'."""
    img = new_img()
    d = ImageDraw.Draw(img)
    color = (100, 200, 130, 255)  # green
    # Horizontal bar
    d.rounded_rectangle([10, 20, 38, 28], radius=2, fill=color)
    # Vertical bar
    d.rounded_rectangle([20, 10, 28, 38], radius=2, fill=color)
    return img


def icon_sdcard():
    img = new_img()
    d = ImageDraw.Draw(img)
    body = (150, 150, 160, 255)
    pin = ACCENT
    d.rounded_rectangle([12, 8, 36, 40], radius=4, outline=body, width=2)
    d.polygon([(18, 8), (30, 8), (34, 14), (14, 14)], fill=body)
    for x in range(16, 33, 4):
        d.line([(x, 10), (x, 17)], fill=pin, width=2)
    d.rounded_rectangle([17, 22, 31, 34], radius=2, outline=pin, width=2)
    return img


def icon_usb():
    img = new_img()
    d = ImageDraw.Draw(img)
    color = (88, 196, 143, 255)
    d.rounded_rectangle([14, 10, 34, 38], radius=6, outline=color, width=2)
    d.rectangle([19, 6, 29, 12], outline=color, width=2)
    d.line([(24, 20), (24, 31)], fill=color, width=3)
    d.line([(20, 24), (28, 24)], fill=color, width=3)
    d.line([(24, 31), (18, 37)], fill=color, width=2)
    d.line([(24, 31), (30, 37)], fill=color, width=2)
    return img


def icon_about():
    img = new_img()
    d = ImageDraw.Draw(img)
    color = WHITE
    d.ellipse([12, 8, 36, 32], outline=color, width=2)
    d.line([(24, 18), (24, 27)], fill=color, width=3)
    d.ellipse([22, 12, 26, 16], fill=color)
    d.line([(18, 38), (30, 38)], fill=color, width=2)
    return img


def icon_menu():
    img = new_img()
    d = ImageDraw.Draw(img)
    color = WHITE
    for y in (14, 24, 34):
        d.rounded_rectangle([12, y, 36, y + 4], radius=2, fill=color)
    return img


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    icons = {
        "folder": icon_folder,
        "file": icon_file,
        "image": icon_image,
        "video": icon_video,
        "audio": icon_audio,
        "archive": icon_archive,
        "text": icon_text,
        "code": icon_code,
        "settings": icon_settings,
        "download": icon_download,
        "back": icon_back,
        "network": icon_network,
        "add": icon_add,
        "sdcard": icon_sdcard,
        "usb": icon_usb,
        "about": icon_about,
        "menu": icon_menu,
    }

    for name, gen_fn in icons.items():
        path = os.path.join(OUT_DIR, f"{name}.png")
        img = gen_fn()
        img.save(path, "PNG")
        size = os.path.getsize(path)
        print(f"  {name}.png ({size} bytes)")

    print(f"\nGenerated {len(icons)} icons in {OUT_DIR}")


if __name__ == "__main__":
    main()
