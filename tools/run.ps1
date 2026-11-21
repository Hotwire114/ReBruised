# Timed test run: starts the game, waits, closes it, prints the interesting log lines.
#   tools\run.ps1 [-Seconds 40] [-Label name]      logs go to logs\run-<label>.log / .err
param([int]$Seconds = 40, [string]$Label = 'last')
$root = Split-Path -Parent $PSScriptRoot
$exe = if ($env:BNB_EXE) { $env:BNB_EXE } else { "$root\build\program\bin\BlackAndBruised.exe" }
Set-Location $root
if (-not $env:PS2X_GS_BACKEND) { $env:PS2X_GS_BACKEND = 'gl' }
if (-not $env:PS2X_GS_THREADS) { $env:PS2X_GS_THREADS = 'auto' }
$out = "logs\run-$Label.log"; $err = "logs\run-$Label.err"
$p = Start-Process -FilePath $exe -ArgumentList "`"$root\game\disc\SLES_516.20`"" `
    -RedirectStandardOutput $out -RedirectStandardError $err -PassThru
$deadline = (Get-Date).AddSeconds($Seconds)
while (-not $p.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
$alive = -not $p.HasExited
if ($alive) { Stop-Process -Id $p.Id -Force }
# leftover game processes (a killed run can leave one holding the logs and the DLLs)
Get-Process BlackAndBruised, ps2EntryRunner -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
"alive at end: $alive  exit: $(if ($alive) { 'killed' } else { $p.ExitCode })"
$pattern = 'missing-target|missing function|unhandled|exception|crash|error|fatal|assert|\[iop\]|loadmodule|LoadModule|sceSif|warn'
Get-Content $out, $err | Select-String -Pattern $pattern | Select-Object -First 40 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(400, $_.Line.Length)) }
"--- last lines of $err"
Get-Content $err -Tail 8 | ForEach-Object { $_.Substring(0, [Math]::Min(400, $_.Length)) }

