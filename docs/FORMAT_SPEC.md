# SWD2k Data Format & Runtime Specification

> **Scope**: Archive container layouts, database schemas, bytecode instruction sets, and game-logic specifications supported by **NewPal**'s clean-room macOS SwiftUI launcher and C11 SDL3 Metal runtime for *New Sword and Fairy* (`SWD2k` data format).  
> **Repository Layout**: Clean-room `Sources/` (macOS SwiftUI launcher & C11 SDL3 Metal VFS core) and `Tools/` (read-only Python/C format inspection scripts). Local extracted/reference outputs (`extracted/`, `db_export/`, `manifests/`, `samples/`) remain strictly untracked and gitignored.

---

## 1. Supported Asset Inventory (BYOD Data Layout)

| Category | Source Archives | Inspector Script | Local Gitignored Output | Expected Counts |
| :--- | :--- | :--- | :--- | :--- |
| Sprite Containers | `all_char.tsw`, `all_item.tsw`, `all_magic.tsw`, `all_map1.tsw`, `all_map2.tsw`, `all_sys.tsw` | `Tools/pal_unpack.py extract-tsw` | `extracted/tsw/` | `1,696 + 82 + 279 + 57 + 74 + 106 = 2,294` slots (`5,438` frames in `all_magic.tsw`; `20,434` total PNGs) |
| Action Scripts | `all_char.act`, `all_item.act`, `all_magic.act`, `all_map1.act`, `all_map2.act`, `all_sys.act` | `Tools/pal_unpack.py extract-act` | `extracted/act/` | `507 + 253 + 43 + 57 + 67 + 23 = 950` slots; `11,239` bytecode streams (`unclean=0`) |
| Sound Effects | `all.snd` | `Tools/pal_unpack.py extract-snd` | `extracted/snd/` | `616` WAV files (`RIFF` passthrough + reconstructed 44-byte PCM WAV headers) |
| Maps & Backgrounds | `huge.lmf`, `huge2.lmf` | `Tools/pal_unpack.py extract-lmf` | `extracted/lmf/` | `320` maps (`huge.lmf`) + `149` maps (`huge2.lmf`) = `469` maps (`477` PNGs + `469` metadata JSONs) |
| Databases (`0x7F`) | `ghero.dat`, `gmon.dat`, `glevel.dat`, `gitem.dat`, `gmagic.dat`, `gmap.dat`, `GFight1..4.dat` | `Tools/pal_db_unpack.py export-all` | `db_export/db/` | `9` heroes, `160` monsters, `732` level templates/scripts, `844` item entries (`252` named items), `667` magic entries (`66` named magics), `265` encounter zones, `1,056` `GFight` scripts |
| Talk VM & Tables | `namex.dat`, `talk1..8.dat`, `maps.dat`, `path1..10.dat` | `Tools/pal_db_unpack.py export-all` | `db_export/scripts/` | `7,098` names, `5,603` non-stub Talk scripts (100% clean decode), `566` map titles, `1,811` quest entries, `4,000+` indexed path slots |
| Streaming Media | `Music/` (`113` `.mp3`/`.wav`), `Video/` (`11` `.bik`) | Direct pass-through | User `PalGame/{Music,Video}/` | Bink videos + MP3/WAV BGM |
| Save/Cache Blobs | `Data/` (`0.cm`, `1.cm`, `mcache.dat`) | Direct binary | User `PalGame/Data/` | Runtime campaign/save state caches |

---

## 2. Binary Container & Archive Formats

### 2.1 Standard `0x1C` Header + 3000-Slot TOC Archives (`all_*.tsw`, `all_*.act`, `all.snd`)
- **Header (`0x00..0x1B`, 28 bytes)**: Seven little-endian `uint32` values; `hdr[6]` (`offset 0x18`) records a nominal slot count that may under-count high-ID sparse slots in `.act` and `.snd` files (e.g., `all_map1.act` sets `hdr[6] = 64` while valid entries occupy slots `1000..1500`; `all.snd` sets `hdr[6] = 664` while valid audio entries continue to slot `930`).
- **Fixed TOC (`0x1C..0x203BB`)**: `3,000` sparse entries of `44` bytes each (`0x2C`):
  - `0x00..0x13`: `char[20]` CP950/Big5 null-padded name
  - `0x14..0x17`: `uint32 size` (in `all.snd`, `size & 0x03FFFFFF`; flags in `rec[0x20] & 3`)
  - `0x18..0x1B`: `uint32 offset`
  - `0x1C..0x1F`: `uint32 slot_id`
  - `0x20..0x2B`: auxiliary metadata

