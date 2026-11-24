import CoreGraphics
import Darwin
import Foundation
import ImageIO
import UniformTypeIdentifiers

private let kDefaultMcpSocket = "/tmp/newpal-mcp.sock"

private nonisolated(unsafe) var gSpawnedGamePid: pid_t = 0

private let mcpSignalHandler: @convention(c) (Int32) -> Void = { sig in
    let pid = gSpawnedGamePid
    if pid > 0 {
        kill(pid, SIGTERM)
    }
    _exit(128 + sig)
}

enum PalMcpServer {
    static func run(socketPath: String, defaultGameDir: String?) -> Never {
        signal(SIGPIPE, SIG_IGN)
        signal(SIGINT, mcpSignalHandler)
        signal(SIGTERM, mcpSignalHandler)

        let session = McpSession(socketPath: socketPath, defaultGameDir: defaultGameDir)
        session.serveStdioLoop()
        session.cleanupChildProcess()
        Foundation.exit(0)
    }
}

private enum McpClientError: Error, LocalizedError {
    case socketCreateFailed(Int32)
    case socketPathTooLong
    case connectFailed(String)
    case ioFailed(String)
    case invalidResponse

    var errorDescription: String? {
        switch self {
        case .socketCreateFailed(let code):
            return "Failed to create Unix socket (errno=\(code))"
        case .socketPathTooLong:
            return "Unix socket path exceeds sun_path capacity"
        case .connectFailed(let detail):
            return "Game MCP socket unavailable: \(detail)"
        case .ioFailed(let detail):
            return "Socket I/O error: \(detail)"
        case .invalidResponse:
            return "Malformed response from NewPal engine"
        }
    }
}

private final class McpSession {
    let socketPath: String
    let cliDefaultGameDir: String?
    var spawnedProcess: Process?
    var nextBridgeReqId: Int64 = 1

    init(socketPath: String, defaultGameDir: String?) {
        self.socketPath = socketPath.isEmpty ? kDefaultMcpSocket : socketPath
        self.cliDefaultGameDir = defaultGameDir
    }

    deinit {
        cleanupChildProcess()
    }

    func logErr(_ msg: String) {
        if let data = "[newpal-mcp] \(msg)\n".data(using: .utf8) {
            FileHandle.standardError.write(data)
        }
    }

    func cleanupChildProcess() {
        gSpawnedGamePid = 0
        guard let proc = spawnedProcess else { return }
        spawnedProcess = nil
        if proc.isRunning {
            _ = try? sendBridge(cmd: "quit", args: [:], timeoutSec: 1.5)
            let deadline = Date().addingTimeInterval(1.5)
            while proc.isRunning && Date() < deadline {
                Thread.sleep(forTimeInterval: 0.05)
            }
            if proc.isRunning {
                proc.terminate()
            }
        }
    }

    func serveStdioLoop() {
        var buffer = Data()
        var chunk = [UInt8](repeating: 0, count: 8192)
        while true {
            let n = read(STDIN_FILENO, &chunk, chunk.count)
            if n > 0 {
                buffer.append(contentsOf: chunk[0..<n])
                while let nlIndex = buffer.firstIndex(of: 0x0A) {
                    let lineData = buffer[..<nlIndex]
                    buffer.removeSubrange(...nlIndex)
                    var trimmed = lineData
                    if trimmed.last == 0x0D { trimmed = trimmed.dropLast() }
                    if !trimmed.isEmpty {
                        handleRpcLine(Data(trimmed))
                    }
                }
            } else if n == 0 {
                if !buffer.isEmpty {
                    handleRpcLine(buffer)
                }
                break
            } else {
                if errno == EINTR { continue }
                break
            }
        }
    }

    private func writeStdoutJson(_ obj: [String: Any]) {
        guard var data = try? JSONSerialization.data(withJSONObject: obj, options: []) else {
            return
        }
        data.append(0x0A)
        data.withUnsafeBytes { raw in
            guard let base = raw.baseAddress else { return }
            var sent = 0
            while sent < raw.count {
                let w = write(STDOUT_FILENO, base.advanced(by: sent), raw.count - sent)
                if w > 0 {
                    sent += w
                } else if w < 0 && errno == EINTR {
                    continue
                } else {
                    break
                }
            }
        }
    }

    private func sendRpcError(id: Any?, code: Int, message: String) {
        guard let id else { return }
        writeStdoutJson([
            "jsonrpc": "2.0",
            "id": id,
            "error": [
                "code": code,
                "message": message
            ]
        ])
    }

