# One-time: vcpkg install
$vcpkg = "$PSScriptRoot\..\vcpkg"
if (!(Test-Path $vcpkg)) {
    git clone https://github.com/microsoft/vcpkg $vcpkg
    & $vcpkg\bootstrap-vcpkg.bat
}
& $vcpkg\vcpkg install portaudio aubio sdl2 nlohmann-json

# Build
mkdir build -ea 0
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE="$vcpkg\scripts\buildsystems\vcpkg.cmake" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build . --config Release