### 2.2 `.tsw` Sprite Container
- Each slot payload starts with `uint32`, followed by `uint16 magic = 0xABCD`, `uint16 nframes`, `uint16 bpp` (`8` or `16`), `uint16 off0`.
- When `bpp == 8`, an inline `256 * uint16` RGB555 palette (`512` bytes) follows at `off + 12`, and the `nframes * 36`-byte frame table sits at `off + off0 + 0x200` (otherwise at `off + off0`).
- Pixels are LZO1X-compressed RGB555 words or ARLE (`0xFFFF` magic) RLE streams with optional secondary alpha/mask LZO1X stream (`c_sz2`, `u_sz2`). Transparency keys map `0x0000`, `0x319F`, and `0x001F` (pure blue key) / `0xF81F` (magenta key) to `alpha = 0`; translucent / shadow color key (`0x026B`) and `0xC000` runs use 50% alpha (`128`).

### 2.3 `.act` Animation Bytecode
- **Slot Payload**: `uint16 sub_action_count` + `uint32 offsets[sub_action_count]` relative to slot start (`0` = empty sub-action).
- **Stream Header**: Begins with a `uint16` header word (typically `0x0000`), followed by a sequence of 2-byte ASCII opcodes (`uint16` little-endian) and fixed `int16` operands (`0` to `7` words).
- **Instruction Set (35 opcodes, `unclean=0` across all 11,239 streams)**:
  - **0 operands**: `DE` (`0x4544`, frame delay end), `VO` (`0x4f56`, end sequence), `2O` (`0x4f32`, secondary end/yield), `ON` (`0x4e4f`, visible on), `IV` (`0x5649`, bit 0 visible), `SM` (`0x534d`, shadow/mirror), `LC` (`0x434c`, clear mod params), `HA` (`0x4148`, flag `0x08`), `MA` (`0x414d`, flag `0x04`), `NA` (`0x414e`, flag `0x2c`).
  - **1 operand (`int16`)**: `NT` (`0x544e`, `.tsw` sprite ID), `FR` (`0x5246`, frame delay base), `OR` (`0x524f`, override frame), `DS` (`0x5344`, duration ticks), `AP` (`0x5041`, frame index & advance layer), `XO` (`0x4f58`), `YO` (`0x4f59`), `AO` (`0x4f41`, opacity), `VW` (`0x5756`), `EQ` (`0x5145`), `TA` (`0x4154`), `EA` (`0x4145`), `XA` (`0x4158`), `YA` (`0x4159`), `TW` (`0x5457`), `CS` (`0x5343`, trigger SFX ID), `SG` (`0x4753`), `LD` (`0x4c44`).
  - **2 operands (`2 x int16`)**: `YX` (`0x5859`, anchor Y/X), `HW` (`0x5748`, box H/W), `GC` (`0x4347`), `BC` (`0x4342`), `RC` (`0x4352`).
  - **3 operands (`3 x int16`)**: `LP` (`0x4c50`, start, end, loop count; `0xFFFF` = infinite).
  - **7 operands (`7 x int16`)**: `LF` (`0x464c`, flash/color transform block).

### 2.4 `.lmf` Map Archives (`huge.lmf` / `huge2.lmf`)
- First `uint32 idx_off` points to trailing `16`-byte directory entries: `(uint32 map_off, uint32 map_size, uint32 map_id, uint32 flags)`.
- Each map block starts with `"MSF3"` + `0x400`-byte header (`w_tiles`, `h_tiles`, `tile_size=16`, `num_tiles`, `bg_type` at `+0x100`; `bg_chunk_offset` at `+0x38`).

### 2.5 `MZ`-Prefixed `.dat` Databases & Tables
All `.dat` files (`ghero`, `gmon`, `glevel`, `gitem`, `gmagic`, `gmap`, `GFight1..4`, `namex`, `talk1..8`, `maps`, `path1..10`) begin with a `0x200`-byte DOS `MZ` header prefix; payload data starts at file offset `0x200`.
- **`maps.dat`**: Payload starts with `uint32 magic = 100`, followed by `25 x uint32` slot offsets (`slot 24 = 0xFFFFFFFF` sentinel):
  - `slot 0`: 26-byte NPC & map object placement records (`flags & 0x8000` initial visibility).
  - `slot 1`: `u32[]` double-indirection string pointer table resolving `map_id -> Music\<track>.mp3/.wav`.
  - `slot 2`: map metadata rows mapping `map_id` -> `msf3_id`, step speed, and BGM index.
  - `slot 16`: `u32[]` pointer table of `1,811` CP950 quest/objective strings (`%Q`-terminated).
  - `slot 19`: packed `(uint16 map_id, char[] cp950_name, "%Q")` entries (`566` map titles, terminated by `0xFFFF`).
