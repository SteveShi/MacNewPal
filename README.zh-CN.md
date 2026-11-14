# NewPal for macOS（新仙剑奇侠传 macOS 原生端口）

[English](README.md) | [简体中文](README.zh-CN.md)

**NewPal**（`com.steveshi.newpal`）是面向《新仙剑奇侠传》（`SWD2k` 引擎）的洁净室原生 macOS 启动器与 **SDL3 Metal** 运行时，采用版权合规的 **BYOD（Bring Your Own Data，自备正版数据）** 架构。

本仓库与编译生成的 `NewPal.app` 内**不包含任何受版权保护的原版美术、音乐、视频、文本或地图文件**。玩家启动后需选择自己合法持有的原版 `PalGame` 目录，由引擎核验全部核心归档的二进制签名后，通过内存虚拟文件系统（VFS）按需直读解码运行。

---

## 工程里程碑路线图

| 里程碑 | 核心目标 | 当前状态 |
| --- | --- | --- |
| **M0** | 原生 SwiftUI BYOD 启动器、23 项 VFS 签名校验器、SDL3 Metal 窗口与 LZO1X/ARLE 解码底座 | ✅ 已完成 |
| **M1** | Bink 视频播放、BGM 流式解码、`.tsw` 色键修复、`.act` 虚拟机、CoreText 富文本与标题/开场流程 | ✅ 已完成 |
| **M2** | 全量 `MSF3` 地图引擎（`huge.lmf` + `huge2.lmf`）、`maps.dat` 元数据联动、3 桶 Y 轴遮挡、8 向角色 `.act` 虚拟机、A* 寻路、死区镜头与 4 人队列跟随 | ✅ 已完成 |
| **M3+** | `Talk.cpp` 剧情脚本虚拟机与地图触发器、NPC 交互、背包/系统菜单、`GFight1.dat` 回合制战斗与存档系统 | 🚧 规划中 |

---

## 架构概览

1. **原生 macOS 启动器（`Sources/App/`）**：
   - 基于 **Swift 6.0**（开启 `SWIFT_STRICT_CONCURRENCY: complete`）与 **SwiftUI** 构建，面向 macOS 14.0+（`arm64`）。
   - 完整提供英文与简体中文双语本地化资源目录（`Sources/App/Resources/Localizable.xcstrings`）。
   - 通过 `NSOpenPanel` 与安全作用域书签挂载原版游戏目录，启动前严格校验全部 **23 项**核心档案与数据库二进制签名（`PalLauncherStore.swift`）。
   - 全功能设置面板（`SettingsView.swift`）：窗口倍率（`640×480`、`1280×960`、全屏等比）、Metal VSync 开关、纹理缩放模式（最近邻锐利像素 / 双线性平滑）、SDL3 主音量调节、静音开关及目录管理，均直接传参控制运行时。
   - Help 菜单内置“新功能介绍”弹窗（`WhatsNewSheetView.swift`）。

2. **零落地内存 VFS 与 SDL3 Metal 运行时（`Sources/Engine/`）**：
   - C11 引擎核心（`PalEngineCore.c` / `PalEngineCore.h`）直连 `libSDL3`（通过 `SDL_CreateRenderer(window, "metal")` 强制绑定 Apple **Metal** 后端）、**FFmpeg**（`libavformat`、`libavcodec`、`libswscale`、`libswresample`、`libavutil`）、**CoreText / CoreGraphics / CoreFoundation** 与 macOS `libiconv`（`CP950 -> UTF-8` 实时转码）。
   - 无需在硬盘解包，直接按需流式读取原版容器：
     - **Bink 视频播放器（`.bik`）**（`pal_bink_probe` 及运行时播放器）：实时解码 `.bik` 动画为 Metal 流式纹理并经 `SDL_AudioStream` 同步回放音轨（新故事开场播放 `Video/begin.bik`；`Video/notice.bik` 为通关后 `talk2.dat` `talk_id=1439` 后日谈《仙剑奇侠传二》预告，保留给结局后日谈播放并在 `--self-test` 中探测校验）。
     - **BGM 背景音乐流式解码器**（`pal_bgm_probe` 及运行时播放）：流式解码 `Music/*.mp3` / `*.wav`（`M001.mp3`、`M020.wav`、`F001.mp3` 等），通过 `libswresample` 重采样为 44.1 kHz 立体声 S16 PCM。
     - **`.tsw` 精灵图集与 ARLE 色键根因修复**：支持 `0xABCD` 魔数、LZO1X + ARLE `0xFFFF`、8-bit/16-bit RGB555，将 `0x319F`（`0x8000`）与 `0x026B`（`0xC000`）双透明色键均精确还原为 `alpha = 0`，叠加发光（`MA`）使用 `SDL_BLENDMODE_ADD`，并按归档 ID 区间路由读取 `all_char` / `all_item` / `all_magic` / `all_sys` / `all_map1` / `all_map2`。
     - **`.act` 动画字节码虚拟机**（`pal_act_vm_init`、`pal_act_vm_step_tick`、`PalActVM`）：35 ms 节拍驱动全部 35 个动作操作码，同时驱动 UI 动画（`all_sys.act`）与 8 方向主角/NPC 动作（`all_char.act`：`001李逍遙`、`002趙靈兒`、`003林月如`、`004阿奴`，涵盖站立 `0..7`、行走 `8..15`、奔跑 `16..23`、`YX` 锚点偏移与 `AP`/`DS`/`DE`/`VO`）。
     - **`MSF3` 全量地图引擎与 3 桶 Y 轴遮挡合成器**（`pal_map_probe_m2`、`vfs_load_lmf_map_by_id`）：跨 `huge.lmf`（320 张地图）与 `huge2.lmf`（149 张地图）加载 16×16 拼图底层、LZO1X 行走/触发网格（`walk_grid & 0x60000000` 碰撞、`& 0xFF` 触发器 ID）、触发器表、前景 ARLE 物件与 3 桶分层渲染（低于角色层、与角色按 `sort_y` 动态排序层、高于角色遮挡层）。
     - **`maps.dat` 全局元数据联动**：对接 `slot 19`（`CP950 -> UTF-8` 淡入地名横幅）、`slot 1`（按 `hdr[0x10c]` / `slot 2` 二级指针查表切换地图 BGM）、`slot 2`（`map_id` 映射 `msf3_id` 及移动步速）与 `slot 0`（26 字节 NPC 放置表，按 `flags & 0x8000` 激活）。
     - **场景移动、死区镜头与 4 人队列跟随**：复刻 `(304, 240)` 镜头死区与地图边界钳位、斜向贴墙滑动碰撞、左键点击全地图 8 向 A* 自动寻路，以及 4 主角队列拖尾跟随缓冲。
     - **CoreText 中文富文本渲染器**（`pal_font_probe_rich_text`、`PalRichTextMetrics`）：解析 SWD2k 排版控制符（`%N` 换行、`%C0`..`%C9` 10 色调色盘、`%S`、`%K`）并栅格化为带投影的 UTF-8 文字纹理。
     - **主菜单 → 新故事开场剧情 → 场景游历循环**：启动直入主菜单竹简界面（`all_sys.act` `10000`、`Music/M001.mp3`）；点击「新的故事」播放 `532` -> `533` -> `Video/begin.bik` -> 进入 `501`（`羅剎峰`），支持自由行走与切图探索。
     - **`all.snd` 音效与 `MZ + 0x200` 数据库**：`RIFF` 直通及 44 字节 PCM WAV 头重构，以及内存 TLV 解析（`ghero.dat`、`gmon.dat`、`gitem.dat`、`gmagic.dat`、`GFight1.dat`、`namex.dat`、`maps.dat`、`talk1.dat`）。

