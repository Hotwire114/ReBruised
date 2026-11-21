# Builds PS2Recomp's host tools (ps2_analyzer, ps2_recomp) from source\ps2recomp with the llvm-mingw toolchain
# used by the Road Rash project. No network access: every FetchContent dependency points at source\fetch.
#   powershell -File tools\build_host_tools.ps1 -Targets ps2_recomp,ps2_analyzer
param([int]$Jobs = 4, [string]$Targets = 'ps2_recomp,ps2_analyzer')
$targetList = @($Targets -split ',')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$toolchain = if ($env:BNB_TOOLCHAIN) { $env:BNB_TOOLCHAIN } else { "$root\llvm-mingw" }
if (-not (Test-Path "$toolchain\bin\clang++.exe")) { throw "llvm-mingw toolchain not found: $toolchain (set BNB_TOOLCHAIN)" }
$env:PATH = "$toolchain\bin;$env:PATH"
$src = "$root\source\ps2recomp"
$out = "$root\build\host-tools"
$fetch = "$root\source\fetch"
$deps = @()
foreach ($d in 'elfio','toml11','fmt','libdwarf','rabbitizer','nlohmann_json') {
    if (-not (Test-Path "$fetch\$d")) { throw "missing dependency source: $fetch\$d" }
    $deps += "-DFETCHCONTENT_SOURCE_DIR_$($d.ToUpper())=$($fetch -replace '\\','/')/$d"
}
& cmake -S $src -B $out -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ `
    -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF `
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON @deps
if ($LASTEXITCODE) { throw 'CMake configure failed' }
& cmake --build $out --target @targetList -j $Jobs
if ($LASTEXITCODE) { throw 'Host tools build failed' }
Write-Host "Built: $($targetList -join ', ') in $out"
