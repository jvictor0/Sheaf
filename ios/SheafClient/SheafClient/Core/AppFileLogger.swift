import Foundation

actor AppFileLogger {
    static let shared = AppFileLogger()

    private let maxBytes: UInt64 = 2_000_000
    private let keepTailBytes: UInt64 = 1_000_000
    private let fileManager = FileManager.default
    private var didPrepare = false

    private lazy var logsDirectoryURL: URL = {
        let documents = fileManager.urls(for: .documentDirectory, in: .userDomainMask).first!
        return documents.appendingPathComponent("Logs", isDirectory: true)
    }()

    private lazy var logFileURL: URL = {
        logsDirectoryURL.appendingPathComponent("sheaf.log", isDirectory: false)
    }()

    private lazy var timestampFormatter: ISO8601DateFormatter = {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        return formatter
    }()

    func log(_ message: String, category: String = "app") {
        prepareIfNeeded()
        rotateIfNeeded()

        let timestamp = timestampFormatter.string(from: Date())
        let line = "\(timestamp) [\(category)] \(message)\n"
        guard let data = line.data(using: .utf8) else { return }

        if let handle = try? FileHandle(forWritingTo: logFileURL) {
            defer { try? handle.close() }
            do {
                try handle.seekToEnd()
                try handle.write(contentsOf: data)
            } catch {
                // Swallow logging failures.
            }
            return
        }

        try? data.write(to: logFileURL, options: [.atomic])
    }

    func currentLogPath() -> String {
        prepareIfNeeded()
        return logFileURL.path
    }

    private func prepareIfNeeded() {
        guard !didPrepare else { return }
        didPrepare = true

        try? fileManager.createDirectory(at: logsDirectoryURL, withIntermediateDirectories: true)
        if !fileManager.fileExists(atPath: logFileURL.path) {
            fileManager.createFile(atPath: logFileURL.path, contents: Data())
        }
    }

    private func rotateIfNeeded() {
        guard
            let attrs = try? fileManager.attributesOfItem(atPath: logFileURL.path),
            let bytes = attrs[.size] as? NSNumber,
            bytes.uint64Value > maxBytes
        else {
            return
        }

        guard let handle = try? FileHandle(forReadingFrom: logFileURL) else { return }
        defer { try? handle.close() }

        let size = bytes.uint64Value
        let offset = size > keepTailBytes ? size - keepTailBytes : 0
        do {
            try handle.seek(toOffset: offset)
            let tail = try handle.readToEnd() ?? Data()
            try tail.write(to: logFileURL, options: [.atomic])
        } catch {
            // Swallow logging failures.
        }
    }
}
