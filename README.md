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

### Optional: SMB2 Support

SMB2 network drive support requires [libsmb2](https://github.com/sahlberg/libsmb2). The Makefile auto-detects whether it is installed and enables it automatically.

```bash
# Clone libsmb2
git clone https://github.com/sahlberg/libsmb2.git
cd libsmb2

# Build and install for Switch
sudo make -f Makefile.platform switch_install
```

After installation, rebuild Xplore and SMB2 will be enabled automatically.