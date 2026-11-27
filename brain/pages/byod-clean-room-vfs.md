---
id: byod-clean-room-vfs
title: "BYOD 版权隔离、23 项签名校验与只读目录存档回退"
category: decision
status: active
tags: [architecture, byod, vfs, legal, save]
created: "2026-11-23T12:56:39"
updated: "2026-11-27T14:19:43"
---

<!-- compiled_truth -->
## 决策结论

MacNewPal 严格采用 **Clean-Room + BYOD (Bring Your Own Data)** 架构：仓库与分发产物 `NewPal.app` 中**零版权素材**（不存放任何原版图像、音频、脚本、视频、地图或数据库文件），运行时由用户通过安全作用域目录选择器指定本地正版 `PalGame` 目录，经内存级虚拟文件系统（VFS）按需解包流式读取。

## 核心约束与实现

1. **仓库与构建隔离**：`.gitignore` 显式忽略 `*.dat`、`*.tsw`、`*.act`、`*.snd`、`*.lmf`、`*.mp3`、`*.wav`、`*.bik`、`*.exe`、`*.dll` 及逆向导出目录（`extracted/`、`db_export/`、`manifests/`、`samples/`），项目代码以 GPL-3.0 协议开源。
2. **启动前 23 项签名门禁**：`PalLauncherStore.swift` 调用 C 引擎校验逻辑，在允许启动前验证 `PalGame` 目录中全部 **23** 个核心容器与数据库二进制签名（`all_*.tsw` / `all_*.act` / `all.snd` 的 `0x1C` 头与 3000-slot TOC、`huge.lmf` / `huge2.lmf`、`MZ + 0x200` 头的 `ghero.dat` / `gmon.dat` / `gitem.dat` / `gmagic.dat` / `glevel.dat` / `gmap.dat` / `GFight1..4.dat` / `maps.dat` / `namex.dat` / `talk1..8.dat`、`Video/begin.bik`、`Video/notice.bik` 等）。
3. **纯内存按需解码**：LZO1X、ARLE (`0xFFFF`)、`.tsw` RGB555 透明色键（`0x319F` / `0x026B`）、`all.snd` PCM/RIFF 与 `.bik`/`.mp3`/`.wav` 均直接从原始容器读入内存并流式送入 Metal/AudioStream，不向磁盘释放解包文件。
4. **只读 BYOD 目录的存档回退**：当用户将 `PalGame` 放在只读卷或只读目录时，存档引擎自动回退写入 macOS `Application Support` 目录，保证 10 槽位存档（`Save/0..9.sav`）不因目录权限失败（见 [[talk-vm-and-world-state-closure]]）。

## 备选方案与权衡

- **拒绝预转换/预解包资源管线**：虽然预转换为 PNG/JSON 可简化引擎代码，但会破坏原版磁盘结构、产生大量中间文件并增加版权分发风险；保持原版容器直读是唯一合规且零冗余的长期方案。


## Timeline

- time: 2026-11-23T12:56:39
  kind: decision
  summary: "Created this page: BYOD 版权隔离、23 项签名校验与只读目录存档回退"
  source: README.md / Sources/App/PalLauncherStore.swift / Sources/Engine/PalEngineCore.c
  affects: [byod-clean-room-vfs]

- time: 2026-11-23T12:57:42
  kind: decision
  summary: "沉淀 BYOD 版权合规边界、23 文件二进制签名校验及只读目录存档回退策略"
  source: README.md / .gitignore / commit 966bc9f
  affects: [byod-clean-room-vfs]

- time: 2026-11-27T14:01:15
  kind: decision
  summary: "变更 BYOD 数据包导入方式：支持导入复制至 macOS 标准目录 ~/Library/Application Support/NewPal/PalGame，并在启动器与设置页添加快捷打开 Application Support 文件夹按钮"
  source: Sources/App/PalLauncherStore.swift / Sources/App/ContentView.swift / Sources/App/SettingsView.swift
  affects: [byod-clean-room-vfs]

- time: 2026-11-27T14:19:43
  kind: decision
  summary: "重构托管式 BYOD 导入与清理流程：严格预检 23 项签名后仅选择性复制所需数据至 ~/Library/Application Support/NewPal/PalGame，统一且唯一从该标准目录读取，新增主动清理游戏数据功能，并将 pal_save_game_slot 调整为优先写入 ~/Library/Application Support/NewPal/Save 以确保存档隔离"
  source: Sources/App/PalLauncherStore.swift / Sources/App/ContentView.swift / Sources/App/SettingsView.swift / Sources/Engine/PalEngineCore.c
  affects: [byod-clean-room-vfs]
