import Foundation

@MainActor
final class ClientSettingsStore: ObservableObject {
    static let shared = ClientSettingsStore()

    @Published var selectedModel: ClientModel {
        didSet {
            defaults.set(selectedModel.rawValue, forKey: modelKey)
        }
    }

    private let defaults: UserDefaults
    private let modelKey: String

    init(defaults: UserDefaults = .standard, modelKey: String = "client.selected_model") {
        self.defaults = defaults
        self.modelKey = modelKey

        if let stored = defaults.string(forKey: modelKey),
           let parsed = ClientModel(rawValue: stored) {
            selectedModel = parsed
        } else {
            selectedModel = .gpt5Mini
        }
    }
}
