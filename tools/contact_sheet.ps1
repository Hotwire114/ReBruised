param([string]$Dir, [string]$Out, [int]$Skip = 0, [int]$Take = 12, [int]$Cols = 4, [int]$W = 480)
Add-Type -AssemblyName System.Drawing
$files = Get-ChildItem "$Dir\*.png" | Where-Object { $_.LastWriteTime -gt (Get-Date).AddHours(-3) } | Sort-Object Name | Select-Object -Skip $Skip -First $Take
$first = [System.Drawing.Image]::FromFile($files[0].FullName); $H = [int]($W * $first.Height / $first.Width); $first.Dispose()
$rows = [Math]::Ceiling($files.Count / $Cols)
$bmp = New-Object System.Drawing.Bitmap ($W * $Cols), ($H * $rows)
$g = [System.Drawing.Graphics]::FromImage($bmp); $g.InterpolationMode = 'HighQualityBicubic'
$font = New-Object System.Drawing.Font 'Arial', 11
$i = 0
foreach ($f in $files) {
  $im = [System.Drawing.Image]::FromFile($f.FullName)
  $x = ($i % $Cols) * $W; $y = [Math]::Floor($i / $Cols) * $H
  $g.DrawImage($im, $x, $y, $W, $H); $g.DrawString($f.Name, $font, [System.Drawing.Brushes]::Yellow, $x + 4, $y + 4)
  $im.Dispose(); $i++
}
$bmp.Save($Out); "saved $Out"

