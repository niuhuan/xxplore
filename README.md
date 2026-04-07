# Xplore

A dual-pane file manager for Nintendo Switch.

ENGLISH | [CHINESE](README-zh.md).

## Build

### Requirements

- [devkitPro](https://devkitpro.org/) (devkitA64 + libnx + switch portlibs)
- Python 3
- [uv](https://github.com/astral-sh/uv)

### Steps

```bash
# 1. Prepare the font: put a full CJK font at scripts/cjk.ttf

# 2. Install Python dependencies (first time only)
uv pip install fonttools brotli

# 3. Generate the subset font -> romfs/fonts/xplore.ttf
uv run python scripts/subset_font.py

# 4. Build
make
```

The generated `xplore.nro` can then be launched from hbmenu.