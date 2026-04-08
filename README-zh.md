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
make DEFINES=-DXPLORE_DEBUG

# 5. 或生成分发目录
make DEFINES=-DXPLORE_DEBUG dist
```

生成的 `xplore.nro` 可通过 hbmenu 运行。

`make dist` 会创建 `dist/switch/`，并把 `xplore.nro` 复制进去，同时把 `scripts/cjk.ttf` 改名为 `xplore.ttf` 一并放入，供外挂字体加载使用。

## 外挂字体

Xplore 会检查运行中的 `.nro` 同目录、同文件名的 `.ttf`。

例如：

```text
sdmc:/switch/xplore/xplore.nro
sdmc:/switch/xplore/xplore.ttf
```

如果 `xplore.ttf` 存在，Xplore 会把它作为整个 UI 的唯一字体使用，不和内置字体混排；如果不存在，则使用 `romfs:/fonts/xplore.ttf`。

这适合在内置子集字体缺字时补充更多字符。

### SMB2 支持

SMB2 网络驱动器支持需要 [libsmb2](https://github.com/sahlberg/libsmb2)，Xplore 会直接链接它。

```bash
# 克隆 libsmb2
git clone https://github.com/sahlberg/libsmb2.git
cd libsmb2

# 为 Switch 编译并安装
sudo make -f Makefile.platform switch_install
```

安装后重新编译 Xplore 即可。


### DEBUG MODE
```shell
make DEFINES=-DXPLORE_DEBUG
```
