# NewPal for macOS（新仙剑奇侠传 macOS 原生端口）

[English](README.md) | [简体中文](README.zh-CN.md)

**NewPal**（`com.steveshi.newpal`）是面向《新仙剑奇侠传》（`SWD2k` 引擎）的洁净室原生 macOS 启动器与 **SDL3 Metal** 运行时，采用版权合规的 **BYOD（Bring Your Own Data，自备正版数据）** 架构。

本仓库与编译生成的 `NewPal.app` 内**不包含任何受版权保护的原版美术、音乐、视频、文本或地图文件**。玩家在启动器中点击「导入数据包…」选择原版游戏文件夹后，启动器会主动校验全部 23 项核心档案的二进制签名，并将引擎所需的游戏数据选择性复制到 macOS 标准目录 `~/Library/Application Support/NewPal/PalGame`（导入完成后即可自由删除原游戏文件夹），后续所有游戏数据统一从该标准目录通过内存虚拟文件系统（VFS）按需直读解码运行。

---

## 工程里程碑路线图

| 里程碑 | 核心目标 | 当前状态 |
| --- | --- | --- |
| **M0** | 原生 SwiftUI BYOD 启动器、23 项 VFS 签名校验器、SDL3 Metal 窗口与 LZO1X/ARLE 解码底座 | ✅ 已完成 |
| **M1** | Bink 视频播放、BGM 流式解码、`.tsw` 色键修复、`.act` 虚拟机、CoreText 富文本与标题/开场流程 | ✅ 已完成 |
| **M2** | 全量 `MSF3` 地图引擎（`huge.lmf` + `huge2.lmf`）、`maps.dat` 元数据联动、3 桶 Y 轴遮挡、8 向角色 `.act` 虚拟机、A* 寻路、死区镜头与 4 人队列跟随 | ✅ 已完成 |
| **M3** | 全 8 卷 `talk1..8.dat` 多库加载器与 Talk 字节码虚拟机（`7,394` 条脚本）、剧情旗标与脚本变量、`%B` 对话分支选项、`0xD8`/`0xD9` 半身立绘、地块触发器、NPC/宝箱交互与 `0x1B` 切图传送 | ✅ 已完成 |
| **M4** | 完整 `0x7F` 数据库（`ghero`/`glevel`/`gitem`/`gmagic`）、队伍属性/换装/仙术/升级运行时、`Talk`（`0x80`..`0x86` 与 `0x309` 物品脚本）联动及游戏内营地菜单（狀態/仙術/物品/系統） | ✅ 已完成 |
| **M5** | `gmon.dat`/`gmap.dat`/`GFight1..4.dat` 回合制战斗、商店买卖 UI（`58000+`）与 10 槽位存档/读档（`Save/0..9.sav`） | ✅ 已完成 |
| **M6** | 全量战斗还原：160 位异常状态掩码（毒伤 DOT 与 `3063`/`3074` 免疫转化、封魔/混乱/昏睡）、四人合体技、同伴倒下怒气加成、原版 `GFight` 公式与 AI、怪物死亡演出及可选 2x 高清模式 | ✅ 已完成 |
| **M7** | 结局后日谈与 Talk 虚拟机闭环（`0x60`/`0xC2`/`0xC4`/`0xC8` 双遍分支直达 `talk 1439 -> Video/notice.bik` 与拔剑切换）及只读 BYOD 目录 `Application Support` 存档回退 | ✅ 已完成 |
| **M8** | 重制级音画与操作升级：SDL3 Metal GPU 后处理着色器（`xBRZ` 与 `CRT-Royale` MSL）、视网膜级 CoreText UI 分层叠加、BGM 平滑淡入淡出与压低及完整 SDL3 手柄支持（`PalGamepad`） | ✅ 已完成 |
| **M9** | MCP 协议支持与 AI 自动化游玩：原生内置 stdio 模型上下文协议（`--mcp-server`），提供 16 项结构化工具、无头超高速仿真与全八章主线自动化通关测试跑道 | ✅ 已完成 |
| **M10** | 全八章主线通关实测、`Palgame.exe` 逆向对齐核查（P0–P3）与 31 条全流程支线回归：战斗/炼蛊/商店/AI/时序逻辑全量对齐原版、真实 `gmap.dat` + `all_map2.act` 战斗场景、双隐藏结局（`1195` / `1199`）与早/中/后期 31 条支线纳入 `--self-test` 回归 | ✅ 已完成 |