    private func sendRpcResult(id: Any?, result: Any) {
        guard let id else { return }
        writeStdoutJson([
            "jsonrpc": "2.0",
            "id": id,
            "result": result
        ])
    }

    private func handleRpcLine(_ data: Data) {
        guard let rawObj = try? JSONSerialization.jsonObject(with: data, options: []),
              let req = rawObj as? [String: Any] else {
            sendRpcError(id: NSNull(), code: -32700, message: "Parse error")
            return
        }
        let id = req["id"]
        guard let method = req["method"] as? String else {
            sendRpcError(id: id, code: -32600, message: "Invalid Request: missing method")
            return
        }
        let params = (req["params"] as? [String: Any]) ?? [:]

        switch method {
        case "initialize":
            let clientProto = params["protocolVersion"] as? String ?? "2024-11-05"
            let supported = (clientProto == "2025-03-26" || clientProto == "2024-11-05") ? clientProto : "2024-11-05"
            sendRpcResult(id: id, result: [
                "protocolVersion": supported,
                "capabilities": [
                    "tools": ["listChanged": false],
                    "resources": ["subscribe": false, "listChanged": false]
                ],
                "serverInfo": [
                    "name": "newpal-mcp",
                    "version": "1.0.0"
                ]
            ])

        case "notifications/initialized", "notifications/cancelled":
            return

        case "ping":
            sendRpcResult(id: id, result: [String: Any]())

        case "tools/list":
            sendRpcResult(id: id, result: ["tools": Self.makeToolDefinitions()])

        case "tools/call":
            guard let name = params["name"] as? String else {
                sendRpcError(id: id, code: -32602, message: "Missing tool name")
                return
            }
            let args = (params["arguments"] as? [String: Any]) ?? [:]
            let result = executeTool(name: name, args: args)
            sendRpcResult(id: id, result: result)

        case "resources/list":
            sendRpcResult(id: id, result: [
                "resources": [
                    [
                        "uri": "newpal://state",
                        "name": "NewPal Game State",
                        "description": "Live structured snapshot of scene, player position, party stats, dialogue, and combat.",
                        "mimeType": "application/json"
                    ],
                    [
                        "uri": "newpal://map/entities",
                        "name": "NewPal Map Entities & Triggers",
                        "description": "Active NPCs, triggers, and local walkability grid for the current map.",
                        "mimeType": "application/json"
                    ]
                ]
            ])

        case "resources/read":
            guard let uri = params["uri"] as? String else {
                sendRpcError(id: id, code: -32602, message: "Missing resource uri")
                return
            }
            do {
                let cmd: String
                switch uri {
                case "newpal://state":
                    cmd = "get_state"
                case "newpal://map/entities":
                    cmd = "get_map_entities"
                default:
                    sendRpcError(id: id, code: -32602, message: "Unknown resource URI: \(uri)")
                    return
                }
                let res = try sendBridge(cmd: cmd, args: [:], timeoutSec: 5.0)
                let text = encodeJsonString(res.header["data"] ?? [:])
                sendRpcResult(id: id, result: [
                    "contents": [
                        [
                            "uri": uri,
                            "mimeType": "application/json",
                            "text": text
                        ]
                    ]
                ])
            } catch {
                sendRpcError(id: id, code: -32000, message: error.localizedDescription)
            }

        default:
            sendRpcError(id: id, code: -32601, message: "Method not found: \(method)")
        }
    }

    // MARK: - Tool Execution

    private func toolText(_ text: String, isError: Bool = false) -> [String: Any] {
        return [
            "content": [
                ["type": "text", "text": text]
            ],
            "isError": isError
        ]
    }

    private func toolJson(_ obj: Any, isError: Bool = false) -> [String: Any] {
        return toolText(encodeJsonString(obj), isError: isError)
    }

    private func encodeJsonString(_ obj: Any) -> String {
        guard JSONSerialization.isValidJSONObject(obj),
              let data = try? JSONSerialization.data(withJSONObject: obj, options: [.sortedKeys]),
              let str = String(data: data, encoding: .utf8) else {
            return "{}"
        }
        return str
    }

    private func launcherDefaults() -> UserDefaults {
        return UserDefaults(suiteName: "com.steveshi.newpal") ?? UserDefaults.standard
    }

