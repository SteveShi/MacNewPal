---
slug: mindmap
title: Feature mindmap
role: feature mindmap
updated: "2026-11-23T12:58:31"
---

# Feature mindmap

```mermaid
mindmap
  root((MacNewPal))
    原生应用层 Swift 6
      BYOD 目录选择与 23 项签名校验
      设置项实装 缩放/着色器/高清/繁简/音频/MCP/无头
      中英双语 Localizable.xcstrings
      Help 菜单 WhatsNewSheetView
    C11 引擎核心
      内存 VFS 与解码 LZO1X/ARLE/CP950/FFmpeg
      地图系统 MSF3/maps.dat/3桶Y排序/A*/4人跟队
      剧情系统 Talk VM 8 Bank 7394脚本/开场100/结局1439
      战斗系统 GFight 88指令/0..74公式/160位状态/合體技
      营地与存档 4页签菜单/商店/10槽位0x6A54+NPAL
    现代体验增强
      Metal MSL 两遍后处理 xBRZ 与 CRT-Royale
      Retina CoreText 富文本与运行时简体模式
      BGM 交叉淡入淡出与胜利闪避
      SDL3 手柄热插拔与键位映射
    测试与发布工程
      内置 --self-test 全链路回归
      QA 测试模块 PAL_ENABLE_TEST_MODE
      原生 MCP 服务器 16工具与无头全8章通关
      独立 DMG 打包 package_dmg.py 与 v1.0.0 发布
```

核心专题索引：[[byod-clean-room-vfs]] · [[c11-core-and-sdl3-metal-pipeline]] · [[talk-vm-and-world-state-closure]] · [[combat-and-map-engine-fidelity]] · [[mcp-native-server-and-headless-playtest]] · [[standalone-dmg-and-unsigned-release]]
