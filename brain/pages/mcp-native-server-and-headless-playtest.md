---
id: mcp-native-server-and-headless-playtest
title: "原生 stdio-to-Unix-Socket MCP 服务器与全 8 章无头自动化测试"
category: decision
status: active
tags: [mcp, testing, automation, headless, playthrough]
created: "2026-11-23T12:56:39"
updated: "2026-11-24T14:28:00"
---

<!-- compiled_truth -->
## 决策结论

在 `NewPal.app` 内直接集成 **原生 Model Context Protocol (MCP) 服务器**（`--mcp-server` 启动参数），采用 **Swift stdio JSON-RPC 2.0 宿主 ↔ Unix Domain Socket (`/tmp/newpal-mcp.sock`) ↔ C 引擎非阻塞主循环桥接** 的双进程架构，使 AI 客户端（Claude Desktop / Antigravity / Cursor / Codex）可直接启动、观测、操控并截屏验收真实游戏运行态。

## 架构与协议边界

1. **为什么拆分为 stdio 宿主 + Unix Socket 引擎**：
   - MCP 客户端要求长期保持干净的 `stdin`/`stdout` JSON-RPC 管道，而 SDL3/AppKit 游戏窗口进程会输出底层日志且生命周期随 `pal_launch_game` / `pal_close_game` 启停。
   - `PalMcpServer.swift` 独占 stdio，按需拉起或附着带 `--mcp-socket` 的游戏子进程，并在收到 `SIGINT`/`SIGTERM` 或 EOF 时自动优雅退出子进程并清理 socket。
   - `PalMcpBridge.c` 在游戏主循环内以非阻塞 `accept`/行缓冲轮询命令，支持异步等待原语（`WAIT_MOVE` / `WAIT_COMBAT` / `WAIT_TALK` / `WAIT_FRAMES` / `WAIT_CAPTURE`），截屏通过 `ImageIO` 编码为 PNG 返回。
2. **16 个 MCP 工具清单**：
   - 生命周期与观测：`pal_launch_game`、`pal_close_game`、`pal_get_state`、`pal_get_map_entities`、`pal_get_inventory_and_saves`、`pal_capture_screen`、`pal_query_flags`。
   - 游戏操控：`pal_title_action`、`pal_move_to`、`pal_interact`、`pal_choose_dialogue`、`pal_combat_action`、`pal_manage_party`、`pal_raw_input`。
   - QA 与调试门控（受 `mcp_allow_qa_tools` 开关控制）：`pal_test_control`、`pal_teleport`。
3. **Launcher 集成与无头开关**：
   - `SettingsView.swift` 与 `PalLauncherStore.swift` 提供 MCP 服务启用开关与无头模式开关（`headless_mode`：隐藏窗口、快进帧步进、自动跳过 `.bik` 视频与渐变等待），无空壳设置项。

## 全 8 章自动化通关验收记录（v1.0.0 基线）

- 使用 `pal_launch_game(headless=true, language="sc", speed_mult=3, god_mode=true, no_encounter=true, auto_instant_kill=true)` 完成全流程验证：
  - **Ch1**：余杭 (`Map 1`) → 梦境罗刹峰/罗刹洞外 (`501`/`502`，Boss `2801` 罗刹鬼婆) → 仙灵岛水月宫 (`Map 31`)
  - **Ch2**：苏州比武招亲擂台 (`Map 49`) → 隐龙窟大殿 (`Map 100`)
  - **Ch3**：玉佛寺 (`Map 78`) → 血池底 (`Map 228`)
  - **Ch4**：扬州城夜 (`Map 143`) → 蛤蟆山 (`Map 147`)
  - **Ch5**：黑蜘蛛森林/蝶恋 (`Map 185`) → 蜀山仙剑派 (`Map 187`)
  - **Ch6**：锁妖塔八/九层 (`Map 202`/`203`) → 锁妖塔剑柱 (`Map 351`)
  - **Ch7**：圣姑家内 (`Map 241`) → 神木林 (`Map 412`，Boss `2839` 水魔兽)
  - **Ch8**：女娲祭坛 (`Map 275`) → 无底深渊 (`Map 620`) → `talk 1439` 结局归零回标题画面（`OP 0x90` `request_return_title`）
- 结束时 `pal_close_game` 正常退出且 socket 自动清理，输出 19 张 `1280×960` 验收截图（详见对话产物 `full_playthrough_report.md`）。

## 关联页面

- [[talk-vm-and-world-state-closure]]
- [[combat-and-map-engine-fidelity]]


## Timeline

- time: 2026-11-23T12:56:39
  kind: decision
  summary: "Created this page: 原生 stdio-to-Unix-Socket MCP 服务器与全 8 章无头自动化测试"
  source: Sources/App/PalMcpServer.swift / Sources/Engine/PalMcpBridge.c / commit 46fbdd0
  affects: [mcp-native-server-and-headless-playtest]

- time: 2026-11-23T12:57:42
  kind: decision
  summary: "记录原生 MCP 桥接协议、16 工具集、无头模式与 v1.0.0 全 8 章通关验证结果"
  source: Sources/App/PalMcpServer.swift / Sources/Engine/PalMcpBridge.c / commit 46fbdd0
  affects: [mcp-native-server-and-headless-playtest]

- time: 2026-11-24T14:28:00
  kind: evidence
  summary: "完成零作弊全流程第三章（白河村→玉佛寺→黑水镇/将军冢/血池底击败赤鬼王→鬼阴山/鬼阴坛触发石长老必败战与赵灵儿离队→鬼阴山麓抵达 Map 121 扬州城郊外），里程碑存档写入 Save/6.sav"
  source: Save/6.sav / ch3_walkthrough_report.md
  affects: [mcp-native-server-and-headless-playtest]