    private func executeTool(name: String, args: [String: Any]) -> [String: Any] {
        switch name {
        case "pal_launch_game":
            return handleLaunchGame(args: args)

        case "pal_close_game":
            return handleCloseGame()

        case "pal_capture_screen":
            return handleCaptureScreen(args: args)

        case "pal_get_state":
            return forwardSimple(cmd: "get_state", args: [:], timeoutSec: 5.0)

        case "pal_get_map_entities":
            var payload: [String: Any] = [:]
            if let r = args["radius"] as? Int { payload["radius"] = r }
            return forwardSimple(cmd: "get_map_entities", args: payload, timeoutSec: 5.0)

        case "pal_get_inventory_and_saves":
            return forwardSimple(cmd: "get_inventory_and_saves", args: [:], timeoutSec: 5.0)

        case "pal_query_flags":
            var payload: [String: Any] = [:]
            if let fs = args["flag_id"] as? Int { payload["flag_id"] = fs }
            if let fe = args["flag_end"] as? Int { payload["flag_end"] = fe }
            if let csv = args["flags_csv"] as? String { payload["flags_csv"] = csv }
            return forwardSimple(cmd: "query_flags", args: payload, timeoutSec: 5.0)

        case "pal_title_action":
            var payload = args
            let t = Double((args["timeout_ms"] as? Int) ?? 5000) / 1000.0 + 3.0
            payload.removeValue(forKey: "cmd")
            return forwardSimple(cmd: "title_action", args: payload, timeoutSec: t)

        case "pal_move_to":
            var payload = args
            let timeoutMs = (args["timeout_ms"] as? Int) ?? 15000
            payload["timeout_ms"] = timeoutMs
            return forwardSimple(cmd: "move_to", args: payload, timeoutSec: Double(timeoutMs) / 1000.0 + 3.0)

        case "pal_interact":
            let timeoutMs = (args["timeout_ms"] as? Int) ?? 5000
            var payload = args
            payload["timeout_ms"] = timeoutMs
            return forwardSimple(cmd: "interact", args: payload, timeoutSec: Double(timeoutMs) / 1000.0 + 3.0)

        case "pal_choose_dialogue":
            return forwardSimple(cmd: "choose_dialogue", args: args, timeoutSec: 5.0)

        case "pal_combat_action":
            let timeoutMs = (args["timeout_ms"] as? Int) ?? 15000
            var payload = args
            payload["timeout_ms"] = timeoutMs
            return forwardSimple(cmd: "combat_action", args: payload, timeoutSec: Double(timeoutMs) / 1000.0 + 3.0)

        case "pal_manage_party":
            return forwardSimple(cmd: "manage_party", args: args, timeoutSec: 5.0)

        case "pal_raw_input":
            return forwardSimple(cmd: "raw_input", args: args, timeoutSec: 10.0)

        case "pal_test_control":
            return forwardSimple(cmd: "test_control", args: args, timeoutSec: 5.0)

        case "pal_teleport":
            return forwardSimple(cmd: "teleport", args: args, timeoutSec: 6.0)

        default:
            return toolText("Unknown tool: \(name)", isError: true)
        }
    }

    private func forwardSimple(cmd: String, args: [String: Any], timeoutSec: Double) -> [String: Any] {
        do {
            let res = try sendBridge(cmd: cmd, args: args, timeoutSec: timeoutSec)
            let ok = (res.header["ok"] as? Bool) ?? false
            if !ok {
                let errMsg = (res.header["error"] as? String) ?? "Engine returned error"
                return toolText(errMsg, isError: true)
            }
            return toolJson(res.header["data"] ?? [:])
        } catch {
            return toolText(error.localizedDescription, isError: true)
        }
    }

