<#
  ドックのウィンドウ (タイトルは既定で "cordmux"、指定の pid のトップレベルウィンドウ) の左上を原点とする座標 (X, Y) を
  マウスで 1 回クリックする。dock-click.ps1 の UI Automation は QTableWidget の升目の中の QPushButton
  (参加者一覧の [枠に載せる] 等) を辿れないので、撮った PNG から読んだ
  位置をそのまま押すツールである。座標は PNG (screenshot.ps1 が PrintWindow で撮ったウィンドウの絵) と同じ
  原点なので、PNG で見た位置がそのまま使える。

  前面を取れなかったときはクリックせずに ok:false を返す (別のアプリへクリックを送らないため)。結果は JSON 1 行 (UTF-8)。
#>
param(
  [Alias('Pid')][int]$TargetPid,
  [int]$X,
  [int]$Y,
  [string]$Title = "cordmux",
  [int]$Timeout = 5000
)

$ErrorActionPreference = "Stop"

function Write-Result {
  param($Result, [int]$ExitCode)
  $json = ($Result | ConvertTo-Json -Compress -Depth 5)
  $bytes = [System.Text.Encoding]::UTF8.GetBytes($json + "`n")
  $out = [Console]::OpenStandardOutput()
  $out.Write($bytes, 0, $bytes.Length)
  $out.Flush()
  exit $ExitCode
}

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class DockClickAtNative {
  public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int count);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter, int X, int Y, int cx, int cy, uint uFlags);
  public const uint SWP_NOSIZE = 0x0001;
  public const uint SWP_NOZORDER = 0x0004;
  public const uint SWP_SHOWWINDOW = 0x0040;
  [DllImport("user32.dll")] public static extern void mouse_event(uint dwFlags, uint dx, uint dy, uint dwData, UIntPtr dwExtraInfo);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
  public const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
  public const uint MOUSEEVENTF_LEFTUP = 0x0004;
  public static IntPtr FindWindow(uint pid, string title) {
    IntPtr found = IntPtr.Zero;
    EnumWindows(delegate (IntPtr hWnd, IntPtr lParam) {
      uint p; GetWindowThreadProcessId(hWnd, out p);
      if (p != pid || !IsWindowVisible(hWnd)) return true;
      var sb = new StringBuilder(512); GetWindowText(hWnd, sb, sb.Capacity);
      if (sb.ToString() == title) { found = hWnd; return false; }
      return true;
    }, IntPtr.Zero);
    return found;
  }
}
"@

$deadline = [DateTime]::UtcNow.AddMilliseconds($Timeout)
$hWnd = [IntPtr]::Zero
while ($hWnd -eq [IntPtr]::Zero -and [DateTime]::UtcNow -lt $deadline) {
  $hWnd = [DockClickAtNative]::FindWindow([uint32]$TargetPid, $Title)
  if ($hWnd -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 250 }
}
if ($hWnd -eq [IntPtr]::Zero) {
  Write-Result @{ ok = $false; pid = $TargetPid; x = $X; y = $Y; error = "window '$Title' of pid $TargetPid not found within $Timeout ms" } 1
}

# ウィンドウを主モニタの (100, 100) へ動かしてから位置を読む。OBS が覚えた浮いたウィンドウの位置がモニタの外 (y が負) に
# なることがあり (ドックのウィンドウ 2 回目で実測: top -853)、そのままではクリックがどこにも届かない。
[void][DockClickAtNative]::SetWindowPos($hWnd, [IntPtr]::Zero, 100, 100, 0, 0, ([DockClickAtNative]::SWP_NOSIZE -bor [DockClickAtNative]::SWP_NOZORDER -bor [DockClickAtNative]::SWP_SHOWWINDOW))
Start-Sleep -Milliseconds 300
$rect = New-Object DockClickAtNative+RECT
[void][DockClickAtNative]::GetWindowRect($hWnd, [ref]$rect)
[void][DockClickAtNative]::SetForegroundWindow($hWnd)
Start-Sleep -Milliseconds 300
if ([DockClickAtNative]::GetForegroundWindow() -ne $hWnd) {
  Write-Result @{ ok = $false; pid = $TargetPid; x = $X; y = $Y; error = "could not bring window '$Title' to the foreground; no click sent" } 1
}

# X が負ならウィンドウの右端からの距離 (列の幅が中身で変わっても、右端に寄せた列のボタンは同じ位置に来る)
$sx = if ($X -lt 0) { $rect.Right + $X } else { $rect.Left + $X }
$sy = $rect.Top + $Y
[void][DockClickAtNative]::SetCursorPos($sx, $sy)
Start-Sleep -Milliseconds 100
[DockClickAtNative]::mouse_event([DockClickAtNative]::MOUSEEVENTF_LEFTDOWN, 0, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 60
[DockClickAtNative]::mouse_event([DockClickAtNative]::MOUSEEVENTF_LEFTUP, 0, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 200

Write-Result @{ ok = $true; pid = $TargetPid; x = $X; y = $Y; screenX = $sx; screenY = $sy; window = @{ left = $rect.Left; top = $rect.Top; right = $rect.Right; bottom = $rect.Bottom } } 0
