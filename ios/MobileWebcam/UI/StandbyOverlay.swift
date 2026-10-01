import SwiftUI

/// Full-screen black overlay: on OLED panels black pixels are off, which saves power and heat
/// during long calls. Capture keeps running underneath. Tap to wake.
struct StandbyOverlay: View {
    let model: AppModel

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            VStack(spacing: 8) {
                Circle()
                    .fill(model.session.connection == .connected ? Color.green : Color.gray)
                    .frame(width: 8, height: 8)
                Text("Streaming — tap to wake")
                    .font(.footnote)
                    .foregroundStyle(Color(white: 0.25))
            }
        }
        .contentShape(Rectangle())
        .onTapGesture {
            withAnimation { model.isStandby = false }
        }
        .accessibilityAddTraits(.isButton)
        .accessibilityLabel("Standby. Tap to wake.")
    }
}
