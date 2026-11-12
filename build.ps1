# Сборка Sam Altman (MinGW-w64 g++ + windres).
# Запуск:  powershell -ExecutionPolicy Bypass -File .\build.ps1
#          powershell -ExecutionPolicy Bypass -File .\build.ps1 -Clean
param([switch]$Clean)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $root

try {
    $build = Join-Path $root 'build'
    if ($Clean -and (Test-Path $build)) {
        Remove-Item $build -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $build | Out-Null

    function Find-Tool([string]$name) {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }

        $roots = @(
            (Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages'),
            'C:\msys64\mingw64\bin',
            'C:\mingw64\bin'
        )
        foreach ($rootPath in $roots) {
            if (-not (Test-Path $rootPath)) { continue }

            $direct = Join-Path $rootPath $name
            if (Test-Path $direct) { return $direct }

            foreach ($pkg in Get-ChildItem $rootPath -Directory -ErrorAction SilentlyContinue) {
                $candidate = Join-Path $pkg.FullName "mingw64\bin\$name"
                if (Test-Path $candidate) { return $candidate }
            }
        }
        throw "Не найден $name. Установите MinGW-w64: winget install BrechtSanders.WinLibs.POSIX.UCRT"
    }

    $gpp     = Find-Tool 'g++.exe'
    $windres = Find-Tool 'windres.exe'
    Write-Host "g++:     $gpp"
    Write-Host "windres: $windres"

    $common = @(
        '-std=c++17', '-O2', '-Wall', '-Wextra',
        '-D_WIN32_WINNT=0x0A00', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN'
    )
    $static = @('-static', '-static-libgcc', '-static-libstdc++')

    # --- 1. Сборщик ресурсов ------------------------------------------------
    Write-Host "[1/4] mkassets..."
    & $gpp @common (Join-Path $root 'tools\mkassets.cpp') -o (Join-Path $build 'mkassets.exe') `
        @static -lgdiplus -lshlwapi -lgdi32 -luser32 -lole32 -luuid
    if ($LASTEXITCODE -ne 0) { throw "mkassets: ошибка компиляции" }

    Copy-Item (Join-Path $root 'assets\face_src.jpg') (Join-Path $build 'face_src.jpg') -Force
    Push-Location $build
    try {
        & .\mkassets.exe 'face_src.jpg' 'face.png' 'icon.ico'
        if ($LASTEXITCODE -ne 0) { throw "mkassets: ошибка генерации" }
    }
    finally { Pop-Location }

    Copy-Item (Join-Path $build 'face.png') (Join-Path $root 'assets\face.png') -Force
    Copy-Item (Join-Path $build 'icon.ico') (Join-Path $root 'assets\icon.ico') -Force

    # --- 2. Ресурсы (иконка, портрет, манифест) -----------------------------
    Write-Host "[2/4] windres..."
    Copy-Item (Join-Path $root 'src\resource.rc')  (Join-Path $build 'resource.rc')  -Force
    Copy-Item (Join-Path $root 'src\app.manifest') (Join-Path $build 'app.manifest') -Force
    Push-Location $build
    try {
        & $windres -O coff -i resource.rc -o resource.o
        if ($LASTEXITCODE -ne 0) { throw "windres: ошибка компиляции ресурсов" }
    }
    finally { Pop-Location }

    # --- 3. Основная программа ----------------------------------------------
    Write-Host "[3/4] main.cpp..."
    & $gpp @common (Join-Path $root 'src\main.cpp') -c -o (Join-Path $build 'main.o')
    if ($LASTEXITCODE -ne 0) { throw "main.cpp: ошибка компиляции" }

    # --- 4. Линковка ---------------------------------------------------------
    Write-Host "[4/4] link..."
    $exe = Join-Path $root 'SamAltman.exe'
    & $gpp (Join-Path $build 'main.o') (Join-Path $build 'resource.o') -o $exe `
        @static -ld3d11 -ldxgi -lgdiplus -lgdi32 -luser32 -lshell32 -lshlwapi `
        -lole32 -loleaut32 -luuid -lwinmm -lpsapi -lshcore
    if ($LASTEXITCODE -ne 0) { throw "link: ошибка" }

    $size = [math]::Round((Get-Item $exe).Length / 1MB, 2)
    Write-Host ""
    Write-Host "Готово: $exe ($size МБ)" -ForegroundColor Green
}
finally {
    Pop-Location
}
