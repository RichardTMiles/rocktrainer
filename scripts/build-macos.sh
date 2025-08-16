#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

export PATH="/opt/homebrew/bin:$PATH"
if ! command -v brew >/dev/null 2>&1; then
  echo "Homebrew not found" >&2
  exit 1
fi

for pkg in portaudio aubio sdl2 nlohmann-json cmake; do
  brew list --versions "$pkg" >/dev/null 2>&1 || brew install "$pkg"
done

cmake -S . -B build
cmake --build build

