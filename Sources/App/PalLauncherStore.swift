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
    var isRunningHeadless: Bool = false

    var hasImportedData: Bool = false
    var importedDataSizeText: String = ""
    var saveSlotCount: Int = 0
    var isImportingData: Bool = false
    var importProgress: Double = 0.0
    var importStatusText: String = ""
    var importErrorMessage: String = ""

    private var headlessProcess: Process?
    private var terminateObserver: NSObjectProtocol?

    private var applicationSupportBaseURL: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
            ?? URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent("Library/Application Support", isDirectory: true)
    }

    var appSupportDirectoryURL: URL {
        applicationSupportBaseURL.appendingPathComponent("NewPal", isDirectory: true)
    }

    var standardGameDataDirectoryURL: URL {
        appSupportDirectoryURL.appendingPathComponent("PalGame", isDirectory: true)
    }

    var saveDirectoryURL: URL {
        applicationSupportBaseURL
            .appendingPathComponent("com.steveshi", isDirectory: true)
            .appendingPathComponent("NewPal", isDirectory: true)
            .appendingPathComponent("Save", isDirectory: true)
    }

    var displayDataPath: String {
        (standardGameDataDirectoryURL.path as NSString).abbreviatingWithTildeInPath
    }

    var displaySavePath: String {
        (saveDirectoryURL.path as NSString).abbreviatingWithTildeInPath
    }

    init() {
        refreshRenderBackendStatus()
        terminateObserver = NotificationCenter.default.addObserver(
            forName: NSApplication.willTerminateNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            MainActor.assumeIsolated {
                self?.stopHeadlessEngine()
            }
        }
    }

    func refreshRenderBackendStatus() {
        var buffer = [CChar](repeating: 0, count: 256)
        let ok = pal_sdl3_query_metal_backend(&buffer, Int32(buffer.count))
        metalAvailable = (ok == 1)
        let endIdx = buffer.firstIndex(of: 0) ?? buffer.count
        availableRenderDrivers = String(decoding: buffer.prefix(endIdx).map { UInt8(bitPattern: $0) }, as: UTF8.self)
    }

    @discardableResult
    func ensureAppSupportDirectory() -> URL {
        let dir = appSupportDirectoryURL
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    func openAppSupportDirectory() {
        let dir = ensureAppSupportDirectory()
        NSWorkspace.shared.open(dir)
    }

    @discardableResult
    func ensureSaveDirectory() -> URL {
        let dir = saveDirectoryURL
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    func openSaveDirectory() {
        let dir = ensureSaveDirectory()
        NSWorkspace.shared.open(dir)
    }

    func refreshStandardGameData() {
        let fm = FileManager.default
        let standardDir = standardGameDataDirectoryURL
        var isDir: ObjCBool = false
        let dirExists = fm.fileExists(atPath: standardDir.path, isDirectory: &isDir) && isDir.boolValue
        let contents = dirExists ? (try? fm.contentsOfDirectory(atPath: standardDir.path)) : nil

        if dirExists, let contents, !contents.isEmpty {
            hasImportedData = true
            let totalBytes = Self.directoryByteSize(at: standardDir)
            importedDataSizeText = ByteCountFormatter.string(fromByteCount: totalBytes, countStyle: .file)
            UserDefaults.standard.set(standardDir.path, forKey: "gameDataPath")
            UserDefaults.standard.removeObject(forKey: "gameDataBookmark")
            validateDirectory(standardDir.path)
        } else {
            hasImportedData = false
            importedDataSizeText = ""
            validation = nil
            UserDefaults.standard.removeObject(forKey: "gameDataPath")
            UserDefaults.standard.removeObject(forKey: "gameDataBookmark")
        }

        let saveDir = saveDirectoryURL
        var count = 0
        for slot in 0..<10 {
            let slotURL = saveDir.appendingPathComponent("\(slot).sav")
            if fm.fileExists(atPath: slotURL.path) {
                count += 1
            }
        }
        saveSlotCount = count
    }

    func importGameDirectory() {
        guard !isImportingData, !isRunningEngine else { return }

        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        panel.prompt = String(localized: "byod.open_panel_prompt")
        panel.message = String(localized: "byod.open_panel_message")

        guard panel.runModal() == .OK, let pickedURL = panel.url else { return }

        importErrorMessage = ""
        lastErrorMessage = ""

        let didScope = pickedURL.startAccessingSecurityScopedResource()
        let effectiveSourceURL = resolveEffectiveSourceDirectory(from: pickedURL)
        let destURL = standardGameDataDirectoryURL.standardizedFileURL

        if effectiveSourceURL.standardizedFileURL == destURL {
            if didScope {
                pickedURL.stopAccessingSecurityScopedResource()
            }
            refreshStandardGameData()
            return
        }

        var report = PalValidationReport()
        _ = effectiveSourceURL.path.withCString { cPath in
            pal_vfs_validate_directory(cPath, &report)
        }
        if report.valid_archives_count < report.total_required_archives {
            if didScope {
                pickedURL.stopAccessingSecurityScopedResource()
            }
            validateDirectory(effectiveSourceURL.path)
            importErrorMessage = String(
                format: String(localized: "byod.import_validation_failed"),
                Int64(report.valid_archives_count),
                Int64(report.total_required_archives)
            )
            return
        }

        isImportingData = true
        importProgress = 0.0
        importStatusText = String(localized: "byod.importing_preparing")

        let appSupportDir = ensureAppSupportDirectory()
        let stagingURL = appSupportDir.appendingPathComponent(".PalGame.importing", isDirectory: true)
        let backupURL = appSupportDir.appendingPathComponent(".PalGame.backup", isDirectory: true)
        let progressTemplate = String(localized: "byod.importing_progress_fmt")

        Task { @MainActor in
            defer {
                if didScope {
                    pickedURL.stopAccessingSecurityScopedResource()
                }
                self.isImportingData = false
            }
            do {
                try await Task.detached(priority: .userInitiated) {
                    try Self.importSelectiveGameData(
                        from: effectiveSourceURL,
                        stagingURL: stagingURL,
                        backupURL: backupURL,
                        to: destURL
                    ) { fraction, copiedBytes, totalBytes in
                        let copiedText = ByteCountFormatter.string(fromByteCount: copiedBytes, countStyle: .file)
                        let totalText = ByteCountFormatter.string(fromByteCount: totalBytes, countStyle: .file)
                        let status = String(format: progressTemplate, copiedText, totalText)
                        Task { @MainActor in
                            self.importProgress = fraction
                            self.importStatusText = status
                        }
                    }
                }.value
                self.importProgress = 1.0
                self.importStatusText = ""
                self.importErrorMessage = ""
                self.refreshStandardGameData()
            } catch {
                self.importProgress = 0.0
                self.importStatusText = ""
                self.importErrorMessage = error.localizedDescription
                self.refreshStandardGameData()
            }
        }
    }

    func deleteImportedGameData() {
        guard !isImportingData, !isRunningEngine else { return }
        let fm = FileManager.default
        let targets = [
            standardGameDataDirectoryURL,
            appSupportDirectoryURL.appendingPathComponent(".PalGame.importing", isDirectory: true),
            appSupportDirectoryURL.appendingPathComponent(".PalGame.backup", isDirectory: true)
        ]
        for url in targets where fm.fileExists(atPath: url.path) {
            do {
                try fm.removeItem(at: url)
            } catch {
                importErrorMessage = error.localizedDescription
            }
        }
        importProgress = 0.0
        importStatusText = ""
        importErrorMessage = ""
        lastErrorMessage = ""
        refreshStandardGameData()
    }

    private func resolveEffectiveSourceDirectory(from pickedURL: URL) -> URL {
        var directReport = PalValidationReport()
        _ = pickedURL.path.withCString { cPath in
            pal_vfs_validate_directory(cPath, &directReport)
        }
        if directReport.valid_archives_count == directReport.total_required_archives {
            return pickedURL
        }

        let fm = FileManager.default
        guard let subURLs = try? fm.contentsOfDirectory(
            at: pickedURL,
            includingPropertiesForKeys: [.isDirectoryKey],
            options: [.skipsHiddenFiles]
        ) else {
            return pickedURL
        }

        var bestURL = pickedURL
        var bestScore = directReport.valid_archives_count

        let sortedSubs = subURLs.sorted { lhs, rhs in
            let lMatch = lhs.lastPathComponent.caseInsensitiveCompare("PalGame") == .orderedSame
            let rMatch = rhs.lastPathComponent.caseInsensitiveCompare("PalGame") == .orderedSame
            if lMatch != rMatch { return lMatch }
            return lhs.lastPathComponent < rhs.lastPathComponent
        }

        for sub in sortedSubs {
            let isDir = (try? sub.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) ?? false
            guard isDir else { continue }
            var subReport = PalValidationReport()
            _ = sub.path.withCString { cPath in
                pal_vfs_validate_directory(cPath, &subReport)
            }
            if subReport.valid_archives_count == subReport.total_required_archives {
                return sub
            }
            if subReport.valid_archives_count > bestScore {
                bestScore = subReport.valid_archives_count
                bestURL = sub
            }
        }
        return bestURL
    }

    private nonisolated static let requiredAndOptionalRootFiles: [String] = [
        "all_char.tsw", "all_item.tsw", "all_magic.tsw", "all_map1.tsw", "all_map2.tsw", "all_sys.tsw",
        "all_char.act", "all_item.act", "all_magic.act", "all_map1.act", "all_map2.act", "all_sys.act",
        "all.snd",
        "huge.lmf", "huge2.lmf",
        "ghero.dat", "gmon.dat", "gitem.dat", "gmagic.dat", "glevel.dat", "gmap.dat",
        "GFight1.dat", "GFight2.dat", "GFight3.dat", "GFight4.dat",
        "namex.dat", "maps.dat",
        "talk1.dat", "talk2.dat", "talk3.dat", "talk4.dat",
        "talk5.dat", "talk6.dat", "talk7.dat", "talk8.dat",
        "path1.dat", "path2.dat", "path3.dat", "path4.dat", "path5.dat",
        "path6.dat", "path7.dat", "path8.dat", "path9.dat", "path10.dat"
    ]

    private nonisolated static let mediaSubdirectoryNames: [String] = ["Music", "Video"]

    private nonisolated static func importSelectiveGameData(
        from sourceURL: URL,
        stagingURL: URL,
        backupURL: URL,
        to destURL: URL,
        onProgress: @Sendable (Double, Int64, Int64) -> Void
    ) throws {
        let fm = FileManager.default
        if fm.fileExists(atPath: stagingURL.path) {
            try fm.removeItem(at: stagingURL)
        }
        try fm.createDirectory(at: stagingURL, withIntermediateDirectories: true)

        struct CopyItem {
            let src: URL
            let dst: URL
            let size: Int64
        }

        var copyPlan: [CopyItem] = []
        let canonicalByLower: [String: String] = Dictionary(
            uniqueKeysWithValues: requiredAndOptionalRootFiles.map { ($0.lowercased(), $0) }
        )
        let mediaDirByLower: [String: String] = Dictionary(
            uniqueKeysWithValues: mediaSubdirectoryNames.map { ($0.lowercased(), $0) }
        )

        let topEntries = try fm.contentsOfDirectory(
            at: sourceURL,
            includingPropertiesForKeys: [.isRegularFileKey, .isDirectoryKey, .fileSizeKey],
            options: [.skipsHiddenFiles]
        )

        for entry in topEntries {
            let rv = try entry.resourceValues(forKeys: [.isRegularFileKey, .isDirectoryKey, .fileSizeKey])
            let lowerName = entry.lastPathComponent.lowercased()

            if rv.isRegularFile == true, let canonicalName = canonicalByLower[lowerName] {
                let sz = Int64(rv.fileSize ?? 0)
                copyPlan.append(CopyItem(src: entry, dst: stagingURL.appendingPathComponent(canonicalName), size: sz))
            } else if rv.isDirectory == true, let canonicalDir = mediaDirByLower[lowerName] {
                let dstDir = stagingURL.appendingPathComponent(canonicalDir, isDirectory: true)
                try fm.createDirectory(at: dstDir, withIntermediateDirectories: true)

                if let enumerator = fm.enumerator(
                    at: entry,
                    includingPropertiesForKeys: [.isRegularFileKey, .isDirectoryKey, .fileSizeKey],
                    options: [.skipsHiddenFiles]
                ) {
                    let basePrefix = entry.standardizedFileURL.path
                    while let item = enumerator.nextObject() as? URL {
                        let itemRV = try item.resourceValues(forKeys: [.isRegularFileKey, .isDirectoryKey, .fileSizeKey])
                        let itemPath = item.standardizedFileURL.path
                        guard itemPath.hasPrefix(basePrefix) else { continue }
                        let rel = String(itemPath.dropFirst(basePrefix.count)).trimmingCharacters(in: CharacterSet(charactersIn: "/"))
                        guard !rel.isEmpty else { continue }
                        let target = dstDir.appendingPathComponent(rel)
                        if itemRV.isDirectory == true {
                            try fm.createDirectory(at: target, withIntermediateDirectories: true)
                        } else if itemRV.isRegularFile == true {
                            let sz = Int64(itemRV.fileSize ?? 0)
                            copyPlan.append(CopyItem(src: item, dst: target, size: sz))
                        }
                    }
                }
            }
        }

        let totalBytes = max(1, copyPlan.reduce(Int64(0)) { $0 + $1.size })
        var copiedBytes: Int64 = 0
        var lastNotify = CFAbsoluteTimeGetCurrent()

        onProgress(0.0, 0, totalBytes)

        do {
            for item in copyPlan {
                let parent = item.dst.deletingLastPathComponent()
                if !fm.fileExists(atPath: parent.path) {
                    try fm.createDirectory(at: parent, withIntermediateDirectories: true)
                }
                try fm.copyItem(at: item.src, to: item.dst)
                copiedBytes += item.size

                let now = CFAbsoluteTimeGetCurrent()
                if now - lastNotify >= 0.08 || copiedBytes >= totalBytes {
                    lastNotify = now
                    let fraction = min(1.0, Double(copiedBytes) / Double(totalBytes))
                    onProgress(fraction, copiedBytes, totalBytes)
                }
            }

            if fm.fileExists(atPath: backupURL.path) {
                try fm.removeItem(at: backupURL)
            }
            var movedExistingToBackup = false
            if fm.fileExists(atPath: destURL.path) {
                try fm.moveItem(at: destURL, to: backupURL)
                movedExistingToBackup = true
            }
            do {
                try fm.moveItem(at: stagingURL, to: destURL)
            } catch {
                if movedExistingToBackup && !fm.fileExists(atPath: destURL.path) {
                    try? fm.moveItem(at: backupURL, to: destURL)
                }
                throw error
            }
            if movedExistingToBackup && fm.fileExists(atPath: backupURL.path) {
                try? fm.removeItem(at: backupURL)
            }
        } catch {
            try? fm.removeItem(at: stagingURL)
            throw error
        }
    }

    private nonisolated static func directoryByteSize(at url: URL) -> Int64 {
        let fm = FileManager.default
        guard let enumerator = fm.enumerator(
            at: url,
            includingPropertiesForKeys: [.isRegularFileKey, .fileSizeKey],
            options: [.skipsHiddenFiles]
        ) else {
            return 0
        }
        var total: Int64 = 0
        for case let fileURL as URL in enumerator {
            if let rv = try? fileURL.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey]),
               rv.isRegularFile == true {
                total += Int64(rv.fileSize ?? 0)
            }
        }
        return total
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
        windowScale: Int,
        vsyncEnabled: Bool,
        nearestFilter: Bool,
        hdMode: Bool,
        shaderFilter: Int,
        masterVolume: Double,
        muteAudio: Bool,
        textLanguage: Int = 0,
        mcpEnabled: Bool = true,
        mcpAllowQaTools: Bool = true,
        mcpHeadlessLaunch: Bool = false,
        mcpSocketPath: String = "/tmp/newpal-mcp.sock"
    ) {
        let gameDir = standardGameDataDirectoryURL.path
        guard hasImportedData, validation?.isValid == true, !isRunningEngine, !isImportingData else { return }
        lastErrorMessage = ""

        if mcpEnabled && mcpHeadlessLaunch {
            let exePath = Bundle.main.executablePath ?? CommandLine.arguments[0]
            let proc = Process()
            proc.executableURL = URL(fileURLWithPath: exePath)
            var cliArgs = [
                "--play", gameDir,
                "--headless",
                "--mcp-socket", mcpSocketPath,
                "--lang", textLanguage == 1 ? "sc" : "tc",
                "--scale", "\(windowScale)",
                "--hd", hdMode ? "1" : "0",
                "--shader", "\(shaderFilter)",
                "--allow-qa", mcpAllowQaTools ? "1" : "0",
                "--skip-videos",
                "--mute"
            ]
#if PAL_ENABLE_TEST_MODE
            if TestModeDefaults.currentConfig().enabled != 0 {
                cliArgs.append("--test-mode")
            }
#endif
            proc.arguments = cliArgs
            proc.standardInput = FileHandle.nullDevice
            proc.terminationHandler = { [weak self] _ in
                DispatchQueue.main.async {
                    guard let self else { return }
                    self.headlessProcess = nil
                    self.isRunningHeadless = false
                    self.isRunningEngine = false
                }
            }
            do {
                try proc.run()
                headlessProcess = proc
                isRunningHeadless = true
                isRunningEngine = true
            } catch {
                lastErrorMessage = error.localizedDescription
                isRunningHeadless = false
                isRunningEngine = false
            }
            return
        }

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
        cfg.mcp_enabled = mcpEnabled ? 1 : 0
        cfg.mcp_allow_qa_tools = mcpAllowQaTools ? 1 : 0
        cfg.headless_mode = 0
        cfg.skip_videos = 0
        Self.writeCString(mcpSocketPath, into: &cfg.mcp_socket_path)
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

    func stopHeadlessEngine() {
        guard let proc = headlessProcess else {
            isRunningHeadless = false
            return
        }
        if proc.isRunning {
            proc.terminate()
        }
        headlessProcess = nil
        isRunningHeadless = false
        isRunningEngine = false
    }

    func mcpConfigSnippet() -> String {
        let exePath = Bundle.main.executablePath ?? "/Applications/NewPal.app/Contents/MacOS/NewPal"
        return """
        {
          "mcpServers": {
            "newpal": {
              "command": "\(exePath)",
              "args": ["--mcp-server"]
            }
          }
        }
        """
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
