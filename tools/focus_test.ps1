# Alt+Tab check: start the game in a fullscreen mode, then switch to another window the way Alt+Tab does and report
# which window has focus and whether the game window is minimized / still covering the screen.
#   tools\focus_test.ps1 -Mode exclusive|borderless
param([string]$Mode = 'exclusive', [int]$Wait = 12)
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public static class FT {
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool IsHungAppWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h, uint m, IntPtr w, IntPtr l, uint f, uint t, out IntPtr r);
  public static bool Responds(IntPtr h) { IntPtr r; return SendMessageTimeout(h, 0, IntPtr.Zero, IntPtr.Zero, 2, 2000, out r) != IntPtr.Zero; }
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  public static string Title(IntPtr h) { var s = new StringBuilder(256); GetWindowText(h, s, 256); return s.ToString(); }
}
"@
Get-Process BlackAndBruised -ErrorAction SilentlyContinue | Stop-Process -Force
$env:TEKKEN4_FULLSCREEN = $Mode
$g = Start-Process build\program\bin\BlackAndBruised.exe -ArgumentList "`"$root\game\disc\SLES_516.20`"", "`"$root\game\BnB.iso`"" -RedirectStandardOutput logs\run-focus.log -RedirectStandardError logs\run-focus.err -PassThru
Remove-Item Env:TEKKEN4_FULLSCREEN
Start-Sleep -Seconds $Wait
$g.Refresh(); $gh = $g.MainWindowHandle
# give the game the focus first (an Alt press lets this process move the foreground)
[FT]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [FT]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero); [FT]::ShowWindow($gh, 9) | Out-Null; [FT]::SetForegroundWindow($gh) | Out-Null; Start-Sleep -Seconds 5
"game window: '$([FT]::Title($gh))' foreground=$([FT]::GetForegroundWindow() -eq $gh) iconic=$([FT]::IsIconic($gh))"
# Alt+Tab: Alt down, Tab, Alt up
[FT]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80
[FT]::keybd_event(0x09, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80
[FT]::keybd_event(0x09, 0, 2, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80
[FT]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
Start-Sleep -Seconds 2
$fg = [FT]::GetForegroundWindow()
"after Alt+Tab: foreground='$([FT]::Title($fg))' gameForeground=$($fg -eq $gh) gameIconic=$([FT]::IsIconic($gh))"
foreach ($i in 1..4) { Start-Sleep -Seconds 3; $g.Refresh(); "away $($i*3) s: alive=$(-not $g.HasExited) responds=$([FT]::Responds($gh)) hung=$([FT]::IsHungAppWindow($gh))" }
if (-not $g.HasExited) {
    [FT]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [FT]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero); [FT]::ShowWindow($gh, 9) | Out-Null; [FT]::SetForegroundWindow($gh) | Out-Null
    Start-Sleep -Seconds 4
    $g.Refresh(); "after coming back: alive=$(-not $g.HasExited) foreground=$([FT]::GetForegroundWindow() -eq $gh) iconic=$([FT]::IsIconic($gh))"
    foreach ($i in 1..4) { Start-Sleep -Seconds 3; $g.Refresh(); if ($g.HasExited) { break }; "back $($i*3) s: responds=$([FT]::Responds($gh)) hung=$([FT]::IsHungAppWindow($gh)) iconic=$([FT]::IsIconic($gh))" }
}
if (-not $g.HasExited) { Stop-Process -Id $g.Id -Force } else { "exit code $($g.ExitCode)" }