---

## 架构概览

1. **原生 macOS 启动器（`Sources/App/`）**：
   - 基于 **Swift 6.0**（开启 `SWIFT_STRICT_CONCURRENCY: complete`）与 **SwiftUI** 构建，面向 macOS 14.0+（`arm64`）。
   - 完整提供英文与简体中文双语本地化资源目录（`Sources/App/Resources/Localizable.xcstrings`）。
   - **托管式 BYOD 数据导入与清理（`PalLauncherStore.swift`）**：点击导入后通过 `NSOpenPanel` 选择原版游戏目录，主动完成全部 **23 项**核心档案签名预检，并仅将引擎所需的封包、数据库与 `Music/`、`Video/` 选择性复制至 `~/Library/Application Support/NewPal/PalGame`（运行时统一且唯一从该标准目录读取，源文件夹可随时删除）。同时提供「打开 Application Support」快捷按钮与带二次确认的「删除游戏数据」功能，可主动清理已导入数据包并严格保留 `~/Library/Application Support/NewPal/Save/` 玩家存档。
   - 全功能设置与启动控件（`SettingsView.swift`、`ContentView.swift`）：窗口倍率（`640×480`、`1280×960`、全屏等比）、**Metal GPU 滤镜**（`原版直出`、`Metal xBRZ 锐化`、`CRT-Royale 显像管`）、画面清晰度（原版 / 2× 高清重制）、Metal VSync 开关、纹理缩放模式、SDL3 主音量调节、静音开关及目录管理，均直接传参控制运行时。
   - Help 菜单内置“新功能介绍”弹窗（`WhatsNewSheetView.swift`）。

