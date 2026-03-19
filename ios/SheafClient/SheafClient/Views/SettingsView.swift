import SwiftUI

struct SettingsView: View {
    @ObservedObject private var settingsStore = ClientSettingsStore.shared

    var body: some View {
        Form {
            Section("Client Settings") {
                Picker("Model", selection: $settingsStore.selectedModelName) {
                    ForEach(settingsStore.availableModels) { model in
                        Text(model.displayName).tag(model.name)
                    }
                }
                .pickerStyle(.menu)
            }
        }
        .navigationTitle("Settings")
        .task {
            await settingsStore.refreshAvailableModels()
        }
    }
}
