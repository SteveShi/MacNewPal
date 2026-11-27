---
id: standalone-dmg-and-unsigned-release
title: "v1.0.0 独立 DMG 打包、无签名分发基线与快速恢复指南"
category: project
status: active
tags: [release, dmg, packaging, gatekeeper, v1.0.0]
created: "2026-11-23T12:56:39"
updated: "2026-11-27T20:30:01"
---

<!-- compiled_truth -->
## 当前发布状态（基线：`v1.2.2`）

- **仓库**：`https://github.com/SteveShi/MacNewPal`（默认分支 `main`，私仓）
- **Release Tag**：`v1.2.2`（commit `c4a9316`，包含启动器界面重排与打开存档目录快捷入口 `8291126`、`v1.2.1` 托管式 BYOD 导入及 `v1.2.0` 全引擎对齐基线）
- **Release 链接**：`https://github.com/SteveShi/MacNewPal/releases/tag/v1.2.2`
- **分发产物**：`NewPal-1.2.2.dmg`（19,148,479 bytes / 18.26 MB）
  - SHA-256：`8d4e06745fa9ff7d391d0de5b1853adc6b15bc75f506f6227c18ed9bcd6726d8`

## 文档语言与命名规范

- **用户与项目说明文档**以简体中文为主文件、英文使用 `*.en_US.md` 并互相在顶部链接：`README.md` ↔ `README.en_US.md`、`docs/TEST_MODE.md` ↔ `docs/TEST_MODE.en_US.md`。
- **逆向技术规格文档** `docs/FORMAT_SPEC.md` 保持英文原版，不额外维护中文译本。

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
   - 自动读取 `project.yml` 的 `MARKETING_VERSION`（当前 `1.2.2`，`CURRENT_PROJECT_VERSION = 6`）→ `xcodebuild Release` → 递归分析 `otool -L` 拷贝全部 Homebrew `SDL3` + `FFmpeg` 动态库闭包到 `Contents/Frameworks/` → `install_name_tool` 重写为 `@rpath` 并移除本地 Homebrew rpath → 全量 ad-hoc 签名（`codesign -s -`）→ `create-dmg` 生成带自定义图标的 `build/NewPal-1.2.2.dmg`。
   - **注意**：升级版本时需同步更新 `project.yml`（`MARKETING_VERSION` / `CURRENT_PROJECT_VERSION`）、`Tools/package_dmg.py`（兜底版本号）、`README.md` / `README.en_US.md` 与 `Sources/App/Resources/Localizable.xcstrings` 中的 `whatsnew.*`。

## 无 Apple Developer ID 签名与 Gatekeeper 约束

- 当前采用 ad-hoc 签名（`CODE_SIGN_IDENTITY: "-"`，`ENABLE_APP_SANDBOX: NO`），首次打开会被 macOS Gatekeeper 拦截。
- `README.md`、`README.en_US.md` 与 GitHub Release Notes 已统一提供 3 种标准启动方式：
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

- time: 2026-11-27T10:35:25
  kind: decision
  summary: "更新发布基线至 v1.2.0（commit b5f8ee9 / NewPal-1.2.0.dmg）"
  source: Tools/package_dmg.py / GitHub Release v1.2.0 / commit b5f8ee9
  affects: [standalone-dmg-and-unsigned-release]

- time: 2026-11-27T14:30:24
  kind: decision
  summary: "更新发布基线至 v1.2.1（commit 4e78758 / NewPal-1.2.1.dmg）"
  source: Tools/package_dmg.py / GitHub Release v1.2.1 / commit 4e78758
  affects: [standalone-dmg-and-unsigned-release]

- time: 2026-11-27T14:47:34
  kind: decision
  summary: "更新发布基线至 v1.2.2（commit c4a9316 / NewPal-1.2.2.dmg）"
  source: Tools/package_dmg.py / GitHub Release v1.2.2 / commit c4a9316
  affects: [standalone-dmg-and-unsigned-release]

- time: 2026-11-27T20:30:01
  kind: decision
  summary: "更新文档命名规范：说明文档以简体中文为主、英文使用 *.en_US.md，逆向规格文档 docs/FORMAT_SPEC.md 保持英文"
  source: README.md / README.en_US.md / docs/TEST_MODE.md / docs/TEST_MODE.en_US.md
  affects: [standalone-dmg-and-unsigned-release]
