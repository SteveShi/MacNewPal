---
slug: stack
title: Tech stack
role: tech-stack choices
updated: "2026-11-23T12:58:31"
---

# Tech stack

| 领域 | 选型 | 理由与约束 |
| --- | --- | --- |
| 应用层 / UI | Swift 6.0 (`SWIFT_STRICT_CONCURRENCY: complete`) + SwiftUI | macOS 14.0+ 原生体验，严格并发检查，零第三方 Swift 依赖 |
| 引擎核心 | C11 (`-Wall -Wextra`) | 零 C++ 运行时开销，直接操作原始二进制结构，经 Bridging Header 直连 Swift；详见 [[c11-core-and-sdl3-metal-pipeline]] |
| 窗口 / 渲染 / 手柄 | `libSDL3` (Homebrew) + Apple Metal + MSL 着色器 | 两遍渲染（离屏 `scene_rt` → `xBRZ`/`CRT-Royale` → Retina UI）与原生手柄热插拔 |
| 视频与音频流 | FFmpeg (`libavformat`, `libavcodec`, `libswscale`, `libswresample`, `libavutil`) | 统一内存解码 `.bik`（Bink）与 `.mp3`/`.wav` BGM，重采样至 44.1kHz S16 PCM |
| 文字与编码 | macOS `CoreText` / `CoreGraphics` / `CoreFoundation` + `libiconv` | `CP950 → UTF-8` 转码、运行时繁简切换与 Retina 级富文本排版 |
| 截屏编码 | macOS `ImageIO` | MCP `pal_capture_screen` 直接输出 RGBA → PNG，无额外图像库依赖 |
| 工程生成与构建 | `XcodeGen` (`project.yml`) + `xcodebuild` (`arm64`) | 声明式维护工程配置，`NewPal.xcodeproj` 不入版本控制 |
| 分发打包 | `Tools/package_dmg.py` + `otool`/`install_name_tool` + `codesign` + `create-dmg` | 自动内嵌 SDL3/FFmpeg 动态库闭包并重写 `@rpath`，当前为 ad-hoc 签名；详见 [[standalone-dmg-and-unsigned-release]] |
| AI 自动化协议 | 原生 stdio JSON-RPC 2.0 MCP + Unix Domain Socket | 16 个工具覆盖启动、状态查询、操控、QA 与截屏；详见 [[mcp-native-server-and-headless-playtest]] |
| 离线格式研究 | Python 3 + `Tools/_lzo_accel.c` | 仅用于本地只读结构分析与文档校对（`docs/FORMAT_SPEC.md`） |
