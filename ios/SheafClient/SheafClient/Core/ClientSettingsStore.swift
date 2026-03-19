import Foundation

@MainActor
final class ClientSettingsStore: ObservableObject {
    static let shared = ClientSettingsStore()

    @Published var selectedModelName: String {
        didSet {
            defaults.set(selectedModelName, forKey: modelKey)
        }
    }
    @Published private(set) var availableModels: [ClientModel] = []

    private let defaults: UserDefaults
    private let modelKey: String

    init(defaults: UserDefaults = .standard, modelKey: String = "client.selected_model") {
        self.defaults = defaults
        self.modelKey = modelKey

        selectedModelName = defaults.string(forKey: modelKey)?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        if selectedModelName.isEmpty {
            selectedModelName = "gpt-5-mini"
        }
    }

    func refreshAvailableModels(client: SheafAPIClient = .shared) async {
        do {
            let models = try await client.listAvailableModels()
            availableModels = models
            alignSelectedModel(with: models)
        } catch {
            if availableModels.isEmpty {
                let fallback = ClientModel(
                    name: "gpt-5-mini",
                    provider: "openai",
                    source: "fallback",
                    metadata: [:],
                    isDefault: true
                )
                availableModels = [fallback]
                alignSelectedModel(with: [fallback])
            }
        }
    }

    private func alignSelectedModel(with models: [ClientModel]) {
        if models.contains(where: { $0.name == selectedModelName }) {
            return
        }
        if let model = models.first(where: \.isDefault) ?? models.first {
            selectedModelName = model.name
        }
    }
}