2. **零落地内存 VFS 与 SDL3 Metal 运行时（`Sources/Engine/`）**：
   - C11 引擎核心（`PalEngineCore.c` / `PalEngineCore.h`）联动 `PalGpuFilter.{h,c}`（SDL3 GPU/Metal MSL 滤镜管线）与 `PalGamepad.{h,c}`（SDL3 手柄热插拔与摇杆/十字键映射），直连 `libSDL3`（优先 `gpu` + Metal MSL 后端并支持回退 `metal`）、**FFmpeg**（`libavformat`、`libavcodec`、`libswscale`、`libavutil`、`libswresample`）、**CoreText / CoreGraphics / CoreFoundation** 与 macOS `libiconv`（`CP950 -> UTF-8` 实时转码）。
   - **双通道 GPU 后处理与视网膜级 UI 合成器**（`PalGpuFilter.c`）：先将 640×480 场景离屏绘制至渲染目标纹理，再使用内嵌 Metal MSL 片段着色器（`xBRZ` 保边锐化或 `CRT-Royale` 扫描线/栅格/辉光）输出至原生交换链，随后清除着色器状态并在独立高清通道将动态 Retina（`1×..4×`）CoreText 对话、营地菜单、商店与战斗 HUD 锐利叠加。
   - 无需在硬盘解包，直接按需流式读取原版容器：
     - **Bink 视频播放器（`.bik`）**（`pal_bink_probe` 及运行时播放器）：实时解码 `.bik` 动画为 Metal 流式纹理并经 `SDL_AudioStream` 同步回放音轨（新故事开场播放 `Video/begin.bik`；`Video/notice.bik` 为通关后 `talk2.dat` `talk_id=1439` 后日谈《仙剑奇侠传二》预告，保留给结局后日谈播放并在 `--self-test` 中探测校验）。
     - **BGM 背景音乐流式解码与平滑淡变**（`pal_bgm_probe` 与音量包络器）：流式解码 `Music/*.mp3` / `*.wav`（`M001.mp3`、`M020.wav`、`F001.mp3` 等），通过 `libswresample` 重采样为 44.1 kHz 立体声 S16 PCM，切图/进出战斗支持平滑淡入淡出，战后结算自动压低伴奏音量。
     - **`.tsw` 精灵图集与 ARLE 色键根因修复**：支持 `0xABCD` 魔数、LZO1X + ARLE `0xFFFF`、8-bit/16-bit RGB555，将 `0x319F`（`0x8000`）与 `0x026B`（`0xC000`）双透明色键均精确还原为 `alpha = 0`，叠加发光（`MA`）使用 `SDL_BLENDMODE_ADD`，并按归档 ID 区间路由读取 `all_char` / `all_item` / `all_magic` / `all_sys` / `all_map1` / `all_map2`。
     - **`.act` 动画字节码虚拟机**（`pal_act_vm_init`、`pal_act_vm_step_tick`、`PalActVM`）：35 ms 节拍驱动全部 35 个动作操作码，同时驱动 UI 动画（`all_sys.act`）与 8 方向主角/NPC 动作（`all_char.act`：`001李逍遙`、`002趙靈兒`、`003林月如`、`004阿奴`，涵盖站立 `0..7`、行走 `8..15`、奔跑 `16..23`、`YX` 锚点偏移与 `AP`/`DS`/`DE`/`VO`）。
     - **`MSF3` 全量地图引擎与 3 桶 Y 轴遮挡合成器**（`pal_map_probe_m2`、`vfs_load_lmf_map_by_id`）：跨 `huge.lmf`（320 张地图）与 `huge2.lmf`（149 张地图）加载 16×16 拼图底层、LZO1X 行走/触发网格（`walk_grid & 0x60000000` 碰撞、`& 0xFF` 触发器 ID）、触发器表、前景 ARLE 物件与 3 桶分层渲染（低于角色层、与角色按 `sort_y` 动态排序层、高于角色遮挡层）。
     - **`maps.dat` 全局元数据联动**：对接 `slot 19`（`CP950 -> UTF-8` 淡入地名横幅）、`slot 1`（按 `hdr[0x10c]` / `slot 2` 二级指针查表切换地图 BGM）、`slot 2`（`map_id` 映射 `msf3_id` 及移动步速）与 `slot 0`（26 字节 NPC 放置表，按 `flags & 0x8000` 激活）。
     - **场景移动、死区镜头与 4 人队列跟随**：复刻 `(304, 240)` 镜头死区与地图边界钳位、斜向贴墙滑动碰撞、左键点击全地图 8 向 A* 自动寻路，以及 4 主角队列拖尾跟随缓冲。
     - **CoreText 中文富文本渲染器**（`pal_font_probe_rich_text`、`PalRichTextMetrics`）：解析 SWD2k 排版控制符（`%N` 换行、`%C0`..`%C9` 10 色调色盘、`%S`、`%D`、`%G`、`%K`）并栅格化为带投影的 UTF-8 文字纹理。
     - **Talk 剧情脚本虚拟机、半身立绘与触发器引擎**（`pal_talk_vm_probe_m3`、`PalTalkVM`、`PalTalkWorldState`）：全 8 卷 `talk1..8.dat` 多库加载器（`talk1`/`talk4` 各 `701` 槽、其余 `1000` 槽 —— 全库 **7,394 / 7,394** 条脚本 100% 无错解码），完整支持剧情旗标（`8192`）、脚本变量（`256`、`obj_rv`）、地图/道具/距离/仅已装备（`0xA7`/`0xA8`）条件跳转、`0xD8`/`0xD9` 动态半身立绘（`all_map1.act` -> `all_map1.tsw`）、`%B` 多选项对话菜单、`0x310(9012)` 对话框关闭、ACT `700` 多格 `HW` 事件热区、`path1..10.dat` NPC AI 虚拟机（支持在队成员距离检测）、`0x14`/`0x26`/`0x27`/`0x28`/`0x3E`/`0x64`/`0xA9` NPC 位置与特殊姿态持久化、`0x30A` 满血检定与 `0x30B` 金钱变量同步、`0x309` 奖励脚本、`gitem.dat` `77777` 32 组迷宫出口（引路蜂/土灵珠）及 `0x1B` 切图传送。
     - **`0x7F` 游戏数据库运行时、炼蛊系统、角色成长与营地菜单**（`pal_db_menu_probe_m4`、`PalGameDatabase`、`PalPartyMemberState`）：完整解析 `ghero.dat`（`9` 位主角）、`gitem.dat`（`252` 件物品与效果/装备脚本）、`gmagic.dat`（`66` 种仙术与 `601` 条脚本含 `5001..5319` 奖励及 `58000+` 商店清单）、`glevel.dat`（`410` 条升级模板，最高 99 级封顶）、`gmon.dat`（`160` 种怪物）、`gmap.dat`（`265` 遇敌区域）与 `GFight1..4.dat`（`1,056` 条战斗编队脚本）。严格对齐 `Palgame.exe` 装备槽位与主角掩码兼容性、装备属性被动加成（`0x7F30200B`）、`0x7F102001` 无条件跳转语义、非战斗道具/仙术恢复（`0x7EFF1001`/`0x7EFF1015`）、紫金葫蘆（`3905`）与煉蠱皿（`3906`）炼丹/炼蛊体系，并与 `Talk` 指令（`0x80`..`0x86`、`0xA7`/`0xA8`、`0x309`）实时联动；按 `I` / `U` / `F1` 打开「狀態 / 仙術 / 物品 / 系統」四分页营地菜单（联动 `all_sys.tsw`、`all_item.act`/`all_item.tsw` 与 CoreText）。
     - **回合制战斗、商店买卖与 10 槽位存档/读档**（`pal_combat_shop_save_probe_m5`、`PalCombatState`、`PalShopMenuState`、`PalLoadOverlayState`）：完整接入 `GFight1..4.dat` 与 `gmon.dat` 回合制战斗，按 `gmap.dat` + `all_map2.act` 精准还原各地图专属战斗背景，完整还原 `all_char.act` `1000..1003` 主角与 `2000+` 怪物动作/死亡演出，并经 P0–P3 逆向核查全面对齐 `Palgame.exe` 物攻/仙术/投掷/偷窃/逃跑/出手顺序/双槽掉落公式（含酒神 9 次上限、乾坤一掷、160 位异常状态掩码与 `3063` 五毒珠免疫/`3074` 七彩玉靈芝转化、四人合体技、同伴倒下激怒、非剧情战败返回标题画面及可选 2x 高清渲染）；支持 `58000+` 脚本商店买卖界面（`all_sys.tsw` `9017`）及 10 槽位读写（兼容原版 `0x6A54` `Save/0..9.sav`、`"NPAL"` V8 扩展尾标持久化剧情变量/AI/灵葫值及只读目录自动回退 macOS `Application Support`，可在主菜单「舊的回憶」或营地「系統」分页中存取）。
     - **主菜单 → 原版开场剧情脚本 → 场景游历循环**：启动直入主菜单竹简界面（`all_sys.act` `10000`、`Music/M001.mp3`），支持「舊的回憶」10 槽读档；点击「新的故事」执行 `talk1.dat` `talk_id=100` 原版开场剧情（`532` -> `533` -> `Video/begin.bik` -> `501` `羅剎峰` -> `502` `羅剎洞外`，夢境李逍遙 `2009` 单人开局），支持自由行走、宝箱开盖状态切换（`OP 0x0A`）与跳出土怪、脚本内遇敌战斗（`2801` / `5001+`）、触发器切图、商店与营地系统。
     - **`all.snd` 音效与 `MZ + 0x200` 数据库**：`RIFF` 直通及 44 字节 PCM WAV 头重构，以及内存 TLV 解析（`ghero.dat`、`gmon.dat`、`gitem.dat`、`gmagic.dat`、`GFight1.dat`、`namex.dat`、`maps.dat`、`talk1.dat`）。

