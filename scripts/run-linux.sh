#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$SCRIPT_DIR/build-linux.sh"
cd "$SCRIPT_DIR/.."
./build/NeonStrings --device "Rocksmith" --latency-ms 20 charts/example.json

