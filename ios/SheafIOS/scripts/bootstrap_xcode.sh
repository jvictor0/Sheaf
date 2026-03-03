#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PKG_DIR="$ROOT/ios/SheafIOS"

if [[ ! -f "$PKG_DIR/Package.swift" ]]; then
  echo "Package.swift not found at $PKG_DIR" >&2
  exit 1
fi

cd "$PKG_DIR"

echo "[1/3] Resolving Swift package dependencies..."
swift package resolve --disable-sandbox

echo "[2/3] Running package tests..."
swift test --disable-sandbox

echo "[3/3] Opening package in Xcode..."
open -a Xcode "$PKG_DIR/Package.swift"

echo "Done. Change Sources/SheafIOS/Resources/Config/SheafConfig.json if you need a different backend URL."
