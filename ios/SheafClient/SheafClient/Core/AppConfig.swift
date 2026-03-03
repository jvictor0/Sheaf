import Foundation

struct AppConfig: Decodable {
    let apiBaseURL: String

    enum CodingKeys: String, CodingKey {
        case apiBaseURL = "api_base_url"
    }

    static func load() -> AppConfig {
        guard let url = configURL() else {
            return AppConfig(apiBaseURL: "http://127.0.0.1:2731")
        }

        guard let data = try? Data(contentsOf: url),
              let config = try? JSONDecoder().decode(AppConfig.self, from: data),
              URL(string: config.apiBaseURL) != nil else {
            return AppConfig(apiBaseURL: "http://127.0.0.1:2731")
        }

        return config
    }

    private static func configURL() -> URL? {
#if SWIFT_PACKAGE
        return Bundle.module.url(forResource: "SheafConfig", withExtension: "json")
#else
        let bundle = Bundle.main
        return bundle.url(forResource: "SheafConfig", withExtension: "json", subdirectory: "Config")
            ?? bundle.url(forResource: "SheafConfig", withExtension: "json", subdirectory: "Resources/Config")
            ?? bundle.url(forResource: "SheafConfig", withExtension: "json")
#endif
    }
}
