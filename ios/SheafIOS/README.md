# Sheaf iOS App (SwiftUI)

This directory contains a new iOS 17+ app implementation for Sheaf with:
- Two-page flow: Conversation List -> Chat -> Back
- Sheaf API integration (`/chats`, metadata, messages, send)
- Markdown segmentation and rendering pipeline
- LaTeX render worker (`WKWebView`) with disk + memory cache

## Structure

- `Sources/SheafIOS/App`: app entry + routing state
- `Sources/SheafIOS/Views`: `ConversationListView`, `ChatView`, message rendering
- `Sources/SheafIOS/ViewModels`: list/chat orchestration
- `Sources/SheafIOS/Networking`: Sheaf API client
- `Sources/SheafIOS/Services`: markdown segmentation + math render/cache
- `Sources/SheafIOS/Resources/MathJax`: worker HTML + local MathJax asset slot

## MathJax Asset

`Sources/SheafIOS/Resources/MathJax/tex-svg.js` is vendored locally.
The worker is configured for file-only loads (no remote navigation).

## Server URL

Server URL is configured in:

`Sources/SheafIOS/Resources/Config/SheafConfig.json`

Default value:

```json
{
  "api_base_url": "http://127.0.0.1:2731"
}
```

## Quick bootstrap

From repo root (with your existing server launcher already running), in another terminal:

```bash
ios/SheafIOS/scripts/bootstrap_xcode.sh
```

This resolves packages, runs tests, and opens the package in Xcode.
