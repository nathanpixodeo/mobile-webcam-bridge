import SwiftUI

@main
struct MobileWebcamApp: App {
    @State private var root = CompositionRoot()

    var body: some Scene {
        WindowGroup {
            RootView(model: root.model)
                .task { await root.start() }
        }
    }
}
