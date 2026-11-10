import SwiftUI

private struct BridgeStatus: Decodable {
    struct Device: Decodable {
        let address: String?
        let office: Bool
    }

    let devices: [Device]
    let seen: Bool?
    let voice_readiness: String?
    let codex_verified: Bool?
}

private struct TestResult: Decodable {
    let result: String
}

private enum BridgeService {
    private static let label = "local.ls26.codex-bridge"

    private static func launchctl(_ arguments: [String]) -> Bool {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/launchctl")
        process.arguments = arguments
        process.standardOutput = FileHandle.nullDevice
        process.standardError = FileHandle.nullDevice
        do {
            try process.run()
            process.waitUntilExit()
            return process.terminationStatus == 0
        } catch {
            return false
        }
    }

    static var isConfigured: Bool {
        FileManager.default.fileExists(atPath: FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/LaunchAgents/\(label).plist").path)
    }

    static var bundledProfile: URL? {
        Bundle.main.url(forResource: "device-profile", withExtension: "json", subdirectory: "Bridge")
    }

    static var needsSenseVoiceUpgrade: Bool {
        let data = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/LS26 Codex")
        let files = FileManager.default
        return isConfigured
            && Bundle.main.url(forResource: "model.int8", withExtension: "onnx",
                               subdirectory: "Bridge/SenseVoice") != nil
            && files.isExecutableFile(atPath: data.appendingPathComponent("Python/bin/python3").path)
            && files.fileExists(atPath: data.appendingPathComponent("launch-managed-bridge.py").path)
            && !files.fileExists(atPath: data.appendingPathComponent("SenseVoice/model.int8.onnx").path)
    }

    static func configure(pairing: URL) -> String? {
        runInstaller(pairing.path)
    }

    static func upgradeSenseVoice() -> String? {
        if let error = runInstaller("--upgrade-sensevoice") { return error }
        if launchctl(["print", "gui/\(getuid())/\(label)"]) && !restartDiscovery() {
            return "SenseVoice 已安装，请重新启动桥接。"
        }
        return nil
    }

    private static func runInstaller(_ argument: String) -> String? {
        guard let script = Bundle.main.url(forResource: "setup_bridge", withExtension: "py",
                                          subdirectory: "Bridge") else {
            return "安装包缺少桥接组件。"
        }
        let process = Process()
        let bundled = script.deletingLastPathComponent().appendingPathComponent("Python/bin/python3")
        process.executableURL = FileManager.default.isExecutableFile(atPath: bundled.path)
            ? bundled : URL(fileURLWithPath: "/usr/bin/python3")
        process.arguments = ["-E", "-s", "-B", script.path, argument]
        let output = Pipe()
        process.standardOutput = output
        process.standardError = FileHandle.nullDevice
        do {
            try process.run()
            let result = output.fileHandleForReading.readDataToEndOfFile()
            process.waitUntilExit()
            if process.terminationStatus == 0 { return nil }
            let detail = String(decoding: result, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
            return detail.isEmpty ? "无法准备桥接，请确认 Mac 已安装 Python 3。" : detail
        } catch {
            return "无法启动桥接安装程序。"
        }
    }

    static func restartDiscovery() -> Bool {
        launchctl(["kickstart", "-k", "gui/\(getuid())/\(label)"])
    }

    static func ensureStarted() -> String? {
        let home = FileManager.default.homeDirectoryForCurrentUser
        let files = FileManager.default
        let data = home.appendingPathComponent("Library/Application Support/LS26 Codex")
        let plist = home.appendingPathComponent("Library/LaunchAgents/\(label).plist")
        guard files.fileExists(atPath: plist.path) else {
            return "请导入随摩突突设备提供的配对文件，助手会准备电脑端桥接。"
        }
        let managed = files.fileExists(atPath: data.appendingPathComponent("launch-managed-bridge.py").path)
        let embeddedPython = files.isExecutableFile(atPath: data.appendingPathComponent("Python/bin/python3").path)
        let requirements = managed ? [("bridge.py", "桥接程序"),
                                      (embeddedPython ? "Python/bin/python3" : "venv/bin/python", "桥接运行时"),
                                      ("status-token", "设备配对配置")] : [
            ("launch-bridge", "桥接启动器"), ("bridge.py", "桥接程序"),
            ("venv/bin/python", "Python 环境"), ("whisper-cli", "Whisper 程序"),
            ("ggml-small.bin", "Whisper 模型"), ("status-token", "设备配对配置"),
            ("voice-key", "语音校验配置"), ("codex-command", "Codex 命令配置")
        ]
        let missing = requirements.filter { !files.fileExists(atPath: data.appendingPathComponent($0.0).path) }
        if !missing.isEmpty {
            return "缺少" + missing.map { $0.1 }.joined(separator: "、") + "。"
        }
        let domain = "gui/\(getuid())"
        let service = "\(domain)/\(label)"
        if !launchctl(["print", service]) {
            _ = launchctl(["bootstrap", domain, plist.path])
        }
        // Without -k, kickstart preserves a bridge that's already running.
        guard launchctl(["kickstart", service]) else {
            return "无法启动桥接，请检查后台服务配置。"
        }
        return nil
    }
}

@MainActor
private final class Connection: ObservableObject {
    @Published var bridgeStarting = false
    @Published var discoverySlow = false
    private var discoveryStarted: Date?
    @Published var bridgeMessage: String?
    @Published var hasChecked = false
    @Published var bridgeOnline = false
    @Published var devices: [BridgeStatus.Device] = []
    @Published var testing = false
    @Published var switching = false
    @Published var testMessage: String?
    @Published var hasSeenDevice = false
    @Published var voiceReadiness: String?
    @Published var codexVerified = false

    private struct BridgeRuntime: Decodable {
        let event_port: UInt16
        let pid: Int32
    }

    private var base: URL? {
        let runtime = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/LS26 Codex/bridge-runtime.json")
        do {
            let state = try JSONDecoder().decode(BridgeRuntime.self, from: Data(contentsOf: runtime))
            guard state.event_port > 0, state.pid > 0, kill(state.pid, 0) == 0 else { return nil }
            return URL(string: "http://127.0.0.1:\(state.event_port)")
        } catch let error as CocoaError where error.code == .fileReadNoSuchFile {
            return URL(string: "http://127.0.0.1:8766")
        } catch {
            return nil
        }
    }

    var status: String {
        if bridgeStarting { return "正在启动电脑端桥接" }
        if !hasChecked { return "正在检查连接" }
        if !bridgeOnline { return "电脑端服务未就绪" }
        if devices.isEmpty { return hasSeenDevice ? "设备已离线 · 等待重连" : "正在寻找设备" }
        if devices.count > 1 { return "发现多台设备" }
        return devices[0].office ? "办公模式已连接" : "已连接 · 娱乐模式"
    }

    var voiceStatus: String {
        switch voiceReadiness {
        case "ready": return "Codex 可接收下一条语音指令"
        case "choice": return "Codex 正在等你说出选项"
        case "busy": return "Codex 正在处理任务或等待确认"
        case "no-target": return "请先在 Codex 完成一轮任务"
        case "disabled": return "语音续办未就绪，请确认已安装 Codex。"
        default: return "正在检查 Codex 语音状态"
        }
    }

    func prepareBridge() async {
        if BridgeService.needsSenseVoiceUpgrade {
            bridgeStarting = true
            let failure = await Task.detached(priority: .utility) {
                BridgeService.upgradeSenseVoice()
            }.value
            bridgeStarting = false
            await startBridge()
            if let failure { testMessage = failure }
            return
        }
        if !BridgeService.isConfigured, BridgeService.bundledProfile != nil {
            await configureBridge()
        } else {
            await startBridge()
        }
    }

    func configureBridge() async {
        guard !bridgeStarting else { return }
        let url: URL
        if let profile = BridgeService.bundledProfile {
            url = profile
        } else {
            let picker = NSOpenPanel()
            picker.title = "选择摩突突设备配对文件"
            picker.allowsMultipleSelection = false
            picker.canChooseDirectories = false
            guard picker.runModal() == .OK, let selected = picker.url else { return }
            url = selected
        }
        bridgeStarting = true
        bridgeMessage = "正在准备桥接组件…"
        let failure = await Task.detached(priority: .utility) {
            BridgeService.configure(pairing: url)
        }.value
        bridgeStarting = false
        if let failure {
            bridgeMessage = failure
        } else {
            await startBridge()
            if bridgeOnline {
                testMessage = "桥接已准备好。请在 Codex 打开本地项目，再确认新增的 Hooks。"
            }
        }
    }

    func startBridge() async {
        guard !bridgeStarting else { return }
        bridgeStarting = true
        bridgeMessage = nil
        defer { bridgeStarting = false }
        await refresh()
        guard !bridgeOnline else { return }
        bridgeMessage = await Task.detached(priority: .utility) {
            BridgeService.ensureStarted()
        }.value
        guard bridgeMessage == nil else { return }
        for _ in 0..<8 {
            await refresh()
            if bridgeOnline { return }
            try? await Task.sleep(for: .milliseconds(350))
        }
        bridgeMessage = "桥接已请求启动，但还没有响应。请检查后台服务日志。"
    }

    func retryDiscovery() async {
        guard bridgeOnline, devices.isEmpty, !bridgeStarting else { return }
        bridgeStarting = true
        discoverySlow = false
        discoveryStarted = Date()
        testMessage = nil
        let restarted = await Task.detached(priority: .utility) {
            BridgeService.restartDiscovery()
        }.value
        bridgeStarting = false
        if restarted {
            await startBridge()
        } else {
            testMessage = "无法重新启动设备发现，请检查电脑端桥接。"
        }
    }

    func openCodex() {
        let workspace = NSWorkspace.shared
        guard let app = workspace.urlForApplication(withBundleIdentifier: "com.openai.codex") else {
            testMessage = "未找到 Codex，请先安装并打开 Codex。"
            return
        }
        workspace.openApplication(at: app, configuration: .init()) { _, _ in }
    }

    func refresh() async {
        defer { hasChecked = true }
        do {
            guard let base else { throw URLError(.cannotConnectToHost) }
            var request = URLRequest(url: base.appendingPathComponent("setup"))
            request.timeoutInterval = 2
            let (data, response) = try await URLSession.shared.data(for: request)
            guard (response as? HTTPURLResponse)?.statusCode == 200 else { throw URLError(.badServerResponse) }
            let snapshot = try JSONDecoder().decode(BridgeStatus.self, from: data)
            let latest = snapshot.devices
            hasSeenDevice = hasSeenDevice || snapshot.seen == true || !latest.isEmpty
            voiceReadiness = snapshot.voice_readiness
            codexVerified = snapshot.codex_verified == true
            if !testing && !switching && latest.first?.office != devices.first?.office { testMessage = nil }
            devices = latest
            bridgeOnline = true
            if latest.isEmpty {
                let started = discoveryStarted ?? Date()
                discoveryStarted = started
                discoverySlow = Date().timeIntervalSince(started) >= 12
            } else {
                discoveryStarted = nil
                discoverySlow = false
            }
        } catch {
            devices = []
            bridgeOnline = false
            voiceReadiness = nil
            codexVerified = false
        }
    }

    func test() async {
        guard devices.count == 1, !testing else { return }
        testing = true
        testMessage = "正在等设备回应…"
        defer { testing = false }
        do {
            guard let base else { throw URLError(.cannotConnectToHost) }
            var request = URLRequest(url: base.appendingPathComponent("setup/test"))
            request.httpMethod = "POST"
            request.timeoutInterval = 6
            let (data, response) = try await URLSession.shared.data(for: request)
            switch (response as? HTTPURLResponse)?.statusCode {
            case 200:
                let result = try JSONDecoder().decode(TestResult.self, from: data)
                switch result.result {
                case "test-busy": testMessage = "自检已回应，请看青灯。当前音频暂未播放自检提示。"
                case "test-light": testMessage = "设备已回应。提示音不可用，请看青灯。"
                default: testMessage = "设备已回应。请确认它亮青灯并播报。"
                }
            case 503: testMessage = "设备已断开，正在等它重新连接。"
            case 409: testMessage = "已有测试在进行，或当前连着多台设备。"
            case 504: testMessage = "连接仍在，但没收到设备的自检回执。"
            default: testMessage = "电脑端桥接返回了异常，请检查连接。"
            }
        } catch {
            testMessage = "无法完成自检，请检查电脑端桥接是否运行。"
        }
        await refresh()
    }

    func switchMode() async {
        guard devices.count == 1, !switching else { return }
        let office = !devices[0].office
        switching = true
        testMessage = "正在等设备确认模式…"
        defer { switching = false }
        do {
            guard let base else { throw URLError(.cannotConnectToHost) }
            var request = URLRequest(url: base.appendingPathComponent("setup/mode"))
            request.httpMethod = "POST"
            request.timeoutInterval = 6
            request.setValue("application/json", forHTTPHeaderField: "Content-Type")
            request.httpBody = try JSONEncoder().encode(["office": office])
            let (_, response) = try await URLSession.shared.data(for: request)
            switch (response as? HTTPURLResponse)?.statusCode {
            case 200: testMessage = office ? "已切到办公模式。" : "已切到娱乐模式。"
            case 501: testMessage = "请更新设备固件后再用菜单切换。"
            default: testMessage = "设备未确认切换，请检查连接或使用设备主按键。"
            }
        } catch {
            testMessage = "无法切换，请检查电脑端桥接。"
        }
        await refresh()
    }
}

private struct ConnectionView: View {
    @ObservedObject var connection: Connection

    private func explanation(_ text: String) -> some View {
        Text(text)
            .lineLimit(nil)
            .fixedSize(horizontal: false, vertical: true)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Label(connection.status, systemImage: connection.devices.isEmpty ? "wifi.slash" : "wifi")
                .font(.headline)
            if !connection.hasChecked {
                explanation("正在读取电脑端桥接状态…")
            } else if !connection.bridgeOnline {
                explanation(connection.bridgeMessage ?? "正在检查摩突突设备的电脑端桥接。")
                if BridgeService.isConfigured {
                    Button("启动桥接") { Task { await connection.startBridge() } }
                        .disabled(connection.bridgeStarting)
                } else {
                    Button(BridgeService.bundledProfile == nil ? "导入设备配对文件" : "重新准备桥接") {
                        Task { await connection.configureBridge() }
                    }
                        .disabled(connection.bridgeStarting)
                }
            } else if connection.devices.isEmpty {
                explanation("请打开摩突突设备，并让它与这台 Mac 连接同一个 Wi‑Fi。")
                if connection.discoverySlow {
                    explanation("设备发现尚未收到回应。确认设备已开机后，可以重新寻找。")
                        .font(.caption).foregroundStyle(.secondary)
                    Button("重新寻找设备") { Task { await connection.retryDiscovery() } }
                        .disabled(connection.bridgeStarting)
                }
            } else if connection.devices.count > 1 {
                explanation("这个版本一次测试一台设备。请让其他设备暂时离线。")
            } else if connection.devices[0].office {
                explanation(connection.voiceStatus)
            } else {
                explanation("可以唤醒小聆聊天，也可快速四击设备主按键切换模式。")
            }
            if let device = connection.devices.first, connection.devices.count == 1 {
                Button {
                    Task { await connection.switchMode() }
                } label: {
                    Label(connection.switching ? "正在切换…" :
                          device.office ? "切换到娱乐模式" : "切换到办公模式",
                          systemImage: device.office ? "gamecontroller" : "briefcase")
                        .font(.system(size: 16, weight: .semibold))
                        .foregroundStyle(.white)
                        .frame(maxWidth: .infinity, minHeight: 28)
                }
                .buttonStyle(.borderedProminent)
                .tint(device.office ? Color(red: 0.09, green: 0.36, blue: 0.26) :
                                      Color(red: 0.10, green: 0.29, blue: 0.58))
                .controlSize(.large)
                .disabled(connection.switching || connection.testing)
            }
            if connection.bridgeOnline {
                Divider()
                Label(connection.codexVerified ? "Codex 联动已验证" : "等待启用 Codex 联动",
                      systemImage: connection.codexVerified ? "checkmark.circle.fill" : "circle.dashed")
                    .font(.subheadline.bold())
                if !connection.codexVerified {
                    explanation("1. 在 Codex 打开或新建一个本地项目。\n2. 到「设置 → Hooks」信任摩突突的条目，再发一条测试消息。")
                        .font(.caption).foregroundStyle(.secondary)
                    Button("打开 Codex") { connection.openCodex() }
                }
            }
            Divider()
            Text("连接与自检").font(.subheadline.bold())
            Button("刷新连接状态") { Task { await connection.refresh() } }
            explanation("重新读取电脑桥接和设备在线状态。")
                .font(.caption).foregroundStyle(.secondary)
            Button("测试设备灯光与声音") { Task { await connection.test() } }
                .disabled(connection.devices.count != 1 || connection.testing || connection.switching)
            explanation("发送自检指令，设备会亮青灯，空闲时会播放提示音。")
                .font(.caption).foregroundStyle(.secondary)
            if let message = connection.testMessage {
                explanation(message).font(.callout).foregroundStyle(.secondary)
            }
            Divider()
            Button("退出助手") { NSApplication.shared.terminate(nil) }
                .buttonStyle(.plain)
        }
        .padding(16)
        .frame(width: 350)
        .task {
            while !Task.isCancelled {
                await connection.refresh()
                try? await Task.sleep(for: .seconds(2))
            }
        }
    }
}

@main
@MainActor
struct LS26Assistant: App {
    @StateObject private var connection: Connection

    init() {
        let connection = Connection()
        _connection = StateObject(wrappedValue: connection)
        Task { await connection.prepareBridge() }
    }

    private static let menuIcon: NSImage? = {
        guard let url = Bundle.main.url(forResource: "AppIcon", withExtension: "icns"),
              let image = NSImage(contentsOf: url) else { return nil }
        // Trim the app icon's margins so the robot fills the status item.
        let source = NSRect(origin: .zero, size: image.size)
            .insetBy(dx: image.size.width * 0.12, dy: image.size.height * 0.12)
        let icon = NSImage(size: NSSize(width: 20, height: 20), flipped: false) { bounds in
            NSBezierPath(roundedRect: bounds, xRadius: 4, yRadius: 4).addClip()
            image.draw(in: bounds, from: source, operation: .sourceOver, fraction: 1)
            return true
        }
        icon.isTemplate = false
        return icon
    }()

    var body: some Scene {
        MenuBarExtra {
            ConnectionView(connection: connection)
        } label: {
            if let icon = Self.menuIcon {
                Image(nsImage: icon)
                    .accessibilityLabel("摩突突助手")
            } else {
                Label("摩突突助手", systemImage: "link")
            }
        }
        .menuBarExtraStyle(.window)
    }
}
