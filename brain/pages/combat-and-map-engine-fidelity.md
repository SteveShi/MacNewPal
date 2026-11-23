---
id: combat-and-map-engine-fidelity
title: "MSF3 地图渲染与 GFight 战斗系统还原边界"
category: concept
status: active
tags: [combat, map, msf3, gfight, swd2k]
created: "2026-11-23T12:56:39"
updated: "2026-11-23T12:57:42"
---

<!-- compiled_truth -->
## 地图与角色引擎（`MSF3` + `.act`）

1. **双 LMF 地图仓库**：`huge.lmf`（320 张地图）+ `huge2.lmf`（149 张地图），16×16 背景瓦片 + LZO1X 压缩的行走/触发网格（`walk_grid & 0x60000000` 判定碰撞，`& 0xFF` 为触发器 ID）。
2. **`maps.dat` 元数据索引**：
   - `slot 0`：26 字节 NPC 放置记录（按 `flags & 0x8000` 过滤）
   - `slot 1`：双层间接寻址 BGM 文件名表（由 `MSF3 hdr[0x10c]` 或 `slot 2` 索引）
   - `slot 2`：`map_id → msf3_id`、步速 `step_speed_px`、阴影方向与环境色偏
   - `slot 19`：CP950 地名表（用于进入地图时的渐隐横幅，已去除调试编号后缀）
3. **渲染与寻路规则**：
   - **3 桶 Y 排序合成器**：`below` / `sort_y` / `above`，配合 `MSF3` 每图透视缩放表（`scale_mode == 2`）与御剑 `action_base`（commit `3a5b521`）。
   - **35ms Tick `.act` 虚拟机**：35 条指令，驱动 UI（`all_sys.act`）与 8 方向角色动作（`all_char.act`：站立 `0..7`、行走 `8..15`、奔跑 `16..23`，含 `YX` 锚点偏移与 `MA`/`AH`/`SG` 混合）。
   - **视野与跟队**：`(304, 240)` 相机死区 + 对角线贴墙滑行 + 点击 8 方向 A* 寻路 + 4 人队列轨迹缓冲（32/64 步）。

## 战斗引擎（`GFight1..4.dat` + `0x7F` 数据库）

1. **数据库规模**：`ghero.dat`（9 角色）、`gmon.dat`（160 怪物）、`gitem.dat`（252 道具）、`gmagic.dat`（66 法术 + 601 脚本）、`glevel.dat`（410 升级增量行）、`gmap.dat`（265 遇敌区）、`GFight1..4.dat`（1,056 战斗编队脚本，含 88 条 `GFight` 指令）。
2. **还原度关键不变量**：
   - **速度排序回合制**与 `0..74` 物理/仙术/暗器伤害公式、属性克制与装备被动加成（`0x7F30200B`）。
   - **160 位异常状态掩码**：毒持续掉血（DOT）、封魔、混乱、昏睡；特判装备 `3063`（五毒珠，免疫毒系）与 `3074`（七彩玉灵芝，异常/HP/MP 转换）。
   - **高阶机制**：4 人合體技（Combo）、队友阵亡狂暴（Ally-Down Fury）、梦境李逍遥 (`2009`) 上限处理、怪物受击 VFX 优先于死亡动画播放（commit `1999c9b`）及原版死亡渐散音效。
   - **完整规格文档**：所有二进制结构、Tag ID 与公式详见仓库 `docs/FORMAT_SPEC.md`。


## Timeline

- time: 2026-11-23T12:56:39
  kind: decision
  summary: "Created this page: MSF3 地图渲染与 GFight 战斗系统还原边界"
  source: docs/FORMAT_SPEC.md / Sources/Engine/PalEngineCore.c
  affects: [combat-and-map-engine-fidelity]

- time: 2026-11-23T12:57:42
  kind: decision
  summary: "沉淀 MSF3 地图渲染、行动 VM 与 GFight 战斗公式的还原边界"
  source: "docs/FORMAT_SPEC.md / Sources/Engine/PalEngineCore.c / commit 966bc9f & abab661"
  affects: [combat-and-map-engine-fidelity]
