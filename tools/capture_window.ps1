# Start a program, wait for its window with the given title, capture it with PrintWindow (works when other windows
# cover it), close the program.
#   tools\capture_window.ps1 -Exe python\pythonw.exe -Arguments "-I","tools\launcher.pyw" -Title "Black & Bruised" -Out logs\launcher.png
param([Parameter(Mandatory)][string]$Exe, [string[]]$Arguments = @(), [Parameter(Mandatory)][string]$Title,
      [Parameter(Mandatory)][string]$Out, [int]$DelayMs = 900)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public static class CapWin {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
  public static IntPtr Find(uint pid, string title) { IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid && IsWindowVisible(h)) {
      var s = new StringBuilder(256); GetWindowText(h, s, 256); if (s.ToString() == title) { found = h; return false; } } return true; }, IntPtr.Zero);
    return found; }
}
"@
$p = if ($Arguments.Count) { Start-Process $Exe -ArgumentList $Arguments -PassThru } else { Start-Process $Exe -PassThru }
try {
    $h = [IntPtr]::Zero
    for ($i = 0; $i -lt 60 -and $h -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 250; $h = [CapWin]::Find($p.Id, $Title) }
    if ($h -eq [IntPtr]::Zero) { throw "window '$Title' not found" }
    Start-Sleep -Milliseconds $DelayMs
    $r = New-Object CapWin+RECT; [CapWin]::GetWindowRect($h, [ref]$r) | Out-Null
    $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
    $g = [System.Drawing.Graphics]::FromImage($bmp); $hdc = $g.GetHdc()
    [CapWin]::PrintWindow($h, $hdc, 2) | Out-Null
    $g.ReleaseHdc($hdc); $g.Dispose()
    $bmp.Save((Join-Path (Get-Location) $Out)); $bmp.Dispose()
    "saved $Out ($($r.R - $r.L)x$($r.B - $r.T))"
} finally { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
