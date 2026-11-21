# Run the game under lldb (llvm-mingw); on a crash print backtraces of all threads to logs\lldb-<label>.txt.
#   tools\debug_run.ps1 -Label d1 [-Pad "t:mask:dur;..."] [-Seconds 300]
param([string]$Label = 'dbg', [string]$Pad = '', [int]$Seconds = 300)
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$tc = if ($env:BNB_TOOLCHAIN) { "$env:BNB_TOOLCHAIN\bin" } else { "$root\llvm-mingw\bin" }
if ($Pad) { $env:BNB_PAD_SCRIPT = $Pad } else { Remove-Item Env:BNB_PAD_SCRIPT -ErrorAction SilentlyContinue }
if (-not $env:PS2X_GS_BACKEND) { $env:PS2X_GS_BACKEND = 'gl' }
$exe = "$root\build\program\bin\BlackAndBruised.exe"
$elf = "$root\game\disc\SLES_516.20"
$cmds = "$root\logs\lldb-$Label.cmds"
@"
settings set target.inherit-env true
process launch -o $root\logs\run-$Label.log -e $root\logs\run-$Label.err -- $elf
"@ | Set-Content $cmds -Encoding ascii
$crash = "$root\logs\lldb-$Label.crash"
@"
thread backtrace all -c 25
register read
quit
"@ | Set-Content $crash -Encoding ascii
$p = Start-Process -FilePath "$tc\lldb.exe" -ArgumentList "-b", "-s", "`"$cmds`"", "-K", "`"$crash`"", "`"$exe`"" `
    -RedirectStandardOutput "$root\logs\lldb-$Label.txt" -RedirectStandardError "$root\logs\lldb-$Label.err.txt" -PassThru -NoNewWindow
$deadline = (Get-Date).AddSeconds($Seconds)
while (-not $p.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep 2 }
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Get-Process BlackAndBruised -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
"lldb finished; output logs\lldb-$Label.txt"
