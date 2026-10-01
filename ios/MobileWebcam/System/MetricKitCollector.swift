import Foundation
import MetricKit

/// Stores MetricKit diagnostic payloads (crashes, hangs, disk/CPU exceptions) on disk and hands
/// them to the session on the next connection — the only crash reporting available without a
/// Mac or App Store distribution.
final class MetricKitCollector: NSObject, MXMetricManagerSubscriber, DiagnosticsSource, @unchecked Sendable {
    private static let maxReportCharacters = 6_000

    private let directory: URL
    private let logger: AppLogger
    private let lock = NSLock()

    init(logger: AppLogger, fileManager: FileManager = .default) {
        let caches = fileManager.urls(for: .cachesDirectory, in: .userDomainMask).first
            ?? fileManager.temporaryDirectory
        directory = caches.appendingPathComponent("diagnostics", isDirectory: true)
        self.logger = logger
        super.init()
    }

    @MainActor
    func register() {
        MXMetricManager.shared.add(self)
    }

    // MARK: - MXMetricManagerSubscriber (arbitrary queue)

    func didReceive(_ payloads: [MXDiagnosticPayload]) {
        lock.lock()
        defer { lock.unlock() }
        do {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            for payload in payloads {
                let file = directory.appendingPathComponent("\(UUID().uuidString).json")
                try payload.jsonRepresentation().write(to: file, options: .atomic)
            }
            logger.warn("diagnostics", "Stored \(payloads.count) MetricKit diagnostic payload(s)")
        } catch {
            logger.error("diagnostics", "Cannot store diagnostics: \(error.localizedDescription)")
        }
    }

    // MARK: - DiagnosticsSource

    func takePendingReports(limit: Int) -> [String] {
        lock.lock()
        defer { lock.unlock() }
        let files = (try? FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: nil)) ?? []
        return files.sorted { $0.lastPathComponent < $1.lastPathComponent }.prefix(limit).compactMap { file in
            defer { try? FileManager.default.removeItem(at: file) }
            guard let data = try? Data(contentsOf: file), let text = String(data: data, encoding: .utf8) else { return nil }
            return text.count > Self.maxReportCharacters ? String(text.prefix(Self.maxReportCharacters)) + "…" : text
        }
    }
}
