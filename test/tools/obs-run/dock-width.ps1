#requires -Version 5.1
<#
  dock-width.ps1 - 実機の OBS で、ドックを最も細くしたときの幅を測るツール (人が手で実行する)。

  OBS の既定のレイアウトでドックを最も細くし、その幅と cordmux の計算した最小幅 (ja・en、フォントサイズ 10、[通常] の Yami) を比べる。
  en の下の帯 ([Start cordmux Recording] と [...]) が 1 段に入るかも見る。人がドックの縁をつまんで縮めると、どこで止まったかを画素で
  読めない。Windows にウィンドウを縮めさせ、Qt が許した最小 (WM_GETMINMAXINFO) で止まった大きさを数で読む。

  する事 (-Mode で選ぶ):
    main      OBS の主ウィンドウ (タイトルが "OBS " で始まる、-Pid の見えているトップレベルウィンドウ) の現在の四角を保存し、SetWindowPos で
              幅 -ShrinkTo (既定 100) に縮める。Windows は Qt の最小で止めるので、止まった幅を読み、主ウィンドウの中のドック
              (UI Automation の ClassName に Dock を含む要素。OBS のドックは OBSDock = QDockWidget の子) の名前と
              BoundingRectangle を全部書く。最後に保存した四角へ戻す。
    floating  浮いたドック (タイトルが -DockTitle、既定 "cordmux" の見えているトップレベルウィンドウ) を同じく縮めて、止まった
              幅を読み、戻す。
    both      (既定) 両方。浮いたドックが無ければ floating は found:false。
  縮めるのは幅だけ (高さは現在のまま)。ウィンドウは前面に出さない (SetWindowPos の SWP_NOZORDER と SWP_NOACTIVATE)。
  -NoRestore を渡したときだけ戻さない (縮めたまま screenshot.ps1 で撮るため。撮った後にもう一度このツールを -Mode の同じ
  値で呼べば、その時点の四角を保存して縮めるだけなので、戻すのは人がウィンドウの大きさを手で戻すか OBS を閉じる)。

  幅は物理の画素 (GetWindowRect と UI Automation の BoundingRectangle) と、ウィンドウの DPI (GetDpiForWindow) で割った
  論理の画素 (96 DPI 換算。Qt と製品の最小幅の計算と同じ単位) の両方を書く。

  実機の OBS で動かしたときの確認はまだない (構文と C# のビルドだけを確認した)。動かなければ
  (UI Automation がドックを返さない等)、screenshot.ps1 で縮めたウィンドウを撮り、画素で測る。

  結果は 1 行の JSON を標準出力に書く (screenshot.ps1 と同じ UTF-8)。
    { "ok": true|false, "pid": <int>, "main": {...}|null, "floating": {...}|null, "error": "<string>"|null }
    main / floating: { "found", "title", "dpi", "before": [x,y,w,h], "shrunk": [x,y,w,h], "restored": [x,y,w,h]|null,
                       "minWidthPx", "minWidthLogical", "docks": [ { "name", "className", "rect": [x,y,w,h], "widthLogical" } ] }
  失敗を握って先へ進む形 (screenshot.ps1 と同じ)。プロセスがないときだけ ok:false。
#>

[CmdletBinding()]
param(
  # screenshot.ps1 と同じ理由で、実体は $TargetPid、呼び出し側の名前は -Pid。
  [Parameter(Mandatory = $true)]
  [Alias('Pid')]
  [int]$TargetPid,

  [ValidateSet('main', 'floating', 'both')]
  [string]$Mode = 'both',

  # 浮いたドックのウィンドウのタイトル (製品の cordmux.dock.window.title。ja と en とも「cordmux」)。
  [string]$DockTitle = 'cordmux',

  # 縮める先の幅 (物理のピクセル)。Qt の最小より小さければ何でもよい。
  [ValidateRange(1, 4000)]
  [int]$ShrinkTo = 100,

  # 縮めてから読むまでの待ち (ミリ秒)。Qt が並べ直して描き直すまで (ドックの周期 500 ms と同じ)。
  [ValidateRange(0, 10000)]
  [int]$SettleMs = 500,

  [switch]$NoRestore
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

try {
  Add-Type -AssemblyName UIAutomationClient
  Add-Type -AssemblyName UIAutomationTypes

  # user32 の薄い包み。screenshot.ps1 と同じ理由でヒアストリングの中だけ英語のコメント。
  $nativeSource = @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class CordmuxDockWidthNative
{
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);
    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")]
    public static extern int GetWindowTextLength(IntPtr hWnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowText(IntPtr hWnd, StringBuilder lpString, int nMaxCount);
    [DllImport("user32.dll")]
    public static extern IntPtr GetAncestor(IntPtr hWnd, uint gaFlags);
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("user32.dll")]
    public static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter, int X, int Y, int cx, int cy, uint uFlags);
    [DllImport("user32.dll")]
    public static extern uint GetDpiForWindow(IntPtr hwnd);

    public const uint GA_ROOT = 2;
    public const uint SWP_NOZORDER = 0x0004;
    public const uint SWP_NOACTIVATE = 0x0010;
    public const uint SWP_NOMOVE = 0x0002;

    // Visible top-level windows of the process with their titles.
    public static List<KeyValuePair<IntPtr, string>> TopLevelWindows(uint targetProcessId)
    {
        var found = new List<KeyValuePair<IntPtr, string>>();
        EnumWindows(delegate (IntPtr hWnd, IntPtr lParam)
        {
            uint windowProcessId;
            GetWindowThreadProcessId(hWnd, out windowProcessId);
            if (windowProcessId != targetProcessId || !IsWindowVisible(hWnd) || GetAncestor(hWnd, GA_ROOT) != hWnd)
            {
                return true;
            }
            int len = GetWindowTextLength(hWnd);
            var sb = new StringBuilder(len + 1);
            if (len > 0) GetWindowText(hWnd, sb, sb.Capacity);
            found.Add(new KeyValuePair<IntPtr, string>(hWnd, sb.ToString()));
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

    public static void Resize(IntPtr hWnd, int width, int height)
    {
        SetWindowPos(hWnd, IntPtr.Zero, 0, 0, width, height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
    }

    public static void Place(IntPtr hWnd, int[] rect)
    {
        SetWindowPos(hWnd, IntPtr.Zero, rect[0], rect[1], rect[2], rect[3], SWP_NOZORDER | SWP_NOACTIVATE);
    }
}
"@
  Add-Type -TypeDefinition $nativeSource

  $proc = Get-Process -Id $TargetPid -ErrorAction SilentlyContinue
  if (-not $proc) {
    throw "process not found for pid $TargetPid"
  }

  $windows = [CordmuxDockWidthNative]::TopLevelWindows([uint32]$TargetPid)

  function Get-LogicalWidth {
    param([double]$Px, [int]$Dpi)
    if ($Dpi -le 0) { return $null }
    return [math]::Round($Px * 96.0 / $Dpi, 1)
  }

  # ウィンドウの中のドック (ClassName に Dock を含む要素) の名前と四角。
  function Get-Docks {
    param([IntPtr]$Hwnd, [int]$Dpi)
    $root = [System.Windows.Automation.AutomationElement]::FromHandle($Hwnd)
    $all = $root.FindAll([System.Windows.Automation.TreeScope]::Subtree, [System.Windows.Automation.Condition]::TrueCondition)
    $docks = @()
    foreach ($el in $all) {
      $cls = $el.Current.ClassName
      if (-not $cls -or $cls -notmatch 'Dock') { continue }
      $b = $el.Current.BoundingRectangle
      if ($b.IsEmpty) { continue }
      $docks += [PSCustomObject]@{
        name         = $el.Current.Name
        className    = $cls
        rect         = @([int]$b.X, [int]$b.Y, [int]$b.Width, [int]$b.Height)
        widthLogical = (Get-LogicalWidth -Px $b.Width -Dpi $Dpi)
      }
    }
    return , $docks
  }

  # 1 つのウィンドウを幅 ShrinkTo まで縮め、止まった大きさとドックを読み、戻す。
  function Measure-Window {
    param([IntPtr]$Hwnd, [string]$Title)
    $dpi = [int][CordmuxDockWidthNative]::GetDpiForWindow($Hwnd)
    $before = [CordmuxDockWidthNative]::Rect($Hwnd)
    [CordmuxDockWidthNative]::Resize($Hwnd, $ShrinkTo, $before[3])
    Start-Sleep -Milliseconds $SettleMs
    $shrunk = [CordmuxDockWidthNative]::Rect($Hwnd)
    $docks = Get-Docks -Hwnd $Hwnd -Dpi $dpi
    $restored = $null
    if (-not $NoRestore) {
      [CordmuxDockWidthNative]::Place($Hwnd, $before)
      Start-Sleep -Milliseconds $SettleMs
      $restored = [CordmuxDockWidthNative]::Rect($Hwnd)
    }
    return [PSCustomObject]@{
      found           = $true
      title           = $Title
      dpi             = $dpi
      before          = $before
      shrunk          = $shrunk
      restored        = $restored
      minWidthPx      = $shrunk[2]
      minWidthLogical = (Get-LogicalWidth -Px $shrunk[2] -Dpi $dpi)
      docks           = $docks
    }
  }

  $main = $null
  $floating = $null
  if ($Mode -eq 'main' -or $Mode -eq 'both') {
    $hit = $windows | Where-Object { $_.Value.StartsWith('OBS ') } | Select-Object -First 1
    $main = if ($hit) { Measure-Window -Hwnd $hit.Key -Title $hit.Value } else { [PSCustomObject]@{ found = $false; titles = @($windows | ForEach-Object { $_.Value }) } }
  }
  if ($Mode -eq 'floating' -or $Mode -eq 'both') {
    $hit = $windows | Where-Object { $_.Value -eq $DockTitle } | Select-Object -First 1
    $floating = if ($hit) { Measure-Window -Hwnd $hit.Key -Title $hit.Value } else { [PSCustomObject]@{ found = $false; titles = @($windows | ForEach-Object { $_.Value }) } }
  }

  Write-JsonResultAndExit -Result ([PSCustomObject]@{ ok = $true; pid = $TargetPid; mode = $Mode; main = $main; floating = $floating; error = $null }) -ExitCode 0
}
catch {
  Write-JsonResultAndExit -Result ([PSCustomObject]@{ ok = $false; pid = $TargetPid; mode = $Mode; main = $null; floating = $null; error = $_.Exception.Message }) -ExitCode 1
}
