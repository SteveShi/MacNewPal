# NewPal for macOS

[English](README.md) | [简体中文](README.zh-CN.md)

**NewPal** (`com.steveshi.newpal`) is a clean-room native macOS launcher and **SDL3 Metal** runtime for *New Sword and Fairy* (新仙劍奇俠傳 / `SWD2k` engine), built with a copyright-safe **Bring Your Own Data (BYOD)** architecture.

Zero copyrighted artwork, audio, scripts, video, or maps are stored in this repository or bundled into `NewPal.app`. Users point the launcher at their own legal copy of `PalGame`, which is verified by binary signatures and streamed in-memory through an on-demand Virtual File System (VFS).

---

## Milestone Roadmap

| Milestone | Scope | Status |
| --- | --- | --- |
| **M0** | Native SwiftUI BYOD Launcher, 23-file VFS Signature Verifier, SDL3 Metal Window & LZO1X/ARLE Decoders | ✅ Completed |
| **M1** | Bink Video (`.bik`), BGM Streamer, `.tsw` Color Keying, `.act` Action VM, CoreText Rich-Text & Title/Opening Sequence | ✅ Completed |
| **M2** | Full `MSF3` Map Loader (`huge.lmf` + `huge2.lmf`), `maps.dat` Integration, 3-Bucket Y-Sorted Occlusion, 8-Dir `all_char.act` VM, A* Pathfinding, Camera Deadzone & 4-Hero Party Follower Chain | ✅ Completed |
| **M3** | Multi-Bank `talk1..8.dat` Loader & `Talk.cpp` Bytecode VM (`7,394` Scripts), Story Flags & Variables, `%B` Choice Menus, `0xD8`/`0xD9` Bust Portraits, Tile Triggers, NPC/Chest Interaction & `0x1B` Map Transitions | ✅ Completed |
| **M4** | Full `0x7F` Databases (`ghero`/`glevel`/`gitem`/`gmagic`), Party Stats/Equip/Magic/Level-Up Runtime, `Talk` Opcodes (`0x80`..`0x86`, `0x309` Scripts) & Interactive Camp/System Menu (`狀態`/`仙術`/`物品`/`系統`) | ✅ Completed |
| **M5+** | `GFight1..4.dat` Combat, Shop UI & Save/Load | 🚧 Planned |

---

## Architecture Overview

1. **Native macOS Launcher (`Sources/App/`)**:
   - Built with **Swift 6.0** (`SWIFT_STRICT_CONCURRENCY: complete`) and **SwiftUI** for macOS 14.0+ (`arm64`).
   - Full `en` / `zh-Hans` localization catalog in `Sources/App/Resources/Localizable.xcstrings`.
   - Security-scoped directory picker (`NSOpenPanel`) validating all **23** core archive and database signatures before enabling launch (`PalLauncherStore.swift`).
   - Real settings wired directly to the SDL3 engine (`SettingsView.swift`): window scale (`640×480`, `1280×960`, fullscreen letterbox), Metal VSync toggle, texture scale mode (nearest pixel-art / bilinear), master volume, audio mute, and saved directory management.
   - Release highlights modal in Help menu (`WhatsNewSheetView.swift`).

