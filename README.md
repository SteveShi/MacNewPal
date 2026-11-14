# NewPal for macOS

[English](README.md) | [简体中文](README.zh-CN.md)

**NewPal** (`com.steveshi.newpal`) is a clean-room native macOS launcher and **SDL3 Metal** runtime for *New Sword and Fairy* (新仙劍奇俠傳 / `SWD2k` engine), built with a copyright-safe **Bring Your Own Data (BYOD)** architecture.

Zero copyrighted artwork, audio, scripts, video, or maps are stored in this repository or bundled into `NewPal.app`. Users point the launcher at their own legal copy of `PalGame`, which is verified by binary signatures and streamed in-memory through an on-demand Virtual File System (VFS).

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
     - **Bink Video (`.bik`)** (`pal_bink_probe` & runtime player): Decodes `Video/notice.bik` and `Video/begin.bik` directly into Metal streaming textures with synchronized `SDL_AudioStream` audio playback.
     - **BGM Audio Streamer** (`pal_bgm_probe` & runtime playback): Streams `Music/*.mp3` and `*.wav` (`M001.mp3`, `F001.mp3`, etc.) resampled via `libswresample` into 44.1 kHz stereo S16 PCM.
     - **`.tsw` Sprites & ARLE Keying**: `0xABCD` magic, LZO1X + ARLE `0xFFFF`, 8-bit/16-bit RGB555 with transparent color keys (`0x319F` / `0x026B` decoded to `alpha = 0`), additive glow (`MA` via `SDL_BLENDMODE_ADD`), and archive-ID-routed lookup across `all_char`, `all_item`, `all_magic`, `all_sys`, `all_map1`, and `all_map2`.
     - **`.act` Action Bytecode VM** (`pal_act_vm_init`, `pal_act_vm_step_tick`, `PalActVM`): 35-opcode 35 ms tick VM driving animations (`ON`/`NO`/`FR`/`DS`/`LP`/`AP`/`MA`/`AH`/`CS`).
     - **CoreText Rich-Text Renderer** (`pal_font_probe_rich_text`, `PalRichTextMetrics`): Parses SWD2k formatting tags (`%N`, `%C0`..`%C9` palette, `%S`, `%K`) into crisp Chinese glyph textures with drop shadows.
     - **Boot → Title Screen → Field/Dialogue Loop**: Plays `notice.bik` -> `begin.bik`, runs the interactive bamboo-scroll Title Screen (`all_sys.act` `10000` sub-actions `90..93` -> `all_sys.tsw` `54..57`, `id=36` cursor, `Music/M001.mp3`), and transitions into `huge.lmf` (`MSF3`) field rendering with `talk1.dat` dialogue boxes (`all_sys.tsw` `82`/`51`).
     - **`all.snd` SFX & `MZ + 0x200` Databases**: RIFF passthrough / 44-byte PCM WAV reconstruction and in-memory TLV parsing (`ghero.dat`, `gmon.dat`, `gitem.dat`, `gmagic.dat`, `GFight1.dat`, `namex.dat`, `maps.dat`, `talk1.dat`).

3. **Reverse Engineering Specification & Verification Toolkit**:
   - Complete binary format & combat formula documentation: [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md).
   - Standalone read-only unpacker and database analyzer scripts in `Tools/`:
     - `Tools/pal_unpack.py`: Manifest generator, LZO1X/ARLE `.tsw` PNG extractor, 35-opcode `.act` disassembler, `all.snd` WAV extractor, and `MSF3` `.lmf` map renderer.
     - `Tools/pal_db_unpack.py`: `0x7F` TLV database dumper (annotating all `88` `GFight` opcodes), `Talk.cpp` bytecode disassembler, `maps.dat` / `namex.dat` / `path1..10.dat` parser.
     - `Tools/_lzo_accel.c`: Standalone C LZO1X shared library accelerator for Python tools.

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

A valid `PalGame` directory reports `Validation: 23/23 (is_valid=1)`, `Renderer: metal`, verified FFmpeg Bink/BGM probes, `.act` VM stepping, CoreText rich-text metrics, decoded map/sprite/action/audio/hero diagnostics, and exits with status `0`.
