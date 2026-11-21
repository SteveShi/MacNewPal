import Foundation
import SwiftUI

@main
@MainActor
enum NewPalLauncherEntry {
    static func main() {
        let args = CommandLine.arguments
        if let idx = args.firstIndex(of: "--self-test"), idx + 1 < args.count {
            let gameDir = args[idx + 1]
            let rc = gameDir.withCString { cDir in
                pal_sdl3_self_test(cDir)
            }
            Foundation.exit(rc)
        }
        if args.contains("--mcp-server") {
            let sock = Self.argValue(in: args, flag: "--socket") ?? "/tmp/newpal-mcp.sock"
            let gdir = Self.argValue(in: args, flag: "--game-dir")
            PalMcpServer.run(socketPath: sock, defaultGameDir: gdir)
        }
        if let playIdx = args.firstIndex(of: "--play"), playIdx + 1 < args.count {
            let rc = Self.runPlayCli(gameDir: args[playIdx + 1], args: args)
            Foundation.exit(rc)
        }
        NewPalApp.main()
    }

    private static func argValue(in args: [String], flag: String) -> String? {
        guard let i = args.firstIndex(of: flag), i + 1 < args.count else { return nil }
        return args[i + 1]
    }

    private static func writeCString(_ value: String, into buffer: UnsafeMutableRawBufferPointer) {
        buffer.initializeMemory(as: UInt8.self, repeating: 0)
        let bytes = Array(value.utf8.prefix(max(0, buffer.count - 1)))
        for (i, b) in bytes.enumerated() {
            buffer[i] = b
        }
    }

    private static func runPlayCli(gameDir: String, args: [String]) -> Int32 {
        var cfg = PalLaunchConfig()
        withUnsafeMutableBytes(of: &cfg.game_dir) { writeCString(gameDir, into: $0) }
        cfg.initial_map_index = 0
        cfg.initial_char_slot = 0
        cfg.window_scale = Int32(argValue(in: args, flag: "--scale").flatMap(Int.init) ?? 2)
        cfg.vsync_enabled = args.contains("--headless") ? 0 : 1
        cfg.nearest_filter = 1
        cfg.hd_mode = Int32(argValue(in: args, flag: "--hd").flatMap(Int.init) ?? 1)
        cfg.shader_filter = Int32(argValue(in: args, flag: "--shader").flatMap(Int.init) ?? 1)
        cfg.master_volume = 0.85
        cfg.mute_audio = args.contains("--mute") || args.contains("--headless") ? 1 : 0
        cfg.text_language = (argValue(in: args, flag: "--lang") == "sc") ? 1 : 0
        cfg.mcp_enabled = 1
        cfg.mcp_allow_qa_tools = Int32(argValue(in: args, flag: "--allow-qa").flatMap(Int.init) ?? 1)
        cfg.headless_mode = args.contains("--headless") ? 1 : 0
        cfg.skip_videos = (args.contains("--skip-videos") || cfg.headless_mode == 1) ? 1 : 0
        let sock = argValue(in: args, flag: "--mcp-socket") ?? "/tmp/newpal-mcp.sock"
        withUnsafeMutableBytes(of: &cfg.mcp_socket_path) { writeCString(sock, into: $0) }
#if PAL_ENABLE_TEST_MODE
        if args.contains("--test-mode") {
            cfg.test_cfg.enabled = 1
            cfg.test_cfg.speed_mult = 1
        }
#endif
        var errBuf = [CChar](repeating: 0, count: 512)
        let rc = pal_sdl3_run_session(&cfg, &errBuf, Int32(errBuf.count))
        if rc != 0 {
            let endIdx = errBuf.firstIndex(of: 0) ?? errBuf.count
            let msg = String(decoding: errBuf.prefix(endIdx).map { UInt8(bitPattern: $0) }, as: UTF8.self)
            if let data = "[newpal --play] \(msg)\n".data(using: .utf8) {
                FileHandle.standardError.write(data)
            }
        }
        return rc
    }
}

struct NewPalApp: App {
    @State private var store = PalLauncherStore()
    @State private var showingWhatsNew = false

    var body: some Scene {
        WindowGroup {
            ContentView(store: store)
                .sheet(isPresented: $showingWhatsNew) {
                    WhatsNewSheetView()
                }
        }
        .commands {
            CommandGroup(replacing: .help) {
                Button("menu.whats_new") {
                    showingWhatsNew = true
                }
            }
        }

        Settings {
            SettingsView(store: store)
        }
    }
}
