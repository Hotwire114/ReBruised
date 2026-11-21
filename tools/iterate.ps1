# Fix-up loop: run (with screenshots) -> collect missing targets -> rebuild, until a run finds nothing new.
#   tools\iterate.ps1 [-Rounds 8] [-Seconds 50] [-Pad "t:mask:dur;..."]
param([int]$Rounds = 8, [int]$Seconds = 50, [string]$Pad = '', [string]$Prefix = 'it')
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
for ($r = 1; $r -le $Rounds; $r++) {
    $label = "$Prefix$r"
    & "$root\tools\shots.ps1" -Seconds $Seconds -Every 10 -Label $label -Pad $Pad | Out-Null
    $out = & .\python\python.exe -I tools\collect_missing.py "logs\run-$label.err"
    $out | ForEach-Object { "[$label] $_" }
    $sizes = (Get-ChildItem "logs\shots-$label" -Filter *.png | ForEach-Object { $_.Length }) -join ','
    "[$label] shot sizes: $sizes"
    if (-not ($out -match 'added ')) { "[$label] nothing new to add"; break }
    & "$root\tools\build_program.ps1" -Jobs 20 2>&1 | Select-Object -Last 1 | ForEach-Object { "[$label] $_" }
}
