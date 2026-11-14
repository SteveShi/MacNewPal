#if PAL_ENABLE_TEST_MODE
import SwiftUI

/// Isolated configuration bridge & UI module for QA Test Mode.
/// Stripped cleanly when `PAL_ENABLE_TEST_MODE` is omitted from compilation conditions.
enum TestModeDefaults {
    static let enabledKey = "testModeEnabled"
    static let godModeKey = "testGodMode"
    static let noEncounterKey = "testNoEncounter"
    static let autoInstantKillKey = "testAutoInstantKill"
    static let speedMultKey = "testSpeedMult"

    static func currentConfig() -> PalTestConfig {
        let ud = UserDefaults.standard
        let rawSpeed = ud.integer(forKey: speedMultKey)
        let clampedSpeed = (1...3).contains(rawSpeed) ? rawSpeed : 1
        return PalTestConfig(
            enabled: ud.bool(forKey: enabledKey) ? 1 : 0,
            god_mode: ud.bool(forKey: godModeKey) ? 1 : 0,
            no_encounter: ud.bool(forKey: noEncounterKey) ? 1 : 0,
            auto_instant_kill: ud.bool(forKey: autoInstantKillKey) ? 1 : 0,
            speed_mult: Int32(clampedSpeed)
        )
    }
}

private struct TestModeControlsGrid: View {
    @AppStorage(TestModeDefaults.enabledKey) private var enabled: Bool = false
    @AppStorage(TestModeDefaults.godModeKey) private var godMode: Bool = false
    @AppStorage(TestModeDefaults.noEncounterKey) private var noEncounter: Bool = false
    @AppStorage(TestModeDefaults.autoInstantKillKey) private var autoInstantKill: Bool = false
    @AppStorage(TestModeDefaults.speedMultKey) private var speedMult: Int = 1

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Toggle("test_mode.enable_toggle", isOn: $enabled)
                .font(.callout.weight(.medium))

            if enabled {
                HStack(spacing: 18) {
                    Toggle("test_mode.god_mode", isOn: $godMode)
                    Toggle("test_mode.no_encounter", isOn: $noEncounter)
                    Toggle("test_mode.auto_instant_kill", isOn: $autoInstantKill)
                }
                .toggleStyle(.checkbox)

                HStack(spacing: 10) {
                    Text("test_mode.speed_mult")
                        .font(.callout)
                    Picker("test_mode.speed_mult", selection: $speedMult) {
                        Text("test_mode.speed_1x").tag(1)
                        Text("test_mode.speed_2x").tag(2)
                        Text("test_mode.speed_3x").tag(3)
                    }
                    .labelsHidden()
                    .pickerStyle(.segmented)
                    .frame(maxWidth: 210)
                }

                Text("test_mode.hotkeys_hint")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
    }
}

struct TestModeLauncherGroupBox: View {
    var body: some View {
        GroupBox {
            TestModeControlsGrid()
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(4)
        } label: {
            Label("test_mode.section_title", systemImage: "wrench.and.screwdriver")
        }
    }
}

struct TestModeSettingsSection: View {
    var body: some View {
        Section("test_mode.section_title") {
            TestModeControlsGrid()
        }
    }
}
#endif