- **`path1..10.dat`**: Payload starts with `uint32 magic = 100` + variable-length `u32` offset table (`n_slots = (off0 - 4) // 4`): `path1` = `2,000` slots; `path2..7` = `300` slots; `path8` = `200` slots; `path9` = raw path stream (`18 B`); `path10` = empty `4 B` header.
- **`0x7F` Databases**: Tag-length-value (TLV) stream (`u32 tag`, top byte `0x7F`; lower bits encode type/opcode). Strings are CP950 terminated by `%Q`.

---

## 3. Database Schemas & Property IDs

### 3.1 Actor Properties (`ghero.dat` / `gmon.dat` / `glevel.dat`, tag `0x7f201001`)
Temporary combat modifier IDs sit at `(base_id - 3)` (`temp + base` summed at read time). Level-up training accumulators sit at `{13, 19, 25, 31, 37, 43, 49}` and are merged into base stats and cleared after battle victory.

| Prop ID (Dec / Hex) | Field Name | Role in Runtime |
| :--- | :--- | :--- |
| `4 / 0x04` | `max_lv` | Level cap (`99` in `glevel`) |
| `5 / 0x05` | `lv` | Current level |
| `9 / 0x09` | `prev_exp` | Pre-battle EXP snapshot |
| `10 / 0x0a` | `next_exp` | EXP threshold for next level |
| `11 / 0x0b` | `cur_exp` | Current accumulated EXP |
| `16 / 0x10`, `17 / 0x11` | `max_hp`, `cur_hp` | HP pair |
| `22 / 0x16`, `23 / 0x17` | `max_mp`, `cur_mp` | MP pair |
| `26/29` (`0x1a/0x1d`) | `temp_atk`, `atk` | Physical Attack / 武術 |
| `32/35` (`0x20/0x23`) | `temp_mag`, `mag` | Magic Power / 靈力 |
| `38/41` (`0x26/0x29`) | `temp_def`, `def` | Defense / 防禦 |
| `44/47` (`0x2c/0x2f`) | `temp_spd`, `spd` | Speed / 身法 (turn order, steal dodge) |
| `50/53` (`0x32/0x35`) | `temp_luck`, `luck` | Luck / 吉運 (physical defense factor, steal & drop) |
| `56/59` .. `86/89` (`0x38/0x3b..0x56/0x59`) | Elemental Resists | Poison (`56/59`), Wind (`62/65`), Lightning (`68/71`), Water (`74/77`), Fire (`80/83`), Earth (`86/89`) |
| `92/95` (`0x5c/0x5f`) | `temp_monster_mag`, `monster_mag` | Monster magic power |
| `101 / 0x65` | `flee_hp_pct` | Monster low-HP flee threshold % |
| `107 / 0x6b` | `rage_chance_pct` | Monster special/status trigger % |
| `125 / 0x7d` | `monster_category` | Monster category / boss type bits |
| `180 / 0xb4`, `182 / 0xb6` | `ai_action_id`, `sprite_or_type` | Default AI `GFight`/Talk ID, battle sprite |
| `187..189` (`0xbb..0xbd`) | `ai_phase1..3_script` | Monster conditional AI branches |
| `190 / 0xbe` | `reward_exp` | Monster EXP reward |
| `203 / 0xcb` | `steal_gold` | Stealable gold (70% success) |
| `204..212` (`0xcc..0xd4`) | `steal1` / `drop1` / `drop2` | Tuples `(chance, qty, item_id)` |
| `219 / 0xdb` | `gourd_soul_value` | Soul/alchemy value |
| `220`, `221` | `initial_magic_id`, `initial_weapon_id` | Hero starting skill/weapon (`ghero`) |

### 3.2 `gitem.dat` & `gmagic.dat` Property IDs
- **`gitem.dat`**: `0`: `target_scope` (`0`=single, `1`=special, `3`=all); `1`: `field_talk_script`; `2`: `battle_fight_script`; `3`: `equip_bonus_script`; `4`: `effect_or_anim_id`; `6`: `reusable_flag` (`1`/`65535`); `7`: `use_count_temp`; `8`: `equip_slot_mask`; `9`: `price`.
- **`gmagic.dat`**: `0`: `target_scope` (`0`/`1`/`3`); `1`: `field_talk_script`; `2`: `battle_fight_script`; `5`: `mp_cost`; `6`: `max_uses` (`65535` = unlimited, `9` = 酒神 cap); `8`: `user_category_mask`.

---

