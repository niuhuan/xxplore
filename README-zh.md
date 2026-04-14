# Xxplore

Nintendo Switch 上专业的文件管理器, 软件安装器。

[English](README.md) | 中文

## 特性

- 可以随意浏览和管理 SD卡 / WebDav / Samba / USB设备 / FTP服务器 中的文件, 也可以安装其中的 NSP / XCI / NSZ / XCZ。
- 打包和解压压缩文件。
- 支持触摸屏操作。
- 启动HTTP服务器, PC只需要浏览器, 不需要安装额外软件, 就可以向Switch安装软件。
- 外挂字体, 显示非SD卡以外驱动器的CJK文件名。
- 双线程带缓存的流模式, 拷贝文件速度更快。
- 文本编辑器

## 截图

界面:

![root](images/root_chs.jpg)

菜单:

![main_menu](images/main_menu_chs.jpg)

网络驱动器:

![remote_config](images/remote_config_chs.jpg)

安装:
![install](images/install_chs.jpg)

## 构建

### 环境要求

- [devkitPro](https://devkitpro.org/)（devkitA64 + libnx + switch portlibs）
- Python 3
- [uv](https://github.com/astral-sh/uv)

### SMB2 支持

SMB2 网络驱动器支持需要 [libsmb2](https://github.com/sahlberg/libsmb2)，Xxplore 会直接链接它。

```bash
# 克隆 libsmb2
git clone https://github.com/sahlberg/libsmb2.git
cd libsmb2

# 为 Switch 编译并安装
sudo make -f Makefile.platform switch_install
```

安装后重新编译 Xxplore 即可。

### 原生依赖

#### minizip（ZIP 浏览 / 解压 / 打包）

Xxplore 使用 `minizip` 实现 ZIP 浏览、解压和打包。

在 devkitPro 的 Switch 环境里，`minizip` 由 `switch-zlib` 包提供，会安装：

- `portlibs/switch/include/minizip/*.h`
- `portlibs/switch/lib/libminizip.a`

在编译 Xxplore 之前，请先通过 devkitPro pacman 安装：

```bash
# Arch Linux / MSYS2 + devkitPro
pacman -S switch-zlib

# macOS / 没有系统 pacman 的 Linux
dkp-pacman -S switch-zlib
```

如果你的 devkitPro 前缀需要更高权限，请自行在前面加上 `sudo`。

#### libusbhsfs（USB 大容量存储）

Xxplore 使用 [libusbhsfs](https://github.com/DarkMatterCore/libusbhsfs) 实现 USB 大容量存储挂载。

根据上游项目说明，`libusbhsfs` 有两种构建模式：

- `BUILD_TYPE=ISC`：仅支持 FAT。
- `BUILD_TYPE=GPL`：支持 FAT + NTFS-3G + lwext4，许可证为 GPLv2 或更新版本。

Xxplore 当前临时使用 **GPL debug 变体** `-lusbhsfsd -lntfs-3g -llwext4`。

这是一个临时性的兼容方案：

- 在当前测试环境里，上游 release 版 `-lusbhsfs` 有时无法稳定枚举 USB 设备。
- 上游 debug 版 `-lusbhsfsd` 的行为正常，所以目前先用 debug 变体保证 USB 挂载稳定。
- 但上游 debug 版同时会打开非常吵的文件日志和文件系统驱动日志，带来额外 I/O，并写出 `sdmc:/libusbhsfs.log`。

因此，当前推荐的是 **静默 debug 版** `libusbhsfs`：

- 保留 `libusbhsfsd` 的运行时行为
- 禁用 `USBHSFS_LOG_*` 文件日志
- 保持 NTFS-3G 和 lwext4 日志关闭

这和 Xxplore 自己的 `XXPLORE_DEBUG` 宏不是一回事。

请先安装所需的文件系统 portlibs，再编译并安装 `libusbhsfs`：

```bash
# Arch Linux / MSYS2 + devkitPro
pacman -S switch-ntfs-3g switch-lwext4

# macOS / 没有系统 pacman 的 Linux
dkp-pacman -S switch-ntfs-3g switch-lwext4

git clone https://github.com/DarkMatterCore/libusbhsfs.git
cd libusbhsfs
make BUILD_TYPE=GPL install
```

如果你按 Xxplore 当前方案使用，请安装已经去掉日志输出的静默 `libusbhsfsd.a`，而不是直接使用上游自带日志的 debug 版。

### 步骤

```bash
# 1. 准备字体：将 CJK 全字符字体放入 scripts/cjk.ttf

# 2. 安装 Python 依赖（首次）
uv pip install fonttools brotli

# 3. 生成子集化字体 -> romfs/fonts/xxplore.ttf
uv run python scripts/subset_font.py

# 4. 编译
make DEFINES=-DXXPLORE_DEBUG

# 5. 或生成分发目录
make DEFINES=-DXXPLORE_DEBUG dist
```

生成的 `xxplore.nro` 可通过 hbmenu 运行。

`make dist` 会创建 `dist/switch/`，并把 `xxplore.nro` 复制进去，同时把 `scripts/cjk.ttf` 改名为 `xxplore.ttf` 一并放入，供外挂字体加载使用。

## 外挂字体

Xxplore 会检查运行中的 `.nro` 同目录、同文件名的 `.ttf`。

例如：

```text
sdmc:/switch/xxplore/xxplore.nro
sdmc:/switch/xxplore/xxplore.ttf
```

如果 `xxplore.ttf` 存在，Xxplore 会把它作为整个 UI 的唯一字体使用，不和内置字体混排；如果不存在，则使用 `romfs:/fonts/xxplore.ttf`。

这适合在内置子集字体缺字时补充更多字符。


### Debug Mode

```bash
make DEFINES=-DXXPLORE_DEBUG
```

`XXPLORE_DEBUG` 只控制 Xxplore 自己的调试代码，不等同于上面提到的临时静默 `libusbhsfsd` 方案。

## 协议

参考 [LICENSE](LICENSE) 文件。

除上述声明的依赖 作者使用 [cjk-fonts-ttf](https://github.com/life888888/cjk-fonts-ttf) 字体。