2. **In-Memory VFS & SDL3 Metal Runtime (`Sources/Engine/`)**:
   - C11 engine core (`PalEngineCore.c` / `PalEngineCore.h`) linked against `libSDL3` (targeting Apple **Metal** via `SDL_CreateRenderer(window, "metal")`), **FFmpeg** (`libavformat`, `libavcodec`, `libswscale`, `libswresample`, `libavutil`), **CoreText / CoreGraphics / CoreFoundation**, and macOS `libiconv` (`CP950 -> UTF-8`).
   - Direct archive streaming from original containers without extracting files to disk:
     - **Bink Video (`.bik`)** (`pal_bink_probe` & runtime player): Decodes `.bik` videos (such as `Video/begin.bik` at New Game start, while `Video/notice.bik` is verified by `--self-test` and reserved for the `talk2.dat` `talk_id=1439` post-game *Sword and Fairy 2* epilogue trailer) directly into Metal streaming textures with synchronized `SDL_AudioStream` audio playback.
     - **BGM Audio Streamer** (`pal_bgm_probe` & runtime playback): Streams `Music/*.mp3` and `*.wav` (`M001.mp3`, `M020.wav`, `F001.mp3`, etc.) resampled via `libswresample` into 44.1 kHz stereo S16 PCM.
     - **`.tsw` Sprites & ARLE Keying**: `0xABCD` magic, LZO1X + ARLE `0xFFFF`, 8-bit/16-bit RGB555 with transparent color keys (`0x319F` / `0x026B` decoded to `alpha = 0`), additive glow (`MA` via `SDL_BLENDMODE_ADD`), and archive-ID-routed lookup across `all_char`, `all_item`, `all_magic`, `all_sys`, `all_map1`, and `all_map2`.
     - **`.act` Action Bytecode VM** (`pal_act_vm_init`, `pal_act_vm_step_tick`, `PalActVM`): 35-opcode 35 ms tick VM driving UI animations (`all_sys.act`) as well as 8-direction hero and NPC sprites (`all_char.act` — `001李逍遙`, `002趙靈兒`, `003林月如`, `004阿奴`: stand `0..7`, walk `8..15`, run `16..23`, `YX` hotspot anchor subtraction, `AP`/`DS`/`DE`/`VO`).
     - **`MSF3` Map & 3-Bucket Compositor** (`pal_map_probe_m2`, `vfs_load_lmf_map_by_id`): Loads maps across `huge.lmf` (320 maps) and `huge2.lmf` (149 maps) with 16×16 background tiles, LZO1X walk/trigger grid (`walk_grid & 0x60000000` collision, `& 0xFF` trigger ID), trigger table, foreground ARLE occlusion sprites, and a 3-bucket Y-sorted compositor (`below`, `sort_y`, `above`).
     - **`maps.dat` World Integration**: Uses `slot 19` (`CP950 -> UTF-8` fade-in map title banners), `slot 1` (double-indirection BGM track lookup indexed by `hdr[0x10c]` / `slot 2`), `slot 2` (`map_id` -> `msf3_id` + `step_speed_px`), and `slot 0` (26-byte NPC placement records filtered by `flags & 0x8000`).
     - **Field Movement, Camera & Party Chain**: Authentic `(304, 240)` camera deadzone clamped to map dimensions, diagonal corner-sliding collision, click-to-move 8-direction A* pathfinding, and 4-hero party follower chain (`32`/`64`-step trail buffer).
     - **CoreText Rich-Text Renderer** (`pal_font_probe_rich_text`, `PalRichTextMetrics`): Parses SWD2k formatting tags (`%N`, `%C0`..`%C9` palette, `%S`, `%D`, `%G`, `%K`) into crisp Chinese glyph textures with drop shadows.
     - **`Talk.cpp` Script VM, Bust Portraits & Trigger Engine** (`pal_talk_vm_probe_m3`, `PalTalkVM`, `PalTalkWorldState`): Multi-bank `talk1..8.dat` loader (`701` slots in `talk1`/`talk4`, `1000` in others — **7,394 / 7,394** scripts cleanly decoded), story flags (`8192`) & script variables (`256`, `obj_rv`), conditional/map/item/distance branches, `0xD8`/`0xD9` animated bust portraits (`all_map1.act` -> `all_map1.tsw`), `%B` dialogue choice extraction, stepped tile triggers, persistent `0x26`/`0x27`/`0x64` NPC state updates, `0x309` rewards, and `0x1B` map transitions.
     - **0x7F Database Runtime, Party Progression & Camp Menu** (`pal_db_menu_probe_m4`, `PalGameDatabase`, `PalPartyMemberState`): Loads `ghero.dat` (`9` heroes), `gitem.dat` (`252` items + equip/field scripts), `gmagic.dat` (`66` spells + `601` scripts including `5001..5319` rewards & `58000+` shop lists), and `glevel.dat` (`410` level-up delta templates). Drives `0x4acbf1`/`0x4ad456` slot & hero-mask checks, passive equipment bonuses (`0x7F30200B`), field healing items/spells (`0x7EFF1001`/`0x7EFF1015`), Talk opcodes (`0x80`, `0x81`/`0x82`/`0xA7`/`0xA8`, `0x83`, `0x84`, `0x86`, `0x309`), and an interactive `[I]`/`[U]`/`[F1]` Camp Menu (`狀態`, `仙術`, `物品`, `系統`) using `all_sys.tsw`, `all_item.act`/`all_item.tsw`, and CoreText.
     - **Title Screen → Opening Sequence → Field Loop**: Launches into the bamboo-scroll Title Screen (`all_sys.act` `10000`, `Music/M001.mp3`); "新的故事" executes splash maps `532` -> `533` -> `Video/begin.bik` -> opening scene `501` (`羅剎峰`) with interactive field movement, NPC interaction, and scripted triggers.
     - **`all.snd` SFX & `MZ + 0x200` Databases**: RIFF passthrough / 44-byte PCM WAV reconstruction and in-memory TLV parsing (`ghero.dat`, `gmon.dat`, `gitem.dat`, `gmagic.dat`, `GFight1.dat`, `namex.dat`, `maps.dat`, `talk1.dat`).

