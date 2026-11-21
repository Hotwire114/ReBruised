# Per-thread CPU load of the running game (which thread limits the frame rate): samples every thread's CPU time
# twice, -Window seconds apart, and prints the busiest as % of one core, with the thread start address
# (module+offset; symbolize with llvm-symbolizer like [bnb-crash] stacks).
#   tools\thread_load.ps1 [-Window 5] [-Top 8]
param([double]$Window = 5, [int]$Top = 8)
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class TL {
  [DllImport("kernel32.dll")] public static extern IntPtr OpenThread(int access, bool inherit, int id);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] public static extern int GetThreadDescription(IntPtr h, out IntPtr desc);
  [DllImport("kernel32.dll")] public static extern IntPtr LocalFree(IntPtr p);
  [DllImport("ntdll.dll")] public static extern int NtQueryInformationThread(IntPtr h, int cls, out IntPtr info, int len, IntPtr ret);
  public static long Start(int id) {
    IntPtr h = OpenThread(0x0040, false, id); if (h == IntPtr.Zero) return 0;
    IntPtr a; int r = NtQueryInformationThread(h, 9, out a, IntPtr.Size, IntPtr.Zero); CloseHandle(h);
    return r == 0 ? a.ToInt64() : 0;
  }
  public static string Name(int id) {
    IntPtr h = OpenThread(0x1000, false, id); if (h == IntPtr.Zero) return "";
    IntPtr p; string s = "";
    if (GetThreadDescription(h, out p) >= 0 && p != IntPtr.Zero) { s = Marshal.PtrToStringUni(p); LocalFree(p); }
    CloseHandle(h); return s;
  }
}
"@
$p = Get-Process BlackAndBruised -ErrorAction Stop
$base = $p.MainModule.BaseAddress.ToInt64()
$mainId = ($p.Threads | Sort-Object StartTime | Select-Object -First 1).Id
$a = @{}; foreach ($t in $p.Threads) { try { $a[$t.Id] = $t.TotalProcessorTime.TotalMilliseconds } catch {} }
Start-Sleep -Milliseconds ([int]($Window * 1000))
$p.Refresh()
$rows = foreach ($t in $p.Threads) {
    try {
        $ms = $t.TotalProcessorTime.TotalMilliseconds - ($a[$t.Id] -as [double])
        $start = [TL]::Start($t.Id)
        [pscustomobject]@{ Id = $t.Id; Load = [math]::Round(100 * $ms / ($Window * 1000), 1); Name = [TL]::Name($t.Id); Main = ($t.Id -eq $mainId)
            Start = if ($start -ge $base -and $start -lt $base + $p.MainModule.ModuleMemorySize) { 'exe+0x{0:x}' -f ($start - $base) } else { '0x{0:x}' -f $start } }
    } catch {}
}
$rows | Sort-Object Load -Descending | Select-Object -First $Top | Format-Table -AutoSize | Out-String -Width 200
