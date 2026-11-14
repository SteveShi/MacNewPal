import SwiftUI

struct SettingsView: View {
    @AppStorage("gameDataPath") private var gameDataPath: String = ""
    @AppStorage("gameDataBookmark") private var gameDataBookmark: Data = Data()
    @AppStorage("windowScale") private var windowScale: Int = 2
    @AppStorage("vsyncEnabled") private var vsyncEnabled: Bool = true
    @AppStorage("nearestFilter") private var nearestFilter: Bool = true
    @AppStorage("masterVolume") private var masterVolume: Double = 0.85
    @AppStorage("muteAudio") private var muteAudio: Bool = false

    let store: PalLauncherStore

    var body: some View {
        Form {
            Section("settings.tab_general") {
                Picker("settings.window_scale", selection: $windowScale) {
                    Text("settings.scale_1x").tag(1)
                    Text("settings.scale_2x").tag(2)
                    Text("settings.scale_fullscreen").tag(0)
                }

                Toggle("settings.vsync", isOn: $vsyncEnabled)

                Picker("settings.pixel_filter", selection: $nearestFilter) {
                    Text("settings.filter_nearest").tag(true)
                    Text("settings.filter_linear").tag(false)
                }
            }

            Section("metrics.sfx") {
                Toggle("settings.mute_audio", isOn: $muteAudio)

                HStack {
                    Text("settings.master_volume")
                    Slider(value: $masterVolume, in: 0.0...1.0)
                        .disabled(muteAudio)
                    Text("\(Int((masterVolume * 100).rounded()))%")
                        .monospacedDigit()
                        .frame(width: 44, alignment: .trailing)
                }
            }

            Section("settings.game_data_folder") {
                Text(gameDataPath.isEmpty ? String(localized: "byod.no_folder_selected") : gameDataPath)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .textSelection(.enabled)

                HStack {
                    Button("byod.choose_folder") {
                        store.chooseGameDirectory { path, bookmark in
                            gameDataPath = path
                            gameDataBookmark = bookmark
                        }
                    }
                    Button("settings.clear_folder", role: .destructive) {
                        gameDataPath = ""
                        gameDataBookmark = Data()
                        store.validateDirectory("")
                    }
                    .disabled(gameDataPath.isEmpty)
                }
            }
        }
        .formStyle(.grouped)
        .frame(width: 480, height: 360)
    }
}
