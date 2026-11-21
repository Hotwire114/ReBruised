# find_missing_functions -> config merge -> ps2_recomp -> ps2_game.dll. Run from anywhere.
param([int]$Jobs = 8)
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
cmd /c ".\python\python.exe -I tools\find_missing_functions.py"
if ($LASTEXITCODE) { throw 'find_missing_functions failed' }
cmd /c ".\python\python.exe -I tools\make_config.py"
if ($LASTEXITCODE) { throw 'make_config failed' }
if (Test-Path generated) { Remove-Item generated -Recurse -Force }
New-Item -ItemType Directory generated | Out-Null
cmd /c ".\bin\tools\ps2_recomp.exe config\bnb.toml > logs\recomp.log 2>&1"
if ($LASTEXITCODE) { throw 'ps2_recomp failed (logs\recomp.log)' }
Select-String -Path logs\recomp.log -Pattern 'Functions processed|Warnings:' | ForEach-Object { $_.Line }
cmd /c ".\python\python.exe -I tools\patch_generated.py"
if ($LASTEXITCODE) { throw 'patch_generated failed' }
cmd /c ".\python\python.exe -I tools\build_game_dll.py --tree source\ps2recomp --generated generated --out bin\runtime\ps2_game.dll --jobs $Jobs > logs\gamedll.log 2>&1"
$code = $LASTEXITCODE
Get-Content logs\gamedll.log -Tail 4
if ($code) { throw 'ps2_game.dll build failed (logs\gamedll.log)' }
