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
| **M3** | Multi-Bank `talk1..8.dat` Loader & Talk Bytecode VM (`7,394` Scripts), Story Flags & Variables, `%B` Choice Menus, `0xD8`/`0xD9` Bust Portraits, Tile Triggers, NPC/Chest Interaction & `0x1B` Map Transitions | ✅ Completed |
| **M4** | Full `0x7F` Databases (`ghero`/`glevel`/`gitem`/`gmagic`), Party Stats/Equip/Magic/Level-Up Runtime, `Talk` Opcodes (`0x80`..`0x86`, `0x309` Scripts) & Interactive Camp/System Menu (`狀態`/`仙術`/`物品`/`系統`) | ✅ Completed |
| **M5** | `gmon.dat`/`gmap.dat`/`GFight1..4.dat` Turn-Based Combat, Shop UI (`58000+`) & 10-Slot Save/Load (`Save/0..9.sav`) | ✅ Completed |
| **M6** | Full Combat Fidelity: 160-Bit Status Masks (Poison DOT + `3063`/`3074` Immunity/Healing, Seal/Confuse/Sleep), 4-Hero Combos (`合體技`), Ally-Down Fury, Authentic `GFight` Formulas & AI, Monster Death Animation & Optional 2x HD Mode | ✅ Completed |
| **M7** | Endgame & Talk VM Closure (`0x60`/`0xC2`/`0xC4`/`0xC8` two-pass `talk 1439 -> Video/notice.bik` epilogue & Sword Pose transitions) + Read-Only BYOD `Application Support` Save Fallback | ✅ Completed |
| **M8** | Remaster Polish: SDL3 Metal GPU Shaders (`xBRZ` & `CRT-Royale` MSL), Retina-Crisp CoreText UI Pass, Smooth BGM Crossfade & Ducking, and Full SDL3 Gamepad Support (`PalGamepad`) | ✅ Completed |
| **M9** | MCP Support & Autonomous Testing: Built-in stdio Model Context Protocol (`--mcp-server`) with 16 tools, headless fast simulation, and full 8-chapter automated speedrun runner | ✅ Completed |

---

## Architecture Overview

1. **Native macOS Launcher (`Sources/App/`)**:
   - Built with **Swift 6.0** (`SWIFT_STRICT_CONCURRENCY: complete`) and **SwiftUI** for macOS 14.0+ (`arm64`).
   - Full `en` / `zh-Hans` localization catalog in `Sources/App/Resources/Localizable.xcstrings`.
   - Security-scoped directory picker (`NSOpenPanel`) validating all **23** core archive and database signatures before enabling launch (`PalLauncherStore.swift`).
   - Real settings wired directly to the SDL3 engine (`SettingsView.swift` & `ContentView.swift`): window scale (`640×480`, `1280×960`, fullscreen letterbox), **Metal GPU Shader** (`Classic`, `Metal xBRZ`, `CRT-Royale`), Resolution Mode (`Original` / `HD Remastered 2×`), Metal VSync toggle, texture scale mode, master volume, audio mute, and saved directory management.
   - Release highlights modal in Help menu (`WhatsNewSheetView.swift`).