## 4. Combat Engine & Rules (`SWD2k` Compatibility)

### 4.1 Battle State Machine & Turn Priority
1. **Priority Calculation**: Each combatant computes `priority = prop(0x2f) + prop(0x2c)` (base Speed + temp Speed).
   - Heroes add random jitter `(-2 + rand(5))`.
   - Flagged priority skills/actions add `+5000`.
   - Status bit `0x18 (24)` (Haste / 仙風雲體術) doubles priority (`priority <<= 1`).
2. **Hero Critical Hit Selection**:
   - Rolls `u = rand(100)`. Triggers crit unless `u < 90 - rand(11)` (i.e., **10%–20% critical chance**).
   - When critical triggers, normal attack advances to `script_id + 1` (e.g., `650 -> 651`, switching `atk_mode` from `1` to `5` = `2x` damage).
3. **Cover / Guard**:
   - When an enemy targets another party member while hero `2000` / `2009` has `HP >= MaxHP / 3` and lacks incapacitating status `0x13`, cover probability scales by `diff = enemy.lv - attacker.lv`: `<=0 -> 10%`, `1..3 -> 15%`, `4..7 -> 20%`, `>7 -> 40%`; fallback `20%` sets Guard status `0x1e` on both.
4. **Victory & Level-Up**:
   - Equipped item `3073` (`0xc01`) on any active alive party member doubles battle gold reward.
   - Battle victory checks `cur_hp > 0 && lv < max_lv && cur_exp >= next_exp`, looking up `glevel` entry `hero_id * 100 + 1 + lv` (or `lv + 200001` for `2009`) to apply stat deltas. Defeated heroes at battle victory are revived with `max_hp / 3` HP and `max_mp / 3` MP.
5. **Monster Flee & Steal/Drop**:
   - Flee trigger: `(cur_hp * 100) / max_hp <= prop(101)`, success chance by `(hero_lv - mon_lv)`: `<=0: 10%`, `1..3: 20%`, `4..7: 30%`, `>=8: 60%`.
   - Steal: item1 chance `(atk.luck - def.spd + 60)%`, else `70%` steal gold (`prop 203`), else item2 chance `(atk.luck - def.spd + 80)%`.
   - Drop: uses `((sum_party_luck / 3 - 30) / 2 - enemy.lv) / 1.5 + 40.0`%.

### 4.2 Damage Formula Modes (`0..74`)
Let `atk_eff = atk.prop(0x1d) + atk.prop(0x1a)`, `mag_eff = atk.prop(0x23) + atk.prop(0x20)`, `def1 = def.prop(0x35) + def.prop(0x32)`, `def2 = def.prop(0x29) + def.prop(0x26)`. Element resistance `res_pct(idx)` uses `(base_res + temp_res) / 100.0`. Negative return = damage; positive return = heal.

- **Cases `1..10` (Hero Physical)**:
  - `base = (atk_eff / 0.72) / max(1, def1) * (1.0 - def2 / 100.0)`
  - `1, 6`: `-base`; `2`: `-0.4 * base`; `3`: `-0.4 * base` (`0.8 / 2`); `4`: `-0.36 * base`; `5`: `-(2 + rand_mod(1)) * base` (`2x` crit); `7`: `-0.9 * base`; `8`: `-0.76 * base`; `9, 10`: `0` (clamped `-1`).
  - Jitter: `5 - rand_mod(0xb)` (`[-5..+5]%`) applied percentage-wise on `1,3,5,6` or subtracted pre-negation on `2,4,7,8`. All non-heal cases clamp `<= -1`.
- **Cases `11..16` (Magic Fixed/Scaled)**:
  - `base = (-atk_power) * 7.2 + (mag_eff >> 2) + (atk_eff / 12)`
  - Jitter `5 - rand_mod(0xa)` (`[-4..+5]%`): `11, 16`: `-base`; `12` (酒神): `-9 * def.cur_mp`; `13`: `-0.75 * base`; `14`: `-240`; `15`: `-2050`.
- **Cases `21..26` (Elemental Magic)**:
  - Same `base` as `11..16`, multiplied by `-((elem_res + 1.0) / 100.0 - 0.01)` (`-elem_res / 100.0`) for `21`=Poison, `22`=Wind, `23`=Lightning, `24`=Water, `25`=Fire, `26`=Earth, jittered `[-4..+5]%`, clamped `<= -1`.
