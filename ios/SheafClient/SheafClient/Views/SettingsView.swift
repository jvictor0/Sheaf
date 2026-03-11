import SwiftUI

struct SettingsView: View {
    @ObservedObject private var settingsStore = ClientSettingsStore.shared

    var body: some View {
        Form {
            Section("Client Settings") {
                Picker("Model", selection: $settingsStore.selectedModel) {
                    ForEach(ClientModel.allCases) { model in
                        Text(model.displayName).tag(model)
                    }
                }
                .pickerStyle(.menu)
            }
        }
        .navigationTitle("Settings")
    }
}