2. **In-Memory VFS & SDL3 Metal Runtime (`Sources/Engine/`)**:
   - C11 engine core (`PalEngineCore.c` / `PalEngineCore.h`) paired with `PalGpuFilter.{h,c}` (SDL3 GPU/Metal MSL shader pipeline) and `PalGamepad.{h,c}` (SDL3 gamepad hotplug & analog/D-Pad input mapper), linked against `libSDL3` (targeting Apple **Metal** via `gpu` + MSL or `metal` fallback), **FFmpeg** (`libavformat`, `libavcodec`, `libswscale`, `libavutil`, `libswresample`), **CoreText / CoreGraphics / CoreFoundation**, and macOS `libiconv` (`CP950 -> UTF-8`).
   - **Two-Pass GPU Post-Processing & Retina UI Compositor** (`PalGpuFilter.c`): Renders the 640×480 scene into an offscreen render target, applies embedded Metal Shading Language (`xBRZ` edge-preserving upscaling or `CRT-Royale` scanline/aperture-grille/bloom) to the native swapchain, then clears the GPU pixel shader so dynamic Retina (`1×..4×`) CoreText dialogue, Camp menu, Shop, HUD, and Combat banners composite razor-sharp on top.
   - Direct archive streaming from original containers without extracting files to disk:
     - **Bink Video (`.bik`)** (`pal_bink_probe` & runtime player): Decodes `.bik` videos (such as `Video/begin.bik` at New Game start, while `Video/notice.bik` is verified by `--self-test` and reserved for the `talk2.dat` `talk_id=1439` post-game *Sword and Fairy 2* epilogue trailer) directly into Metal streaming textures with synchronized `SDL_AudioStream` audio playback.
     - **BGM Audio Streamer** (`pal_bgm_probe` & smooth fade envelope): Streams `Music/*.mp3` and `*.wav` (`M001.mp3`, `M020.wav`, `F001.mp3`, etc.) resampled via `libswresample` into 44.1 kHz stereo S16 PCM with smooth crossfades across scene/map/Talk switches and automatic victory audio ducking.
     - **`.tsw` Sprites & ARLE Keying**: `0xABCD` magic, LZO1X + ARLE `0xFFFF`, 8-bit/16-bit RGB555 with transparent color keys (`0x319F` / `0x026B` decoded to `alpha = 0`), additive glow (`MA` via `SDL_BLENDMODE_ADD`), and archive-ID-routed lookup across `all_char`, `all_item`, `all_magic`, `all_sys`, `all_map1`, and `all_map2`.
     - **`.act` Action Bytecode VM** (`pal_act_vm_init`, `pal_act_vm_step_tick`, `PalActVM`): 35-opcode 35 ms tick VM driving UI animations (`all_sys.act`) as well as 8-direction hero and NPC sprites (`all_char.act` — `001李逍遙`, `002趙靈兒`, `003林月如`, `004阿奴`: stand `0..7`, walk `8..15`, run `16..23`, `YX` hotspot anchor subtraction, `AP`/`DS`/`DE`/`VO`).
     - **`MSF3` Map & 3-Bucket Compositor** (`pal_map_probe_m2`, `vfs_load_lmf_map_by_id`): Loads maps across `huge.lmf` (320 maps) and `huge2.lmf` (149 maps) with 16×16 background tiles, LZO1X walk/trigger grid (`walk_grid & 0x60000000` collision, `& 0xFF` trigger ID), trigger table, foreground ARLE occlusion sprites, and a 3-bucket Y-sorted compositor (`below`, `sort_y`, `above`).
     - **`maps.dat` World Integration**: Uses `slot 19` (`CP950 -> UTF-8` fade-in map title banners), `slot 1` (double-indirection BGM track lookup indexed by `hdr[0x10c]` / `slot 2`), `slot 2` (`map_id` -> `msf3_id` + `step_speed_px`), and `slot 0` (26-byte NPC placement records filtered by `flags & 0x8000`).
     - **Field Movement, Camera & Party Chain**: Authentic `(304, 240)` camera deadzone clamped to map dimensions, diagonal corner-sliding collision, click-to-move 8-direction A* pathfinding, and 4-hero party follower chain (`32`/`64`-step trail buffer).
     - **CoreText Rich-Text Renderer** (`pal_font_probe_rich_text`, `PalRichTextMetrics`): Parses SWD2k formatting tags (`%N`, `%C0`..`%C9` palette, `%S`, `%D`, `%G`, `%K`) into crisp Chinese glyph textures with drop shadows.
     - **Talk Script VM, Bust Portraits & Trigger Engine** (`pal_talk_vm_probe_m3`, `PalTalkVM`, `PalTalkWorldState`): Multi-bank `talk1..8.dat` loader (`701` slots in `talk1`/`talk4`, `1000` in others — **7,394 / 7,394** scripts cleanly decoded), story flags (`8192`) & script variables (`256`, `obj_rv`), conditional/map/item/distance branches, `0xD8`/`0xD9` animated bust portraits (`all_map1.act` -> `all_map1.tsw`), `%B` dialogue choice extraction, stepped tile triggers, persistent `0x26`/`0x27`/`0x64` NPC state updates, `0x309` rewards, and `0x1B` map transitions.
     - **0x7F Database Runtime, Party Progression & Camp Menu** (`pal_db_menu_probe_m4`, `PalGameDatabase`, `PalPartyMemberState`): Loads `ghero.dat` (`9` heroes), `gitem.dat` (`252` items + equip/field scripts), `gmagic.dat` (`66` spells + `601` scripts including `5001..5319` rewards & `58000+` shop lists), `glevel.dat` (`410` level-up delta templates), `gmon.dat` (`160` monsters), `gmap.dat` (`265` encounter zones), and `GFight1..4.dat` (`1,056` encounter scripts). Enforces equipment slot & hero-mask compatibility rules, passive equipment bonuses (`0x7F30200B`), field healing items/spells (`0x7EFF1001`/`0x7EFF1015`), Talk opcodes (`0x80`, `0x81`/`0x82`/`0xA7`/`0xA8`, `0x83`, `0x84`, `0x86`, `0x309`), and an interactive `[I]`/`[U]`/`[F1]` Camp Menu (`狀態`, `仙術`, `物品`, `系統`) using `all_sys.tsw`, `all_item.act`/`all_item.tsw`, and CoreText.
     - **Turn-Based Combat, Shop UI & 10-Slot Save/Load** (`pal_combat_shop_save_probe_m5`, `PalCombatState`, `PalShopMenuState`, `PalLoadOverlayState`): Supports speed-sorted turn-based encounters from `GFight1..4.dat` and `gmon.dat` (`all_map2.tsw` battlegrounds, `all_char.act` `1000..1003` heroes & `2000+` monsters with static standing poses, action-only poses, and monster death animations, full `0..74` physical/magic/throwing formulas, elemental & equipment modifiers, 160-bit status ailments with `3063` 五毒珠 immunity & `3074` 七彩玉靈芝 HP/MP conversion, `合體技` 4-hero combos, ally-down fury, `2009` cap, flee, drops, EXP/level-ups, and optional 2x HD rendering), interactive Buy/Sell Shop overlays (`58000+` scripts with `all_sys.tsw` `9017`), and 10-slot Save/Load (`Save/0..9.sav` supporting original `0x6A54` saves, `"NPAL"` extended footers, and automatic macOS `Application Support` fallback for read-only BYOD directories) from both the Title Screen ("舊的回憶") and the Camp `系統` tab.
     - **Title Screen → Authentic Opening Script → Field Loop**: Launches into the bamboo-scroll Title Screen (`all_sys.act` `10000`, `Music/M001.mp3`) with Title Load ("舊的回憶"); "新的故事" runs the authentic `talk1.dat` `talk_id=100` opening (`532` -> `533` -> `Video/begin.bik` -> `501` `羅剎峰` -> `502` `羅剎洞外`, Dream 李逍遙 `2009` solo) with interactive field movement, chests/objects (`OP 0x0A` state toggle & pop-out `土怪`), mid-script combat (`2801` / `5001+`), scripted triggers, shops, and camp menus.
     - **`all.snd` SFX & `MZ + 0x200` Databases**: RIFF passthrough / 44-byte PCM WAV reconstruction and in-memory TLV parsing (`ghero.dat`, `gmon.dat`, `gitem.dat`, `gmagic.dat`, `GFight1.dat`, `namex.dat`, `maps.dat`, `talk1.dat`).

