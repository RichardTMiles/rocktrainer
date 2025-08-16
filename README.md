# rocktrainer

## Prerequisites

### macOS (Homebrew)
```
brew install portaudio aubio sdl2 nlohmann-json cmake
```

### Ubuntu/Debian (Apt)
```
sudo apt-get install build-essential cmake pkg-config portaudio19-dev libaubio-dev libsdl2-dev nlohmann-json3-dev
```

### Windows (vcpkg)
```
git clone https://github.com/microsoft/vcpkg
./vcpkg/bootstrap-vcpkg.bat
./vcpkg/vcpkg install portaudio aubio sdl2 nlohmann-json
```

## Build
```
cmake -S . -B build
cmake --build build
```

## Run
```
./build/NeonStrings --device "Rocksmith" --latency-ms 20 charts/example.json
```

## Development

Enable the git hooks to make sure the build passes before pushing:
```
git config core.hooksPath .githooks
```

The pre-push hook runs the same build as the CI workflow. If [`act`](https://github.com/nektos/act) is installed it will execute
 the GitHub Actions workflow, otherwise it performs a local CMake build.
