---
id: standalone-dmg-and-unsigned-release
title: "v1.0.0 独立 DMG 打包、无签名分发基线与快速恢复指南"
category: project
status: active
tags: [release, dmg, packaging, gatekeeper, v1.0.0]
created: "2026-11-23T12:56:39"
updated: "2026-11-23T12:57:42"
---

<!-- compiled_truth -->
## 当前发布状态（基线：`v1.0.0`）

- **仓库**：`https://github.com/SteveShi/MacNewPal`（默认分支 `main`，私仓）
- **Release Tag**：`v1.0.0`（commit `46fbdd0`，工作区干净，M0–M9 里程碑全部完成）
- **Release 链接**：`https://github.com/SteveShi/MacNewPal/releases/tag/v1.0.0`
- **分发产物**：`NewPal-1.0.0.dmg`（18,835,268 bytes / ~17.96 MB）
  - SHA-256：`5241bf94b0fb161ef6b2f6dafa9b401dc08cfaece03d9ef9589f73206ac85a4c`

## 独立构建与打包流水线

1. **生成工程与编译**：
   ```bash
   xcodegen generate
   xcodebuild -project NewPal.xcodeproj -scheme NewPal -configuration Release -derivedDataPath build build
   ```
2. **自检回归（需本机 `PalGame` 数据目录，开发机位于 `/Users/steve/Downloads/PalGame`）**：
   ```bash
   ./build/Build/Products/Release/NewPal.app/Contents/MacOS/NewPal --self-test /Users/steve/Downloads/PalGame
   ```
   验收标准：输出 `Validation: 23/23 (is_valid=1)`、`7394/7394` Talk scripts、`[SelfTest] ALL CHECKS PASSED.` 且退出码为 `0`。
3. **独立 DMG 打包（`Tools/package_dmg.py`）**：
   - 自动 `xcodebuild Release` → 递归分析 `otool -L` 拷贝全部 Homebrew `SDL3` + `FFmpeg` 动态库闭包到 `Contents/Frameworks/` → `install_name_tool` 重写为 `@rpath` 并移除本地 Homebrew rpath → 全量 ad-hoc 签名（`codesign -s -`）→ `create-dmg` 生成带自定义图标的 `build/NewPal-1.0.0.dmg`。
   - **注意**：升级版本时需同步更新 `project.yml`（`MARKETING_VERSION` / `CURRENT_PROJECT_VERSION`）、`Tools/package_dmg.py`（`DMG_OUTPUT` 文件名）与 `WhatsNewSheetView.swift`。

## 无 Apple Developer ID 签名与 Gatekeeper 约束

- 当前采用 ad-hoc 签名（`CODE_SIGN_IDENTITY: "-"`，`ENABLE_APP_SANDBOX: NO`），首次打开会被 macOS Gatekeeper 拦截。
- `README.md`、`README.zh-CN.md` 与 GitHub Release Notes 已统一提供 3 种标准启动方式：
  1. Finder 右键（Control+点击）→ 打开（推荐）
  2. 系统设置 → 隐私与安全性 → 仍要打开
  3. 终端移除隔离属性：`xattr -cr /Applications/NewPal.app`

## 后续迭代快速恢复清单

- 代码改动后立即执行 `xcodebuild` 编译验证，并通过 `--self-test` + MCP 自动化测试回归（见 [[mcp-native-server-and-headless-playtest]]）。
- 新增用户可见文案统一写入 `Sources/App/Resources/Localizable.xcstrings`（禁止硬编码）。
- 重大版本更新须同步更新 Help 菜单中的 `WhatsNewSheetView.swift`（严禁放入 About 菜单）。


## Timeline

- time: 2026-11-23T12:56:39
  kind: decision
  summary: "Created this page: v1.0.0 独立 DMG 打包、无签名分发基线与快速恢复指南"
  source: Tools/package_dmg.py / GitHub Release v1.0.0 / commit 46fbdd0
  affects: [standalone-dmg-and-unsigned-release]

- time: 2026-11-23T12:57:42
  kind: decision
  summary: "记录 v1.0.0 发布产物、独立 DMG 打包流水线、无签名 Gatekeeper 引导与快速恢复指令"
  source: Tools/package_dmg.py / README.md / GitHub Release v1.0.0
  affects: [standalone-dmg-and-unsigned-release]
