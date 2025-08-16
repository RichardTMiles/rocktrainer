$ErrorActionPreference = 'Stop'
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location (Join-Path $ScriptDir '..')

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Write-Error 'cmake not found in PATH'
}

$vcpkg = Join-Path (Get-Location) 'vcpkg'
if (!(Test-Path $vcpkg)) {
    git clone https://github.com/microsoft/vcpkg $vcpkg
    & "$vcpkg\bootstrap-vcpkg.bat"
}
& "$vcpkg\vcpkg" install portaudio aubio sdl2 nlohmann-json

cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$vcpkg\scripts\buildsystems\vcpkg.cmake" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --config Release