3. **Data Format Specification, Test Module & Verification Toolkit**:
   - Complete data format & combat formula specification: [docs/FORMAT_SPEC.md](docs/FORMAT_SPEC.md).
   - Optional QA Test Mode module documentation (`PAL_ENABLE_TEST_MODE`): [docs/TEST_MODE.md](docs/TEST_MODE.md) ([简体中文](docs/TEST_MODE.zh-CN.md)).
   - Standalone read-only format inspector and database analyzer scripts in `Tools/`:
     - `Tools/pal_unpack.py`: Manifest generator, LZO1X/ARLE `.tsw` PNG exporter, 35-opcode `.act` script parser, `all.snd` WAV exporter, and `MSF3` `.lmf` map renderer.
     - `Tools/pal_db_unpack.py`: `0x7F` TLV database dumper (annotating all `88` `GFight` opcodes), Talk bytecode parser, `maps.dat` / `namex.dat` / `path1..10.dat` inspector.
     - `Tools/_lzo_accel.c`: Standalone C LZO1X shared library accelerator for Python tools.

---

## First Launch & Gatekeeper Notice (Unsigned App)

Because NewPal is an open-source community release built without an Apple Developer ID certificate (using ad-hoc code-signing), macOS Gatekeeper may display a security prompt on first launch (e.g. *"Apple cannot verify NewPal for malicious software"* or *"NewPal is damaged and cannot be opened"*).

Please use one of the following standard methods to launch the app:

### Recommended: Right-Click (Control + Click) Open
1. After dragging `NewPal.app` into `/Applications`;
2. Locate `NewPal.app` in Finder, **hold the Control key and click (or right-click)** the icon;
3. Select **"Open"** from the context menu;
4. In the confirmation dialog, click **"Open"**. The app will remember this approval and open normally on subsequent double-clicks.

### Alternative 1: Privacy & Security Settings
1. Attempt to open the app once (getting the prompt), then open **System Settings**;
2. Navigate to **Privacy & Security**;
3. Scroll down to the **Security** section where it says *"NewPal was blocked from use because it is not from an identified developer"*;
4. Click **"Open Anyway"** and authenticate with your password or Touch ID.

### Alternative 2: Terminal Quarantine Removal
If macOS displays an alert saying the application is "damaged and cannot be opened", open **Terminal** and strip the quarantine extended attribute:
```bash
xattr -cr /Applications/NewPal.app
```

---

## Interactive Field Controls (`PAL_SCENE_FIELD`)