3. **数据格式规范、测试模块与离线校验工具链**：
   - 完整数据格式与战斗公式规范：[docs/FORMAT_SPEC.md](docs/FORMAT_SPEC.md)。
   - 可选 QA 测试模式模块说明（`PAL_ENABLE_TEST_MODE`）：[docs/TEST_MODE.zh-CN.md](docs/TEST_MODE.zh-CN.md)（[English](docs/TEST_MODE.md)）。
   - 独立只读 Python 解析工具位于 `Tools/`：
     - `Tools/pal_unpack.py`：生成目录清单、LZO1X/ARLE `.tsw` 导图、35 操作码 `.act` 脚本解码、`all.snd` WAV 导出及 `MSF3` 地图渲染。
     - `Tools/pal_db_unpack.py`：`0x7F` TLV 数据库导出（含全部 88 个 `GFight` 战斗指令注释）、Talk 字节码解码及 `maps.dat` / `namex.dat` / `path1..10.dat` 解析。
     - `Tools/_lzo_accel.c`：供离线工具加速使用的 C 语言 LZO1X 解压库。

---

## 首次运行与安全提示（未签名应用 Gatekeeper 说明）

由于本项目为开源自建发布版，未购买 Apple 开发者计划证书进行官方公证（采用 Ad-hoc 本地签名）。macOS 的 Gatekeeper 安全机制在首次打开应用时可能会弹出拦截警告（例如：*“无法打开‘NewPal’，因为 Apple 无法检查其是否包含恶意软件”* 或 *“‘NewPal’已损坏，无法打开”*）。

