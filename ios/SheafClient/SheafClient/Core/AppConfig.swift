import Foundation

struct AppConfig: Decodable {
    let apiBaseURL: String
    let dictationBaseURL: String

    enum CodingKeys: String, CodingKey {
        case apiBaseURL = "api_base_url"
        case dictationBaseURL = "dictation_base_url"
    }

    static func load() -> AppConfig {
        guard let url = configURL() else {
            return AppConfig(
                apiBaseURL: "http://joyos-mac-mini.tail77a6ef.ts.net:2731",
                dictationBaseURL: "http://joyos-mac-mini.tail77a6ef.ts.net:8787"
            )
        }

        guard let data = try? Data(contentsOf: url),
              let config = try? JSONDecoder().decode(AppConfig.self, from: data),
              URL(string: config.apiBaseURL) != nil else {
            return AppConfig(
                apiBaseURL: "http://joyos-mac-mini.tail77a6ef.ts.net:2731",
                dictationBaseURL: "http://joyos-mac-mini.tail77a6ef.ts.net:8787"
            )
        }

        let dictationBaseURL = URL(string: config.dictationBaseURL) != nil
            ? config.dictationBaseURL
            : "http://joyos-mac-mini.tail77a6ef.ts.net:8787"

        return AppConfig(
            apiBaseURL: config.apiBaseURL,
            dictationBaseURL: dictationBaseURL
        )
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
