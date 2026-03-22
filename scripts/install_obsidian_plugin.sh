#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEFAULT_SOURCE="$ROOT/apps/obsidian-replica"
PLUGIN_ID="sheaf-obsidian-replica"
DEFAULT_OBSIDIAN_DOCUMENTS="/Users/joyo/Library/Mobile Documents/iCloud~md~obsidian/Documents"

usage() {
  cat <<EOF
Usage:
  $(basename "$0") --vault <vault-path> [--source <plugin-source-path>]
  $(basename "$0") --all-existing [--documents-root <obsidian-documents-path>] [--source <plugin-source-path>]

Copies the built Obsidian plugin into:
  <vault-path>/.obsidian/plugins/$PLUGIN_ID

Behavior:
  - copies manifest.json and main.js
  - copies styles.css when present
  - preserves destination data.json if it already exists
  - replaces a destination symlink with a real plugin directory
  - in --all-existing mode, updates every vault that already has $PLUGIN_ID installed

Example:
  $(basename "$0") --vault "/path/to/ObsidianVault"
  $(basename "$0") --all-existing
EOF
}

VAULT_PATH=""
SOURCE_PATH="$DEFAULT_SOURCE"
DOCUMENTS_ROOT="$DEFAULT_OBSIDIAN_DOCUMENTS"
INSTALL_ALL_EXISTING=0

install_to_vault() {
  local vault_path="$1"
  local source_path="$2"

  if [[ ! -d "$vault_path" ]]; then
    echo "Vault path does not exist: $vault_path" >&2
    return 1
  fi

  local dest_root="$vault_path/.obsidian/plugins"
  local dest_path="$dest_root/$PLUGIN_ID"
  local tmp_parent="${TMPDIR:-/tmp}"
  local stage_dir
  stage_dir="$(mktemp -d "$tmp_parent/${PLUGIN_ID}.XXXXXX")"

  mkdir -p "$dest_root"

  cp "$source_path/manifest.json" "$stage_dir/"
  cp "$source_path/main.js" "$stage_dir/"

  if [[ -f "$source_path/styles.css" ]]; then
    cp "$source_path/styles.css" "$stage_dir/"
  fi

  if [[ -f "$dest_path/data.json" ]]; then
    cp "$dest_path/data.json" "$stage_dir/data.json"
  fi

  if [[ -L "$dest_path" ]]; then
    rm "$dest_path"
  elif [[ -d "$dest_path" ]]; then
    rm -rf "$dest_path"
  elif [[ -e "$dest_path" ]]; then
    echo "Destination exists but is not a directory or symlink: $dest_path" >&2
    rm -rf "$stage_dir"
    return 1
  fi

  mv "$stage_dir" "$dest_path"

  echo "Installed $PLUGIN_ID to $dest_path"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --vault)
      VAULT_PATH="${2:-}"
      shift 2
      ;;
    --source)
      SOURCE_PATH="${2:-}"
      shift 2
      ;;
    --documents-root)
      DOCUMENTS_ROOT="${2:-}"
      shift 2
      ;;
    --all-existing)
      INSTALL_ALL_EXISTING=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ "$INSTALL_ALL_EXISTING" -eq 1 && -n "$VAULT_PATH" ]]; then
  echo "Use either --vault or --all-existing, not both" >&2
  usage >&2
  exit 1
fi

if [[ "$INSTALL_ALL_EXISTING" -eq 0 && -z "$VAULT_PATH" ]]; then
  echo "--vault or --all-existing is required" >&2
  usage >&2
  exit 1
fi

if [[ ! -f "$SOURCE_PATH/manifest.json" || ! -f "$SOURCE_PATH/main.js" ]]; then
  echo "Plugin source must contain manifest.json and main.js: $SOURCE_PATH" >&2
  exit 1
fi

if [[ "$INSTALL_ALL_EXISTING" -eq 1 ]]; then
  if [[ ! -d "$DOCUMENTS_ROOT" ]]; then
    echo "Documents root does not exist: $DOCUMENTS_ROOT" >&2
    exit 1
  fi

  plugin_paths=()
  while IFS= read -r plugin_path; do
    plugin_paths+=("$plugin_path")
  done < <(find "$DOCUMENTS_ROOT" -path "*/.obsidian/plugins/$PLUGIN_ID" 2>/dev/null | sort)

  if [[ "${#plugin_paths[@]}" -eq 0 ]]; then
    echo "No existing $PLUGIN_ID installs found under $DOCUMENTS_ROOT"
    exit 0
  fi

  for plugin_path in "${plugin_paths[@]}"; do
    vault_path="${plugin_path%/.obsidian/plugins/$PLUGIN_ID}"
    install_to_vault "$vault_path" "$SOURCE_PATH"
  done

  echo "Updated ${#plugin_paths[@]} vault(s)"
  exit 0
fi

install_to_vault "$VAULT_PATH" "$SOURCE_PATH"
