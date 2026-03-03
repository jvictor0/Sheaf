// swift-tools-version: 5.10
import PackageDescription

let package = Package(
    name: "SheafIOS",
    platforms: [
        .iOS(.v17),
        .macOS(.v14)
    ],
    products: [
        .executable(name: "SheafIOSApp", targets: ["SheafIOSApp"])
    ],
    dependencies: [
        .package(url: "https://github.com/apple/swift-markdown", from: "0.5.0")
    ],
    targets: [
        .executableTarget(
            name: "SheafIOSApp",
            dependencies: [
                .product(name: "Markdown", package: "swift-markdown")
            ],
            path: "Sources/SheafIOS",
            resources: [
                .process("Resources")
            ]
        ),
        .testTarget(
            name: "SheafIOSTests",
            dependencies: ["SheafIOSApp"],
            path: "Tests/SheafIOSTests"
        )
    ]
)
