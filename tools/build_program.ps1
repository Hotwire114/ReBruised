# Full program build with llvm-mingw: runtime (source\ps2recomp, Tekken 4 patched tree) + generated game code in one
# exe, every runtime handler available (no prebuilt-runner export limits).
#   tools\build_program.ps1 [-Jobs 16] [-Configure] [-RuntimeOnly] [-NoRecomp]
param([int]$Jobs = 16, [switch]$Configure, [switch]$RuntimeOnly, [switch]$NoRecomp)
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
# llvm-mingw: BNB_TOOLCHAIN, else llvm-mingw\ in the project root (a folder or a link to one)
$tc = if ($env:BNB_TOOLCHAIN) { $env:BNB_TOOLCHAIN } else { "$root\llvm-mingw" }
if (-not (Test-Path "$tc\bin\clang++.exe")) { throw "llvm-mingw toolchain not found: $tc (set BNB_TOOLCHAIN or link llvm-mingw\)" }
$env:PATH = "$tc\bin;$env:PATH"
$env:BNB_FULL_RUNTIME = '1'
# raylib fix (window creation retried, no crash on a failed window): source\fetch is not tracked
cmd /c ".\python\python.exe -I tools\patch_raylib.py" ; if ($LASTEXITCODE) { throw 'patch_raylib' }
if (-not $RuntimeOnly -and -not $NoRecomp) {
    cmd /c ".\python\python.exe -I tools\find_missing_functions.py" ; if ($LASTEXITCODE) { throw 'find_missing_functions' }
    cmd /c ".\python\python.exe -I tools\make_config.py" ; if ($LASTEXITCODE) { throw 'make_config' }
    # recompile into build\generated.new, patch there, then copy only changed files into generated\ (incremental)
    if (Test-Path build\generated.new) { Remove-Item build\generated.new -Recurse -Force }
    New-Item -ItemType Directory build\generated.new | Out-Null
    (Get-Content config\bnb.toml -Raw) -replace '(?m)^output = .*$', 'output = "build/generated.new/"' | Set-Content config\bnb-program.toml -Encoding utf8
    cmd /c ".\build\host-tools\ps2xRecomp\ps2_recomp.exe config\bnb-program.toml > logs\recomp.log 2>&1" ; if ($LASTEXITCODE) { throw 'ps2_recomp' }
    Select-String -Path logs\recomp.log -Pattern 'Functions processed|Warnings:' | ForEach-Object { $_.Line }
    $env:BNB_GEN_DIR = "$root\build\generated.new"
    cmd /c ".\python\python.exe -I tools\patch_generated.py" ; if ($LASTEXITCODE) { throw 'patch_generated' }
    Remove-Item Env:BNB_GEN_DIR
    cmd /c ".\python\python.exe -I tools\sync_generated.py" ; if ($LASTEXITCODE) { throw 'sync_generated' }
    cmd /c ".\python\python.exe -I tools\make_batches.py" ; if ($LASTEXITCODE) { throw 'make_batches' }
}
# VU microprograms -> native C++ (T4 lifter, tools\vu\vu_lift.py) from the micro memory images in build\vucode\vu1|vu0
# (captured from the running game with PS2X_VU_CAPTURE=<dir>; game code, local only). BNB_VULIFT=0 skips it.
if (-not $RuntimeOnly -and $env:BNB_VULIFT -ne '0' -and (Test-Path build\vucode\vu1\*.bin)) {
    New-Item -ItemType Directory -Force build\vulift | Out-Null
    $vuArgs = '--vu1 build\vucode\vu1'
    if (Test-Path build\vucode\vu0\*.bin) { $vuArgs += ' --vu0 build\vucode\vu0' }
    cmd /c ".\python\python.exe -I tools\vu\vu_lift.py build\vulift.new $vuArgs --parts 4 --heads config\vulift_heads.txt --report build\vulift\vulift_report.txt" ; if ($LASTEXITCODE) { throw 'vu_lift' }
    foreach ($p in Get-ChildItem build\vulift.new\*.cpp) {   # copy only changed parts (incremental)
        $dst = "build\vulift\$($p.Name)"
        if (-not (Test-Path $dst) -or (Get-FileHash $p).Hash -ne (Get-FileHash $dst).Hash) { Copy-Item $p $dst -Force }
    }
    Remove-Item build\vulift.new -Recurse -Force
} elseif ($env:BNB_VULIFT -eq '0') { Remove-Item build\vulift\*.cpp -ErrorAction SilentlyContinue }
$out = "$root\build\program"
$fetch = ($root -replace '\\', '/') + '/source/fetch'
if ($Configure -or -not (Test-Path "$out\CMakeCache.txt")) {
    $deps = foreach ($d in 'raylib', 'imgui', 'rlimgui', 'elfio', 'toml11', 'fmt', 'libdwarf', 'rabbitizer', 'sse2neon', 'nlohmann_json') {
        "-DFETCHCONTENT_SOURCE_DIR_$($d.ToUpper())=$fetch/$d" }
    $ff = ($root -replace '\\','/') + '/build/ffmpeg-repack/ffmpeg-mingw.zip'
    cmd /c "cmake -S source\ps2recomp -B `"$out`" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DPS2X_BUILD_RECOMP=OFF -DPS2X_BUILD_ANALYZER=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF -DPS2X_ENABLE_SCCACHE=OFF -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF -DPS2X_ENABLE_DEBUG_UI=ON -DPS2X_GAME_DLL=OFF -DPS2X_ENABLE_FFMPEG=ON -DFETCHCONTENT_FULLY_DISCONNECTED=ON `"-DFFMPEG_PREBUILT_URL=$ff`" `"-DCMAKE_PROJECT_INCLUDE=$($root -replace '\\','/')/port/bnb_project_include.cmake`" $($deps -join ' ') > logs\program-configure.log 2>&1"
    if ($LASTEXITCODE) { Get-Content logs\program-configure.log -Tail 30; throw 'configure failed (logs\program-configure.log)' }
}
$target = if ($RuntimeOnly) { 'ps2_runtime' } else { 'BlackAndBruised BlackAndBruisedLauncher' }
cmd /c "cmake --build `"$out`" --target $target -j $Jobs > logs\program-build.log 2>&1"
$code = $LASTEXITCODE
Get-Content logs\program-build.log | Select-String -Pattern 'error|FAILED' | Select-Object -First 25 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(300, $_.Line.Length)) }
Get-Content logs\program-build.log -Tail 3
if ($code) { throw "build failed (logs\program-build.log)" }

