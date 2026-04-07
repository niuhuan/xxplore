# Xplore

Nintendo Switch 双栏文件管理器。

[English](README.md) | 中文

## 构建

### 环境要求

- [devkitPro](https://devkitpro.org/)（devkitA64 + libnx + switch portlibs）
- Python 3
- [uv](https://github.com/astral-sh/uv)

### 步骤

```bash
# 1. 准备字体：将 CJK 全字符字体放入 scripts/cjk.ttf

# 2. 安装 Python 依赖（首次）
uv pip install fonttools brotli

# 3. 生成子集化字体 -> romfs/fonts/xplore.ttf
uv run python scripts/subset_font.py

# 4. 编译
make
```

生成的 `xplore.nro` 可通过 hbmenu 运行。