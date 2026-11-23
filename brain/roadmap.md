---
slug: roadmap
title: Roadmap
role: milestones
updated: "2026-11-23T12:58:31"
---

# Roadmap

M0–M9 全部既定里程碑已实现，并已于 `v1.0.0`（commit `46fbdd0`）发布首个独立 DMG 版本。

```mermaid
gantt
    title MacNewPal 里程碑与发布进度
    dateFormat YYYY-MM-DD
    axisFormat %m-%d
    section 已完成核心引擎 (M0-M7)
    M0 启动器/VFS 23项校验/LZO1X & ARLE        :done, m0, 2026-11-13, 1d
    M1 Bink/BGM/.act VM/CoreText/标题与开场     :done, m1, 2026-11-14, 1d
    M2 MSF3地图/3桶排序/8向移动/A*/跟队         :done, m2, 2026-11-15, 1d
    M3 Talk VM 7394脚本/半身像/触发器与传送     :done, m3, 2026-11-16, 1d
    M4 0x7F数据库/营地菜单/升级装备             :done, m4, 2026-11-17, 1d
    M5 回合制战斗/商店/10槽位存档               :done, m5, 2026-11-18, 1d
    M6 160位状态/合體技/GFight公式/死亡动画     :done, m6, 2026-11-19, 1d
    M7 结局闭环 talk 1439/只读目录存档回退      :done, m7, 2026-11-19, 1d
    section 体验打磨与 AI 生态 (M8-M9 & v1.0.0)
    M8 Metal xBRZ & CRT/Retina UI/BGM淡变/手柄  :done, m8, 2026-11-20, 1d
    M9 原生 MCP 16工具/无头全8章通关/v1.0.0 DMG :done, m9, 2026-11-21, 1d
```

## 已交付基线（`v1.0.0`）

- **M0–M9 全部完成**：详见 [[standalone-dmg-and-unsigned-release]] 与 [[mcp-native-server-and-headless-playtest]]。
- **GitHub Release**：`v1.0.0` 已发布独立 `NewPal-1.0.0.dmg`（内嵌 SDL3/FFmpeg 依赖、无签名 Gatekeeper 引导文档与中英双语 README）。

## 后续方向（待定 / 需用户确认）

以下方向尚未立项，待后续会话按需确认优先级：
- [ ] **开发者签名与公证（Notarization）**：如未来申请 Apple Developer ID，可替换 ad-hoc 签名并接入 Sparkle 自动更新。
- [ ] **端到端自然剧情深度走查**：借助现有 MCP 16 工具，进一步做不跳剧情的逐章纯手工/AI 走查，排查长流程支线与边角脚本细节。
- [ ] **GitHub Actions CI/CD**：自动化 `xcodegen` + 编译构建与 Release 打包（当前为本机手动执行）。
