---
slug: background
title: Project background
role: project background
updated: "2026-11-23T12:58:31"
---

# Project background

**MacNewPal**（`NewPal`，Bundle ID：`com.steveshi.newpal`）是由 **Steve Shi（轩楝）** 开发的《新仙剑奇侠传》（`SWD2k` 引擎）**Clean-Room 原生 macOS（Apple Silicon `arm64`）运行时与启动器**，采用 GPL-3.0 开源协议与零版权素材的 **BYOD（Bring Your Own Data）** 架构。

## 核心目标

1. **原生 Apple Silicon 高还原运行**：无需 Windows 虚拟机、Wine 或兼容层，在 macOS 14.0+ 上以原生 SDL3 Metal 流畅运行原版 `PalGame` 数据并完整通关全 8 章主线与结局彩蛋（详见 [[talk-vm-and-world-state-closure]]、[[combat-and-map-engine-fidelity]]）。
2. **版权安全与零侵入**：仓库与 `NewPal.app` 不包含任何原版版权素材，启动前严格执行 23 项二进制签名校验，只读目录下自动回退存档（详见 [[byod-clean-room-vfs]]）。
3. **现代高清化与交互体验**：提供 Metal `xBRZ` / `CRT-Royale` 后处理着色器、Retina 清晰 CoreText 繁简中文字体渲染、SDL3 手柄支持与原生 SwiftUI 配置界面（详见 [[c11-core-and-sdl3-metal-pipeline]]）。
4. **AI 原生可测性**：内置标准 stdio MCP 服务器（16 工具）与无头高速仿真模式，让 AI 代理能够直接操控游戏、截屏验收与回归测试（详见 [[mcp-native-server-and-headless-playtest]]）。

## 非目标

- **不分发版权游戏数据**：绝不将原版 `.dat`/`.tsw`/`.act`/`.lmf`/`.bik`/`.mp3` 等资源纳入仓库或打包进 DMG。
- **不维护 Intel / OpenGL 旧栈**：仅面向 Apple Silicon (`arm64`) 与 Metal 现代图形管线。
- **不做非正式魔改引擎**：优先忠实还原 `SWD2k` 原版数值、脚本与剧情闭环，在此基础上提供可选画质增强与 QA 工具。

## 目标用户

- 拥有正版《新仙剑奇侠传》`PalGame` 数据、希望在 Apple Silicon Mac 上开箱即玩的玩家。
- 关注经典中文 RPG 引擎逆向、二进制格式文档（`docs/FORMAT_SPEC.md`）与 AI 游戏测试（MCP）的开发者。
