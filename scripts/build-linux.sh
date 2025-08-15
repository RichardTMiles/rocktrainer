#!/usr/bin/env bash
set -euo pipefail
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config \
  portaudio19-dev libportaudio2 libaubio-dev libsdl2-dev nlohmann-json3-dev
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
cmake --build . -j