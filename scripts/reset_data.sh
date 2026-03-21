#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DATA_DIR="$ROOT/data"
ARCHIVE_DIR="$ROOT/data_archive"
STAMP="$(date -u +%Y%m%d_%H%M%S)"

if [[ -d "$DATA_DIR" ]]; then
  mkdir -p "$ARCHIVE_DIR"
  mv "$DATA_DIR" "$ARCHIVE_DIR/data_$STAMP"
  echo "Archived existing data to $ARCHIVE_DIR/data_$STAMP"
fi

mkdir -p "$DATA_DIR/user_dbs" "$DATA_DIR/system_prompts"
echo "Created fresh data layout at $DATA_DIR"
