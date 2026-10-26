param([string]$titleMatch, [string]$out)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices;
public class W2 {
  public delegate bool EnumWindowsProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  public struct RECT { public int L, T, R, B; }
  public static IntPtr Found = IntPtr.Zero;
  public static string titleMatch = "";
  public static bool Cb(IntPtr h, IntPtr l) {
    if (!IsWindowVisible(h)) return true;
    var sb = new StringBuilder(256); GetWindowText(h, sb, 256);
    if (sb.ToString().Contains(titleMatch)) { Found = h; return false; }
    return true;
  }
}
"@
[W2]::SetProcessDPIAware() | Out-Null
[W2]::titleMatch = $titleMatch
[W2]::EnumWindows([W2+EnumWindowsProc]{ param($h,$l) [W2]::Cb($h,$l) }, [IntPtr]::Zero) | Out-Null
$h = [W2]::Found
if ($h -eq [IntPtr]::Zero) { Write-Output "NOWINDOW"; exit 1 }
[W2]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 600
$r = New-Object W2+RECT
[W2]::GetWindowRect($h, [ref]$r) | Out-Null
$wd = $r.R - $r.L; $hg = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap($wd, $hg)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
$out = (Join-Path (Get-Location) $out)
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Output "SAVED $out ${wd}x${hg}"
