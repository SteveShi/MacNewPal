import AppKit
import Foundation
import SwiftUI

struct ArchiveCheckItem: Identifiable, Sendable {
    let id: String
    let filename: String
    let category: String
    let exists: Bool
    let signatureOK: Bool
    let itemCount: Int
    let detail: String
}

struct ValidationSummaryModel: Sendable {
    let isValid: Bool
    let validArchivesCount: Int
    let totalRequiredArchives: Int
    let charSpriteSlots: Int
    let charActSlots: Int
    let sndSlots: Int
    let hugeMapCount: Int
    let huge2MapCount: Int
    let heroCount: Int
    let monsterCount: Int
    let itemCount: Int
    let magicCount: Int
    let mapTitleCount: Int
    let namexCount: Int
    let encodingName: String
    let sdlRenderDriver: String
    let sampleMapTitle: String
    let sampleDialogue: String
    let summaryMessage: String
    let checks: [ArchiveCheckItem]
}

@MainActor
@Observable
final class PalLauncherStore {
    var availableRenderDrivers: String = ""
    var metalAvailable: Bool = false
    var validation: ValidationSummaryModel?
    var lastErrorMessage: String = ""
    var isRunningEngine: Bool = false

    private var activeScopedURL: URL?

    init() {
        refreshRenderBackendStatus()
    }

    func refreshRenderBackendStatus() {
        var buffer = [CChar](repeating: 0, count: 256)
        let ok = pal_sdl3_query_metal_backend(&buffer, Int32(buffer.count))
        metalAvailable = (ok == 1)
        let endIdx = buffer.firstIndex(of: 0) ?? buffer.count
        availableRenderDrivers = String(decoding: buffer.prefix(endIdx).map { UInt8(bitPattern: $0) }, as: UTF8.self)
    }

    func restoreBookmarkIfNeeded(storedPath: String, storedBookmarkData: Data) -> String {
        if !storedBookmarkData.isEmpty {
            var stale = false
            if let url = try? URL(
                resolvingBookmarkData: storedBookmarkData,
                options: [.withSecurityScope],
                relativeTo: nil,
                bookmarkDataIsStale: &stale
            ) {
                activeScopedURL?.stopAccessingSecurityScopedResource()
                if url.startAccessingSecurityScopedResource() {
                    activeScopedURL = url
                }
                return url.path
            }
        }
        return storedPath
    }

    func chooseGameDirectory(onSelected: (String, Data) -> Void) {
        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        panel.prompt = String(localized: "byod.open_panel_prompt")
        panel.message = String(localized: "byod.legal_notice")

        if panel.runModal() == .OK, let url = panel.url {
            activeScopedURL?.stopAccessingSecurityScopedResource()
            if url.startAccessingSecurityScopedResource() {
                activeScopedURL = url
            }
            let bookmark = (try? url.bookmarkData(
                options: [.withSecurityScope],
                includingResourceValuesForKeys: nil,
                relativeTo: nil
            )) ?? Data()
            onSelected(url.path, bookmark)
            validateDirectory(url.path)
        }
    }

