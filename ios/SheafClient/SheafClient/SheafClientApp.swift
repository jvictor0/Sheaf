import SwiftUI

@main
struct SheafClientApp: App {
    @StateObject private var appState = AppState()

    init() {
        Task {
            let path = await AppFileLogger.shared.currentLogPath()
            await AppFileLogger.shared.log("App launched. Log file: \(path)")
        }
    }

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(appState)
        }
    }
}
