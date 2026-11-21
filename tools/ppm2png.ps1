# Convert binary PPM (P6) files to PNG (texture / render-target dumps).   tools\ppm2png.ps1 logs\texdump\*.ppm
param([Parameter(Mandatory, ValueFromRemainingArguments)][string[]]$Paths)
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing @"
using System; using System.Drawing; using System.Drawing.Imaging; using System.IO; using System.Text;
public static class Ppm {
  public static void ToPng(string src, string dst) {
    byte[] d = File.ReadAllBytes(src); int pos = 0; string[] tok = new string[4];
    for (int t = 0; t < 4; t++) {
      while (char.IsWhiteSpace((char)d[pos])) pos++;
      var sb = new StringBuilder(); while (!char.IsWhiteSpace((char)d[pos])) sb.Append((char)d[pos++]);
      tok[t] = sb.ToString();
    }
    pos++;
    int w = int.Parse(tok[1]), h = int.Parse(tok[2]);
    var bmp = new Bitmap(w, h, PixelFormat.Format24bppRgb);
    var bd = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format24bppRgb);
    var row = new byte[bd.Stride];
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) { int s = pos + (y * w + x) * 3; row[x*3] = d[s+2]; row[x*3+1] = d[s+1]; row[x*3+2] = d[s]; }
      System.Runtime.InteropServices.Marshal.Copy(row, 0, bd.Scan0 + y * bd.Stride, bd.Stride);
    }
    bmp.UnlockBits(bd); bmp.Save(dst, ImageFormat.Png); bmp.Dispose();
  }
}
"@
foreach ($p in $Paths) { foreach ($f in Get-ChildItem $p) { $out = [IO.Path]::ChangeExtension($f.FullName, '.png'); [Ppm]::ToPng($f.FullName, $out); $out } }
