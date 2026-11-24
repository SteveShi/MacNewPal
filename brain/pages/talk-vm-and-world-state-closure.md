---
id: talk-vm-and-world-state-closure
title: "SWD2k Talk 字节码虚拟机、结局闭环与存档扩展架构"
category: concept
status: active
tags: [talk-vm, script, save, endgame, swd2k]
created: "2026-11-23T12:56:39"
updated: "2026-11-25T01:24:56"
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
   - **`OP 0x3E` 重定位布局**：`[obj, map_id, unk2, tx, ty, act_id, sub_state, dir]`（不覆盖 `npc_talk_override`；跨地图重定位时必须清除陈旧 `ws->npc_ai_override[obj] = 0xFFFFu`，并由 `pal_npc_ai_step` `case 8` 拦截越界路径点；当 `act_id == 700` 时必须保留完整 `dir` 索引 `0..29` 而非 `& 7` 截断）
   - **`OP 0x42` 离队分离布局（全 8 Bank 463 处指令全量审计）**：真实 7 字参数布局为 **`[obj, act_id, sub_state, dir, talk_id, ai_script_id, flags]`**（绝无坐标字段 `(tx, ty)`）；离队对象必须落位在原队伍成员的实际像素坐标 `(prev_px, prev_py)`，并将 `dir & 7`、`talk_id` 与 `ai_script_id` 分别同步写入 `npc_dir_override`、`npc_talk_override`、`npc_ai_override`（避免将 `talk_id=99/200` 误当作 `tx` 瞬移进地图墙体）。
   - **`OP 0x1C` NPC AI 路径机**：驱动 `path*.dat`（opcodes `32, 34, 35, 39..44, 0xFFFF` 等）并持久化于 `ws->npc_ai_override`
   - `0xD8`/`0xD9` 半身像动画（经 `all_map1.act` → `all_map1.tsw`）、`%B` 对话分支选项、`OP 0x6B`/`0x9A` `skip_wait` 快进、以及战斗暂停期间的 Talk VM 竞态保护。
3. **`ACT 700` 真实 `HW` 碰撞盒与 `0x20`/`0x2000` 触碰事件区体系（全量 4,117 名全局 NPC 审计结论）**：
   - 第 2–8 章大量主线隐形拌线（90+ 处）使用 **`act_id == 700`、`sight == 0`、`(flags & 0x2000) != 0`**（如 `0xF000`/`0x7000`/`0xB000`/`0x3000`）而不带 `0x0020`，且在 `nd->dir` 中存放 `all_char.act` slot `700` 的完整子动作形状索引 `0..29`（经 `HW (w, h)` 定义精确矩形，如 `sub 10 = 22×1` 横向拌线、`sub 9 = 1×22` 纵向屏障）。
   - 统一由 `pal_npc_is_touch_event` + `pal_npc_touch_hit_px` 判定：对拥有 `vm.hitbox_w/h` 的 `act=700` 使用像素 AABB 重叠判定（并禁止在初始化与 `OP 0x3E` 中对 `act=700` 执行 `dir & 7` 截断）；激活 `act=700` 时（`OP 0x27`/AI op `28`）必须初始化其 ACT VM 以加载真实 `hitbox_w/h`。
   - `act=701` 仅在初次入图且 `npc_talk_override[obj_id] == 0` 时自动触发；`act=702` 莲花台、`712` 机关、`704` 锁门等在行走中 `32px` 内触发（排除 `0x2000 && talk >= 5001` 敌怪）。
   - 配有 `last_touch_npc_obj`（迟滞清除）与脚本结束同图落点防往复 ping-pong 保护。
4. **倍速队伍跟随与寻路不变量**：
   - 队伍跟随延迟帧数 `kLagTicks` 按 `speed_mult` 等比缩放（`ceil(kLagTicks[p] / speed_mult)`，最小 `2` 帧），确保倍速下跟随者与领队保持原版物理像素间距，避免 `OP 0xCC` 距离校验（如隐龙窟石门开锁）误判超距。
   - `move_to(trigger_id)` 采用两阶段 BFS：Pass 0 绕行非目标触发器格子（防止相邻多门布局如林家后院误踩旁门），仅当目标必须经由中间过场触发器到达时回退至 Pass 1 无避让 BFS。