    func validateDirectory(_ path: String) {
        refreshRenderBackendStatus()
        guard !path.isEmpty else {
            validation = nil
            lastErrorMessage = ""
            return
        }

        var report = PalValidationReport()
        _ = path.withCString { cPath in
            pal_vfs_validate_directory(cPath, &report)
        }

        let checkCount = Int(min(max(0, report.check_count), PAL_MAX_CHECKS))
        var checkItems: [ArchiveCheckItem] = []
        withUnsafeBytes(of: report.checks) { raw in
            let ptr = raw.bindMemory(to: PalArchiveCheck.self)
            for i in 0..<checkCount {
                let c = ptr[i]
                let fname = Self.stringFromTuple(c.filename)
                checkItems.append(ArchiveCheckItem(
                    id: fname,
                    filename: fname,
                    category: Self.stringFromTuple(c.category),
                    exists: c.exists != 0,
                    signatureOK: c.signature_ok != 0,
                    itemCount: Int(c.item_count),
                    detail: Self.stringFromTuple(c.detail)
                ))
            }
        }

        validation = ValidationSummaryModel(
            isValid: report.is_valid != 0,
            validArchivesCount: Int(report.valid_archives_count),
            totalRequiredArchives: Int(report.total_required_archives),
            charSpriteSlots: Int(report.char_sprite_slots),
            charActSlots: Int(report.char_act_slots),
            sndSlots: Int(report.snd_slots),
            hugeMapCount: Int(report.huge_map_count),
            huge2MapCount: Int(report.huge2_map_count),
            heroCount: Int(report.hero_count),
            monsterCount: Int(report.monster_count),
            itemCount: Int(report.item_count),
            magicCount: Int(report.magic_count),
            mapTitleCount: Int(report.map_title_count),
            namexCount: Int(report.namex_count),
            encodingName: Self.stringFromTuple(report.encoding_name),
            sdlRenderDriver: Self.stringFromTuple(report.sdl_render_driver),
            sampleMapTitle: Self.stringFromTuple(report.sample_map_title),
            sampleDialogue: Self.stringFromTuple(report.sample_dialogue),
            summaryMessage: Self.stringFromTuple(report.summary_message),
            checks: checkItems
        )
    }

    func launchEngine(
        gameDir: String,
        windowScale: Int,
        vsyncEnabled: Bool,
        nearestFilter: Bool,
        hdMode: Bool,
        shaderFilter: Int,
        masterVolume: Double,
        muteAudio: Bool,
        textLanguage: Int = 0
    ) {
        guard validation?.isValid == true, !isRunningEngine else { return }
        lastErrorMessage = ""
        isRunningEngine = true

        var cfg = PalLaunchConfig()
        Self.writeCString(gameDir, into: &cfg.game_dir)
        cfg.initial_map_index = 0
        cfg.initial_char_slot = 0
        cfg.window_scale = Int32(windowScale)
        cfg.vsync_enabled = vsyncEnabled ? 1 : 0
        cfg.nearest_filter = nearestFilter ? 1 : 0
        cfg.hd_mode = hdMode ? 1 : 0
        cfg.shader_filter = Int32(shaderFilter)
        cfg.master_volume = Float(min(max(masterVolume, 0.0), 1.0))
        cfg.mute_audio = muteAudio ? 1 : 0
        cfg.text_language = Int32(textLanguage)
#if PAL_ENABLE_TEST_MODE
        cfg.test_cfg = TestModeDefaults.currentConfig()
#endif

        var errBuf = [CChar](repeating: 0, count: 512)
        let rc = pal_sdl3_run_session(&cfg, &errBuf, Int32(errBuf.count))
        isRunningEngine = false

        if rc != 0 {
            let endIdx = errBuf.firstIndex(of: 0) ?? errBuf.count
            lastErrorMessage = String(decoding: errBuf.prefix(endIdx).map { UInt8(bitPattern: $0) }, as: UTF8.self)
        }
    }

    private static func stringFromTuple<T>(_ tuple: T) -> String {
        withUnsafeBytes(of: tuple) { raw in
            let bytes = raw.bindMemory(to: UInt8.self)
            let len = bytes.firstIndex(of: 0) ?? bytes.count
            return String(decoding: bytes.prefix(len), as: UTF8.self)
        }
    }

    private static func writeCString<T>(_ value: String, into tuple: inout T) {
        withUnsafeMutableBytes(of: &tuple) { raw in
            raw.initializeMemory(as: UInt8.self, repeating: 0)
            let utf8 = Array(value.utf8.prefix(max(0, raw.count - 1)))
            for (idx, b) in utf8.enumerated() {
                raw[idx] = b
            }
        }
    }
}
