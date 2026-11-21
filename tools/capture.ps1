# Saves a PNG of a process's largest top-level window, even off-screen (PrintWindow
# with PW_RENDERFULLCONTENT captures the D3D12 swap chain through DWM).
#   powershell -File tools\capture.ps1 -ProcessId 1234 -Out shot.png
param([int]$ProcessId, [string]$Out = "shot.png")
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Collections.Generic;
public static class W {
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr p);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  public struct RECT { public int L, T, R, B; }
  public static IntPtr Find(uint want) {
    IntPtr best = IntPtr.Zero; int area = 0;
    EnumWindows((h, p) => { uint pid; GetWindowThreadProcessId(h, out pid);
      if (pid == want && IsWindowVisible(h)) { RECT r; GetClientRect(h, out r); int a = r.R * r.B; if (a > area) { area = a; best = h; } }
      return true; }, IntPtr.Zero);
    return best;
  }
}
"@
$h = [W]::Find([uint32]$ProcessId)
if ($h -eq [IntPtr]::Zero) { Write-Output "window not found"; exit 1 }
$r = New-Object W+RECT; [void][W]::GetClientRect($h, [ref]$r)
$bmp = New-Object System.Drawing.Bitmap ([Math]::Max(1,$r.R)), ([Math]::Max(1,$r.B))
$g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
[void][W]::PrintWindow($h, $dc, 3)   # PW_CLIENTONLY | PW_RENDERFULLCONTENT
$g.ReleaseHdc($dc); $g.Dispose(); $bmp.Save($Out); $bmp.Dispose()
Write-Output "saved $Out"
