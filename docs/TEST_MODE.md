# NewPal QA 测试模式模块说明

[简体中文](TEST_MODE.md) | [English](TEST_MODE.en_US.md)

**QA 测试模式**用于辅助体验与测试《新仙剑奇侠传》全流程（包含人物无敌、免遇敌切换、战斗一键秒杀、入战自动秒杀、满血满蓝、+5 万文、全员升级、物资补给、移动倍速、穿墙、跳过等待及快速存读档）。本模块采用**独立可裁剪架构**设计，由单一编译开关 `PAL_ENABLE_TEST_MODE` 控制，正式发布时可零残留关闭或移除。

---

## 1. 模块组成与隔离边界

测试模式逻辑与主引擎、战斗公式、UI 启动器完全解耦，仅通过极窄回调接口与内联桩函数挂接：

| 文件路径 | 职责 | `PAL_ENABLE_TEST_MODE == 0` 时的表现 |
| :--- | :--- | :--- |
| `Sources/Engine/PalTestMode.h` | 公开配置结构 `PalTestConfig`、宿主回调表 `PalTestHostOps`、运行时状态 `PalTestState` | 退化为空结构体与零开销 `static inline` 空桩函数 |
| `Sources/Engine/PalTestMode.c` | 游戏内悬浮状态胶囊、13 项交互式工具箱面板、快捷键处理、Toast 提示与 CoreText 纹理缓存 | 整文件经 `#if PAL_ENABLE_TEST_MODE` 跳过编译 |
| `Sources/App/TestModeSection.swift` | SwiftUI 启动器卡片 `TestModeLauncherGroupBox`、设置页分区 `TestModeSettingsSection` 与 `UserDefaults` 读取 | 整文件经 `#if PAL_ENABLE_TEST_MODE` 跳过编译 |
| `Sources/Engine/PalEngineCore.c` | 仅含 `PalTestBridgeCtx` 桥接回调与 6 处轻量钩子（HP/MP 过滤、异常状态过滤、免遇敌拦截、倍速、悬浮叠加层） | 桥接回调全量编译裁剪 |
| `project.yml` | 全局开关：`GCC_PREPROCESSOR_DEFINITIONS` 中的 `PAL_ENABLE_TEST_MODE=1` 与 `SWIFT_ACTIVE_COMPILATION_CONDITIONS` 中的 `PAL_ENABLE_TEST_MODE` | 修改此处即可整体关闭模块 |

### 自检隔离与存档安全

- `--self-test` 无头自检流程**不会**调用 `pal_test_bind_runtime`，所有战斗公式、异常状态免疫、升级与 AI 回归校验均在纯净原始环境下执行，不受测试模式开关干扰。
- 快速存读档固定操作 **Slot 9（`Save/9.sav`）**，不会覆盖玩家正常的 `0..8` 存档槽。

---

## 2. 正式发布时如何取消或移除本模块

### 方式一：单开关零残留关闭（推荐）

打开 `project.yml`，将 `PAL_ENABLE_TEST_MODE` 移除或设为 `0`：

```yaml
SWIFT_ACTIVE_COMPILATION_CONDITIONS: "$(inherited)"
GCC_PREPROCESSOR_DEFINITIONS: "$(inherited) PAL_ENABLE_TEST_MODE=0"
```

重新生成工程并编译 Release：

```bash
xcodegen generate
xcodebuild -project NewPal.xcodeproj -scheme NewPal -configuration Release -derivedDataPath build clean build
```

关闭后：
- `PalTestMode.c` 与 `TestModeSection.swift` 不生成任何可执行代码或 UI 入口；
- `PalLaunchConfig` 自动剔除 `test_cfg` 字段；
- 主循环中的测试模式钩子全部由编译器内联消除。

### 方式二：从代码库物理删除

