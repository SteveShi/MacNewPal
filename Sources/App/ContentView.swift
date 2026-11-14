import SwiftUI

struct ContentView: View {
    @AppStorage("gameDataPath") private var gameDataPath: String = ""
    @AppStorage("gameDataBookmark") private var gameDataBookmark: Data = Data()
    @AppStorage("windowScale") private var windowScale: Int = 2
    @AppStorage("vsyncEnabled") private var vsyncEnabled: Bool = true
    @AppStorage("nearestFilter") private var nearestFilter: Bool = true
    @AppStorage("hdMode") private var hdMode: Bool = true
    @AppStorage("shaderFilter") private var shaderFilter: Int = 1
    @AppStorage("masterVolume") private var masterVolume: Double = 0.85
    @AppStorage("muteAudio") private var muteAudio: Bool = false

    let store: PalLauncherStore

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                headerSection
                byodSection
                launchSection
#if PAL_ENABLE_TEST_MODE
                TestModeLauncherGroupBox()
#endif
                if let val = store.validation {
                    checklistSection(val)
                }
            }
            .padding(20)
        }
        .frame(minWidth: 740, minHeight: 620)
        .onAppear {
            let resolved = store.restoreBookmarkIfNeeded(storedPath: gameDataPath, storedBookmarkData: gameDataBookmark)
            if !resolved.isEmpty {
                gameDataPath = resolved
                store.validateDirectory(resolved)
            }
        }
    }

    private var headerSection: some View {
        HStack(alignment: .center, spacing: 14) {
            Image(systemName: "gamecontroller.fill")
                .font(.system(size: 32))
                .foregroundStyle(.tint)

            VStack(alignment: .leading, spacing: 3) {
                Text("app.title")
                    .font(.title2.bold())
                Text("app.subtitle")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
            }

            Spacer()

            VStack(alignment: .trailing, spacing: 4) {
                Label(
                    store.metalAvailable ? "badge.sdl3_metal_ready" : "badge.sdl3_metal_missing",
                    systemImage: store.metalAvailable ? "checkmark.seal.fill" : "exclamationmark.triangle.fill"
                )
                .font(.caption.weight(.semibold))
                .foregroundStyle(store.metalAvailable ? .green : .red)
                if !store.availableRenderDrivers.isEmpty {
                    Text(store.availableRenderDrivers)
                        .font(.caption2.monospaced())
                        .foregroundStyle(.secondary)
                }
            }
        }
        .padding(14)
        .background(.quaternary.opacity(0.45), in: RoundedRectangle(cornerRadius: 12))
    }

    private var byodSection: some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 10) {
                HStack {
                    Text("byod.card_title")
                        .font(.headline)
                    Spacer()
                    if let val = store.validation {
                        Label(
                            val.isValid ? "byod.status_verified" : "byod.status_incomplete",
                            systemImage: val.isValid ? "checkmark.circle.fill" : "xmark.circle.fill"
                        )
                        .font(.subheadline.weight(.semibold))
                        .foregroundStyle(val.isValid ? .green : .orange)
                    }
                }

                Text("byod.legal_notice")
                    .font(.callout)
                    .foregroundStyle(.secondary)

                HStack {
                    Text(gameDataPath.isEmpty ? String(localized: "byod.no_folder_selected") : gameDataPath)
                        .font(.callout.monospaced())
                        .foregroundStyle(gameDataPath.isEmpty ? .tertiary : .primary)
                        .lineLimit(1)
                        .truncationMode(.middle)
                        .textSelection(.enabled)

                    Spacer()

                    Button("byod.choose_folder") {
                        store.chooseGameDirectory { path, bookmark in
                            gameDataPath = path
                            gameDataBookmark = bookmark
                        }
                    }
                    .buttonStyle(.borderedProminent)

                    Button("byod.revalidate") {
                        store.validateDirectory(gameDataPath)
                    }
                    .disabled(gameDataPath.isEmpty)
                }

                if let val = store.validation, !val.summaryMessage.isEmpty {
                    Text(val.summaryMessage)
                        .font(.caption.monospaced())
                        .foregroundStyle(.secondary)
                }
            }
            .padding(6)
        }
    }

    private func checklistSection(_ val: ValidationSummaryModel) -> some View {
        GroupBox("checklist.title") {
            LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible())], spacing: 6) {
                ForEach(val.checks) { item in
                    HStack(spacing: 8) {
                        Image(systemName: (item.exists && item.signatureOK) ? "checkmark.circle.fill" : "xmark.octagon.fill")
                            .foregroundStyle((item.exists && item.signatureOK) ? .green : .red)
                        VStack(alignment: .leading, spacing: 1) {
                            Text(item.filename)
                                .font(.caption.bold().monospaced())
                            Text(item.detail)
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                                .lineLimit(1)
                        }
                        Spacer()
                    }
                    .padding(.vertical, 3)
                }
            }
            .padding(6)
        }
    }

    private var launchSection: some View {
        GroupBox("launch.section_title") {
            VStack(alignment: .leading, spacing: 10) {
                HStack(spacing: 12) {
                    VStack(alignment: .leading, spacing: 8) {
                        HStack(spacing: 12) {
                            Text("settings.render_quality")
                                .font(.callout.weight(.medium))
                                .frame(minWidth: 92, alignment: .leading)
                            Picker("settings.render_quality", selection: $hdMode) {
                                Text("settings.quality_original").tag(false)
                                Text("settings.quality_hd").tag(true)
                            }
                            .labelsHidden()
                            .pickerStyle(.segmented)
                            .frame(maxWidth: 310)
                        }

                        HStack(spacing: 12) {
                            Text("settings.gpu_shader")
                                .font(.callout.weight(.medium))
                                .frame(minWidth: 92, alignment: .leading)
                            Picker("settings.gpu_shader", selection: $shaderFilter) {
                                Text("settings.shader_none").tag(0)
                                Text("settings.shader_xbrz").tag(1)
                                Text("settings.shader_crt").tag(2)
                            }
                            .labelsHidden()
                            .pickerStyle(.segmented)
                            .frame(maxWidth: 310)
                        }
                    }

                    Spacer()

                    Button {
                        store.launchEngine(
                            gameDir: gameDataPath,
                            windowScale: windowScale,
                            vsyncEnabled: vsyncEnabled,
                            nearestFilter: nearestFilter,
                            hdMode: hdMode,
                            shaderFilter: shaderFilter,
                            masterVolume: masterVolume,
                            muteAudio: muteAudio
                        )
                    } label: {
                        Label("launch.button", systemImage: "play.fill")
                            .padding(.horizontal, 8)
                            .padding(.vertical, 4)
                    }
                    .buttonStyle(.borderedProminent)
                    .controlSize(.large)
                    .disabled(store.validation?.isValid != true || store.isRunningEngine)
                }

                if !store.lastErrorMessage.isEmpty {
                    Text(store.lastErrorMessage)
                        .font(.caption.monospaced())
                        .foregroundStyle(.red)
                }
            }
            .padding(6)
        }
    }
}
