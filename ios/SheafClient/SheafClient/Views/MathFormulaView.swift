import SwiftUI
import WebKit
#if os(iOS)
import UIKit
#else
import AppKit
#endif

struct MathFormulaView: View {
    @Environment(\.colorScheme) private var colorScheme

    let tex: String
    let block: Bool

    @State private var asset: MathAsset?

    private var appearance: MathAppearance {
        colorScheme == .dark ? .dark : .light
    }

    var body: some View {
        Group {
            if let asset {
                let bottomInset = max(0, -asset.baseline)
                InlineSVGView(svg: asset.svg, appearance: appearance)
                    .frame(width: max(1, asset.width), height: max(18, asset.height + bottomInset), alignment: .leading)
                    .fixedSize(horizontal: !block, vertical: true)
            } else {
                Text(block ? "$$\(tex)$$" : "$\(tex)$")
                    .font(.system(.footnote, design: .monospaced))
                    .foregroundStyle(.secondary)
                    .redacted(reason: .placeholder)
            }
        }
        .task(id: MathCacheKey.make(tex: tex, block: block, appearance: appearance)) {
            asset = await MathJaxRenderService.shared.render(tex: tex, block: block, appearance: appearance)
        }
    }
}

private struct InlineSVGView: PlatformWebViewRepresentable {
    let svg: String
    let appearance: MathAppearance

#if os(iOS)
    func makeUIView(context: Context) -> WKWebView {
        makeWebView()
    }

    func updateUIView(_ webView: WKWebView, context: Context) {
        updateWebView(webView)
    }
#else
    func makeNSView(context: Context) -> WKWebView {
        makeWebView()
    }

    func updateNSView(_ webView: WKWebView, context: Context) {
        updateWebView(webView)
    }
#endif

    private func makeWebView() -> WKWebView {
        let config = WKWebViewConfiguration()
        config.defaultWebpagePreferences.allowsContentJavaScript = false
        let view = WKWebView(frame: .zero, configuration: config)
#if os(iOS)
        view.isOpaque = false
        view.backgroundColor = .clear
        view.scrollView.isScrollEnabled = false
        view.isUserInteractionEnabled = false
#else
        view.setValue(false, forKey: "drawsBackground")
#endif
        return view
    }

    private func updateWebView(_ webView: WKWebView) {
        let color = appearance == .dark ? "#FFFFFF" : "#111111"
        let html = """
        <html>
          <head>
            <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\" />
            <style>
              html, body { margin: 0; padding: 0; background: transparent; overflow: hidden; display: inline-block; color: \(color); }
              svg { display: block; width: auto; height: auto; color: inherit; }
            </style>
          </head>
          <body>\(svg)</body>
        </html>
        """
        webView.loadHTMLString(html, baseURL: nil)
    }
}

#if os(iOS)
private typealias PlatformWebViewRepresentable = UIViewRepresentable
#else
private typealias PlatformWebViewRepresentable = NSViewRepresentable
#endif