- **Cases `31..42` (Level-Scaled Fixed / Elemental)**:
  - `lv = attacker.prop(5)`, jitter `[-4..+5]%`: `31`: `-(lv+90)`; `32,35`: `-(lv + 100*FireRes)`; `33`: `-(lv+160)`; `34`: `-(lv + 220*WaterRes)`; `36`: `-(lv+880)`; `37`: `-(lv+150)`; `38`: `-(lv + 640*WindRes)`; `39`: `-(lv + 680*LightRes)`; `40`: `-(lv + 640*WaterRes)`; `41`: `-(lv + 650*FireRes)`; `42`: `-(lv + 660*EarthRes)`.
- **Cases `51..52` (Monster Physical)**:
  - `base = max(0.0, (atk_eff / 0.72) - def2)`
  - `51`: `-base`; `52`: `-(2 + rand_mod(1)) * base`; `+/-5%` jitter.
- **Cases `61..67` (Monster Magic)**:
  - `base = max(0, (atk.prop(0x5f)+atk.prop(0x5c)) + (-atk_power)*7.2 - def2 * 0.72)`.
  - `61`: `-base`; `62..67`: `-base * ElemRes/100.0` (`Poison..Earth`).
- **Cases `0, 0x44..0x4a` (`68..74`)**:
  - `0x44 (68)`: raw `atk_power` applied to `cur_mp` (`prop 0x17`).
  - `0x45 (69)`: `atk_power`% of `def.max_hp`.
  - `0x46, 0x49`: fixed value (`atk_power`), inverted to positive (heal) if target is undead (`status 0x80` / item `3074`).
  - `0x47 (71)`: deducts player Gold (e.g., 乾坤一擲 / 銅錢鏢).
  - `0x48 (72)`: `0` unless undead/spirit target.
  - `0x4a (74)`: `-((attacker.lv - 9) * 5 + 240)`.

### 4.3 Post-Formula Status Modifiers
- Attacker `status 0x14 (20)`: halves damage; `status 0x16 (22)`: doubles damage.
- Defender `status 0x15 (21)`: doubles damage; `status 0x17 (23)` or `0x1f (31)`: halves damage.
- Defender `status 0x19 (25)`: absorbs damage into `MaxHP`; `0x1a (26)`: reflects into `CurHP` + triggers counter; `0x1c (28)` / `0x1e (30)`: nullify (`dmg = 0`).

### 4.4 `GFight` Script VM (All 88 Opcodes)
All 88 opcodes (`0x7f00ffff`, `0x7f00f002`, `0x7f00f027`, `0x7f00f036`, `0x7f002001..0x7f002056`) are cataloged in `COMBAT_OPCODES` inside `Tools/pal_db_unpack.py` and annotated directly onto exported `GFight1..4.json`. Key control/combat opcodes include: `0x2004` (`spawn_effect_or_motion`), `0x2007` (`resolve_hit_guard_and_damage`), `0x200b` (`modify_actor_prop`), `0x2015` (`set_atk_mode_and_power`), `0x201a` (`try_capture_or_steal_item`), `0x2026`/`0x203c`/`0x2052`/`0x2055`/`0x2056` (conditional script branches), `0x203e` (`sync_actor_frame_step`), and `0x204d..0x204f` (counter/reflect/death triggers).

---

## 5. macOS Port Architecture (BYOD + SDL3 Metal)

1. **Windowing, Input, Audio & 2D Presentation -> SDL3 (`libsdl3`)**:
   - Implemented in `Sources/Engine/PalEngineCore.c`, `Sources/Engine/PalGpuFilter.c`, and `Sources/Engine/PalGamepad.c` targeting Metal/GPU via SDL3 at logical `640x480` presentation (`SDL_SetRenderLogicalPresentation(..., SDL_LOGICAL_PRESENTATION_LETTERBOX)`) plus optional MSL `xBRZ` / `CRT-Royale` shaders and Retina-crisp CoreText UI compositing.
   - Streams `all.snd` (`RIFF` passthrough & 44-byte PCM WAV reconstruction) via `SDL_AudioStream`, MP3/WAV BGM with `libswresample` crossfades & victory ducking, and **FFmpeg (`libavcodec`/`libswscale`)** Bink (`.bik`) playback (`Video/`).
2. **BYOD In-Memory VFS & SwiftUI Launcher**:
   - Zero-extraction VFS (`Sources/Engine/`) validates all 23 file signatures and streams sprites/maps/SFX/data directly from the user's own `PalGame` directory mounted by `Sources/App/`.
3. **Full Gameplay Core**:
   - Complete Talk VM (`talk1..8.dat` + `namex.dat`), `.act` frame player (`DE/NT/FR/YX/HW/ON/AP/VO/LP/CS`), `MSF3` walkability + `path1..10.dat` routes, and the full turn-based combat engine on top of **SDL3**.
