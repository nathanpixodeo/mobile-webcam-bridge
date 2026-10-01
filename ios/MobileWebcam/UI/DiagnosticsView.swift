import BridgeKit
import SwiftUI

/// Everything useful when debugging without Xcode: build identity, live transport numbers, the
/// test-pattern switch and the newest log lines (the same lines the host receives).
struct DiagnosticsView: View {
    @Bindable var model: AppModel
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            List {
                Section("Build") {
                    LabeledContent("App", value: "\(model.identity.app.version) (\(model.identity.app.build))")
                    LabeledContent("Git", value: model.identity.app.gitSha)
                    LabeledContent("Device", value: model.identity.device.model)
                    LabeledContent("System", value: model.identity.device.os)
                    LabeledContent("Port", value: "\(model.port)")
                }
                Section("Transport") {
                    LabeledContent("Round trip", value: model.session.roundTripMs.map { "\($0) ms" } ?? "—")
                    LabeledContent("Video sent", value: "\(model.session.videoFps) fps, \(model.session.videoKbps) kbps")
                    LabeledContent("Video frames dropped", value: "\(model.session.droppedVideoFrames)")
                }
                Section {
                    Toggle("Test pattern and tone", isOn: $model.useTestPattern)
                } footer: {
                    Text("Replaces the camera and microphone with synthetic media, to tell capture problems "
                        + "from transport problems.")
                }
                Section("Log") {
                    ForEach(Array(model.recentLogs.reversed().enumerated()), id: \.offset) { item in
                        LogLine(entry: item.element)
                    }
                }
            }
            .navigationTitle("Diagnostics")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
            .task {
                while !Task.isCancelled {
                    model.refreshLogs()
                    try? await Task.sleep(for: .seconds(1))
                }
            }
        }
    }
}

private struct LogLine: View {
    let entry: LogEntry

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text("\(entry.level.rawValue.uppercased()) · \(entry.category)")
                .font(.caption2.monospaced())
                .foregroundStyle(color)
            Text(entry.message)
                .font(.caption.monospaced())
                .textSelection(.enabled)
        }
    }

    private var color: Color {
        switch entry.level {
        case .debug: .secondary
        case .info: .blue
        case .warn: .orange
        case .error: .red
        }
    }
}