请按以下方式之一正常启动：

### 推荐方法：右键（Control + 单击）打开
1. 将 `NewPal.app` 拖入 `/Applications`（应用程序）文件夹；
2. 在 Finder 中找到 `NewPal.app`，**按住 Control 键并单击（或鼠标右键单击）**图标；
3. 在弹出的快捷菜单中选择 **“打开” (Open)**；
4. 在随后弹出的确认对话框中，点击 **“打开”** 即可正常启动，此后即可双击直接运行。

### 备用方法一：系统设置中允许打开
1. 双击被系统拦截后，打开 **“系统设置” (System Settings)**；
2. 进入 **“隐私与安全性” (Privacy & Security)**；
3. 向下滚动至 **“安全性” (Security)** 区域，会看到 *“已阻止使用‘NewPal’，因为来自身份不明的开发者”*；
4. 点击旁边的 **“仍要打开” (Open Anyway)** 并验证系统密码即可。

### 备用方法二：终端移除隔离属性 (Quarantine)
若系统提示“已损坏，无法打开，你应该将它移到废纸篓”，请打开“终端” (Terminal) 执行以下命令移除 macOS 隔离标记：
```bash
xattr -cr /Applications/NewPal.app
```

---

## 场景交互快捷键（`PAL_SCENE_FIELD`）

| 按键 / 操作 | 功能说明 |
| --- | --- |
| `WASD` / 方向键 | 8 方向移动（支持斜向贴墙滑动） |
| 按 `Shift` | 切换走路 / 奔跑状态（`action_base = 8` / `16`） |
| 鼠标左键点击地图 / NPC | 8 方向 A* 自动寻路至目标格，或走近点击的 NPC/宝箱自动触发对话 |
| `Space` / `Return` / `E` | 与面朝邻近 NPC/宝箱对话、推进脚本对话或确认所选分支 / 菜单项 |
| `Up` / `Down` / `1`..`8` | 出现 `%B` 多选分支时切换 / 直选选项 |
| `Esc` / 右键 / `I` / `U` / `F1` | 打开或关闭四分页队伍系统菜单（`狀態`、`仙術`、`物品`、`系統`，含换装、道具/仙术使用、10 槽存档/读档、返回主菜单与退出游戏）；或取消商店 / 战斗子菜单 |
| `F` | 触发回合制战斗（`PAL_SCENE_COMBAT`，按当前地图遇敌区或默认编队 `2801` 开战） |
| `O` | 打开购买/出售商店界面（`gmagic.dat` `58001` 或剧情 `0x309` 触发商店，按 `←`/`→`/`Tab` 切换买/卖） |
| `M` | 巡礼切换精选测试地图（`501`, `1`, `2`, `3`, `502`） |
| `[` / `]` | 在当前 `.lmf` 归档（`huge.lmf` / `huge2.lmf`）中切换上一张 / 下一张地图 |
| `P` | 开关 4 人队列跟随 |
| `V` | 轮换领队主角（`001李逍遙` / `002趙靈兒` / `003林月如` / `004阿奴`） |
| `G` | 开关碰撞网格、触发器与脚底判定框调试叠加层 |
| `C` | 开关穿墙（NoClip）模式 |
| `T` | 在未运行剧情脚本时开关示例 CoreText 对话框 |
| `B` | 重播开场动画 `Video/begin.bik` |
| `Tab` | 显示 / 隐藏顶部调试 HUD（默认隐藏） |
| **游戏手柄（SDL3）** | 十字键 / 左摇杆：8 向移动与菜单导航；`A`（下键）：交互 / 对话 / 确认；`B`（右键）：取消 / 返回；`X`（左键）：切换走路/奔跑；`Y`（上键）/ `Start`：营地菜单；`LB` / `RB`：上/下一主角或页签；`Back`：调试 HUD |

---

## 编译与自检（macOS）