直接删除 `Sources/Engine/PalTestMode.h`、`Sources/Engine/PalTestMode.c` 与 `Sources/App/TestModeSection.swift`，并移除 `PalEngineCore.h`、`PalEngineCore.c`、`PalLauncherStore.swift`、`ContentView.swift`、`SettingsView.swift` 和 `project.yml` 中标注 `PAL_ENABLE_TEST_MODE` 的挂载片段即可。

---

## 3. 功能清单与快捷键

只要编译时开启了 `PAL_ENABLE_TEST_MODE=1`，即便启动前未在启动器中勾选，进入游戏后也可随时按 `F12` 或 `Option + ~` 唤醒工具箱。

| 序号 | 功能 | 机制说明 | 快捷键 / 触发方式 |
| :-: | :--- | :--- | :--- |
| 1 | **测试模式总开关** | 运行时随时启用或隐藏测试状态胶囊与面板 | `F12`（`Option + ~` 亦可唤醒并展开面板） |
| 2 | **QA 测试工具箱** | 在场景/战斗画面右上角显示胶囊状态条，点击或按键展开 13 项面板（支持鼠标悬停点击与键盘触发） | `~` / `F11` / 点击右上角 `[測試工具箱 ~]` |
| 3 | **人物无敌（God Mode）** | 在队角色免疫 HP/MP 扣减（敌方攻击、毒素 DOT、合击耗血）、免疫负面状态位（毒/昏睡/混乱/眩晕/封魔/降抗）、仙术免耗蓝、且不会判定全军覆没 | `F2` / 面板 `[1]` / 启动器开关 |
| 4 | **免遇敌（No Encounter）** | 屏蔽大地图漫游怪追击（`pal_npc_ai_step`）与触碰自动入战；剧情必经战斗与按 `F` 主动切磋照常工作 | `F3` / 面板 `[2]` / 启动器开关 |
| 5 | **战斗一键秒杀** | 击杀所有存活敌方并播放原版死亡动画与音效，按正常战斗流程发放经验、金钱、掉落物、升级并置位 `story_flags[3001] = 1` | `K`（战斗中）/ `F4` / 面板 `[3]` |
| 6 | **入战自动秒杀** | 开启后，每次进入战斗首帧自动触发一键秒杀并进入胜利结算流程 | 面板 `[4]` / 启动器开关 |
| 7 | **全队满状态回血回蓝** | 将当前在队全员 HP/MP 回满并清空所有战斗异常状态 | `F5` / 面板 `[5]` |
| 8 | **测试金钱 +50,000 文** | 立即增加 `50,000` 文用于商店测试 | `F6` / 面板 `[6]` |
| 9 | **全员立即升 1 级** | 在队成员依 `glevel.dat` 成长模板同步升 1 级，更新面板属性并解锁对应仙术 | `F7` / 面板 `[7]` |
| 10 | **测试物资补给** | 发放 `gitem.dat` 核验道具：`4401`（止血草 ×20）、`4403`（行軍丹 ×20）、`4405`（金創藥 ×10）、`4301`（鼠兒果 ×20）、`4357`（蜂王蜜 ×10）、`4251`（還魂香 ×10）、`4254`（天香續命露 ×5）、`3613`（蜂巢 ×10）、`3702`（血玲瓏 ×5） | 面板 `[8]` |
| 11 | **大地图移动倍速** | 在 `1×` / `2×` / `3×` 间循环切换大地图移动步速 | `F8` / 面板 `[9]` / 启动器分段选择 |
| 12 | **穿墙模式（NoClip）** | 切换是否无视地图可行走碰撞格 | `F9` / 面板 `[0]` |
| 13 | **跳过剧情等待 / 淡入淡出** | 清空 `talk_vm.wait_ticks`、淡入步骤与镜头平移动画，快速推进长段演出 | `F10` / 面板 `[-]` |
| 14 | **快速存/读档（Slot 9）** | 使用专属测试槽位 `Save/9.sav` 快速保存或加载进度（面板展开时大地图有效） | 面板 `S`（存）/ `L`（读） |
