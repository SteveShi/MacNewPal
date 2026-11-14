# NewPal for macOS（新仙剑奇侠传 macOS 原型）

[English](README.md) | [简体中文](README.zh-CN.md)

**NewPal**（`com.steveshi.newpal`）是面向《新仙剑奇侠传》（`SWD2k` 引擎）的洁净室原生 macOS 启动器与 **SDL3 Metal** 运行时原型，采用版权合规的 **BYOD（Bring Your Own Data，自备正版数据）** 架构。

本仓库与编译生成的 `NewPal.app` 内**不包含任何受版权保护的原版美术、音乐、文本或地图文件**。玩家启动后需选择自己合法持有的原版 `PalGame` 目录，由引擎核验全部核心归档的二进制签名后，通过内存虚拟文件系统（VFS）按需直读解码运行。

---

## 架构概览

1. **原生 macOS 启动器（`Sources/App/`）**：
   - 基于 **Swift 6.0**（开启 `SWIFT_STRICT_CONCURRENCY: complete`）与 **SwiftUI** 构建，面向 macOS 14.0+（`arm64`）。
   - 完整提供英文与简体中文双语本地化资源目录（`Sources/App/Resources/Localizable.xcstrings`）。
   - 通过 `NSOpenPanel` 与安全作用域书签挂载原版游戏目录，启动前严格校验全部 **23 项**核心档案与数据库二进制签名（`PalLauncherStore.swift`）。
   - 全功能设置面板（`SettingsView.swift`）：窗口倍率（`640×480`、`1280×960`、全屏等比）、Metal VSync 开关、纹理缩放模式（最近邻锐利像素 / 双线性平滑）、SDL3 主音量调节、静音开关及目录管理，均直接传参控制运行时。
   - Help 菜单内置“新功能介绍”弹窗（`WhatsNewSheetView.swift`）。

2. **零落地内存 VFS 与 SDL3 Metal 运行时（`Sources/Engine/`）**：
   - C11 引擎核心（`PalEngineCore.c` / `PalEngineCore.h`）直连 `libSDL3`（通过 `SDL_CreateRenderer(window, "metal")` 强制绑定 Apple **Metal** 后端）与 macOS `libiconv`（`CP950 -> UTF-8` 实时转码）。
   - 无需在硬盘解包，直接按需流式读取原版容器：
     - `.tsw` 精灵图集（`0xABCD` 魔数、LZO1X + ARLE `0xFFFF`、8-bit/16-bit RGB555 及 `0x319F` / `0x026B` 色键透明/半透处理）。
     - `.act` 动作脚本字节码（完整解析自 `0x45bae2` 逆向确认的 35 个操作码）。
     - `all.snd` 音效包（`RIFF` 直通及 44 字节 PCM WAV 头重构，经 `SDL_AudioStream` 回放）。
     - `huge.lmf` / `huge2.lmf`（`MSF3` LZO1X 图块池与地图网格渲染）。
     - `MZ + 0x200` 数据库（`ghero.dat`、`gmon.dat`、`gitem.dat`、`gmagic.dat`、`GFight1.dat`、`namex.dat`、`maps.dat`、`talk1.dat`）。

3. **逆向工程规范与离线验证工具链**：
   - 完整格式规范与战斗公式文档：[REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md)。
   - 独立只读Python解析工具位于 `Tools/`：
     - `Tools/pal_unpack.py`：生成目录清单、LZO1X/ARLE `.tsw` 导图、35 操作码 `.act` 反汇编、`all.snd` WAV 导出及 `MSF3` 地图渲染。
     - `Tools/pal_db_unpack.py`：`0x7F` TLV 数据库导出（含全部 88 个 `GFight` 战斗指令注释）、`Talk.cpp` 脚本反汇编及 `maps.dat` / `namex.dat` / `path1..10.dat` 解析。
     - `Tools/_lzo_accel.c`：供离线工具加速使用的 C 语言 LZO1X 解压库。

---

## 编译与自检（macOS）

### 环境依赖
- macOS 14.0+（`arm64`）及 Xcode 命令行工具
- [Homebrew](https://brew.sh/) 软件包：`sdl3`、`xcodegen`

```bash
brew install sdl3 xcodegen
```

### 生成工程、编译与运行无头自检
```bash
xcodegen generate
xcodebuild -project NewPal.xcodeproj -scheme NewPal -configuration Release -derivedDataPath build build
./build/Build/Products/Release/NewPal.app/Contents/MacOS/NewPal --self-test /path/to/PalGame
```

指向有效的 `PalGame` 目录时，自检将输出 `Validation: 23/23 (is_valid=1)`、`Renderer: metal`、实时解码的主角/地图/精灵/音效诊断信息，并以退出码 `0` 结束。
