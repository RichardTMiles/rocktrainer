#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

missing=()
for cmd in cmake pkg-config g++; do
  if ! command -v "$cmd" >/dev/null 2>&1; then
    missing+=("$cmd")
  fi
done
for pkg in sdl2 portaudio-2.0 aubio nlohmann_json; do
  if ! pkg-config --exists "$pkg"; then
    missing+=("$pkg (pkg-config)")
  fi
done
if [ "${#missing[@]}" -ne 0 ]; then
  echo "Missing dependencies: ${missing[*]}" >&2
  echo "Install using apt: sudo apt-get install build-essential cmake pkg-config portaudio19-dev libaubio-dev libsdl2-dev nlohmann-json3-dev" >&2
  exit 1
fi

cmake -S . -B build
cmake --build build

