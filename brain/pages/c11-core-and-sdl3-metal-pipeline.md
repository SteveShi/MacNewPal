---
id: c11-core-and-sdl3-metal-pipeline
title: "C11 核心引擎、SDL3 Metal 两遍后处理与 CoreText 富文本渲染"
category: decision
status: active
tags: [engine, c11, sdl3, metal, shader, coretext]
created: "2026-11-23T12:56:39"
updated: "2026-11-23T12:57:42"
---

<!-- compiled_truth -->
## 决策结论

采用 **Swift 6.0 (SwiftUI 启动器/MCP 宿主) + C11 (单核引擎与子模块) + SDL3 Metal 渲染管线 + CoreText 原生中文渲染** 的分层架构，而非 C++ 重型框架、OpenGL 或原版点阵字库。

## 关键架构选择与原因

1. **C11 单体核心 + 职责清晰的 C 子模块**：
   - `PalEngineCore.c`（~24K 行）集中管理 VFS、状态机、地图、Talk VM、战斗与 UI，避免过度抽象带来的跨模块生命周期开销。
   - 可独立演进的硬件/测试边界拆分为独立模块：`PalGpuFilter.{h,c}`（Metal MSL 着色器）、`PalGamepad.{h,c}`（SDL3 手柄热插拔与映射）、`PalMcpBridge.{h,c}`（Unix Socket MCP 桥接）、`PalTestMode.{h,c}`（`PAL_ENABLE_TEST_MODE` QA 测试模块）。
   - 通过 `NewPal-Bridging-Header.h` 与 Swift 6.0 (`SWIFT_STRICT_CONCURRENCY: complete`) 零胶水成本互通。
2. **SDL3 Metal 两遍渲染管线（`PalGpuFilter.c`）**：
   - **Pass 1（游戏画面）**：将原版 `640×480` 场景渲染到离屏 `scene_rt`，消除 Metal 场景切换黑屏/撕裂（commit `1fb2f8d`）；再通过内嵌 Metal Shading Language（MSL）将 `scene_rt` 放大绘制到窗口 swapchain，支持 `Classic`、`Metal xBRZ`（边缘保持高清化）与 `CRT-Royale`（扫描线/栅格/泛光）。
   - **Pass 2（Retina UI 叠加）**：清除 GPU 像素着色器后，以原生 Retina 分辨率（`1×..4×`）叠加 CoreText 对话框、营地菜单、商店、HUD 与战斗文字，避免后处理滤镜把汉字糊化。
3. **CoreText + `libiconv` 替代原版点阵字库**：
   - 原版文本采用 Big5 (`CP950`) 编码并内嵌 `%N` / `%C0..%C9` / `%S` / `%D` / `%G` / `%K` 富文本控制符。
   - 使用系统 `libiconv` 转为 UTF-8，配合 `CoreText` / `CoreGraphics` 渲染带投影的高清字形纹理；并内置运行时简体中文模式（`text_language: 0=繁体 / 1=简体`，commit `4af2f56`），同时严格清理了地图横幅/商店/战斗日志中的调试 ID 后缀（commit `a00cb4e`）。
4. **FFmpeg 统一流媒体解码**：
   - `libavformat`/`libavcodec`/`libswscale`/`libswresample` 统一解码 Bink 视频（`.bik`）与 BGM（`.mp3`/`.wav`），统一重采样至 44.1 kHz 立体声 S16 PCM，并实现场景切换交叉淡入淡出与战斗胜利音量闪避（ducking）。

## 影响范围

- 运行平台锁定 macOS 14.0+ (`arm64`)，不维护 Intel / OpenGL 兼容分支。
- 独立分发必须使用 [[standalone-dmg-and-unsigned-release]] 打包 Homebrew 的 `libSDL3` 与 FFmpeg 依赖闭包。


## Timeline

- time: 2026-11-23T12:56:39
  kind: decision
  summary: "Created this page: C11 核心引擎、SDL3 Metal 两遍后处理与 CoreText 富文本渲染"
  source: project.yml / Sources/Engine/PalGpuFilter.c / Sources/Engine/PalEngineCore.c
  affects: [c11-core-and-sdl3-metal-pipeline]

- time: 2026-11-23T12:57:42
  kind: decision
  summary: "沉淀 C11 引擎选型、SDL3 Metal 两遍后处理与 CoreText 繁简渲染架构决策"
  source: "project.yml / Sources/Engine/PalGpuFilter.c / commit cf77c7c & 4af2f56"
  affects: [c11-core-and-sdl3-metal-pipeline]
