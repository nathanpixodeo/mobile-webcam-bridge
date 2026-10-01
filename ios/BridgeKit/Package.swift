// swift-tools-version: 6.0
// BridgeKit holds every piece of the iOS companion app that does not need Apple-only
// frameworks: the wire protocol, Annex-B handling, scheduling and session policy. It builds and
// tests on Linux so CI can verify it cheaply, and the app target links it as a local package.
import PackageDescription

let package = Package(
    name: "BridgeKit",
    platforms: [.iOS(.v17), .macOS(.v14)],
    products: [
        .library(name: "BridgeKit", targets: ["BridgeKit"]),
    ],
    targets: [
        .target(name: "BridgeKit"),
        .testTarget(name: "BridgeKitTests", dependencies: ["BridgeKit"]),
    ]
)
