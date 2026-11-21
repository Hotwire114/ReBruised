# Sampling profiler for one thread of the running game: suspends it ~every millisecond, reads RIP, resumes; then
# symbolizes the hottest addresses (llvm-symbolizer from the toolchain) and sums them per function.
#   tools\sample_thread.ps1 [-ThreadId <id>] [-Seconds 5] [-Top 25]     (no -ThreadId: the busiest thread)
param([int]$ThreadId = 0, [double]$Seconds = 5, [int]$Top = 25)
$root = Split-Path -Parent $PSScriptRoot
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Collections.Generic; using System.Diagnostics;
public static class TS {
  [DllImport("kernel32.dll")] static extern IntPtr OpenThread(int access, bool inherit, int id);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32.dll")] static extern int SuspendThread(IntPtr h);
  [DllImport("kernel32.dll")] static extern int ResumeThread(IntPtr h);
  [DllImport("kernel32.dll")] static extern bool GetThreadContext(IntPtr h, IntPtr ctx);
  [DllImport("winmm.dll")] static extern int timeBeginPeriod(int ms);
  public static Dictionary<long, int> Sample(int id, double seconds) {
    var counts = new Dictionary<long, int>();
    IntPtr h = OpenThread(0x0002 | 0x0008, false, id);   // SUSPEND_RESUME | GET_CONTEXT
    if (h == IntPtr.Zero) throw new Exception("OpenThread failed");
    IntPtr raw = Marshal.AllocHGlobal(1232 + 16);
    IntPtr ctx = new IntPtr((raw.ToInt64() + 15) & ~15L);
    timeBeginPeriod(1);
    var sw = Stopwatch.StartNew();
    while (sw.Elapsed.TotalSeconds < seconds) {
      for (int i = 0; i < 1232; i += 8) Marshal.WriteInt64(ctx, i, 0);
      Marshal.WriteInt32(ctx, 0x30, 0x100001);               // ContextFlags = CONTEXT_AMD64 | CONTEXT_CONTROL
      if (SuspendThread(h) >= 0) {
        if (GetThreadContext(h, ctx)) {
          long rip = Marshal.ReadInt64(ctx, 0xF8);
          int c; counts.TryGetValue(rip, out c); counts[rip] = c + 1;
        }
        ResumeThread(h);
      }
      System.Threading.Thread.Sleep(1);
    }
    Marshal.FreeHGlobal(raw); CloseHandle(h);
    return counts;
  }
}
"@
$p = Get-Process BlackAndBruised -ErrorAction Stop
if (-not $ThreadId) {
    $a = @{}; foreach ($t in $p.Threads) { try { $a[$t.Id] = $t.TotalProcessorTime.TotalMilliseconds } catch {} }
    Start-Sleep -Milliseconds 1500; $p.Refresh()
    $ThreadId = ($p.Threads | Sort-Object { try { $_.TotalProcessorTime.TotalMilliseconds - $a[$_.Id] } catch { 0 } } -Descending | Select-Object -First 1).Id
}
$counts = [TS]::Sample($ThreadId, $Seconds)
$total = ($counts.Values | Measure-Object -Sum).Sum
$exe = $p.MainModule.FileName; $base = $p.MainModule.BaseAddress.ToInt64(); $size = $p.MainModule.ModuleMemorySize
$mods = $p.Modules | ForEach-Object { [pscustomobject]@{ Name = $_.ModuleName; Base = $_.BaseAddress.ToInt64(); End = $_.BaseAddress.ToInt64() + $_.ModuleMemorySize } }
$inExe = @{}; $other = @{}
foreach ($kv in $counts.GetEnumerator()) {
    if ($kv.Key -ge $base -and $kv.Key -lt $base + $size) { $inExe[$kv.Key - $base] = $kv.Value }
    else {
        $m = $mods | Where-Object { $kv.Key -ge $_.Base -and $kv.Key -lt $_.End } | Select-Object -First 1
        $n = if ($m) { $m.Name } else { '?' }; $other[$n] = ($other[$n] -as [int]) + $kv.Value
    }
}
# symbolize exe addresses (image base 0x140000000) in one llvm-symbolizer call
$sym = $null; foreach ($c in @("$env:BNB_TOOLCHAIN\bin\llvm-symbolizer.exe", "$(Split-Path -Parent $PSScriptRoot)\llvm-mingw\bin\llvm-symbolizer.exe")) { if (Test-Path $c) { $sym = $c; break } }
$byFn = @{}
$keys = @($inExe.Keys)
if ($sym -and $keys.Count) {
    $in = ($keys | ForEach-Object { '0x{0:x}' -f (0x140000000 + $_) }) -join "`n"
    $out = $in | & $sym --obj=$exe --functions=linkage --no-inlines --demangle 2>$null
    $names = @($out | Where-Object { $_ -ne '' } | Select-Object -Index (0..($keys.Count * 2) | Where-Object { $_ % 2 -eq 0 }))
    for ($i = 0; $i -lt $keys.Count; $i++) { $n = if ($i -lt $names.Count) { $names[$i] } else { '?' }; $byFn[$n] = ($byFn[$n] -as [int]) + $inExe[$keys[$i]] }
}
"thread $ThreadId : $total samples in $Seconds s"
$rows = @($byFn.GetEnumerator() | ForEach-Object { [pscustomobject]@{ Pct = [math]::Round(100 * $_.Value / $total, 1); Where = $_.Key } }) +
        @($other.GetEnumerator() | ForEach-Object { [pscustomobject]@{ Pct = [math]::Round(100 * $_.Value / $total, 1); Where = "[$($_.Key)]" } })
$rows | Sort-Object Pct -Descending | Select-Object -First $Top | Format-Table -AutoSize | Out-String -Width 250
