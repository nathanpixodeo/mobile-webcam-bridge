import BridgeKit
import SwiftUI

struct RootView: View {
    @Bindable var model: AppModel
    @State private var isShowingDiagnostics = false

    var body: some View {
        ZStack {
            NavigationStack {
                List {
                    Section {
                        ConnectionHeader(status: model.session.connection, port: model.port)
                    }
                    Section("Streams") {
                        StreamRow(title: "Camera", systemImage: "video.fill", state: model.session.video.state,
                                  detail: videoDetail)
                        StreamRow(title: "Microphone", systemImage: "mic.fill", state: model.session.audio.state,
                                  detail: model.session.audio.reason)
                    }
                    Section("Device") {
                        LabeledContent("Thermal", value: model.system.thermal.statusName.rawValue.capitalized)
                        LabeledContent("Battery", value: batteryDescription)
                        if model.system.lowPower {
                            Label("Low Power Mode is on", systemImage: "battery.25")
                                .foregroundStyle(.orange)
                        }
                    }
                    if model.needsPermissions {
                        Section {
                            Button("Allow camera and microphone") {
                                Task { await model.requestPermissions() }
                            }
                        } footer: {
                            Text("If access was denied earlier, enable it in Settings › Privacy & Security.")
                        }
                    }
                    Section {
                        Button {
                            withAnimation { model.isStandby = true }
                        } label: {
                            Label("Standby (black screen)", systemImage: "moon.fill")
                        }
                        Button {
                            isShowingDiagnostics = true
                        } label: {
                            Label("Diagnostics", systemImage: "stethoscope")
                        }
                    } footer: {
                        Text("Keep this app open while streaming. Standby blacks out the screen to save power; "
                            + "tap anywhere to wake it.")
                    }
                }
                .navigationTitle("Mobile Webcam")
                .sheet(isPresented: $isShowingDiagnostics) {
                    DiagnosticsView(model: model)
                }
            }
            if model.isStandby {
                StandbyOverlay(model: model)
                    .transition(.opacity)
            }
        }
        .statusBarHidden(model.isStandby)
        .persistentSystemOverlays(model.isStandby ? .hidden : .automatic)
        .onChange(of: model.useTestPattern) {
            model.testPatternChanged()
        }
    }

    private var videoDetail: String? {
        let video = model.session.video
        if let reason = video.reason { return reason }
        guard let width = video.width, let height = video.height, let fps = video.fps else { return nil }
        return "\(width)×\(height) @ \(fps) fps"
    }

    private var batteryDescription: String {
        guard let battery = model.system.battery else { return "Unknown" }
        return "\(battery.levelPercent) %" + (battery.charging ? " (charging)" : "")
    }
}

private struct ConnectionHeader: View {
    let status: ConnectionStatus
    let port: UInt16

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: symbol)
                .font(.title)
                .foregroundStyle(color)
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.headline)
                Text(subtitle).font(.subheadline).foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 4)
    }

    private var title: String {
        switch status {
        case .starting: "Starting…"
        case .listening: "Waiting for the computer"
        case .listenerFailed: "Cannot listen for the computer"
        case .handshaking: "Computer connecting…"
        case .connected: "Connected to the computer"
        }
    }

    private var subtitle: String {
        switch status {
        case .starting, .handshaking: "USB port \(port)"
        case .listening(let port): "Plug in the USB cable and start the bridge on the PC (port \(port))"
        case .listenerFailed(let message): message
        case .connected: "Streams start when an app opens the camera or microphone"
        }
    }

    private var symbol: String {
        switch status {
        case .connected: "cable.connector"
        case .listenerFailed: "exclamationmark.triangle.fill"
        default: "cable.connector.slash"
        }
    }

    private var color: Color {
        switch status {
        case .connected: .green
        case .listenerFailed: .red
        default: .secondary
        }
    }
}

private struct StreamRow: View {
    let title: String
    let systemImage: String
    let state: StreamState
    let detail: String?

    var body: some View {
        HStack {
            Label(title, systemImage: systemImage)
            Spacer()
            VStack(alignment: .trailing, spacing: 2) {
                Text(state.rawValue.capitalized)
                    .foregroundStyle(color)
                if let detail {
                    Text(detail).font(.caption).foregroundStyle(.secondary)
                }
            }
        }
    }

    private var color: Color {
        switch state {
        case .running: .green
        case .starting: .orange
        case .interrupted: .yellow
        case .error: .red
        case .off: .secondary
        }
    }
}
