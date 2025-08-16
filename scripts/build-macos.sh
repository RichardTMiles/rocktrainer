#!/usr/bin/env bash
set -euo pipefail

git config core.hooksPath .githooks

# Ensure Homebrew bin is on PATH (Apple Silicon)
export PATH="/opt/homebrew/bin:$PATH"

# Install deps if missing
brew list --versions portaudio  >/dev/null 2>&1 || brew install portaudio
brew list --versions aubio      >/dev/null 2>&1 || brew install aubio
brew list --versions sdl2       >/dev/null 2>&1 || brew install sdl2
brew list --versions nlohmann-json >/dev/null 2>&1 || brew install nlohmann-json
brew list --versions cmake >/dev/null 2>&1 || brew install cmake

mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
cmake --build . -j