param([string]$titleMatch, [int]$x, [int]$y)
Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices;
public class W3 {
  public delegate bool EnumWindowsProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
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
[W3]::SetProcessDPIAware() | Out-Null
[W3]::titleMatch = $titleMatch
[W3]::EnumWindows([W3+EnumWindowsProc]{ param($h,$l) [W3]::Cb($h,$l) }, [IntPtr]::Zero) | Out-Null
$h = [W3]::Found
if ($h -eq [IntPtr]::Zero) { Write-Output "NOWINDOW"; exit 1 }
$r = New-Object W3+RECT
[W3]::GetWindowRect($h, [ref]$r) | Out-Null
# client area: strip ~8px borders and ~30px caption (WS_OVERLAPPEDWINDOW)
$cx = $r.L + 8 + $x; $cy = $r.T + 31 + $y
[W3]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Milliseconds 300
[W3]::SetCursorPos($cx, $cy) | Out-Null; Start-Sleep -Milliseconds 150
[W3]::mouse_event(2,0,0,0,[UIntPtr]::Zero); [W3]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
Write-Output "CLICKED at client($x,$y) screen($cx,$cy)"