5. **`0x7F` 脚本联动、迷宫脱出表与双用途道具分发**：
   - 物品/法术字段脚本、奖赏脚本（`gmagic.dat` `5001..5319`）与商店列表（`58000+`）通过统一 `0x7F` 执行器与 `0x309` 指令桥接；`pal_camp_use_item_direct` + `camp_dispatch_talk_if_any` 支持任务/剧情道具（如破天鎚、包袱、手卷、石鑰匙）触发 Talk 脚本。
   - **真实 32 组迷宫脱出表（`gitem.dat` 脚本 `77777`）**：逆向 `Palgame.exe` (`0x4482a8–0x4484b5`) 确认原版并非读取 `maps.dat` slot 21（仅残留 2 组轩辕剑/DOMO 旧表），而是在 `gitem.dat` 脚本 `77777`（`0x7F30201E 0 <5000..5319> <val>`）定义全 32 组脱出区（含第 5 组血池 `224..228 -> Map 88 (45, 13)`）。`pal_db_load_file` 现优先解析 `gitem.dat` 脚本 `77777`（上限 `PAL_MAX_DUNGEON_EXITS = 64`），`OP 0x1B (raw_mid == 1000)` 支持原版 `flag 1620` 特判（`354..401 -> 404/405`）与 `引路蜂 (3901)` 非迷宫误用自动退还（`pending_escape_refund_item`）。
   - **双用途道具野外使用优先**：MCP `manage_party(action="use_item")` 必须先检查 `pal_item_can_use_in_field` 再检查 `pal_item_equip_slot`，确保五灵珠（如 `土靈珠 3071`）、`木劍`、`純陽劍` 等可装备且具备野外脚本的道具触发野外功能而非静默装备。
   - 已修复野外物品触发 Talk 脚本的 `0x7F102046` 分发（commit `1999c9b`）及 `OP 0x34` 昼夜/色调渐变解码（commit `863b769`）。
6. **首尾主线闭环（不可破坏的不变量）**：
   - **开场 (`talk_id=100`, `talk1.dat`)**：`Map 532 (open)` → `Map 533` → `Video/begin.bik` → `Map 501 (羅剎峰)` → `Map 502 (羅剎洞外)`，梦境李逍遥 (`hero 2009`) 单人开局；需避免梦境双主角重复与前置启动空等（commit `1999c9b`、`863b769`）。
   - **结局 (`talk_id=1439`, `talk2.dat`)**：两段式闭合（`0x60`/`0xC2`/`0xC4`/`0xC8`）——首次调用置 `obj_rv[1439][1] = 1`，第二次调用播放 `Video/notice.bik`（《仙剑二》预告彩蛋）并执行 `OP 0x90` 设置 `request_return_title = 1` 返回竹简标题界面。
7. **10 槽位存档格式兼容（V5）**：
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

- time: 2026-11-23T16:26:08
  kind: decision
  summary: "沉淀第二章深度走查确认的 ACT 700 HW 碰撞盒 + 0x2000 剧情拌线、OP 0x42 真实 7 字布局与倍速跟随/两阶段避让 BFS 不变量"
  source: Sources/Engine/PalEngineCore.c / commit dac0853
  affects: [talk-vm-and-world-state-closure]

- time: 2026-11-24T14:28:00
  kind: decision
  summary: "沉淀第三章深度走查根因修复：gitem.dat 脚本 77777 全量 32 组迷宫脱出表、引路蜂无效使用退还与双用途道具（土靈珠等）野外使用优先于装备"
  source: Sources/Engine/PalEngineCore.c / Palgame.exe 0x4482a8-0x4484b5
  affects: [talk-vm-and-world-state-closure]

- time: 2026-11-25T01:24:56
  kind: decision
  summary: "沉淀第五回主线终局贯通闭环：黑蜘蛛森林Map 594触碰触发器NPC 589（talk 621）切入Map 548毒娘子决战、酒剑仙飞剑破除妖法夺雷灵珠、彩依化蝶殉情与御剑飞抵蜀山仙剑派Map 187，并固化第五回通关存档Save/8.sav"
  source: Sources/Engine/PalEngineCore.c / maps.dat / talk1.dat / Save/8.sav
  affects: [talk-vm-and-world-state-closure]
