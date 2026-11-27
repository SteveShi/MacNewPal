# NewPal QA Test Mode Module

[简体中文](TEST_MODE.md) | [English](TEST_MODE.en_US.md)

The **QA Test Mode** module provides full-playthrough debugging and verification controls (God Mode, encounter toggle, one-hit combat instant kill, party progression, movement speed scaling, cutscene skip, and quick save/load) as an **isolated, compile-time removable subsystem** controlled by `PAL_ENABLE_TEST_MODE`.

---

## 1. Module Architecture & Isolation

All Test Mode logic lives outside core gameplay paths and binds to the runtime only through narrow callback hooks and zero-overhead inline stubs:

| File | Role | Stripped When `PAL_ENABLE_TEST_MODE == 0` |
| :--- | :--- | :--- |
| `Sources/Engine/PalTestMode.h` | Public `PalTestConfig` struct (Swift-bridged), `PalTestHostOps` callback table, `PalTestState` struct | Collapses to empty stub structs and `static inline` no-ops |
| `Sources/Engine/PalTestMode.c` | Runtime overlay UI (top-right pill + 13-row toolbox), keyboard/mouse dispatch, CoreText toast/cache | Compiled out (`#if PAL_ENABLE_TEST_MODE`) |
| `Sources/App/TestModeSection.swift` | SwiftUI `@AppStorage` launcher card (`TestModeLauncherGroupBox`) and Settings section (`TestModeSettingsSection`) | Compiled out (`#if PAL_ENABLE_TEST_MODE`) |
| `Sources/Engine/PalEngineCore.c` | Thin host hooks (`PalTestBridgeCtx`, delta filter, status bit guard, encounter guard, speed multiplier, overlay render) | All `PalTestBridgeCtx` callbacks compiled out |
| `project.yml` | Master build switch (`PAL_ENABLE_TEST_MODE=1` in `GCC_PREPROCESSOR_DEFINITIONS` and `PAL_ENABLE_TEST_MODE` in `SWIFT_ACTIVE_COMPILATION_CONDITIONS`) | Toggle to disable module cleanly |

### Self-Test Isolation & Save Safety

- `--self-test` does **not** call `pal_test_bind_runtime`, ensuring automated unit/regression probes (`PalEngineCore.c` combat formulas, status immunity, level-ups, AI scripts) execute uncontaminated regardless of build flags.
- Quick Save / Quick Load exclusively target **Slot 9 (`Save/9.sav`)**, never overwriting player slots `0..8`.

---

## 2. How to Disable or Remove for Release Builds

### Option A: One-Line Compile-Time Strip (Recommended)

Edit `project.yml` and remove or set `PAL_ENABLE_TEST_MODE=0`:

```yaml
SWIFT_ACTIVE_COMPILATION_CONDITIONS: "$(inherited)"
GCC_PREPROCESSOR_DEFINITIONS: "$(inherited) PAL_ENABLE_TEST_MODE=0"
```

Then regenerate the Xcode project and rebuild:

```bash
xcodegen generate
xcodebuild -project NewPal.xcodeproj -scheme NewPal -configuration Release -derivedDataPath build clean build
```

When disabled:
- `PalTestMode.c` compiles zero executable instructions.
- `PalEngineCore.h` removes `test_cfg` from `PalLaunchConfig`.
- `PalEngineCore.c` reduces all `pal_test_*` call sites to inline `0` / no-ops compiled away by optimization.
- `TestModeSection.swift` and `#if PAL_ENABLE_TEST_MODE` SwiftUI blocks are omitted completely.

### Option B: Permanent Physical Removal

Delete `Sources/Engine/PalTestMode.h`, `Sources/Engine/PalTestMode.c`, and `Sources/App/TestModeSection.swift`, and remove the `#if PAL_ENABLE_TEST_MODE` blocks from `Sources/Engine/PalEngineCore.h`, `Sources/Engine/PalEngineCore.c`, `Sources/App/PalLauncherStore.swift`, `Sources/App/ContentView.swift`, `Sources/App/SettingsView.swift`, and `project.yml`.

---

## 3. Feature Reference & Hotkeys

Even if **Enable Test Mode in Game** is left unchecked in the launcher, you can wake the module mid-game with `F12` or `Option + ~` (as long as `PAL_ENABLE_TEST_MODE=1`).

| # | Feature | Behavior | Hotkey / UI |
| :-: | :--- | :--- | :--- |
| 1 | **Master Switch** | Toggles Test Mode overlay and cheats on/off globally | `F12` (`Option + ~` to wake) |
| 2 | **Toolbox Panel** | Expands/collapses the 13-row interactive overlay (mouse & keyboard) | `~` / `F11` / Top-right pill |
| 3 | **God Mode (人物無敵)** | Blocks party HP/MP loss (enemy attacks, DOT, combo HP cost), blocks harmful combat status bits (poison, sleep, confuse, stun, seal, elemental debuffs), waives spell MP costs, and prevents defeat | `F2` / Panel `1` / Launcher |
| 4 | **No Encounters (免遇敵)** | Suppresses roaming monster aggro/chase and touch-triggered auto-battles; scripted boss/story battles and `[F]` skirmish remain active | `F3` / Panel `2` / Launcher |
| 5 | **Combat Instant Kill (戰鬥一鍵秒殺)** | Defeats all alive enemies, plays death animations & SFX, awards authentic EXP/Gold/drops/level-ups, and sets `story_flags[3001] = 1` | `K` (in combat) / `F4` / Panel `3` |
| 6 | **Auto Instant-Kill (入戰自動秒殺)** | Automatically triggers Combat Instant Kill on the first frame of every battle | Panel `4` / Launcher |
| 7 | **Full Party Heal (全隊滿狀態)** | Restores full HP/MP for active party members and clears all combat status ailments | `F5` / Panel `5` |
| 8 | **Add Gold (+50,000 文)** | Grants `+50,000` currency for shop and equipment testing | `F6` / Panel `6` |
| 9 | **Party Level Up (+1 級)** | Levels up all active party members by +1 via `glevel.dat` growth tables and unlocks new spells | `F7` / Panel `7` |
| 10 | **Test Item Supply (補給物資)** | Adds verified consumables/throwables from `gitem.dat`: `4401` (止血草 ×20), `4403` (行軍丹 ×20), `4405` (金創藥 ×10), `4301` (鼠兒果 ×20), `4357` (蜂王蜜 ×10), `4251` (還魂香 ×10), `4254` (天香續命露 ×5), `3613` (蜂巢 ×10), `3702` (血玲瓏 ×5) | Panel `8` |
| 11 | **Field Speed (`1×`/`2×`/`3×`)** | Cycles world map walking/running step multiplier | `F8` / Panel `9` / Launcher |
| 12 | **NoClip (穿牆模式)** | Toggles terrain walk-grid collision bypass | `F9` / Panel `0` |
| 13 | **Skip Cutscene Wait** | Clears `talk_vm.wait_ticks`, fade steps, and camera pan waits to advance cutscenes faster | `F10` / Panel `-` |
| 14 | **Quick Save / Load (Slot 9)** | Saves/loads dedicated QA slot `Save/9.sav` from Field mode | Panel `S` / `L` (panel open, Field only) |