### 环境依赖
- macOS 14.0+（`arm64`）及 Xcode 命令行工具
- [Homebrew](https://brew.sh/) 软件包：`sdl3`、`ffmpeg`、`xcodegen`

```bash
brew install sdl3 ffmpeg xcodegen
```

### 游戏数据包准备（标准文件夹命名）
- 标准数据文件夹名称：**`PalGame`**
- 标准 macOS 存放路径：**`~/Library/Application Support/NewPal/PalGame`**
- 在启动器中点击 **「导入数据包…」** 选择原版游戏文件夹后，启动器会自动完成 23 项签名校验并仅复制所需游戏文件（`*.tsw`、`*.act`、`all.snd`、`huge*.lmf`、`*.dat`、`Music/`、`Video/`）到 `~/Library/Application Support/NewPal/PalGame`，导入成功后可自由删除原游戏文件夹。
- 可随时点击 **「打开 Application Support」** 在 Finder 中查看 `~/Library/Application Support/NewPal/`，或使用 **「删除游戏数据」** 主动清理已导入数据包（`Save/` 中的玩家存档会被完整保留）。

### 生成工程、编译与运行无头自检
```bash
xcodegen generate
xcodebuild -project NewPal.xcodeproj -scheme NewPal -configuration Release -derivedDataPath build build
./build/Build/Products/Release/NewPal.app/Contents/MacOS/NewPal --self-test /path/to/PalGame
```

指向有效的 `PalGame` 目录时，自检将输出 `Validation: 23/23 (is_valid=1)`、`Renderer: metal`、FFmpeg Bink/BGM 探测、UI 与角色 `.act` VM 步进、`MSF3` + `maps.dat` + A* 寻路探测（`MapM2[1]`、`MapM2[2]`、`MapM2[501]`）、`TalkM3`（8 卷共 `7394/7394` 条脚本无错解码，传送 `6001`/`6007`、宝箱 `7012` 与剧情 `104`/`105` 状态推进）、`DbM4`（`0x7F` 数据表：`9` 角色、`252` 道具、`66` 仙术、`410` 升级记录，`h2000` 穿脱装备/道具/仙术/升级推演与 `0x309` 物品脚本解析 `5150 -> 3254 皮帽`）、`CombatShopSaveM5`（`160` 怪物、`265` 遇敌区域、`1056` 战斗编队、`2801` 对战 `羅剎鬼婆` 结算升级、`58001` 商店交易、`Save/0.sav` 读取与临时 `Save/99.sav` 往返校验、P0–P3 原版逻辑对齐校验、双隐藏结局 `1195` / `1199` 及 **早/中/后期全部 31 条支线端到端校验**）、`Full8Chapters`（全八章关键节点全绿）、`RemasterM8`（`gpu` + MSL `xbrz`/`crt` + `gamepad`）、CoreText 富文本渲染与实时解码诊断信息，并以 `[SelfTest] ALL CHECKS PASSED.` 与退出码 `0` 结束。

### 模型上下文协议（MCP）AI 自动化游玩与测试服务
NewPal 原生内置标准 JSON-RPC 2.0 stdio MCP 服务端，供各类 AI 编码与自主代理直接控制游戏、寻路探索与自动化测试：
```json
{
  "mcpServers": {
    "newpal": {
      "command": "/Applications/NewPal.app/Contents/MacOS/NewPal",
      "args": ["--mcp-server"]
    }
  }
}
```

### 独立分发 DMG 镜像一键打包
如需将编译产物与所有外部动态链接库完全内置打包为开箱即用的 `.dmg` 磁盘镜像（含自动重写 `@rpath`、清除 Homebrew 依赖泄漏与 Ad-hoc 签名）：
```bash
./Tools/package_dmg.py
```
生成文件位于：`build/NewPal-1.2.0.dmg`。

---

## 法律与商标声明

- **独立兼容性项目**：本项目为旨在实现跨平台兼容性与历史软件保存的独立洁净室（clean-room）开源运行时，与大宇资讯（Softstar Entertainment）、软星科技及原著作权人无任何从属、合作或官方授权关系。
- **不含受版权保护的游戏资产**：本仓库代码按 GPL-3.0 协议（[LICENSE](LICENSE)）开源，不包含任何受版权保护的原版游戏程序、美术素材、音乐音效、视频或剧本文件，运行时须由用户自行提供合法取得的正版 `PalGame` 数据。
- **商标归属**：《新仙劍奇俠傳》（*Sword and Fairy*）及相关角色、道具名称与商标均归各自原权利人所有。
