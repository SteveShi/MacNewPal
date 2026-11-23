---
id: talk-vm-and-world-state-closure
title: "SWD2k Talk 字节码虚拟机、结局闭环与存档扩展架构"
category: concept
status: active
tags: [talk-vm, script, save, endgame, swd2k]
created: "2026-11-23T12:56:39"
updated: "2026-11-23T15:29:36"
---

<!-- compiled_truth -->
## 核心机制

`SWD2k` 的全部剧情演出、对话分支、宝箱开关、商店调用、场景切换与结局触发均由 **Talk 字节码虚拟机**（`PalTalkVM` + `PalTalkWorldState`）驱动，MacNewPal 在 `--self-test` 中已验证 **8 个 Bank 共 7,394 / 7,394 条脚本** 完整无错解码。

## 关键数据与指令设计

1. **多 Bank 分布**：`talk1..8.dat`（`talk1` 与 `talk4` 各 `701` 槽位，其余各 `1000` 槽位）。
2. **持久世界状态（`PalTalkWorldState`）**：
   - `8192` 个剧情布尔/整型标志位（`story_flags`）
   - `256` 个脚本变量 + 每对象寄存器 `obj_rv[PAL_OBJ_CAP][4]`（与渲染/动作子状态 `orig_sub_state` 严格分离：`OP 0x7E`/`0x7F` 读写真实 `sub_state`）
   - 全局 NPC 重定位与 `0x26`/`0x27`/`0x64` 状态持久化、宝箱状态切换（`OP 0x0A`）
   - **`OP 0x3E` 重定位布局**：`[obj, map_id, unk2, tx, ty, act_id, sub_state, dir]`（不覆盖 `npc_talk_override`；跨地图重定位时必须清除陈旧 `ws->npc_ai_override[obj] = 0xFFFFu`，并由 `pal_npc_ai_step` `case 8` 拦截越界路径点）
   - **`OP 0x1C` NPC AI 路径机**：驱动 `path*.dat`（opcodes `32, 34, 35, 39..44, 0xFFFF` 等）并持久化于 `ws->npc_ai_override`
   - `0xD8`/`0xD9` 半身像动画（经 `all_map1.act` → `all_map1.tsw`）、`%B` 对话分支选项、`OP 0x6B`/`0x9A` `skip_wait` 快进、以及战斗暂停期间的 Talk VM 竞态保护。
3. **`npc_flags & 0x20` 触碰/邻近触发体系（全量 4,117 名全局 NPC 审计结论）**：
   - `0x20` 本质是邻近/触碰触发，而非全图自动触发：`act=701` 仅在初次入图且 `npc_talk_override[obj_id] == 0` 时自动触发；`act=700` 事件区在入图（含动态重检 `auto_trig_recheck`）`72px` 内或行走中 `48px` 内触发；`act=702` 莲花台、`712` 机关、`704` 锁门等在行走中 `32px` 内触发（排除 `0x2000 && talk >= 5001` 敌怪）。
   - 配有 `last_touch_npc_obj`（`radius + 16px` 迟滞清除）与脚本结束同图落点 `<= 40px` 防往复 ping-pong 保护。
4. **`0x7F` 脚本联动与 `0x309` 分发**：
   - 物品/法术字段脚本、奖赏脚本（`gmagic.dat` `5001..5319`）与商店列表（`58000+`）通过统一 `0x7F` 执行器与 `0x309` 指令桥接；`pal_camp_use_item_direct` + `camp_dispatch_talk_if_any` 支持任务/剧情道具（如破天鎚、包袱、手卷）触发 Talk 脚本。
   - 已修复野外物品触发 Talk 脚本的 `0x7F102046` 分发（commit `1999c9b`）及 `OP 0x34` 昼夜/色调渐变解码（commit `863b769`）。
5. **首尾主线闭环（不可破坏的不变量）**：
   - **开场 (`talk_id=100`, `talk1.dat`)**：`Map 532 (open)` → `Map 533` → `Video/begin.bik` → `Map 501 (羅剎峰)` → `Map 502 (羅剎洞外)`，梦境李逍遥 (`hero 2009`) 单人开局；需避免梦境双主角重复与前置启动空等（commit `1999c9b`、`863b769`）。
   - **结局 (`talk_id=1439`, `talk2.dat`)**：两段式闭合（`0x60`/`0xC2`/`0xC4`/`0xC8`）——首次调用置 `obj_rv[1439][1] = 1`，第二次调用播放 `Video/notice.bik`（《仙剑二》预告彩蛋）并执行 `OP 0x90` 设置 `request_return_title = 1` 返回竹简标题界面。
6. **10 槽位存档格式兼容（V5）**：
   - 兼容原版 Windows `0x6A54` 字节结构（`Save/0..9.sav`），并在文件尾附加 `"NPAL"` 魔数扩展尾块，现已升级至 **V5（`PalNpalSaveFooterV5`）**，在 V4 基础上额外持久化 `obj_rv[PAL_OBJ_CAP][4]` 与 `npc_ai_override[PAL_OBJ_CAP]`，并通过 `pal_load_footer_v4_fields` 保持对 V1–V4 与原版存档的完整向后兼容；只读 BYOD 目录下自动回退至 macOS `Application Support`（见 [[byod-clean-room-vfs]]）。


## Timeline

- time: 2026-11-23T12:56:39
  kind: decision
  summary: "Created this page: SWD2k Talk 字节码虚拟机、结局闭环与存档扩展架构"
  source: docs/FORMAT_SPEC.md / Sources/Engine/PalEngineCore.c
  affects: [talk-vm-and-world-state-closure]

- time: 2026-11-23T12:57:42
  kind: decision
  summary: "沉淀 Talk VM 8 Bank 架构、关键剧情脚本闭环及 v3 存档扩展格式"
  source: "docs/FORMAT_SPEC.md / Sources/Engine/PalEngineCore.c / commit 9f9be40 & 966bc9f"
  affects: [talk-vm-and-world-state-closure]

- time: 2026-11-23T15:29:36
  kind: decision
  summary: "沉淀 V5 存档尾格式、0x20 邻近触发架构、0x3E/0x1C 路径状态与子状态寄存器分离不变量"
  source: Sources/Engine/PalEngineCore.c / commit e07457d
  affects: [talk-vm-and-world-state-closure]
