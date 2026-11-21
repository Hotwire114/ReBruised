# Run the game and capture its window every few seconds (desktop capture of the window rectangle).
#   tools\shots.ps1 -Seconds 90 -Every 10 -Label b5 [-Pad "t:button:dur;..."]
# Shots: logs\shots-<label>\NNN.png ; logs: logs\run-<label>.log/.err
#   -BurstAt s -BurstCount n [-BurstMs 50]: n extra captures BurstMs apart from s seconds on (flicker hunting).
#   -Size "w,h": resize the window once (client area follows; detail checks at the internal resolution).
#   -Click "x,y" posts a left click (client coordinates) to the window before each shot (debug panel tabs).
param([int]$Seconds = 60, [int]$Every = 10, [int]$BurstAt = 0, [int]$BurstCount = 0, [int]$BurstMs = 50, [string]$Label = 'shots', [string]$Pad = '', [string]$Click = '', [string]$Size = '', [int]$KeyVk = 0, [int]$KeyScan = 0,
      [switch]$ScreenCapture)
$root = Split-Path -Parent $PSScriptRoot
$exe = if ($env:BNB_EXE) { $env:BNB_EXE } else { "$root\build\program\bin\BlackAndBruised.exe" }
Set-Location $root
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
}
"@
if ($Pad) { $env:BNB_PAD_SCRIPT = $Pad } else { Remove-Item Env:BNB_PAD_SCRIPT -ErrorAction SilentlyContinue }
$env:PS2X_GS_FRAME_DUMP = (Resolve-Path logs).Path + "\shots-$Label"
if (-not $env:PS2X_GS_BACKEND) { $env:PS2X_GS_BACKEND = 'gl' }
if (-not $env:PS2X_GS_THREADS) { $env:PS2X_GS_THREADS = 'auto' }
$dir = "logs\shots-$Label"; New-Item -ItemType Directory -Force $dir | Out-Null
$p = Start-Process -FilePath $exe -ArgumentList "`"$root\game\disc\SLES_516.20`"" `
    -RedirectStandardOutput "logs\run-$Label.log" -RedirectStandardError "logs\run-$Label.err" -PassThru
$start = Get-Date; $n = 0
while (-not $p.HasExited -and ((Get-Date) - $start).TotalSeconds -lt $Seconds) { try {
    $elapsed = ((Get-Date) - $start).TotalSeconds
    if ($BurstCount -gt 0 -and $elapsed -ge $BurstAt -and -not $burstDone) { $burstLeft = $BurstCount; $burstDone = $true }
    if ($burstLeft -gt 0) { Start-Sleep -Milliseconds $BurstMs; $burstLeft-- } else { Start-Sleep -Seconds $Every }
    $p.Refresh()
    $h = $p.MainWindowHandle
    if ($h -and $h -ne [IntPtr]::Zero) { $gameWindow = $h } elseif ($gameWindow) { $h = $gameWindow }
    if (-not $h -or $h -eq [IntPtr]::Zero) { continue }
    # HWND_TOPMOST, SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW: z-order works across processes where foreground does not
    if ($burstLeft -le 0 -or -not $topDone) { [W]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, 0x43) | Out-Null; Start-Sleep -Milliseconds 300; $topDone = $burstLeft -gt 0 }
    if ($Size -and -not $sized) { $wh = $Size.Split(','); [W]::SetWindowPos($h, [IntPtr](-1), 0, 0, [int]$wh[0], [int]$wh[1], 0x40) | Out-Null; $sized = $true; Start-Sleep -Milliseconds 500 }
    if ($KeyVk -and $burstLeft -gt 0) { [W]::PostMessage($h, 0x0100, [IntPtr]$KeyVk, [IntPtr](1 -bor ($KeyScan -shl 16))) | Out-Null; Start-Sleep -Milliseconds 40; [W]::PostMessage($h, 0x0101, [IntPtr]$KeyVk, [IntPtr]([int64]0xC0000001 -bor ($KeyScan -shl 16))) | Out-Null; Start-Sleep -Milliseconds 120 } # KeyEvery: press a key before each burst capture
    if ($Click) {
        $xy = $Click.Split(','); $lp = [IntPtr](([int]$xy[1] -shl 16) -bor ([int]$xy[0] -band 0xFFFF))
        [W]::PostMessage($h, 0x0200, [IntPtr]0, $lp) | Out-Null; Start-Sleep -Milliseconds 120   # WM_MOUSEMOVE
        [W]::PostMessage($h, 0x0201, [IntPtr]1, $lp) | Out-Null; Start-Sleep -Milliseconds 120   # WM_LBUTTONDOWN
        [W]::PostMessage($h, 0x0202, [IntPtr]0, $lp) | Out-Null; Start-Sleep -Milliseconds 400   # WM_LBUTTONUP
    }
    $r = New-Object W+RECT; [W]::GetClientRect($h, [ref]$r) | Out-Null
    $pt = New-Object W+POINT; [W]::ClientToScreen($h, [ref]$pt) | Out-Null
    $w = $r.R - $r.L; $hh = $r.B - $r.T
    if ($w -le 0 -or $hh -le 0) { continue }
    # The window's own picture (PrintWindow, PW_CLIENTONLY | PW_RENDERFULLCONTENT): a desktop capture also caught any
    # window lying over the game (white bands from a window on top, 2026-11-16). -ScreenCapture: the old way.
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    if ($ScreenCapture) { $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size) }
    else { $hdc = $g.GetHdc(); [W]::PrintWindow($h, $hdc, 3) | Out-Null; $g.ReleaseHdc($hdc) }
    $g.Dispose()
    $t = [int]((Get-Date) - $start).TotalSeconds
    $ms = [int]((Get-Date) - $start).TotalMilliseconds; $file = Join-Path $dir ("{0:D6}ms.png" -f $ms); $bmp.Save($file); $bmp.Dispose(); $n++
} catch { }
}
$alive = -not $p.HasExited
if ($alive) { Stop-Process -Id $p.Id -Force }
# leftover game processes (a killed run can leave one holding the logs and the DLLs)
if (-not $env:BNB_SHOTS_KEEP_OTHERS) { Get-Process BlackAndBruised, ps2EntryRunner -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue }
"alive at end: $alive; $n shots in $dir"

