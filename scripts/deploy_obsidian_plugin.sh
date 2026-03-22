#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PLUGIN_DIR="$ROOT/apps/obsidian-replica"
INSTALLER="$ROOT/scripts/install_obsidian_plugin.sh"

if [[ -x /opt/homebrew/bin/npm ]]; then
  export PATH="/opt/homebrew/bin:$PATH"
fi

if ! command -v npm >/dev/null 2>&1; then
  echo "npm is not available on PATH, and /opt/homebrew/bin/npm was not found." >&2
  exit 1
fi

(
  cd "$PLUGIN_DIR"
  npm run build
)

"$INSTALLER" --all-existing
