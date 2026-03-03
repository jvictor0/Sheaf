import Foundation

struct MathAsset: Codable, Hashable {
    let svg: String
    let width: Double
    let height: Double
    let baseline: Double
}

enum MathCacheKey {
    static func make(tex: String, block: Bool) -> String {
        let input = "\(block ? "block" : "inline")::\(tex)"
        return Data(input.utf8).base64EncodedString()
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "+", with: "-")
    }
}

actor MathCache {
    private let memory = NSCache<NSString, NSData>()
    private let folder: URL

    init() {
        let caches = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first
            ?? URL(fileURLWithPath: NSTemporaryDirectory(), isDirectory: true)
        let folder = caches.appendingPathComponent("sheaf-math-cache", isDirectory: true)
        self.folder = folder
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
    }

    func get(_ key: String) -> MathAsset? {
        if let data = memory.object(forKey: key as NSString) as Data? {
            return try? JSONDecoder().decode(MathAsset.self, from: data)
        }

        let file = folder.appendingPathComponent(key).appendingPathExtension("json")
        guard let data = try? Data(contentsOf: file) else { return nil }
        memory.setObject(data as NSData, forKey: key as NSString)
        return try? JSONDecoder().decode(MathAsset.self, from: data)
    }

    func set(_ asset: MathAsset, for key: String) {
        guard let data = try? JSONEncoder().encode(asset) else { return }
        memory.setObject(data as NSData, forKey: key as NSString)
        let file = folder.appendingPathComponent(key).appendingPathExtension("json")
        try? data.write(to: file, options: .atomic)
    }
}
