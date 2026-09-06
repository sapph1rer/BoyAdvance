param(
    [string]$QtRoot = "C:\Qt\6.10.2\mingw_64",
    [string]$MingwRoot = "C:\Qt\Tools\mingw1310_64",
    [string]$OutExe = "emu_qt.exe",
    [switch]$Console,
    [switch]$EmbedBios
)

$ErrorActionPreference = "Stop"

$qtBin = Join-Path $QtRoot "bin"
$qtInc = Join-Path $QtRoot "include"
$qtLib = Join-Path $QtRoot "lib"
$mingwBin = Join-Path $MingwRoot "bin"

if (!(Test-Path $qtBin)) { throw "Qt bin not found: $qtBin" }
if (!(Test-Path $qtInc)) { throw "Qt include not found: $qtInc" }
if (!(Test-Path $qtLib)) { throw "Qt lib not found: $qtLib" }
if (!(Test-Path $mingwBin)) { throw "MinGW bin not found: $mingwBin" }

$env:PATH = "$qtBin;$mingwBin;$env:PATH"

$sources = @(
    "main_qt.cpp",
    "gba.cpp", "bios.cpp", "cartridge.cpp", "bus.cpp", "cpu.cpp",
    "ppu.cpp", "timers.cpp", "interrupts.cpp", "keypad.cpp", "dma.cpp",
    "swi.cpp", "apu.cpp", "backup.cpp", "save_detect.cpp"
)

$includeFlags = @(
    "-I$qtInc",
    "-I$(Join-Path $qtInc 'QtCore')",
    "-I$(Join-Path $qtInc 'QtGui')",
    "-I$(Join-Path $qtInc 'QtWidgets')",
    "-I$(Join-Path $qtInc 'QtMultimedia')"
)

$linkFlags = @(
    "-L$qtLib",
    "-lQt6Widgets", "-lQt6Multimedia", "-lQt6Gui", "-lQt6Core",
    "-lole32", "-lwinmm", "-lws2_32"
)

Write-Host "Building $OutExe ..."
$subsystemFlags = @()
if (-not $Console) {
    # Build as GUI app (no extra console window).
    $subsystemFlags += "-mwindows"
}

$defineFlags = @()
if ($EmbedBios) {
    if (!(Test-Path "bios_embedded.hpp")) {
        throw "bios_embedded.hpp not found. Run: python make_bios_header.py path\to\gba_bios.bin bios_embedded.hpp"
    }
    Write-Host "Embedding BIOS from bios_embedded.hpp into the executable."
    $defineFlags += "-DGBAEMU_EMBEDDED_BIOS"
}

& g++ -std=c++17 -O2 @sources @includeFlags @subsystemFlags @defineFlags @linkFlags -o $OutExe
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

$deploy = Join-Path $qtBin "windeployqt.exe"
if (Test-Path $deploy) {
    Write-Host "Running windeployqt ..."
    & $deploy --no-translations $OutExe
}

Write-Host "Done -> $OutExe"