3. **Reverse Engineering Specification & Verification Toolkit**:
   - Complete binary format & combat formula documentation: [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md).
   - Standalone read-only unpacker and database analyzer scripts in `Tools/`:
     - `Tools/pal_unpack.py`: Manifest generator, LZO1X/ARLE `.tsw` PNG extractor, 35-opcode `.act` disassembler, `all.snd` WAV extractor, and `MSF3` `.lmf` map renderer.
     - `Tools/pal_db_unpack.py`: `0x7F` TLV database dumper (annotating all `88` `GFight` opcodes), `Talk.cpp` bytecode disassembler, `maps.dat` / `namex.dat` / `path1..10.dat` parser.
     - `Tools/_lzo_accel.c`: Standalone C LZO1X shared library accelerator for Python tools.

---

## Interactive Field Controls (`PAL_SCENE_FIELD`)

| Input | Action |
| --- | --- |
| `WASD` / Arrow Keys | 8-direction movement with diagonal corner sliding |
| `Shift` (press) | Toggle Walk / Run mode (`action_base = 8` / `16`) |
| Left-Click on map / NPC | 8-direction A* pathfinding to clicked tile or auto-walk & talk to clicked NPC |
| `Space` / `Return` / `E` | Interact with adjacent facing NPC/chest, advance active dialogue, or confirm selected choice / menu item |
| `Up` / `Down` / `1`..`8` | Highlight / pick `%B` dialogue branch options when a prompt is open |
| `I` / `U` / `F1` | Toggle the 4-tab Camp / System Menu (`狀態`, `仙術`, `物品`, `系統`) with equip, item/magic use, and save/title actions |
| `M` | Cycle featured tour maps (`501`, `1`, `2`, `3`, `502`) |
| `[` / `]` | Previous / next map in the current `.lmf` archive (`huge.lmf` / `huge2.lmf`) |
| `P` | Toggle 4-hero party follower chain |
| `V` | Swap lead hero across `001李逍遙`, `002趙靈兒`, `003林月如`, `004阿奴` |
| `G` | Toggle walkability / trigger / footprint debug overlay |
| `C` | Toggle NoClip walk-through mode |
| `T` | Toggle sample CoreText dialogue box (when no Talk script is active) |
| `B` | Replay `Video/begin.bik` |
| `Tab` | Toggle debug HUD |
| `Esc` | Close active Camp Menu / dialogue, or return to Title Screen |

---

## Build & Verify (macOS)

### Prerequisites
- macOS 14.0+ (`arm64`) with Xcode command-line tools
- [Homebrew](https://brew.sh/) packages: `sdl3`, `ffmpeg`, `xcodegen`

```bash
brew install sdl3 ffmpeg xcodegen
```

### Generate Project, Build & Run Self-Test
```bash
xcodegen generate
xcodebuild -project NewPal.xcodeproj -scheme NewPal -configuration Release -derivedDataPath build build
./build/Build/Products/Release/NewPal.app/Contents/MacOS/NewPal --self-test /path/to/PalGame
```

A valid `PalGame` directory reports `Validation: 23/23 (is_valid=1)`, `Renderer: metal`, verified FFmpeg Bink/BGM probes, UI and character `.act` VM stepping, `MSF3` + `maps.dat` + A* probes (`MapM2[1]`, `MapM2[2]`, `MapM2[501]`), `TalkM3` (`8` banks, `7394/7394` clean scripts, portals `6001`/`6007`, chest `7012`, cutscenes `104`/`105`), `DbM4` (`0x7F` databases: `9` heroes, `252` items, `66` magics, `410` level rows, `h2000` equip/item/magic/level-up progression, and `0x309` script item lookup `5150 -> 3254 皮帽`), CoreText rich-text metrics, decoded map/sprite/action/audio/hero diagnostics, and `[SelfTest] ALL CHECKS PASSED.` with exit code `0`.
