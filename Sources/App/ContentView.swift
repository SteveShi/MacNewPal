import AppKit
import SwiftUI

struct ContentView: View {
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
    @State private var showingDeleteDataConfirm: Bool = false

    let store: PalLauncherStore

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
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
        .frame(minWidth: 880, minHeight: 620)
        .onAppear {
            store.refreshStandardGameData()
        }
        .alert("byod.delete_confirm_title", isPresented: $showingDeleteDataConfirm) {
            Button("byod.delete_confirm_action", role: .destructive) {
                store.deleteImportedGameData()
            }
            Button("byod.cancel", role: .cancel) {}
        } message: {
            Text("byod.delete_confirm_message")
        }
    }

    private var headerSection: some View {
        HStack(alignment: .center, spacing: 14) {
            Image(systemName: "gamecontroller.fill")
                .font(.system(size: 32))
                .foregroundStyle(.tint)

            Text("app.title")
                .font(.title2.bold())

            Spacer()

            Label(
                store.metalAvailable ? "badge.sdl3_metal_ready" : "badge.sdl3_metal_missing",
                systemImage: store.metalAvailable ? "checkmark.seal.fill" : "exclamationmark.triangle.fill"
            )
            .font(.caption.weight(.semibold))
            .foregroundStyle(store.metalAvailable ? .green : .red)
            .padding(.horizontal, 10)
            .padding(.vertical, 5)
            .background(
                (store.metalAvailable ? Color.green : Color.red).opacity(0.12),
                in: Capsule()
            )
        }
        .padding(14)
        .background(.quaternary.opacity(0.45), in: RoundedRectangle(cornerRadius: 12))
    }

    private var byodSection: some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 12) {
                HStack(alignment: .center) {
                    Text("byod.card_title")
                        .font(.headline)
                    Spacer()
                    statusBadge
                }

                Text("byod.legal_notice")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                Grid(alignment: .leadingFirstTextBaseline, horizontalSpacing: 12, verticalSpacing: 8) {
                    GridRow {
                        Text("byod.data_dir_label")
                            .font(.caption.weight(.semibold))
                            .foregroundStyle(.secondary)
                            .fixedSize(horizontal: true, vertical: false)

                        VStack(alignment: .leading, spacing: 3) {
                            HStack(spacing: 8) {
                                Text(store.displayDataPath)
                                    .font(.caption.monospaced())
                                    .foregroundStyle(store.hasImportedData ? .primary : .secondary)
                                    .lineLimit(1)
                                    .truncationMode(.middle)
                                    .textSelection(.enabled)

                                if store.hasImportedData && !store.importedDataSizeText.isEmpty {
                                    Text(store.importedDataSizeText)
                                        .font(.caption2.monospacedDigit().weight(.medium))
                                        .foregroundStyle(.secondary)
                                        .padding(.horizontal, 6)
                                        .padding(.vertical, 2)
                                        .background(.quaternary, in: Capsule())
                                }
                            }

                            if store.hasImportedData && store.validation?.isValid == true {
                                Text("byod.imported_ready_note")
                                    .font(.caption2)
                                    .foregroundStyle(.green)
                            } else if !store.hasImportedData {
                                Text("byod.no_folder_selected")
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                            }
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                    }

                    GridRow {
                        Text("byod.save_dir_label")
                            .font(.caption.weight(.semibold))
                            .foregroundStyle(.secondary)
                            .fixedSize(horizontal: true, vertical: false)

                        HStack(spacing: 8) {
                            Text(store.displaySavePath)
                                .font(.caption.monospaced())
                                .foregroundStyle(.secondary)
                                .lineLimit(1)
                                .truncationMode(.middle)
                                .textSelection(.enabled)

                            Group {
                                if store.saveSlotCount > 0 {
                                    Text(String(format: String(localized: "byod.save_count_fmt"), Int64(store.saveSlotCount)))
                                } else {
                                    Text("byod.save_count_empty")
                                }
                            }
                            .font(.caption2.monospacedDigit())
                            .foregroundStyle(.secondary)
                            .padding(.horizontal, 6)
                            .padding(.vertical, 2)
                            .background(.quaternary, in: Capsule())
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
                .padding(10)
                .background(.quaternary.opacity(0.35), in: RoundedRectangle(cornerRadius: 8))

                HStack(spacing: 8) {
                    Button {
                        store.importGameDirectory()
                    } label: {
                        Label(
                            store.hasImportedData ? "byod.reimport_data" : "byod.import_data",
                            systemImage: "square.and.arrow.down"
                        )
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(store.isImportingData || store.isRunningEngine)

                    Button {
                        store.refreshStandardGameData()
                    } label: {
                        Label("byod.revalidate", systemImage: "arrow.clockwise")
                    }
                    .disabled(store.isImportingData)

                    Button(role: .destructive) {
                        showingDeleteDataConfirm = true
                    } label: {
                        Label("byod.delete_data", systemImage: "trash")
                    }
                    .disabled(!store.hasImportedData || store.isImportingData || store.isRunningEngine)

                    Spacer(minLength: 12)

                    Button {
                        store.openAppSupportDirectory()
                    } label: {
                        Label("byod.open_data_folder", systemImage: "folder")
                    }

                    Button {
                        store.openSaveDirectory()
                    } label: {
                        Label("byod.open_save_folder", systemImage: "archivebox")
                    }
                }

                if store.isImportingData {
                    VStack(alignment: .leading, spacing: 4) {
                        ProgressView(value: store.importProgress)
                        Text(store.importStatusText)
                            .font(.caption.monospacedDigit())
                            .foregroundStyle(.secondary)
                    }
                }

                if !store.importErrorMessage.isEmpty {
                    Text(store.importErrorMessage)
                        .font(.caption)
                        .foregroundStyle(.red)
                }

                if let val = store.validation, !val.summaryMessage.isEmpty {
                    Text(val.summaryMessage)
                        .font(.caption.monospaced())
                        .foregroundStyle(.secondary)
                }
            }
            .padding(8)
        }
    }

    @ViewBuilder
    private var statusBadge: some View {
        if let val = store.validation {
            let color: Color = val.isValid ? .green : .orange
            Label(
                val.isValid ? "byod.status_verified" : "byod.status_incomplete",
                systemImage: val.isValid ? "checkmark.circle.fill" : "xmark.circle.fill"
            )
            .font(.caption.weight(.semibold))
            .foregroundStyle(color)
            .padding(.horizontal, 10)
            .padding(.vertical, 4)
            .background(color.opacity(0.12), in: Capsule())
        } else {
            Label("byod.status_not_imported", systemImage: "tray.and.arrow.down")
                .font(.caption.weight(.semibold))
                .foregroundStyle(.secondary)
                .padding(.horizontal, 10)
                .padding(.vertical, 4)
                .background(.quaternary.opacity(0.6), in: Capsule())
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
                HStack(alignment: .center, spacing: 16) {
                    Grid(alignment: .leading, horizontalSpacing: 12, verticalSpacing: 10) {
                        GridRow {
                            Text("settings.game_language")
                                .font(.callout.weight(.medium))
                                .fixedSize(horizontal: true, vertical: false)
                            AlignedSegmentedPicker(
                                labels: [
                                    String(localized: "settings.lang_traditional"),
                                    String(localized: "settings.lang_simplified")
                                ],
                                selection: $gameLanguage,
                                distribution: .fillEqually
                            )
                        }
                        GridRow {
                            Text("settings.render_quality")
                                .font(.callout.weight(.medium))
                                .fixedSize(horizontal: true, vertical: false)
                            AlignedSegmentedPicker(
                                labels: [
                                    String(localized: "settings.quality_original"),
                                    String(localized: "settings.quality_hd")
                                ],
                                selection: Binding(
                                    get: { hdMode ? 1 : 0 },
                                    set: { hdMode = ($0 == 1) }
                                ),
                                distribution: .fillEqually
                            )
                        }

                        GridRow {
                            Text("settings.gpu_shader")
                                .font(.callout.weight(.medium))
                                .fixedSize(horizontal: true, vertical: false)
                            AlignedSegmentedPicker(
                                labels: [
                                    String(localized: "settings.shader_none"),
                                    String(localized: "settings.shader_xbrz"),
                                    String(localized: "settings.shader_crt")
                                ],
                                selection: $shaderFilter,
                                distribution: .fillProportionally
                            )
                        }
                    }
                    .fixedSize(horizontal: true, vertical: false)

                    Spacer(minLength: 16)

                    if store.isRunningHeadless {
                        Button(role: .destructive) {
                            store.stopHeadlessEngine()
                        } label: {
                            Label("launch.stop_headless", systemImage: "stop.fill")
                                .padding(.horizontal, 8)
                                .padding(.vertical, 4)
                        }
                        .buttonStyle(.borderedProminent)
                        .tint(.red)
                        .controlSize(.large)
                        .fixedSize()
                    } else {
                        Button {
                            store.launchEngine(
                                windowScale: windowScale,
                                vsyncEnabled: vsyncEnabled,
                                nearestFilter: nearestFilter,
                                hdMode: hdMode,
                                shaderFilter: shaderFilter,
                                masterVolume: masterVolume,
                                muteAudio: muteAudio,
                                textLanguage: gameLanguage,
                                mcpEnabled: mcpEnabled,
                                mcpAllowQaTools: mcpAllowQaTools,
                                mcpHeadlessLaunch: mcpHeadlessLaunch
                            )
                        } label: {
                            Label("launch.button", systemImage: "play.fill")
                                .padding(.horizontal, 8)
                                .padding(.vertical, 4)
                        }
                        .buttonStyle(.borderedProminent)
                        .controlSize(.large)
                        .fixedSize()
                        .disabled(!store.hasImportedData || store.validation?.isValid != true || store.isRunningEngine || store.isImportingData)
                    }
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

private struct AlignedSegmentedPicker: NSViewRepresentable {
    let labels: [String]
    @Binding var selection: Int
    var distribution: NSSegmentedControl.Distribution = .fillEqually

    func makeCoordinator() -> Coordinator {
        Coordinator(self)
    }

    func makeNSView(context: Context) -> NSSegmentedControl {
        let sc = NSSegmentedControl(
            labels: labels,
            trackingMode: .selectOne,
            target: context.coordinator,
            action: #selector(Coordinator.changed(_:))
        )
        sc.segmentDistribution = distribution
        if selection >= 0 && selection < labels.count {
            sc.selectedSegment = selection
        }
        sc.setContentHuggingPriority(.defaultLow, for: .horizontal)
        sc.setContentCompressionResistancePriority(.required, for: .horizontal)
        return sc
    }

    func updateNSView(_ sc: NSSegmentedControl, context: Context) {
        context.coordinator.parent = self
        if sc.segmentCount != labels.count {
            sc.segmentCount = labels.count
        }
        for (i, title) in labels.enumerated() where sc.label(forSegment: i) != title {
            sc.setLabel(title, forSegment: i)
        }
        if sc.segmentDistribution != distribution {
            sc.segmentDistribution = distribution
        }
        if sc.selectedSegment != selection && selection >= 0 && selection < sc.segmentCount {
            sc.selectedSegment = selection
        }
    }

    func sizeThatFits(_ proposal: ProposedViewSize, nsView: NSSegmentedControl, context: Context) -> CGSize? {
        let intrinsic = nsView.intrinsicContentSize
        let h = ceil(intrinsic.height)
        if let pw = proposal.width, pw.isFinite {
            return CGSize(width: ceil(max(intrinsic.width, pw)), height: h)
        }
        return CGSize(width: ceil(intrinsic.width), height: h)
    }

    @MainActor
    final class Coordinator: NSObject {
        var parent: AlignedSegmentedPicker

        init(_ parent: AlignedSegmentedPicker) {
            self.parent = parent
        }

        @objc func changed(_ sender: NSSegmentedControl) {
            let idx = sender.selectedSegment
            if idx >= 0 && idx < parent.labels.count && parent.selection != idx {
                parent.selection = idx
            }
        }
    }
}
