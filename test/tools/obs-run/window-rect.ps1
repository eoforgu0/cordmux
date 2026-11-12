#requires -Version 5.1
<#
  window-rect.ps1 - 名指した実行ファイルの主ウィンドウ (クラス -ClassName の見えているトップレベルウィンドウのうち面積が最大の物) の四角と、
  最大化か・最小化かと、そのウィンドウがある画面の四角を読むツール (読むだけ。ウィンドウにも画面にも触らない)。

  使い道: 遅れの測り (call-1 の callLatencySteps) のウィンドウキャプチャが Vesktop のウィンドウを捉えられなかったとき、実行スクリプトが
  「Vesktop が最大化されて画面を覆っているか」を見て、覆っていれば画面キャプチャへ切り替える。覆っていなければ切り替えない
  (画面キャプチャは利用者の別のウィンドウもキャプチャするため)。決めるのは実行スクリプト (run-window-plan.mjs の windowFillsMonitor)。このツールは数を返すだけ。

  座標は物理の画素 (プロセスをモニターごとの DPI 対応にして読む。OBS の画面キャプチャの一覧の "<機種>: 1920x1080 @ x,y" と同じ物差し)。
  ウィンドウのタイトルは読まない (利用者の通話の題やサーバー名を結果に持たない)。

  結果は 1 行の JSON を標準出力に書く (UTF-8、BOM なし)。
    { "ok", "found", "count", "rect": [x,y,w,h]|null, "zoomed", "iconic", "monitor": [x,y,w,h]|null, "error" }
  見つからなければ ok は真で found が偽 (ウィンドウがないのは異常ではなく、判断の情報になる)。
#>

[CmdletBinding()]
param(
  [string]$ProcessName = 'vesktop',
  [string]$ClassName = 'Chrome_WidgetWin_1'
)

$ErrorActionPreference = 'Stop'

function Write-JsonResultAndExit {
  # screenshot.ps1 と同じ理由: 標準出力へ UTF-8 (BOM なし) のバイト列を直接書く。
  param($Result, [int]$ExitCode)
  $json = $Result | ConvertTo-Json -Depth 6 -Compress
  $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
  $bytes = $utf8NoBom.GetBytes($json)
  $stdout = [System.Console]::OpenStandardOutput()
  $stdout.Write($bytes, 0, $bytes.Length)
  $stdout.Flush()
  exit $ExitCode
}

# user32 の薄い包み。screenshot.ps1 と同じ理由でヒアストリングの中だけ英語のコメント。
$nativeSource = @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class CordmuxWindowRectNative
{
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }

    [StructLayout(LayoutKind.Sequential)]
    public struct MONITORINFO { public int cbSize; public RECT rcMonitor; public RECT rcWork; public uint dwFlags; }

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);
    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")]
    public static extern bool IsZoomed(IntPtr hWnd);
    [DllImport("user32.dll")]
    public static extern bool IsIconic(IntPtr hWnd);
    [DllImport("user32.dll")]
    public static extern IntPtr GetAncestor(IntPtr hWnd, uint gaFlags);
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassName(IntPtr hWnd, StringBuilder lpClassName, int nMaxCount);
    [DllImport("user32.dll")]
    public static extern IntPtr MonitorFromWindow(IntPtr hWnd, uint dwFlags);
    [DllImport("user32.dll")]
    public static extern bool GetMonitorInfo(IntPtr hMonitor, ref MONITORINFO lpmi);
    [DllImport("user32.dll")]
    public static extern bool SetProcessDpiAwarenessContext(IntPtr value);

    public const uint GA_ROOT = 2;
    public const uint MONITOR_DEFAULTTONEAREST = 2;

    // Per-monitor v2, so every rectangle below is in physical pixels.
    public static void UsePhysicalPixels()
    {
        SetProcessDpiAwarenessContext(new IntPtr(-4));
    }

    public static string ClassOf(IntPtr hWnd)
    {
        var sb = new StringBuilder(256);
        GetClassName(hWnd, sb, sb.Capacity);
        return sb.ToString();
    }

    // Visible top-level windows of the given processes with the given class.
    public static List<IntPtr> Find(HashSet<uint> pids, string className)
    {
        var found = new List<IntPtr>();
        EnumWindows(delegate (IntPtr hWnd, IntPtr lParam)
        {
            uint pid;
            GetWindowThreadProcessId(hWnd, out pid);
            if (!pids.Contains(pid) || !IsWindowVisible(hWnd) || GetAncestor(hWnd, GA_ROOT) != hWnd) return true;
            if (ClassOf(hWnd) != className) return true;
            found.Add(hWnd);
            return true;
        }, IntPtr.Zero);
        return found;
    }

    public static int[] Rect(IntPtr hWnd)
    {
        RECT r;
        GetWindowRect(hWnd, out r);
        return new int[] { r.Left, r.Top, r.Right - r.Left, r.Bottom - r.Top };
    }

    public static int[] MonitorRect(IntPtr hWnd)
    {
        IntPtr m = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
        var info = new MONITORINFO();
        info.cbSize = Marshal.SizeOf(typeof(MONITORINFO));
        if (m == IntPtr.Zero || !GetMonitorInfo(m, ref info)) return null;
        RECT r = info.rcMonitor;
        return new int[] { r.Left, r.Top, r.Right - r.Left, r.Bottom - r.Top };
    }
}
"@
Add-Type -TypeDefinition $nativeSource

try {
  [CordmuxWindowRectNative]::UsePhysicalPixels()
  $pids = New-Object 'System.Collections.Generic.HashSet[uint32]'
  foreach ($p in @(Get-Process -Name $ProcessName -ErrorAction SilentlyContinue)) { [void]$pids.Add([uint32]$p.Id) }
  $handles = if ($pids.Count -gt 0) { [CordmuxWindowRectNative]::Find($pids, $ClassName) } else { @() }
  if (@($handles).Count -eq 0) {
    Write-JsonResultAndExit -Result ([PSCustomObject]@{ ok = $true; found = $false; count = 0; rect = $null; zoomed = $false; iconic = $false; monitor = $null; error = $null }) -ExitCode 0
  }
  # 主ウィンドウ = 面積が最大のウィンドウ (ほかの見えている同じクラスのウィンドウは、共有の小さなウィンドウなど)。
  $best = $null
  $bestArea = -1
  foreach ($h in $handles) {
    $r = [CordmuxWindowRectNative]::Rect($h)
    $area = [int64]$r[2] * [int64]$r[3]
    if ($area -gt $bestArea) { $best = $h; $bestArea = $area }
  }
  Write-JsonResultAndExit -Result ([PSCustomObject]@{
      ok      = $true
      found   = $true
      count   = @($handles).Count
      rect    = [CordmuxWindowRectNative]::Rect($best)
      zoomed  = [bool][CordmuxWindowRectNative]::IsZoomed($best)
      iconic  = [bool][CordmuxWindowRectNative]::IsIconic($best)
      monitor = [CordmuxWindowRectNative]::MonitorRect($best)
      error   = $null
    }) -ExitCode 0
}
catch {
  Write-JsonResultAndExit -Result ([PSCustomObject]@{ ok = $false; found = $false; count = 0; rect = $null; zoomed = $false; iconic = $false; monitor = $null; error = $_.Exception.Message }) -ExitCode 1
}
