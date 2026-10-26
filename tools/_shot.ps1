param([string]$titleMatch, [string]$out)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  public struct RECT { public int L, T, R, B; }
}
"@
$h = [W]::FindWindow($null, $titleMatch)
if ($h -eq [IntPtr]::Zero) { Write-Output "NOWINDOW"; exit 1 }
[W]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 500
$r = New-Object W+RECT
[W]::GetWindowRect($h, [ref]$r) | Out-Null
$wd = $r.R - $r.L; $hg = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap($wd, $hg)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Output "SAVED ${out} ${wd}x${hg}"
