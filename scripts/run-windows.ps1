$ErrorActionPreference = 'Stop'
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
& "$ScriptDir\build-windows.ps1"
Set-Location (Join-Path $ScriptDir '..')
.\build\Release\NeonStrings.exe --device "Rocksmith" --latency-ms 20 charts\example.json

