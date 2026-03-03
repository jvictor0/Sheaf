import Foundation
import WebKit
#if os(iOS)
import UIKit
#else
import AppKit
#endif

actor MathJaxRenderService {
    static let shared = MathJaxRenderService()

    private let cache = MathCache()
    @MainActor private lazy var worker = MathJaxWorker()

    func warmup() async {
        _ = await MainActor.run { worker }
    }

    func render(tex: String, block: Bool) async -> MathAsset? {
        let key = MathCacheKey.make(tex: tex, block: block)
        if let cached = await cache.get(key) {
            return cached
        }

        let rendered = await worker.render(tex: tex, block: block)
        guard let rendered else { return nil }
        await cache.set(rendered, for: key)
        return rendered
    }
}

@MainActor
final class MathJaxWorker: NSObject, WKNavigationDelegate {
    private let webView: WKWebView
    private var isLoaded = false

    override init() {
        let config = WKWebViewConfiguration()
        config.defaultWebpagePreferences.allowsContentJavaScript = true
        config.websiteDataStore = .nonPersistent()

        let contentController = WKUserContentController()
        config.userContentController = contentController

        webView = WKWebView(frame: .zero, configuration: config)
#if os(iOS)
        webView.isOpaque = false
        webView.backgroundColor = .clear
#else
        webView.setValue(false, forKey: "drawsBackground")
#endif

        super.init()

        webView.navigationDelegate = self
#if os(iOS)
        webView.scrollView.isScrollEnabled = false
#endif

        loadWorkerPageIfNeeded()
    }

    func render(tex: String, block: Bool) async -> MathAsset? {
        loadWorkerPageIfNeeded()
        for _ in 0..<20 where !isLoaded {
            try? await Task.sleep(nanoseconds: 100_000_000)
        }
        guard isLoaded else { return nil }

        let escaped = tex
            .replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
            .replacingOccurrences(of: "\n", with: "\\n")
        let js = "renderMath(\"\(escaped)\", \(block ? "true" : "false"));"

        return await withCheckedContinuation { continuation in
            webView.evaluateJavaScript(js) { value, _ in
                guard let dict = value as? [String: Any],
                      let svg = dict["svg"] as? String,
                      let width = dict["width"] as? Double,
                      let height = dict["height"] as? Double,
                      let baseline = dict["baseline"] as? Double else {
                    continuation.resume(returning: nil)
                    return
                }
                continuation.resume(returning: MathAsset(svg: svg, width: width, height: height, baseline: baseline))
            }
        }
    }

    private func loadWorkerPageIfNeeded() {
        guard !isLoaded else { return }
        guard let url = Bundle.module.url(forResource: "mathjax-worker", withExtension: "html", subdirectory: "MathJax") else {
            return
        }
        let readURL = url.deletingLastPathComponent()
        webView.loadFileURL(url, allowingReadAccessTo: readURL)
    }

    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        isLoaded = true
    }

    func webView(
        _ webView: WKWebView,
        decidePolicyFor navigationAction: WKNavigationAction,
        decisionHandler: @escaping (WKNavigationActionPolicy) -> Void
    ) {
        let allowed = navigationAction.request.url?.isFileURL ?? false
        decisionHandler(allowed ? .allow : .cancel)
    }
}
