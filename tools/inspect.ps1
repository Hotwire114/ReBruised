# Decompile the functions containing the given addresses (headless Ghidra, saved BNB project, no re-analysis).
#   tools\inspect.ps1 out.txt 0x13bdd4 0x13befc ...
#   tools\inspect.ps1 -List out.txt 0x13bdd4        disassembly listing instead (or a range 0xA-0xB)
param([switch]$List, [Parameter(Mandatory)][string]$Out, [Parameter(ValueFromRemainingArguments)][string[]]$Addresses)
$script = if ($List) { 'ListAddress.java' } else { 'InspectAddress.java' }
$root = Split-Path -Parent $PSScriptRoot
# JDK 21 for Ghidra: JAVA_HOME, else jdk\ in the project root (a folder or a link)
if (-not $env:JAVA_HOME) { $env:JAVA_HOME = "$root\jdk" }
$outPath = if ([IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $root $Out }
$args2 = @("$root\ghidra-project", 'BNB', '-process', 'SLES_516.20', '-noanalysis', '-readOnly',
           '-scriptPath', "$root\tools\ghidra_scripts", '-postScript', $script, $outPath) + $Addresses
& 'C:\Recomp\tools\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat' @args2 *> "$root\logs\inspect.log"
Get-Content $outPath
