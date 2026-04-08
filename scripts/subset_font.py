"""
Subset cjk.ttf to include only characters used in the project source files,
plus full ASCII range. Outputs a minimal font to romfs/fonts/xplore.ttf.

Run after adding/changing i18n translations to keep the font up to date.

Usage:  uv run python scripts/subset_font.py
Deps:   uv pip install fonttools brotli
"""

import glob
import os
import sys

from fontTools.subset import main as subset_main

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INPUT_FONT = os.path.join(PROJECT_ROOT, "scripts", "cjk.ttf")
OUTPUT_FONT = os.path.join(PROJECT_ROOT, "romfs", "fonts", "xplore.ttf")

PATTERNS = [
    "source/**/*.cpp",
    "source/**/*.hpp",
    "include/**/*.hpp",
    "include/**/*.h",
    "romfs/i18n/*.ini",
    "romfs/i18n/*.txt",
]


def collect_chars() -> set[str]:
    """Collect all unique characters from source files + full ASCII range."""
    chars: set[str] = set()

    for code in range(0x20, 0x7F):
        chars.add(chr(code))

    for pattern in PATTERNS:
        full = os.path.join(PROJECT_ROOT, pattern)
        for filepath in glob.glob(full, recursive=True):
            if "build" in filepath:
                continue
            with open(filepath, encoding="utf-8", errors="ignore") as f:
                chars.update(f.read())

    chars = {c for c in chars if ord(c) >= 0x20}
    return chars


def main() -> None:
    if not os.path.isfile(INPUT_FONT):
        print(f"Error: source font not found: {INPUT_FONT}", file=sys.stderr)
        sys.exit(1)

    os.makedirs(os.path.dirname(OUTPUT_FONT), exist_ok=True)

    chars = collect_chars()
    print(f"Collected {len(chars)} unique characters from source files")

    unicodes = ",".join(f"U+{ord(c):04X}" for c in sorted(chars))

    args = [
        INPUT_FONT,
        f"--output-file={OUTPUT_FONT}",
        f"--unicodes={unicodes}",
        "--layout-features=*",
        "--flavor=",
        "--no-hinting",
        "--desubroutinize",
    ]
    subset_main(args)

    src_size = os.path.getsize(INPUT_FONT) / 1024
    dst_size = os.path.getsize(OUTPUT_FONT) / 1024
    print(f"Done: {src_size:.0f} KB -> {dst_size:.0f} KB ({OUTPUT_FONT})")


if __name__ == "__main__":
    main()
