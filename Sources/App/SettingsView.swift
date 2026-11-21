import AppKit
import SwiftUI

struct SettingsView: View {
    @AppStorage("gameDataPath") private var gameDataPath: String = ""
    @AppStorage("gameDataBookmark") private var gameDataBookmark: Data = Data()
    @AppStorage("windowScale") private var windowScale: Int = 2
    @AppStorage("vsyncEnabled") private var vsyncEnabled: Bool = true
    @AppStorage("nearestFilter") private var nearestFilter: Bool = true
    @AppStorage("hdMode") private var hdMode: Bool = true
    @AppStorage("shaderFilter") private var shaderFilter: Int = 1
    @AppStorage("masterVolume") private var masterVolume: Double = 0.85
    @AppStorage("muteAudio") private var muteAudio: Bool = false
    @AppStorage("gameLanguage") private var gameLanguage: Int = 0
    @AppStorage("mcpEnabled") private var mcpEnabled: Bool = true
    @AppStorage("mcpHeadlessLaunch") private var mcpHeadlessLaunch: Bool = false
    @AppStorage("mcpAllowQaTools") private var mcpAllowQaTools: Bool = true
    @State private var copiedMcpConfig: Bool = false

    let store: PalLauncherStore

    var body: some View {
        Form {
            Section("settings.tab_general") {
                Picker("settings.game_language", selection: $gameLanguage) {
                    Text("settings.lang_traditional").tag(0)
                    Text("settings.lang_simplified").tag(1)
                }
                .pickerStyle(.segmented)
                Text("settings.language_note")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Picker("settings.render_quality", selection: $hdMode) {
                    Text("settings.quality_original").tag(false)
                    Text("settings.quality_hd").tag(true)
                }
                .pickerStyle(.segmented)

                Picker("settings.gpu_shader", selection: $shaderFilter) {
                    Text("settings.shader_none").tag(0)
                    Text("settings.shader_xbrz").tag(1)
                    Text("settings.shader_crt").tag(2)
                }
                .pickerStyle(.segmented)

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
                .disabled(hdMode || shaderFilter != 0)
                if hdMode || shaderFilter != 0 {
                    Text("settings.hd_filter_note")
                        .font(.caption)
                        .foregroundStyle(.secondary)
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
#if PAL_ENABLE_TEST_MODE
            TestModeSettingsSection()
#endif
            Section("settings.mcp_section") {
                Toggle("settings.mcp_enable", isOn: $mcpEnabled)
                Toggle("settings.mcp_headless", isOn: $mcpHeadlessLaunch)
                    .disabled(!mcpEnabled)
                Toggle("settings.mcp_allow_qa", isOn: $mcpAllowQaTools)
                    .disabled(!mcpEnabled)

                HStack {
                    Text("settings.mcp_socket_label")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Spacer()
                    Text("/tmp/newpal-mcp.sock")
                        .font(.caption.monospaced())
                        .foregroundStyle(.secondary)
                        .textSelection(.enabled)
                }

                HStack {
                    Button(copiedMcpConfig ? "settings.mcp_copied" : "settings.mcp_copy_config") {
                        NSPasteboard.general.clearContents()
                        NSPasteboard.general.setString(store.mcpConfigSnippet(), forType: .string)
                        copiedMcpConfig = true
                    }
                    Spacer()
                }

                Text("settings.mcp_note")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            if let val = store.validation {
                vfsMetricsSection(val)
            }
        }
        .formStyle(.grouped)
        .frame(minWidth: 620, minHeight: 580)
    }

    private func vfsMetricsSection(_ val: ValidationSummaryModel) -> some View {
        Section("metrics.section_title") {
            LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible()), GridItem(.flexible())], spacing: 10) {
                metricTile("metrics.archives", value: "\(val.validArchivesCount)/\(val.totalRequiredArchives)")
                metricTile("metrics.sprites", value: "\(val.charSpriteSlots)")
                metricTile("metrics.maps", value: "\(val.hugeMapCount) + \(val.huge2MapCount)")
                metricTile("metrics.sfx", value: "\(val.sndSlots)")
                metricTile("metrics.titles", value: "\(val.mapTitleCount)")
                metricTile("metrics.heroes", value: "\(val.heroCount) / \(val.monsterCount)")
            }
            .padding(.vertical, 4)

            if !val.sampleMapTitle.isEmpty || !val.sampleDialogue.isEmpty {
                VStack(alignment: .leading, spacing: 4) {
                    if !val.sampleMapTitle.isEmpty {
                        Text("\(val.encodingName) • \(val.sampleMapTitle)")
                            .font(.caption.weight(.medium))
                    }
                    if !val.sampleDialogue.isEmpty {
                        Text(val.sampleDialogue)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .lineLimit(2)
                    }
                }
            }
        }
    }

    private func metricTile(_ titleKey: LocalizedStringKey, value: String) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(titleKey)
                .font(.caption)
                .foregroundStyle(.secondary)
            Text(value)
                .font(.title3.bold().monospacedDigit())
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(8)
        .background(.background.opacity(0.6), in: RoundedRectangle(cornerRadius: 8))
    }
}
