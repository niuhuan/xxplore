# Xplore

A professional file manager and software installer for Nintendo Switch.

English | [中文](README-zh.md)

## Features

- Freely browse and manage files on SD card / WebDav / Samba / USB devices, and install NSP / XCI / NSZ / XCZ packages.
- Efficient file management with dual-pane mode for faster copy and move operations between different drives.
- Touchscreen support.
- Installer buffering with non-linear read/write, keeping network installs fast as well.
- Built-in HTTP server so a PC browser can install software to the Switch without any extra desktop software.
- External font support for displaying CJK filenames from drives other than the SD card.

## Screenshots

Interface:

![root](images/root.jpg)

Menu:

![main_menu](images/main_menu.jpg)

Network drive:

![remote_config](images/remote_config.jpg)

Install:
![install](images/install.jpg)

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

`make dist` creates `dist/switch/`, copies `xplore.nro` into it, and also copies `scripts/cjk.ttf` renamed as `xplore.ttf` for external font loading.

## External Font

Xplore checks for an external `.ttf` next to the running `.nro`.

Example:

```text
sdmc:/switch/xplore/xplore.nro
sdmc:/switch/xplore/xplore.ttf
```

If `xplore.ttf` exists, Xplore uses that font for all UI text. There is no fallback mixing with the built-in font. If it does not exist, Xplore uses `romfs:/fonts/xplore.ttf`.

This is useful when the built-in subset font is missing characters you need.

### SMB2 Support

SMB2 network drive support requires [libsmb2](https://github.com/sahlberg/libsmb2). Xplore links against it directly.

```bash
# Clone libsmb2
git clone https://github.com/sahlberg/libsmb2.git
cd libsmb2

# Build and install for Switch
sudo make -f Makefile.platform switch_install
```

After installation, rebuild Xplore.

### Debug Mode

```bash
make DEFINES=-DXPLORE_DEBUG
```

## License

See [LICENSE](LICENSE).

In addition to the dependencies listed above, the author uses the font from [cjk-fonts-ttf](https://github.com/life888888/cjk-fonts-ttf).
