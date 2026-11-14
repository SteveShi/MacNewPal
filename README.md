# NewPal for macOS

[English](README.md) | [简体中文](README.zh-CN.md)

**NewPal** (`com.steveshi.newpal`) is a clean-room native macOS launcher and **SDL3 Metal** runtime prototype for *New Sword and Fairy* (新仙劍奇俠傳 / `SWD2k` engine), built with a copyright-safe **Bring Your Own Data (BYOD)** architecture.

Zero copyrighted artwork, audio, scripts, or maps are stored in this repository or bundled into `NewPal.app`. Users point the launcher at their own legal copy of `PalGame`, which is verified by binary signatures and streamed in-memory through an on-demand Virtual File System (VFS).

---

## Architecture Overview

1. **Native macOS Launcher (`Sources/App/`)**:
   - Built with **Swift 6.0** (`SWIFT_STRICT_CONCURRENCY: complete`) and **SwiftUI** for macOS 14.0+ (`arm64`).
   - Full `en` / `zh-Hans` localization catalog in `Sources/App/Resources/Localizable.xcstrings`.
   - Security-scoped directory picker (`NSOpenPanel`) validating all **23** core archive and database signatures before enabling launch (`PalLauncherStore.swift`).
   - Real settings wired directly to the SDL3 engine (`SettingsView.swift`): window scale (`640×480`, `1280×960`, fullscreen letterbox), Metal VSync toggle, texture scale mode (nearest pixel-art / bilinear), master volume, audio mute, and saved directory management.
   - Release highlights modal in Help menu (`WhatsNewSheetView.swift`).

2. **In-Memory VFS & SDL3 Metal Runtime (`Sources/Engine/`)**:
   - C11 engine core (`PalEngineCore.c` / `PalEngineCore.h`) linked against `libSDL3` (targeting Apple **Metal** via `SDL_CreateRenderer(window, "metal")`) and macOS `libiconv` (`CP950 -> UTF-8`).
   - Direct archive streaming from original containers without extracting files to disk:
     - `.tsw` sprites (`0xABCD` magic, LZO1X + ARLE `0xFFFF`, 8-bit/16-bit RGB555 with color keys `0x319F` / `0x026B`).
     - `.act` action bytecode (`35` opcodes decoded from `0x45bae2`).
     - `all.snd` audio effects (RIFF passthrough & 44-byte PCM WAV reconstruction into `SDL_AudioStream`).
     - `huge.lmf` / `huge2.lmf` (`MSF3` LZO1X tile pools & map cell grids).
     - `MZ + 0x200` databases (`ghero.dat`, `gmon.dat`, `gitem.dat`, `gmagic.dat`, `GFight1.dat`, `namex.dat`, `maps.dat`, `talk1.dat`).

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
- [Homebrew](https://brew.sh/) packages: `sdl3`, `xcodegen`

```bash
brew install sdl3 xcodegen
```

### Generate Project, Build & Run Self-Test
```bash
xcodegen generate
xcodebuild -project NewPal.xcodeproj -scheme NewPal -configuration Release -derivedDataPath build build
./build/Build/Products/Release/NewPal.app/Contents/MacOS/NewPal --self-test /path/to/PalGame
```

A valid `PalGame` directory reports `Validation: 23/23 (is_valid=1)`, `Renderer: metal`, decoded map/sprite/action/audio diagnostics, and exits with status `0`.