    private func handleLaunchGame(args: [String: Any]) -> [String: Any] {
        // Attach if another GUI or headless engine is already listening cleanly
        if (try? sendBridge(cmd: "ping", args: [:], timeoutSec: 0.6)) != nil {
            if let st = try? sendBridge(cmd: "get_state", args: [:], timeoutSec: 2.0) {
                var data = (st.header["data"] as? [String: Any]) ?? [:]
                data["attached_existing"] = true
                return toolJson(data)
            }
        }

        let ud = launcherDefaults()
        let fallbackDir = "/Users/steve/Downloads/PalGame"
        let resolvedDir: String
        if let argDir = args["game_dir"] as? String, !argDir.isEmpty {
            resolvedDir = argDir
        } else if let cliDir = cliDefaultGameDir, !cliDir.isEmpty {
            resolvedDir = cliDir
        } else if let udDir = ud.string(forKey: "gameDataPath"), !udDir.isEmpty {
            resolvedDir = udDir
        } else if FileManager.default.fileExists(atPath: fallbackDir) {
            resolvedDir = fallbackDir
        } else {
            return toolText("Missing game_dir: specify game_dir or configure it in NewPal Launcher first.", isError: true)
        }

        let headless: Bool = {
            if let b = args["headless"] as? Bool { return b }
            if ud.object(forKey: "mcpHeadlessLaunch") != nil {
                return ud.bool(forKey: "mcpHeadlessLaunch")
            }
            return true
        }()
        let lang: String = {
            if let l = args["language"] as? String, (l == "sc" || l == "tc") { return l }
            return ud.integer(forKey: "gameLanguage") == 1 ? "sc" : "tc"
        }()
        let scale = (args["scale"] as? Int) ?? 2
        let hd: Bool = {
            if let b = args["hd"] as? Bool { return b }
            return ud.object(forKey: "hdMode") != nil ? ud.bool(forKey: "hdMode") : true
        }()
        let shader: Int = {
            if let s = args["shader"] as? Int { return s }
            return ud.object(forKey: "shaderFilter") != nil ? ud.integer(forKey: "shaderFilter") : 1
        }()
        let skipVideos = (args["skip_videos"] as? Bool) ?? headless
        let mute = (args["mute"] as? Bool) ?? headless
        let testMode = (args["test_mode"] as? Bool) ?? true
        let allowQa: Bool = {
            if let b = args["allow_qa"] as? Bool { return b }
            return ud.object(forKey: "mcpAllowQaTools") != nil ? ud.bool(forKey: "mcpAllowQaTools") : true
        }()

        let exePath = Bundle.main.executablePath ?? CommandLine.arguments[0]
        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: exePath)
        var cliArgs = [
            "--play", resolvedDir,
            "--mcp-socket", socketPath,
            "--lang", lang,
            "--scale", "\(scale)",
            "--hd", hd ? "1" : "0",
            "--shader", "\(shader)",
            "--allow-qa", allowQa ? "1" : "0"
        ]
        if headless { cliArgs.append("--headless") }
        if skipVideos { cliArgs.append("--skip-videos") }
        if mute { cliArgs.append("--mute") }
        if testMode { cliArgs.append("--test-mode") }
        proc.arguments = cliArgs
        proc.standardInput = FileHandle.nullDevice
        proc.standardOutput = FileHandle.standardError
        proc.standardError = FileHandle.standardError

        do {
            try proc.run()
        } catch {
            return toolText("Failed to spawn NewPal runtime: \(error.localizedDescription)", isError: true)
        }
        spawnedProcess = proc
        gSpawnedGamePid = proc.processIdentifier