| Input | Action |
| --- | --- |
| `WASD` / Arrow Keys | 8-direction movement with diagonal corner sliding |
| `Shift` (press) | Toggle Walk / Run mode (`action_base = 8` / `16`) |
| Left-Click on map / NPC | 8-direction A* pathfinding to clicked tile or auto-walk & talk to clicked NPC |
| `Space` / `Return` / `E` | Interact with adjacent facing NPC/chest, advance active dialogue, or confirm selected choice / menu item |
| `Up` / `Down` / `1`..`8` | Highlight / pick `%B` dialogue branch options when a prompt is open |
| `Esc` / Right-Click / `I` / `U` / `F1` | Open or close the 4-tab Camp / System Menu (`狀態`, `仙術`, `物品`, `系統`: equip, item/magic use, 10-slot Save/Load, return to Title, or Exit Game); also closes active Shop / Combat submenus |
| `F` | Trigger turn-based combat (`PAL_SCENE_COMBAT`, resolving current map zone encounter or formation `2801`) |
| `O` | Open the Buy/Sell Shop overlay (`gmagic.dat` `58001` or active `0x309` shop ID; switch modes with `←`/`→`/`Tab`) |
| `M` | Cycle featured tour maps (`501`, `1`, `2`, `3`, `502`) |
| `[` / `]` | Previous / next map in the current `.lmf` archive (`huge.lmf` / `huge2.lmf`) |
| `P` | Toggle 4-hero party follower chain |
| `V` | Swap lead hero across `001李逍遙`, `002趙靈兒`, `003林月如`, `004阿奴` |
| `G` | Toggle walkability / trigger / footprint debug overlay |
| `C` | Toggle NoClip walk-through mode |
| `T` | Toggle sample CoreText dialogue box (when no Talk script is active) |
| `B` | Replay `Video/begin.bik` |
| `Tab` | Toggle debug HUD (hidden by default) |
| **Gamepad (SDL3)** | D-Pad / Left Stick: 8-dir move & menus; `A` (South): Interact / Confirm; `B` (East): Cancel / Back; `X` (West): Walk / Run; `Y` (North) / `Start`: Camp Menu; `LB` / `RB`: Prev / Next Hero or Page; `Back`: Debug HUD |

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

A valid `PalGame` directory reports `Validation: 23/23 (is_valid=1)`, `Renderer: metal`, verified FFmpeg Bink/BGM probes, UI and character `.act` VM stepping, `MSF3` + `maps.dat` + A* probes (`MapM2[1]`, `MapM2[2]`, `MapM2[501]`), `TalkM3` (`8` banks, `7394/7394` clean scripts, portals `6001`/`6007`, chest `7012`, cutscenes `104`/`105`), `DbM4` (`0x7F` databases: `9` heroes, `252` items, `66` magics, `410` level rows, `h2000` equip/item/magic/level-up progression, and `0x309` script item lookup `5150 -> 3254 皮帽`), `CombatShopSaveM5` (`160` monsters, `265` zones, `1056` fight scripts, formation `2801` battle simulation against `羅剎鬼婆`, shop `58001` transaction, `Save/0.sav` parsing & ephemeral `Save/99.sav` round-trip), CoreText rich-text metrics, decoded map/sprite/action/audio/hero diagnostics, and `[SelfTest] ALL CHECKS PASSED.` with exit code `0`.

### Model Context Protocol (MCP) Server for AI Playtesting
NewPal includes a built-in JSON-RPC 2.0 stdio MCP server for autonomous AI playtesting, navigation, and debugging:
```bash
# Add to your MCP client configuration (Claude Desktop / Antigravity / Cursor / Codex)
{
  "mcpServers": {
    "newpal": {
      "command": "/Applications/NewPal.app/Contents/MacOS/NewPal",
      "args": ["--mcp-server"]
    }
  }
}
```

### Standalone DMG Distribution Packaging
To create a standalone, relocatable `.dmg` with bundled dylibs, ad-hoc codesigning, and custom volume icon:
```bash
./Tools/package_dmg.py
```
Output DMG: `build/NewPal-1.0.0.dmg`.

---

## Legal & Trademark Notice

- **Independent Compatibility Project**: This project is an independent, clean-room open-source runtime implemented for cross-platform interoperability and preservation. It is not affiliated with, endorsed by, or sponsored by Softstar Entertainment Inc. (大宇資訊 / 軟星科技) or the original rightsholders.
- **No Copyrighted Game Assets**: All source code in this repository is licensed under the GPL-3.0 License (`LICENSE`), and zero copyrighted game data, artwork, music, video, or scripts are included. A legitimate user-provided copy of `PalGame` is required at runtime.
- **Trademarks**: *Sword and Fairy* (*新仙劍奇俠傳*) and all related game titles, characters, and trademarks remain the property of their respective owners.
