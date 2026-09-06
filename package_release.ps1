param(
    [string]$QtRoot = "C:\Qt\6.10.2\mingw_64",
    [string]$MingwRoot = "C:\Qt\Tools\mingw1310_64",
    [string]$DistDir = "dist\gba_emulator_windows",
    [string]$ZipPath = "dist\gba_emulator_windows.zip",
    [string]$SdlExeName = "emu.exe",
    [string]$QtExeName = "emu_qt.exe",
    [switch]$IncludeFfmpeg,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($root)) { $root = (Get-Location).Path }

Push-Location $root
try {
    if (-not $SkipBuild) {
        $sdlInc = Join-Path $root "third_party\SDL2_2_32_10\SDL2-2.32.10\x86_64-w64-mingw32\include\SDL2"
        $sdlLib = Join-Path $root "third_party\SDL2_2_32_10\SDL2-2.32.10\x86_64-w64-mingw32\lib"
        if (!(Test-Path $sdlInc)) { throw "SDL include path not found: $sdlInc" }
        if (!(Test-Path $sdlLib)) { throw "SDL lib path not found: $sdlLib" }

        Write-Host "Building emu.exe (SDL)..."
        & g++ -std=c++17 -O2 `
            main.cpp gba.cpp bios.cpp cartridge.cpp bus.cpp cpu.cpp `
            ppu.cpp timers.cpp interrupts.cpp keypad.cpp dma.cpp swi.cpp `
            frontend_sdl.cpp apu.cpp backup.cpp save_detect.cpp `
            "-I$sdlInc" "-L$sdlLib" -lmingw32 -lSDL2main -lSDL2 `
            -o $SdlExeName
        if ($LASTEXITCODE -ne 0) { throw "SDL build failed." }

        Write-Host "Building $QtExeName (Qt GUI)..."
        & powershell -ExecutionPolicy Bypass -File (Join-Path $root "build_qt_gui.ps1") `
            -QtRoot $QtRoot -MingwRoot $MingwRoot -OutExe $QtExeName
        if ($LASTEXITCODE -ne 0) { throw "Qt build failed." }
    }

    $distPath = if ([System.IO.Path]::IsPathRooted($DistDir)) { $DistDir } else { Join-Path $root $DistDir }
    $zipOut = if ([System.IO.Path]::IsPathRooted($ZipPath)) { $ZipPath } else { Join-Path $root $ZipPath }

    if (Test-Path $distPath) { Remove-Item -Recurse -Force $distPath }
    New-Item -ItemType Directory -Path $distPath | Out-Null

    $baseFiles = @(
        $SdlExeName, $QtExeName, "SDL2.dll",
        "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "Qt6Multimedia.dll", "Qt6Network.dll", "Qt6Svg.dll",
        "D3Dcompiler_47.dll", "opengl32sw.dll",
        "libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll",
        "README.md"
    )
    if ($IncludeFfmpeg) {
        $baseFiles += @("avcodec-61.dll", "avformat-61.dll", "avutil-59.dll", "swresample-5.dll", "swscale-8.dll")
    }
    foreach ($f in $baseFiles) {
        if (Test-Path $f) {
            Copy-Item -Force $f -Destination $distPath
        } else {
            Write-Warning "Missing file: $f"
        }
    }

    $pluginDirs = @("platforms", "styles", "imageformats", "iconengines", "multimedia", "networkinformation", "tls", "generic")
    foreach ($d in $pluginDirs) {
        if (Test-Path $d) {
            Copy-Item -Recurse -Force $d -Destination (Join-Path $distPath $d)
        } else {
            Write-Warning "Missing plugin dir: $d"
        }
    }
    if (-not $IncludeFfmpeg) {
        $ffmpegPlugin = Join-Path $distPath "multimedia\\ffmpegmediaplugin.dll"
        if (Test-Path $ffmpegPlugin) { Remove-Item -Force $ffmpegPlugin }
    }

    $readmeDist = @"
GBA Emulator - Windows distribution

Included executables:
- $QtExeName (GUI, no console window)
- $SdlExeName (CLI/SDL frontend)

Quick start:
1) Put your .gba file and (optional) bios.bin in the same folder, or load from File menu.
2) Run $QtExeName for GUI usage.
3) Saves are written as .sav next to the ROM file.

Important:
- BIOS/ROM are not bundled.
- Respect legal requirements for BIOS/ROM ownership and distribution in your region.
"@
    if (-not $IncludeFfmpeg) {
        $readmeDist += "`r`n- FFmpeg plugin was excluded to reduce dependency complexity.`r`n"
    }
    Set-Content -Path (Join-Path $distPath "README_DIST.txt") -Value $readmeDist -Encoding UTF8

    $zipParent = Split-Path -Parent $zipOut
    if (!(Test-Path $zipParent)) { New-Item -ItemType Directory -Path $zipParent | Out-Null }
    if (Test-Path $zipOut) { Remove-Item -Force $zipOut }
    Compress-Archive -Path (Join-Path $distPath "*") -DestinationPath $zipOut -CompressionLevel Optimal

    Write-Host "Distribution ready:"
    Write-Host " - Folder: $distPath"
    Write-Host " - Zip:    $zipOut"
}
finally {
    Pop-Location
}
