# Brand assets from the ReBruised logo (our own artwork): a transparent logo PNG for the launcher and the app icon.
#   tools\make_brand_assets.ps1 [-Source port\launcher\assets\rebruised-logo.png] [-IconCrop "80,140,520,740"]
# Writes port\launcher\assets\rebruised-logo-alpha.png, rebruised-icon-256.png and port\launcher\assets\rebruised.ico.
param([string]$Source = 'port\launcher\assets\rebruised-logo.png', [string]$IconCrop = '')
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing @"
using System; using System.Drawing; using System.Drawing.Imaging; using System.Collections.Generic; using System.IO;
public static class Brand {
  // white outside the logo (flood fill from the border) -> transparent; the anti-aliased rim gets partial alpha
  public static Bitmap KeyWhite(Bitmap src) {
    int w = src.Width, h = src.Height;
    var bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb);
    using (var g = Graphics.FromImage(bmp)) g.DrawImage(src, 0, 0, w, h);
    var data = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadWrite, PixelFormat.Format32bppArgb);
    var px = new byte[w * h * 4];
    System.Runtime.InteropServices.Marshal.Copy(data.Scan0, px, 0, px.Length);
    Func<int,int> minc = i => Math.Min(px[i*4], Math.Min(px[i*4+1], px[i*4+2]));
    var outside = new bool[w * h];
    var q = new Queue<int>();
    for (int x = 0; x < w; x++) { q.Enqueue(x); q.Enqueue((h-1)*w + x); }
    for (int y = 0; y < h; y++) { q.Enqueue(y*w); q.Enqueue(y*w + w-1); }
    while (q.Count > 0) {
      int i = q.Dequeue();
      if (outside[i] || minc(i) < 246) continue;
      outside[i] = true;
      int x = i % w, y = i / w;
      if (x > 0) q.Enqueue(i-1); if (x < w-1) q.Enqueue(i+1); if (y > 0) q.Enqueue(i-w); if (y < h-1) q.Enqueue(i+w);
    }
    for (int i = 0; i < w*h; i++) {
      if (outside[i]) { px[i*4+3] = 0; continue; }
      int x = i % w, y = i / w; bool edge = false;
      for (int dy = -2; dy <= 2 && !edge; dy++) for (int dx = -2; dx <= 2 && !edge; dx++) {
        int xx = x+dx, yy = y+dy; if (xx>=0 && yy>=0 && xx<w && yy<h && outside[yy*w+xx]) edge = true; }
      if (edge) { int m = minc(i); double a = Math.Min(1.0, Math.Max(0.0, (255.0 - m) / 30.0)); px[i*4+3] = (byte)(a*255); }
    }
    System.Runtime.InteropServices.Marshal.Copy(px, 0, data.Scan0, px.Length);
    bmp.UnlockBits(data);
    return bmp;
  }
  public static Bitmap Fit(Bitmap src, Rectangle crop, int size, int pad) {
    var dst = new Bitmap(size, size, PixelFormat.Format32bppArgb);
    using (var g = Graphics.FromImage(dst)) {
      g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
      g.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.HighQuality;
      g.CompositingQuality = System.Drawing.Drawing2D.CompositingQuality.HighQuality;
      double s = Math.Min((size - 2.0*pad) / crop.Width, (size - 2.0*pad) / crop.Height);
      int dw = (int)Math.Round(crop.Width * s), dh = (int)Math.Round(crop.Height * s);
      g.DrawImage(src, new Rectangle((size-dw)/2, (size-dh)/2, dw, dh), crop, GraphicsUnit.Pixel);
    }
    return dst;
  }
  static System.Drawing.Drawing2D.GraphicsPath Round(RectangleF r, float rad) {
    var p = new System.Drawing.Drawing2D.GraphicsPath(); float d = rad * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90); p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
    p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90); p.AddArc(r.X, r.Bottom - d, d, d, 90, 90); p.CloseFigure(); return p;
  }
  // the app icon in the logo's style: orange badge, black outline, cream rim, the bandage cross
  public static Bitmap Badge(int S) {
    var b = new Bitmap(S, S, PixelFormat.Format32bppArgb);
    using (var g = Graphics.FromImage(b)) {
      g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
      g.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.HighQuality;
      Color cream = Color.FromArgb(255, 252, 240, 224), ink = Color.FromArgb(255, 20, 14, 10), orange = Color.FromArgb(255, 255, 106, 0);
      float m = S * 0.02f;
      using (var p = Round(new RectangleF(m, m, S - 2*m, S - 2*m), S * 0.24f)) g.FillPath(new SolidBrush(cream), p);
      float m2 = S * 0.065f;
      using (var p = Round(new RectangleF(m2, m2, S - 2*m2, S - 2*m2), S * 0.20f)) g.FillPath(new SolidBrush(ink), p);
      float m3 = S * 0.105f;
      var inner = new RectangleF(m3, m3, S - 2*m3, S - 2*m3);
      using (var p = Round(inner, S * 0.165f)) {
        using (var grad = new System.Drawing.Drawing2D.LinearGradientBrush(inner, Color.FromArgb(255, 255, 140, 40), orange, 90f)) g.FillPath(grad, p);
      }
      // bandage cross
      float L = S * 0.70f, W = S * 0.25f, ol = S * 0.035f;
      var st = g.Save();
      g.TranslateTransform(S / 2f, S / 2f);
      foreach (float ang in new[] { -42f, 42f }) {
        var gs = g.Save(); g.RotateTransform(ang);
        var r = new RectangleF(-L/2, -W/2, L, W);
        using (var p = Round(r, W * 0.32f)) { g.FillPath(new SolidBrush(cream), p); g.DrawPath(new Pen(ink, ol), p); }
        // holes at both ends
        foreach (float sx in new[] { -1f, 1f })
          for (int k = 0; k < 2; k++) {
            float hx = sx * L * 0.36f, hy = (k == 0 ? -1 : 1) * W * 0.17f, hr = S * 0.017f;
            g.FillEllipse(new SolidBrush(Color.FromArgb(255, 205, 180, 150)), hx - hr, hy - hr, 2*hr, 2*hr);
          }
        g.Restore(gs);
      }
      // centre pad over the crossing
      var pad = new RectangleF(-W * 0.42f, -W * 0.42f, W * 0.84f, W * 0.84f);
      using (var p = Round(pad, W * 0.14f)) { g.FillPath(new SolidBrush(Color.FromArgb(255, 238, 214, 186)), p); g.DrawPath(new Pen(Color.FromArgb(160, 20, 14, 10), ol * 0.5f), p); }
      g.Restore(st);
    }
    return b;
  }
  // ICO with PNG-compressed images (Vista+)
  public static void WriteIco(string path, Bitmap[] images) {
    var pngs = new List<byte[]>();
    foreach (var b in images) { using (var ms = new MemoryStream()) { b.Save(ms, ImageFormat.Png); pngs.Add(ms.ToArray()); } }
    using (var f = new BinaryWriter(File.Create(path))) {
      f.Write((ushort)0); f.Write((ushort)1); f.Write((ushort)images.Length);
      int offset = 6 + 16 * images.Length;
      for (int i = 0; i < images.Length; i++) {
        int s = images[i].Width;
        f.Write((byte)(s >= 256 ? 0 : s)); f.Write((byte)(s >= 256 ? 0 : s)); f.Write((byte)0); f.Write((byte)0);
        f.Write((ushort)1); f.Write((ushort)32); f.Write(pngs[i].Length); f.Write(offset);
        offset += pngs[i].Length;
      }
      foreach (var p in pngs) f.Write(p);
    }
  }
}
"@
$out = 'port\launcher\assets'
# decode with WIC keeping the alpha channel (WebP or PNG); a source without alpha gets its white surround keyed out
Add-Type -AssemblyName PresentationCore
$dec = [System.Windows.Media.Imaging.BitmapDecoder]::Create([Uri](Resolve-Path $Source).Path, 'PreservePixelFormat', 'OnLoad')
$cv = New-Object System.Windows.Media.Imaging.FormatConvertedBitmap
$cv.BeginInit(); $cv.Source = $dec.Frames[0]; $cv.DestinationFormat = [System.Windows.Media.PixelFormats]::Bgra32; $cv.EndInit()
$enc = New-Object System.Windows.Media.Imaging.PngBitmapEncoder
$enc.Frames.Add([System.Windows.Media.Imaging.BitmapFrame]::Create($cv))
$fs = [IO.File]::Create("$root\$out\rebruised-logo-alpha.png"); $enc.Save($fs); $fs.Close()
$logo = [System.Drawing.Bitmap]::FromFile("$root\$out\rebruised-logo-alpha.png")
if ($logo.GetPixel(2, 2).A -eq 255) { $keyed = [Brand]::KeyWhite($logo); $logo.Dispose(); $keyed.Save("$root\$out\rebruised-logo-alpha.png", [System.Drawing.Imaging.ImageFormat]::Png); $logo = $keyed }
# icon: a badge in the logo's style (a crop of the "R" also caught the "e"), drawn at 1024 and scaled down
$badge = [Brand]::Badge(1024)
$full = New-Object System.Drawing.Rectangle 0, 0, 1024, 1024
$sizes = 256, 128, 64, 48, 32, 24, 16
$imgs = foreach ($s in $sizes) { [Brand]::Fit($badge, $full, $s, 0) }
$imgs[0].Save("$root\$out\rebruised-icon-256.png", [System.Drawing.Imaging.ImageFormat]::Png)
[Brand]::WriteIco("$root\$out\rebruised.ico", $imgs)
"logo $($logo.Width)x$($logo.Height), icon sizes $($sizes -join ',') -> $out\rebruised.ico"
