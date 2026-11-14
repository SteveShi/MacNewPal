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
        NewPalApp.main()
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
