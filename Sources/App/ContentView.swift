import SwiftUI

struct ContentView: View {
    @AppStorage("gameDataPath") private var gameDataPath: String = ""
    @AppStorage("gameDataBookmark") private var gameDataBookmark: Data = Data()
    @AppStorage("windowScale") private var windowScale: Int = 2
    @AppStorage("vsyncEnabled") private var vsyncEnabled: Bool = true
    @AppStorage("nearestFilter") private var nearestFilter: Bool = true
    @AppStorage("masterVolume") private var masterVolume: Double = 0.85
    @AppStorage("muteAudio") private var muteAudio: Bool = false
    @State private var initialMapIndex: Int = 0
    @State private var initialCharSlot: Int = 0

    let store: PalLauncherStore

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                headerSection
                byodSection
                if let val = store.validation {
                    metricsSection(val)
                    if !val.heroes.isEmpty {
                        heroPreviewSection(val.heroes)
                    }
                    checklistSection(val)
                }
                launchSection
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

    private func metricsSection(_ val: ValidationSummaryModel) -> some View {
        GroupBox("metrics.section_title") {
            VStack(alignment: .leading, spacing: 10) {
                LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible()), GridItem(.flexible())], spacing: 10) {
                    metricTile("metrics.archives", value: "\(val.validArchivesCount)/\(val.totalRequiredArchives)")
                    metricTile("metrics.sprites", value: "\(val.charSpriteSlots)")
                    metricTile("metrics.maps", value: "\(val.hugeMapCount) + \(val.huge2MapCount)")
                    metricTile("metrics.sfx", value: "\(val.sndSlots)")
                    metricTile("metrics.titles", value: "\(val.mapTitleCount)")
                    metricTile("metrics.heroes", value: "\(val.heroCount) / \(val.monsterCount)")
                }

                if !val.sampleMapTitle.isEmpty || !val.sampleDialogue.isEmpty {
                    Divider()
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
            .padding(6)
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

    private func heroPreviewSection(_ heroes: [HeroPreviewItem]) -> some View {
        GroupBox("hero.preview_title") {
            VStack(spacing: 6) {
                ForEach(heroes.prefix(4)) { hero in
                    HStack {
                        Text(hero.name)
                            .font(.subheadline.weight(.semibold))
                            .frame(width: 96, alignment: .leading)
                        Text("HP \(hero.hpText)")
                            .font(.caption.monospacedDigit())
                            .frame(width: 90, alignment: .leading)
                        Text("MP \(hero.mpText)")
                            .font(.caption.monospacedDigit())
                            .frame(width: 80, alignment: .leading)
                        Spacer()
                        Text(hero.statsText)
                            .font(.caption.monospaced())
                            .foregroundStyle(.secondary)
                    }
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
                HStack(spacing: 18) {
                    let maxMap = max(0, (store.validation?.hugeMapCount ?? 1) - 1)
                    let maxSprite = max(0, (store.validation?.charSpriteSlots ?? 1) - 1)

                    Stepper(value: $initialMapIndex, in: 0...maxMap) {
                        HStack {
                            Text("launch.initial_map")
                            Text("#\(initialMapIndex)")
                                .monospacedDigit()
                        }
                    }

                    Stepper(value: $initialCharSlot, in: 0...maxSprite) {
                        HStack {
                            Text("launch.initial_char")
                            Text("#\(initialCharSlot)")
                                .monospacedDigit()
                        }
                    }

                    Spacer()

                    Button {
                        store.launchEngine(
                            gameDir: gameDataPath,
                            initialMapIndex: initialMapIndex,
                            initialCharSlot: initialCharSlot,
                            windowScale: windowScale,
                            vsyncEnabled: vsyncEnabled,
                            nearestFilter: nearestFilter,
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

                Text("launch.controls_hint")
                    .font(.caption)
                    .foregroundStyle(.secondary)

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