3. **逆向工程规范与离线验证工具链**：
   - 完整格式规范与战斗公式文档：[REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md)。
   - 独立只读 Python 解析工具位于 `Tools/`：
     - `Tools/pal_unpack.py`：生成目录清单、LZO1X/ARLE `.tsw` 导图、35 操作码 `.act` 反汇编、`all.snd` WAV 导出及 `MSF3` 地图渲染。
     - `Tools/pal_db_unpack.py`：`0x7F` TLV 数据库导出（含全部 88 个 `GFight` 战斗指令注释）、`Talk.cpp` 脚本反汇编及 `maps.dat` / `namex.dat` / `path1..10.dat` 解析。
     - `Tools/_lzo_accel.c`：供离线工具加速使用的 C 语言 LZO1X 解压库。

---

## 场景交互快捷键（`PAL_SCENE_FIELD`）

| 按键 / 操作 | 功能说明 |
| --- | --- |
| `WASD` / 方向键 | 8 方向移动（支持斜向贴墙滑动） |
| 按住 `Shift` | 奔跑模式（`action_base = 16`） |
| 鼠标左键点击地图 | 8 方向 A* 自动寻路前往目标格子 |
| `M` | 巡礼切换精选测试地图（`501`, `1`, `2`, `3`, `502`） |
| `[` / `]` | 在当前 `.lmf` 归档（`huge.lmf` / `huge2.lmf`）中切换上一张 / 下一张地图 |
| `P` | 开关 4 人队列跟随 |
| `V` | 轮换领队主角（`001李逍遙` / `002趙靈兒` / `003林月如` / `004阿奴`） |
| `G` | 开关碰撞网格、触发器与脚底判定框调试叠加层 |
| `C` | 开关穿墙（NoClip）模式 |
| `T` / `Space` / `Return` | 开关 CoreText 对话框 / 翻页下一句台词 |
| `B` | 重播开场动画 `Video/begin.bik` |
| `Tab` | 隐藏 / 显示顶部调试 HUD |
| `Esc` | 淡出返回主菜单标题界面 |

---

## 编译与自检（macOS）

### 环境依赖
- macOS 14.0+（`arm64`）及 Xcode 命令行工具
- [Homebrew](https://brew.sh/) 软件包：`sdl3`、`ffmpeg`、`xcodegen`

```bash
brew install sdl3 ffmpeg xcodegen
```

### 生成工程、编译与运行无头自检
```bash
xcodegen generate
xcodebuild -project NewPal.xcodeproj -scheme NewPal -configuration Release -derivedDataPath build build
./build/Build/Products/Release/NewPal.app/Contents/MacOS/NewPal --self-test /path/to/PalGame
```

指向有效的 `PalGame` 目录时，自检将输出 `Validation: 23/23 (is_valid=1)`、`Renderer: metal`、FFmpeg Bink/BGM 探测、UI 与角色 `.act` VM 步进、`MSF3` + `maps.dat` + A* 寻路探测（`MapM2[1]`、`MapM2[2]`、`MapM2[501]`）、CoreText 富文本渲染与实时解码的主角/地图/精灵/音效诊断信息，并以 `[SelfTest] ALL CHECKS PASSED.` 与退出码 `0` 结束。