        let deadline = Date().addingTimeInterval(12.0)
        while Date() < deadline {
            if !proc.isRunning {
                cleanupChildProcess()
                return toolText("NewPal runtime exited unexpectedly during startup (status \(proc.terminationStatus))", isError: true)
            }
            if (try? sendBridge(cmd: "ping", args: [:], timeoutSec: 0.4)) != nil {
                if let st = try? sendBridge(cmd: "get_state", args: [:], timeoutSec: 3.0) {
                    var data = (st.header["data"] as? [String: Any]) ?? [:]
                    data["attached_existing"] = false
                    data["pid"] = proc.processIdentifier
                    return toolJson(data)
                }
            }
            Thread.sleep(forTimeInterval: 0.1)
        }
        return toolText("Timed out waiting for NewPal MCP socket at \(socketPath)", isError: true)
    }

    private func handleCloseGame() -> [String: Any] {
        let wasSpawned = (spawnedProcess != nil)
        cleanupChildProcess()
        if !wasSpawned {
            // Do not force-kill user's manual GUI window unless explicitly requested; quit only if launched or attach ping works?
            // Wait: spec says pal_close_game shuts down spawned child OR requests quit gracefully when asked explicitly.
            _ = try? sendBridge(cmd: "quit", args: [:], timeoutSec: 1.5)
        }
        return toolJson(["closed": true])
    }

    private func handleCaptureScreen(args: [String: Any]) -> [String: Any] {
        let scale = (args["scale"] as? Int == 2) ? 2 : 1
        do {
            let res = try sendBridge(cmd: "capture_screen", args: ["scale": scale], timeoutSec: 8.0)
            guard (res.header["ok"] as? Bool) == true else {
                return toolText((res.header["error"] as? String) ?? "capture_screen failed", isError: true)
            }
            guard let rgba = res.binaryRGBA,
                  let w = res.header["width"] as? Int,
                  let h = res.header["height"] as? Int,
                  let pngData = Self.encodePng(rgba: rgba, width: w, height: h) else {
                return toolText("Failed to encode screenshot RGBA to PNG", isError: true)
            }
            if let savePath = args["save_path"] as? String, !savePath.isEmpty {
                try? pngData.write(to: URL(fileURLWithPath: savePath))
            }
            let meta: [String: Any] = [
                "width": w,
                "height": h,
                "png_bytes": pngData.count,
                "state": res.header["state"] ?? [:]
            ]
            return [
                "content": [
                    [
                        "type": "image",
                        "data": pngData.base64EncodedString(),
                        "mimeType": "image/png"
                    ],
                    [
                        "type": "text",
                        "text": encodeJsonString(meta)
                    ]
                ],
                "isError": false
            ]
        } catch {
            return toolText(error.localizedDescription, isError: true)
        }
    }

    // MARK: - PNG & Unix Socket Helpers

    private static func encodePng(rgba: Data, width: Int, height: Int) -> Data? {
        guard width > 0, height > 0, rgba.count >= width * height * 4 else { return nil }
        guard let cs = CGColorSpace(name: CGColorSpace.sRGB) else { return nil }
        guard let provider = CGDataProvider(data: rgba as CFData) else { return nil }
        let bitmapInfo = CGBitmapInfo(
            rawValue: CGImageAlphaInfo.noneSkipLast.rawValue | CGBitmapInfo.byteOrder32Big.rawValue
        )
        guard let cgImg = CGImage(
            width: width,
            height: height,
            bitsPerComponent: 8,
            bitsPerPixel: 32,
            bytesPerRow: width * 4,
            space: cs,
            bitmapInfo: bitmapInfo,
            provider: provider,
            decode: nil,
            shouldInterpolate: false,
            intent: .defaultIntent
        ) else {
            return nil
        }
        guard let mdata = CFDataCreateMutable(kCFAllocatorDefault, 0),
              let dest = CGImageDestinationCreateWithData(mdata, UTType.png.identifier as CFString, 1, nil) else {
            return nil
        }
        CGImageDestinationAddImage(dest, cgImg, nil)
        guard CGImageDestinationFinalize(dest) else { return nil }
        return mdata as Data
    }

    private func sendBridge(cmd: String, args: [String: Any], timeoutSec: Double) throws -> (header: [String: Any], binaryRGBA: Data?) {
        let fd = Darwin.socket(AF_UNIX, SOCK_STREAM, 0)
        if fd < 0 { throw McpClientError.socketCreateFailed(errno) }
        defer { Darwin.close(fd) }

        var one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
        var rcvBuf: Int32 = 262144
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvBuf, socklen_t(MemoryLayout<Int32>.size))

        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        let pathUtf8 = Array(socketPath.utf8)
        let maxLen = MemoryLayout.size(ofValue: addr.sun_path)
        guard pathUtf8.count < maxLen else { throw McpClientError.socketPathTooLong }
        withUnsafeMutableBytes(of: &addr.sun_path) { raw in
            raw.initializeMemory(as: UInt8.self, repeating: 0)
            for (i, b) in pathUtf8.enumerated() { raw[i] = b }
        }

        let connRc = withUnsafePointer(to: &addr) { ptr in
            ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { saPtr in
                Darwin.connect(fd, saPtr, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        if connRc != 0 {
            throw McpClientError.connectFailed("connect(\(socketPath)) failed with errno=\(errno). Call pal_launch_game first or start NewPal with MCP enabled.")
        }

        var req = args
        req["id"] = nextBridgeReqId
        req["cmd"] = cmd
        nextBridgeReqId += 1

        guard var reqData = try? JSONSerialization.data(withJSONObject: req, options: []) else {
            throw McpClientError.ioFailed("Failed to serialize request JSON")
        }
        reqData.append(0x0A)
        try writeAll(fd: fd, data: reqData)

        let deadline = Date().addingTimeInterval(max(timeoutSec, 0.5))
        var rx = Data()
        var scratch = [UInt8](repeating: 0, count: 65536)

        // Read newline-terminated header line
        var headerData: Data?
        while headerData == nil {
            if let nl = rx.firstIndex(of: 0x0A) {
                headerData = Data(rx[..<nl])
                rx.removeSubrange(...nl)
                break
            }
            let n = try pollAndRead(fd: fd, scratch: &scratch, deadline: deadline)
            if n == 0 {
                throw McpClientError.ioFailed("Engine closed connection")
            }
            rx.append(contentsOf: scratch[0..<n])
        }

        guard let hdrData = headerData,
              let hdrObj = try? JSONSerialization.jsonObject(with: hdrData, options: []),
              let header = hdrObj as? [String: Any] else {
            throw McpClientError.invalidResponse
        }

        var binary: Data?
        if (header["binary_rgba"] as? Bool) == true, let expectedBytes = header["bytes"] as? Int, expectedBytes > 0 {
            while rx.count < expectedBytes {
                let n = try pollAndRead(fd: fd, scratch: &scratch, deadline: deadline)
                if n == 0 {
                    throw McpClientError.ioFailed("Truncated binary capture payload")
                }
                rx.append(contentsOf: scratch[0..<n])
            }
            binary = Data(rx[..<expectedBytes])
        }

        return (header, binary)
    }

    private func writeAll(fd: Int32, data: Data) throws {
        try data.withUnsafeBytes { raw in
            guard let base = raw.baseAddress else { return }
            var off = 0
            while off < raw.count {
                let w = Darwin.write(fd, base.advanced(by: off), raw.count - off)
                if w > 0 {
                    off += w
                } else if w < 0 && errno == EINTR {
                    continue
                } else {
                    throw McpClientError.ioFailed("write failed errno=\(errno)")
                }
            }
        }
    }

    private func pollAndRead(fd: Int32, scratch: inout [UInt8], deadline: Date) throws -> Int {
        while true {
            let remMs = Int32(max(1.0, deadline.timeIntervalSinceNow * 1000.0))
            if deadline.timeIntervalSinceNow <= 0 {
                throw McpClientError.ioFailed("Timed out waiting for response from NewPal engine")
            }
            var pfd = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
            let prc = Darwin.poll(&pfd, 1, min(remMs, 500))
            if prc < 0 {
                if errno == EINTR { continue }
                throw McpClientError.ioFailed("poll failed errno=\(errno)")
            }
            if prc == 0 {
                if deadline.timeIntervalSinceNow <= 0 {
                    throw McpClientError.ioFailed("Timed out waiting for response from NewPal engine")
                }
                continue
            }
            let n = scratch.withUnsafeMutableBytes { buf in
                Darwin.read(fd, buf.baseAddress, buf.count)
            }
            if n < 0 && (errno == EINTR || errno == EAGAIN) { continue }
            if n < 0 { throw McpClientError.ioFailed("read failed errno=\(errno)") }
            return n
        }
    }

    // MARK: - Tool Schema Definitions (16 Tools)

    private static func makeToolDefinitions() -> [[String: Any]] {
        return [
        [
            "name": "pal_launch_game",
            "description": "Launch NewPal (visible GUI window or fast headless session) or attach cleanly to an already running NewPal window.",
            "inputSchema": [
                "type": "object",
                "properties": [
                    "game_dir": ["type": "string", "description": "Path to original PalGame archive folder. Optional if already configured in Launcher."],
                    "headless": ["type": "boolean", "description": "Run hidden offscreen headless simulation (~8x faster, auto-skips videos/fades). Defaults to Launcher setting or true."],
                    "language": ["type": "string", "enum": ["tc", "sc"], "description": "Text language: tc (Original Traditional Chinese) or sc (Simplified Chinese)."],
                    "scale": ["type": "integer", "enum": [1, 2], "description": "Window resolution scale: 1=640x480, 2=1280x960."],
                    "hd": ["type": "boolean", "description": "Enable HD 2x text & sprite sharpening."],
                    "shader": ["type": "integer", "enum": [0, 1, 2], "description": "0=Classic, 1=Metal xBRZ, 2=CRT-Royale."],
                    "skip_videos": ["type": "boolean", "description": "Auto-skip Bink cutscene videos (.bik)."],
                    "mute": ["type": "boolean", "description": "Mute game audio."],
                    "test_mode": ["type": "boolean", "description": "Enable in-game QA test mode module."],
                    "allow_qa": ["type": "boolean", "description": "Allow teleport and QA test/flag modification tools."]
                ]
            ]
        ],
        [
            "name": "pal_close_game",
            "description": "Close the running NewPal game session cleanly and clean up its socket.",
            "inputSchema": ["type": "object", "properties": [String: Any]()]
        ],
        [
            "name": "pal_get_state",
            "description": "Get full structured snapshot of scene, sub_state, map, hero coords, party stats/magics, dialogue text/choices, active combat status, shop/camp menu, and test flags.",
            "inputSchema": ["type": "object", "properties": [String: Any]()]
        ],
        [
            "name": "pal_get_map_entities",
            "description": "Inspect current map NPCs (obj_id, coordinates, distance, talk_id, aggro, can_interact_now), aggregated trigger zones, and an optional local ASCII walkability map.",
            "inputSchema": [
                "type": "object",
                "properties": [
                    "radius": ["type": "integer", "description": "ASCII map radius around player in tiles (1..25, default 12; 0 omits ASCII map)."]
                ]
            ]
        ],
        [
            "name": "pal_get_inventory_and_saves",
            "description": "List all items in party inventory (with equip_slot and localized descriptions) and metadata for all 10 save slots (0..9).",
            "inputSchema": ["type": "object", "properties": [String: Any]()]
        ],
        [
            "name": "pal_capture_screen",
            "description": "Capture the complete rendered game screen (world + HD CoreText dialogue/menus/HUD) as a PNG image.",
            "inputSchema": [
                "type": "object",
                "properties": [
                    "scale": ["type": "integer", "enum": [1, 2], "description": "1 = 640x480 (default), 2 = 1280x960."],
                    "save_path": ["type": "string", "description": "Optional local path to save a copy of the PNG."]
                ]
            ]
        ],
        [
            "name": "pal_query_flags",
            "description": "Read story_flags and script_vars for verifying quest progression or branching.",
            "inputSchema": [
                "type": "object",
                "properties": [
                    "flag_id": ["type": "integer", "description": "Start story flag index (or single flag)."],
                    "flag_end": ["type": "integer", "description": "Inclusive end story flag index (max 256 per query)."],
                    "flags_csv": ["type": "string", "description": "Optional comma-separated flag indices, e.g. '1001,2839,3001'."]
                ]
            ]
        ],
        [
            "name": "pal_title_action",
            "description": "Control title screen flow: start a new game, load a save slot, or return to the title screen.",
            "inputSchema": [
                "type": "object",
                "required": ["action"],
                "properties": [
                    "action": ["type": "string", "enum": ["new_game", "load_slot", "return_title"]],
                    "slot": ["type": "integer", "description": "Save slot index 0..9 when action is load_slot."]
                ]
            ]
        ],
        [
            "name": "pal_move_to",
            "description": "Move the party leader on the field map via A* pathfinding to coordinates, an NPC obj_id (auto-interacts on arrival), a trigger_id, or directional steps.",
            "inputSchema": [
                "type": "object",
                "properties": [
                    "tx": ["type": "integer", "description": "Target tile X."],
                    "ty": ["type": "integer", "description": "Target tile Y."],
                    "npc_obj_id": ["type": "integer", "description": "Walk to NPC obj_id and begin interaction."],
                    "trigger_id": ["type": "integer", "description": "Walk onto map trigger zone trigger_id."],
                    "dir": ["type": "string", "enum": ["up", "down", "left", "right", "ul", "ur", "dl", "dr"], "description": "Direction when stepping without explicit tx/ty."],
                    "steps": ["type": "integer", "description": "Tile steps in dir (1..24, default 1)."],
                    "wait": ["type": "boolean", "description": "Wait until arrival, dialogue, combat, map change, or stall (default true)."],
                    "timeout_ms": ["type": "integer", "description": "Wait timeout in ms (default 15000)."]
                ]
            ]
        ],
        [
            "name": "pal_interact",
            "description": "Interact with an adjacent NPC/chest, advance active dialogue, advance post-battle settlement, or skip non-input cutscene waits.",
            "inputSchema": [
                "type": "object",
                "properties": [
                    "npc_obj_id": ["type": "integer", "description": "Optional specific nearby NPC obj_id to interact with."],
                    "skip_wait": ["type": "boolean", "description": "Fast-forward active non-input cutscene wait/fade/pan."],
                    "timeout_ms": ["type": "integer"]
                ]
            ]
        ],
        [
            "name": "pal_choose_dialogue",
            "description": "Select and confirm a dialogue choice option by its 0-based index.",
            "inputSchema": [
                "type": "object",
                "required": ["index"],
                "properties": [
                    "index": ["type": "integer", "description": "0-based index of the dialogue choice."]
                ]
            ]
        ],
        [
            "name": "pal_combat_action",
            "description": "Execute a turn-based combat command for the active hero or advance post-battle settlement.",
            "inputSchema": [
                "type": "object",
                "required": ["action"],
                "properties": [
                    "action": [
                        "type": "string",
                        "enum": ["attack", "magic", "item", "defend", "combo", "flee", "auto_battle_turn", "advance_settlement"]
                    ],
                    "target_idx": ["type": "integer", "description": "Enemy index (or ally index for healing/revive magic or items)."],
                    "skill_id": ["type": "integer", "description": "Magic skill_id from active hero's magics list."],
                    "item_id": ["type": "integer", "description": "Item ID from inventory."],
                    "wait": ["type": "boolean", "description": "Wait for round animation to complete (default true)."],
                    "timeout_ms": ["type": "integer"]
                ]
            ]
        ],
        [
            "name": "pal_manage_party",
            "description": "Manage party outside combat: use field item, equip/unequip gear, buy/sell in open shop, or save/load save slots 0..9.",
            "inputSchema": [
                "type": "object",
                "required": ["action"],
                "properties": [
                    "action": [
                        "type": "string",
                        "enum": ["use_item", "equip_item", "unequip_slot", "shop_buy", "shop_sell", "shop_close", "save_slot", "load_slot"]
                    ],
                    "hero_idx": ["type": "integer", "description": "Party slot index 0..3."],
                    "item_id": ["type": "integer", "description": "Item ID."],
                    "equip_slot": ["type": "integer", "description": "Equipment slot 0=Head..5=Foot for unequip_slot."],
                    "slot": ["type": "integer", "description": "Save slot 0..9 for save_slot/load_slot."]
                ]
            ]
        ],
        [
            "name": "pal_raw_input",
            "description": "Low-level input injection fallback for menus or UI testing: tap key, click logical 640x480 point, wait frames, or skip video.",
            "inputSchema": [
                "type": "object",
                "required": ["action"],
                "properties": [
                    "action": ["type": "string", "enum": ["key_tap", "mouse_click", "wait_frames", "skip_video"]],
                    "key": ["type": "string", "description": "Key name for key_tap (up, down, left, right, space, return, escape, e, i, tab, 1..9, f2..f12)."],
                    "x": ["type": "integer", "description": "Logical X coordinate (0..639) for mouse_click."],
                    "y": ["type": "integer", "description": "Logical Y coordinate (0..479) for mouse_click."],
                    "button": ["type": "string", "enum": ["left", "right"], "description": "Mouse button for mouse_click."],
                    "frames": ["type": "integer", "description": "Number of frames to advance for wait_frames (1..600)."],
                    "wait": ["type": "boolean"]
                ]
            ]
        ],
        [
            "name": "pal_test_control",
            "description": "QA / testing helpers (requires mcp_allow_qa_tools): toggle cheats, heal, add gold, level up, trigger combat formation, instant kill, or set story_flags.",
            "inputSchema": [
                "type": "object",
                "properties": [
                    "action": ["type": "string", "enum": ["instant_kill_combat"]],
                    "god_mode": ["type": "boolean"],
                    "no_encounter": ["type": "boolean"],
                    "auto_instant_kill": ["type": "boolean"],
                    "speed_mult": ["type": "integer", "enum": [1, 2, 3]],
                    "noclip": ["type": "boolean"],
                    "heal_all": ["type": "boolean"],
                    "level_up": ["type": "boolean", "description": "Level up active party members (+1 level)."],
                    "level_up_count": ["type": "integer", "description": "Number of levels to boost active party (e.g. 5, 10)."],
                    "target_level": ["type": "integer", "description": "Target level to directly boost active party up to (e.g. 20)."],
                    "add_money": ["type": "integer"],
                    "flag_id": ["type": "integer"],
                    "flag_val": ["type": "integer"],
                    "battle_id": ["type": "integer", "description": "Immediately trigger encounter formation battle_id."]
                ]
            ]
        ],
        [
            "name": "pal_teleport",
            "description": "QA teleport directly to any map_id (optionally setting tile tx, ty, and direction).",
            "inputSchema": [
                "type": "object",
                "required": ["map_id"],
                "properties": [
                    "map_id": ["type": "integer", "description": "Target Pal map_id (e.g. 1=Inn Room, 2=Inn Hall, 501=Luosha Peak)."],
                    "tx": ["type": "integer"],
                    "ty": ["type": "integer"],
                    "dir": ["type": "string", "enum": ["up", "down", "left", "right", "ul", "ur", "dl", "dr"]]
                ]
            ]
        ]
        ]
    }
}
