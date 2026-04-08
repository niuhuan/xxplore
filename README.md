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
make DEFINES=-DXPLORE_DEBUG

# 5. Or build a distributable folder
make DEFINES=-DXPLORE_DEBUG dist
```

The generated `xplore.nro` can then be launched from hbmenu.

`make dist` creates `dist/switch/` and copies `xplore.nro` there, plus `scripts/cjk.ttf` renamed to `xplore.ttf` for external font loading.

## External Font

Xplore checks for an external `.ttf` next to the running `.nro`.

Example:

```text
sdmc:/switch/xplore/xplore.nro
sdmc:/switch/xplore/xplore.ttf
```

If `xplore.ttf` exists, Xplore uses that font for all UI text. There is no fallback mixing with the built-in font. If it does not exist, Xplore uses `romfs:/fonts/xplore.ttf`.

This is useful when the built-in subset font is missing characters you need.

### Optional: SMB2 Support

SMB2 network drive support requires [libsmb2](https://github.com/sahlberg/libsmb2). Xplore links against it directly.

```bash
# Clone libsmb2
git clone https://github.com/sahlberg/libsmb2.git
cd libsmb2

# Build and install for Switch
sudo make -f Makefile.platform switch_install
```

After installation, rebuild Xplore.
