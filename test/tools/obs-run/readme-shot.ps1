#requires -Version 5.1
<#
  readme-shot.ps1 - --step hold で開いている OBS に cordmux のドックをドッキングし、OBS の主ウィンドウを決まった大きさで撮って PNG にするツール。

  README の画像のように、実際の OBS の画面 (プレビューの 4 分割と名前ラベル) と、OBS の中にドッキングしたドックを 1 枚で見せるために使う。
  撮り直しのたびに OBS を起動し直さずに済むように、待ち、ドッキングし、大きさをそろえ、撮るまでを 1 回の呼び出しで行う。手順は README.md の
  「実際の OBS の画面を撮る」にある。

  する事:
    1. -HoldInfo の hold-<tag>.json が書かれ、その OBS が動いているまで待ち、OBS の pid と設定の場所と通知ファイルの場所を読む。
    2. -VoiceOn なら、hold-<tag>.json の voiceToneFile (hold の --voice-users で決まる) を空で置き、全部の声に音を乗せる
       (声の持ち主の枠に話している人の光が出る。OBS の監視は切ってあるので音は鳴らない)。
    3. -ExpectLabels の数のタイルの名前ラベルを描き終えるまで (OBS のログの "Composite label ready" の行の tile の種類の数) 待つ。
       -WaitSeconds を過ぎたら待たずに撮り、結果の labelsReady を偽にする。
    4. 主ウィンドウ (タイトルが "OBS " で始まる) の最大化と最小化を、元の大きさへ戻す。
    5. ドックが浮いていれば (タイトルが -DockTitle の見えているトップレベルウィンドウがあれば)、タイトルバーをダブルクリックしたときと
       同じメッセージを送って、主ウィンドウにドッキングする。浮いたウィンドウが消え、主ウィンドウの中にドックが現れるまで待ち、
       現れなければ理由を書いて止まる。既にドッキングしていれば何もしない (撮り直し)。下の「ドッキングの仕方」を見よ。
    6. 主ウィンドウを、見える縁の大きさが -Width x -Height (論理の画素。ウィンドウの DPI で物理の画素に直す) になるように、その画面の
       作業領域の左上へ置く。
    7. ドックの幅を -DockWidth (論理の画素) にする。0 なら変えない。下の「幅の変え方」を見よ。主ウィンドウをもう一度置き (ドッキング
       した直後の幅で主ウィンドウの最小の大きさが広がり、6 の大きさが止められていることがあるため)、幅が合わなければ理由を書いて止まる。
    8. -SettleMs 待って主ウィンドウを撮る。-Method printwindow (既定) は PrintWindow (PW_RENDERFULLCONTENT) で、ほかのウィンドウの裏や
       画面の外でも撮れる (screenshot.ps1 と同じ)。プレビューが黒く写ったときは -Method screen で撮り直す。こちらは主ウィンドウを
       最前面に出して (フォーカスは奪わない) 画面から切り出すので、主ウィンドウが画面の中に収まっている必要がある。撮った絵は
       DWM の見える縁 (DWMWA_EXTENDED_FRAME_BOUNDS) で切り、見えない縁の分を除く。
    9. 幅が -OutWidth を超えるときは縮め (広げはしない)、README に置く 1 枚にする。

  ドッキングの仕方:
    OBS 33.0.1 の obs_frontend_add_dock_by_id (frontend/OBSStudioAPI.cpp) は、プラグインのドックを AddDockWidget(dock,
    Qt::RightDockWidgetArea) で右の領域に足してから setFloating(true) で浮かせる。Qt の QDockWidget は、浮いたドックのタイトルバー
    (Windows が描く枠) のダブルクリック (NonClientAreaMouseButtonDblClick) で setFloating(false) になり、足した領域へ戻る
    (qtbase 6.11.1 の qdockwidget.cpp の nonClientAreaMouseEvent)。Qt はダブルクリックを Windows から受け取らず、同じボタンの押下が
    同じウィンドウにダブルクリックの時間の内に 2 回来たら自分で作る (qguiapplication.cpp の processMouseEvent。時刻の差が 0 の 2 回は
    数えない)。そこで WM_NCLBUTTONDBLCLK (Qt は押下として読む) を $doubleClickGapMs あけて 2 回置く。
    押す位置は、タイトルバーの高さの中ほどで、クライアント領域の左の縁から 3 物理画素外。Qt はタイトルの範囲の中の押下をドラッグの
    始まりとして扱い、ドラッグを終えるときにウィンドウをアクティブにする (endDrag の activateWindow) ので、範囲の外を押してフォーカスを
    奪わない。ダブルクリックの処理は押した位置を見ない。
    OBS 33 の既定の [フルハイト ドック] では、右の領域は主ウィンドウの上端から下端までの列になり、コントロールのドックはその左に並ぶ。
    プレビューの右でコントロールの上に置くには、hold に --obs-full-height-docks off を渡す (README.md の「実際の OBS の画面を撮る」)。

  幅の変え方:
    ドッキングしたドックはウィンドウではないので、dock-width.ps1 のように SetWindowPos で大きさを変えられない。幅を読むのは dock-width.ps1
    と同じく UI Automation (主ウィンドウの子のうち、クラス名に Dock を含み、名前が -DockTitle の要素の BoundingRectangle)。変えるのは、
    ドックの縁の仕切りを左ボタンで引っ張ったときと同じメッセージ (WM_LBUTTONDOWN、左ボタンを押したままの WM_MOUSEMOVE、WM_LBUTTONUP)
    を主ウィンドウに置いて行う。Qt の QMainWindow は、押した所が仕切りなら、押したまま動かした分だけ仕切りを動かす (qmainwindowlayout_p.h
    の QMainWindowLayoutSeparatorHelper)。Qt は左ボタンの状態をメッセージの wParam から読む (qwindowspointerhandler.cpp) ので、本物の
    マウスのボタンは要らない。仕切りは、ドックが主ウィンドウの左に付いていれば右の縁、ほかは左の縁にある。幅を読み直し、合うまで
    $dockWidthTries 回まで繰り返す。

  どのメッセージも PostMessage でウィンドウに置くだけで、本物のマウスのカーソルは動かさず、ウィンドウを前面にも出さない。撮る間に PC の
  マウスを OBS の上で動かすと、Qt がダブルクリックや仕切りの引っ張りと取り違えうるので、触らないでおく。

  出力 (-OutDir。既定は hold-<tag>.json のフォルダー):
    <Prefix>-obs.png   主ウィンドウ (等倍)
    <Prefix>-shot.png  幅を -OutWidth 以下に縮めたもの (README に置く。-OutWidth 以下なら等倍のまま)
  結果は 1 行の JSON を標準出力に書く (screenshot.ps1 と同じ UTF-8)。失敗は ok:false と error で、終了コード 1。

  -SelfTest: 置き方、切り方、縮め方、押す位置と引っ張る位置、ドックの選び方、ログの読み方の計算と、作った画像を縮める処理を確認する。
  ウィンドウは開かず、OBS にも触らない。
#>

[CmdletBinding()]
param(
  # --step hold が書く <out>/window3b/hold-<tag>.json。
  [string]$HoldInfo = '',

  # hold-<tag>.json を使わないときの OBS の pid (screenshot.ps1 と同じ理由で、実体は $TargetPid)。
  [Alias('Pid')]
  [int]$TargetPid = 0,

  [string]$OutDir = '',
  [string]$Prefix = 'readme',

  # 主ウィンドウの見える縁の大きさ (論理の画素。96 DPI 換算)。
  [ValidateRange(400, 8000)]
  [int]$Width = 1600,
  [ValidateRange(300, 8000)]
  [int]$Height = 900,

  # ドッキングしたドックの幅 (論理の画素)。0 は変えない。
  [ValidateRange(0, 4000)]
  [int]$DockWidth = 420,

  # ドックのタイトル (製品の cordmux.dock.window.title。dock-width.ps1 と同じ)。
  [string]$DockTitle = 'cordmux',

  # README に置く 1 枚の幅の上限 (物理の画素)。
  [ValidateRange(200, 16000)]
  [int]$OutWidth = 1600,

  [ValidateSet('printwindow', 'screen')]
  [string]$Method = 'printwindow',

  [switch]$VoiceOn,

  # 名前ラベルを描き終えるのを待つタイルの数 (0 は待たない)。
  [ValidateRange(0, 16)]
  [int]$ExpectLabels = 4,

  # hold-<tag>.json と名前ラベルを待つ上限 (秒)。
  [ValidateRange(0, 3600)]
  [int]$WaitSeconds = 180,

  # 置いてから撮るまでの待ち (ミリ秒)。Qt が並べ直して描き直し、光が立ち上がるまで。
  [ValidateRange(0, 60000)]
  [int]$SettleMs = 3000,

  [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'

# ダブルクリックの 2 回の押下の間 (ミリ秒)。0 より長く、Windows のダブルクリックの時間 (既定 500。設定の最短 200) より短く。
$doubleClickGapMs = 100
# ダブルクリックしてから、浮いたウィンドウが消えてドックが主ウィンドウの中に現れるまで待つ上限 (ミリ秒) と、ダブルクリックを試す回数。
$dockWaitMs = 3000
$dockTries = 3
# 幅を合わせるために仕切りを引っ張る回数の上限 (物理の画素への丸めで 1 回では合わないことがある)。
$dockWidthTries = 4

function Write-JsonResultAndExit {
  # screenshot.ps1 と同じ理由: 標準出力へ UTF-8 (BOM なし) のバイト列を直接書く。
  param($Result, [int]$ExitCode)
  $json = $Result | ConvertTo-Json -Depth 8 -Compress
  $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
  $bytes = $utf8NoBom.GetBytes($json)
  $stdout = [System.Console]::OpenStandardOutput()
  $stdout.Write($bytes, 0, $bytes.Length)
  $stdout.Flush()
  exit $ExitCode
}

Add-Type -AssemblyName System.Drawing

# user32 と dwmapi の薄い包み。screenshot.ps1 と同じ理由でヒアストリングの中だけ英語のコメント。
$nativeSource = @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class CordmuxReadmeShotNative
{
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }

    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int X; public int Y; }

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
    public static extern int GetWindowTextLength(IntPtr hWnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowText(IntPtr hWnd, StringBuilder lpString, int nMaxCount);
    [DllImport("user32.dll")]
    public static extern IntPtr GetAncestor(IntPtr hWnd, uint gaFlags);
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("user32.dll")]
    public static extern bool GetClientRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("user32.dll")]
    public static extern bool ClientToScreen(IntPtr hWnd, ref POINT lpPoint);
    [DllImport("user32.dll")]
    public static extern bool ScreenToClient(IntPtr hWnd, ref POINT lpPoint);
    [DllImport("user32.dll")]
    public static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter, int X, int Y, int cx, int cy, uint uFlags);
    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")]
    public static extern uint GetDpiForWindow(IntPtr hwnd);
    [DllImport("user32.dll")]
    public static extern IntPtr MonitorFromWindow(IntPtr hWnd, uint dwFlags);
    [DllImport("user32.dll")]
    public static extern bool GetMonitorInfo(IntPtr hMonitor, ref MONITORINFO lpmi);
    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);
    [DllImport("user32.dll")]
    public static extern bool SetProcessDpiAwarenessContext(IntPtr value);
    [DllImport("user32.dll")]
    public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr value);
    [DllImport("dwmapi.dll")]
    public static extern int DwmGetWindowAttribute(IntPtr hwnd, int dwAttribute, out RECT pvAttribute, int cbAttribute);

    public const uint GA_ROOT = 2;
    public const uint MONITOR_DEFAULTTONEAREST = 2;
    public const uint PW_RENDERFULLCONTENT = 2;
    public const int SW_SHOWNOACTIVATE = 4;
    public const int DWMWA_EXTENDED_FRAME_BOUNDS = 9;
    public const uint SWP_NOSIZE = 0x0001;
    public const uint SWP_NOMOVE = 0x0002;
    public const uint SWP_NOZORDER = 0x0004;
    public const uint SWP_NOACTIVATE = 0x0010;
    public const uint WM_MOUSEMOVE = 0x0200;
    public const uint WM_LBUTTONDOWN = 0x0201;
    public const uint WM_LBUTTONUP = 0x0202;
    public const uint WM_NCLBUTTONDBLCLK = 0x00A3;
    public const int HTCAPTION = 2;
    public const int MK_LBUTTON = 0x0001;
    public static readonly IntPtr HWND_TOPMOST = new IntPtr(-1);
    public static readonly IntPtr HWND_NOTOPMOST = new IntPtr(-2);

    // Per-monitor v2, so every rectangle below is in physical pixels. The thread context is set too, because the
    // process context cannot change once the process has a window.
    public static bool UsePhysicalPixels()
    {
        SetProcessDpiAwarenessContext(new IntPtr(-4));
        return SetThreadDpiAwarenessContext(new IntPtr(-4)) != IntPtr.Zero;
    }

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

    // The visible frame (without the invisible resize borders). Falls back to the window rectangle.
    public static int[] FrameRect(IntPtr hWnd)
    {
        RECT r;
        if (DwmGetWindowAttribute(hWnd, DWMWA_EXTENDED_FRAME_BOUNDS, out r, Marshal.SizeOf(typeof(RECT))) != 0) return Rect(hWnd);
        return new int[] { r.Left, r.Top, r.Right - r.Left, r.Bottom - r.Top };
    }

    // The client area in screen coordinates.
    public static int[] ClientRectOnScreen(IntPtr hWnd)
    {
        RECT r;
        if (!GetClientRect(hWnd, out r)) return null;
        var p = new POINT();
        if (!ClientToScreen(hWnd, ref p)) return null;
        return new int[] { p.X, p.Y, r.Right - r.Left, r.Bottom - r.Top };
    }

    public static int[] ToClient(IntPtr hWnd, int x, int y)
    {
        var p = new POINT();
        p.X = x;
        p.Y = y;
        ScreenToClient(hWnd, ref p);
        return new int[] { p.X, p.Y };
    }

    // MAKELPARAM. The receiver reads the halves back as signed shorts (GET_X_LPARAM / GET_Y_LPARAM).
    public static IntPtr MakeLParam(int x, int y)
    {
        return new IntPtr(unchecked((int)(((uint)(y & 0xFFFF) << 16) | (uint)(x & 0xFFFF))));
    }

    public static int LParamX(IntPtr lParam) { return unchecked((short)(lParam.ToInt64() & 0xFFFF)); }
    public static int LParamY(IntPtr lParam) { return unchecked((short)((lParam.ToInt64() >> 16) & 0xFFFF)); }

    public static bool Post(IntPtr hWnd, uint message, int wParam, int x, int y)
    {
        return PostMessage(hWnd, message, new IntPtr(wParam), MakeLParam(x, y));
    }

    public static int[] WorkArea(IntPtr hWnd)
    {
        IntPtr m = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
        var info = new MONITORINFO();
        info.cbSize = Marshal.SizeOf(typeof(MONITORINFO));
        if (m == IntPtr.Zero || !GetMonitorInfo(m, ref info)) return null;
        RECT r = info.rcWork;
        return new int[] { r.Left, r.Top, r.Right - r.Left, r.Bottom - r.Top };
    }

    public static void Place(IntPtr hWnd, int[] rect)
    {
        SetWindowPos(hWnd, IntPtr.Zero, rect[0], rect[1], rect[2], rect[3], SWP_NOZORDER | SWP_NOACTIVATE);
    }

    public static void SetTopmost(IntPtr hWnd, bool topmost)
    {
        SetWindowPos(hWnd, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}
"@
Add-Type -TypeDefinition $nativeSource -ReferencedAssemblies System.Drawing

# ---- 計算 (セルフテストで確かめる。ウィンドウに触らない) ------------------------------

# 論理の画素 (96 DPI 換算) を、DPI のウィンドウの物理の画素に直す。
function ConvertTo-PhysicalPx {
  param([int]$Logical, [int]$Dpi)
  if ($Dpi -le 0) { return $Logical }
  return [int][math]::Round($Logical * $Dpi / 96.0)
}

# 物理の画素を論理の画素 (96 DPI 換算。小数 1 桁) に直す (dock-width.ps1 の Get-LogicalWidth と同じ)。
function ConvertTo-LogicalPx {
  param([int]$Physical, [int]$Dpi)
  if ($Dpi -le 0) { return [double]$Physical }
  return [math]::Round($Physical * 96.0 / $Dpi, 1)
}

# ウィンドウの四角と見える縁の四角から、見えない縁の幅 (左、上、右、下) を返す。
function Get-FrameInsets {
  param([int[]]$WindowRect, [int[]]$FrameRect)
  $left = $FrameRect[0] - $WindowRect[0]
  $top = $FrameRect[1] - $WindowRect[1]
  $right = ($WindowRect[0] + $WindowRect[2]) - ($FrameRect[0] + $FrameRect[2])
  $bottom = ($WindowRect[1] + $WindowRect[3]) - ($FrameRect[1] + $FrameRect[3])
  return , @([math]::Max(0, $left), [math]::Max(0, $top), [math]::Max(0, $right), [math]::Max(0, $bottom))
}

# 見える縁を Frame (x, y, w, h) にするために SetWindowPos へ渡すウィンドウの四角。
function Get-WindowRectForFrame {
  param([int[]]$Frame, [int[]]$Insets)
  return , @(($Frame[0] - $Insets[0]), ($Frame[1] - $Insets[1]), ($Frame[2] + $Insets[0] + $Insets[2]), ($Frame[3] + $Insets[1] + $Insets[3]))
}

# 主ウィンドウを作業領域の左上に置くときの、見える縁の四角とウィンドウの四角。
function Get-MainPlacement {
  param([int[]]$WorkArea, [int[]]$MainSize, [int[]]$Insets)
  $frame = @($WorkArea[0], $WorkArea[1], $MainSize[0], $MainSize[1])
  return [PSCustomObject]@{ frame = $frame; window = (Get-WindowRectForFrame -Frame $frame -Insets $Insets) }
}

# ウィンドウ全体を撮った絵の中の、見える縁の四角 (絵の外へはみ出さないように切り詰める)。
function Get-CropRect {
  param([int[]]$WindowRect, [int[]]$FrameRect)
  $x = [math]::Max(0, $FrameRect[0] - $WindowRect[0])
  $y = [math]::Max(0, $FrameRect[1] - $WindowRect[1])
  $w = [math]::Min($FrameRect[2], $WindowRect[2] - $x)
  $h = [math]::Min($FrameRect[3], $WindowRect[3] - $y)
  return , @($x, $y, [math]::Max(0, $w), [math]::Max(0, $h))
}

# 幅を OutWidthPx 以下に縮めるときの大きさと比。広げはしない。
function Get-ScaledSize {
  param([int]$Width, [int]$Height, [int]$OutWidthPx)
  $scale = [math]::Min(1.0, $OutWidthPx / [double]$Width)
  return [PSCustomObject]@{ width = [int][math]::Round($Width * $scale); height = [int][math]::Round($Height * $scale); scale = $scale }
}

# OBS のログの行から、名前ラベルを描き終えたタイルの番号 (重ならない、昇順)。
function Get-ReadyLabelTiles {
  param([string[]]$Lines)
  $tiles = New-Object 'System.Collections.Generic.SortedSet[int]'
  foreach ($line in $Lines) {
    $m = [regex]::Match($line, 'Composite label ready: tile=(\d+)')
    if ($m.Success) { [void]$tiles.Add([int]$m.Groups[1].Value) }
  }
  return , @($tiles)
}

# 浮いたドックのタイトルバーのダブルクリックで押す位置 (画面の座標)。高さはタイトルバーの帯 (見える縁の上端からクライアント領域の
# 上端まで) の中ほど、横はクライアント領域の左の縁から 3 物理画素外。Qt のタイトルの範囲 (左右はクライアント領域の幅) の外なので、
# 押下はドラッグを始めない (上の「ドッキングの仕方」)。3 は、論理の画素に直して丸めても範囲の外に残る幅 (200% でも 1.5 で左へ 1)。
function Get-TitleDoubleClickPoint {
  param([int[]]$FrameRect, [int[]]$ClientRect)
  $x = $ClientRect[0] - 3
  $y = [int][math]::Floor(($FrameRect[1] + $ClientRect[1]) / 2)
  return , @($x, $y)
}

# ドックの幅を TargetWidthPx (物理の画素) にするために引っ張る仕切りの、押す位置と放す位置 (画面の座標)。
# Qt の仕切りはドックの外に接する数論理画素の帯 (OBS の Yami は 2 から 4、QDockAreaLayout::separatorRect) なので、ドックの縁から
# 1 論理画素ほど外を押す。ドックが主ウィンドウのクライアント領域の左に付いていれば (左の領域) 右の縁の仕切り、ほかは左の縁の仕切り。
function Get-SeparatorDrag {
  param([int[]]$DockRect, [int[]]$ClientRect, [int]$TargetWidthPx, [int]$Dpi)
  $k = [math]::Max(1, [int][math]::Round($Dpi / 96.0))
  $y = $DockRect[1] + [int][math]::Floor($DockRect[3] / 2)
  $grow = $TargetWidthPx - $DockRect[2]
  if (($DockRect[0] - $ClientRect[0]) -le (8 * $k)) {
    $x = $DockRect[0] + $DockRect[2] + $k - 1
    return [PSCustomObject]@{ edge = 'right'; from = @($x, $y); to = @(($x + $grow), $y) }
  }
  $x = $DockRect[0] - $k
  return [PSCustomObject]@{ edge = 'left'; from = @($x, $y); to = @(($x - $grow), $y) }
}

# ドックの幅 (物理の画素) が、求めた幅 (論理の画素) に合っているか。Qt は論理の画素で並べて物理の画素へ丸めるので、
# 1 論理画素の差 (物理の画素では拡大率を切り上げた数) までは合っているとみなす。
function Test-DockWidth {
  param([int]$WidthPx, [int]$TargetLogical, [int]$Dpi)
  $tolerance = [math]::Max(1, [int][math]::Ceiling($Dpi / 96.0))
  return ([math]::Abs($WidthPx - (ConvertTo-PhysicalPx -Logical $TargetLogical -Dpi $Dpi)) -le $tolerance)
}

# UI Automation の要素 (name、className、rect、offscreen) の中から、名前が Title のドック (クラス名に Dock を含む。OBS のドックは
# OBSDock = QDockWidget の子クラス) を選ぶ。見えていない物と大きさのない物は選ばない。
function Select-DockElement {
  param($Elements, [string]$Title)
  return @($Elements | Where-Object { $_.className -match 'Dock' -and $_.name -eq $Title -and -not $_.offscreen -and $_.rect[2] -gt 0 -and $_.rect[3] -gt 0 }) | Select-Object -First 1
}

# 撮った絵を Width x Height に縮めた 1 枚 (24 bit)。
function New-ScaledImage {
  param([System.Drawing.Bitmap]$Source, [int]$Width, [int]$Height)
  $out = New-Object System.Drawing.Bitmap($Width, $Height, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
  $g = [System.Drawing.Graphics]::FromImage($out)
  try {
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    # 縁で外側の黒を拾って暗くならないように、縁の外は鏡に写した絵で埋めて縮める。
    $attr = New-Object System.Drawing.Imaging.ImageAttributes
    $attr.SetWrapMode([System.Drawing.Drawing2D.WrapMode]::TileFlipXY)
    $dest = New-Object System.Drawing.Rectangle(0, 0, $Width, $Height)
    $g.DrawImage($Source, $dest, 0, 0, $Source.Width, $Source.Height, [System.Drawing.GraphicsUnit]::Pixel, $attr)
    $attr.Dispose()
  }
  finally {
    $g.Dispose()
  }
  return $out
}

# ---- セルフテスト ------------------------------------------------------------

if ($SelfTest) {
  # 結果は撮るときと同じく 1 行の JSON (window-size.ps1 の -SelfTest と同じ形)。
  $failures = New-Object System.Collections.Generic.List[string]
  $checks = New-Object System.Collections.Generic.List[object]
  function Test-Check {
    param([bool]$Cond, [string]$Label)
    if (-not $Cond) { $failures.Add($Label) }
    $checks.Add([PSCustomObject]@{ ok = $Cond; label = $Label })
  }
  function Test-Same {
    param($Got, $Want)
    return (($Got -join ',') -eq ($Want -join ','))
  }

  Test-Check ((ConvertTo-PhysicalPx -Logical 1600 -Dpi 96) -eq 1600) '96 DPI では論理の画素がそのまま物理の画素になる'
  Test-Check ((ConvertTo-PhysicalPx -Logical 1600 -Dpi 144) -eq 2400) '144 DPI では 1.5 倍になる'
  Test-Check ((ConvertTo-PhysicalPx -Logical 441 -Dpi 120) -eq 551) '120 DPI では 1.25 倍を丸める'
  Test-Check ((ConvertTo-LogicalPx -Physical 525 -Dpi 120) -eq 420) '物理の画素を論理の画素に戻す'

  $insets = Get-FrameInsets -WindowRect @(-7, 0, 1614, 907) -FrameRect @(0, 0, 1600, 900)
  Test-Check (Test-Same $insets @(7, 0, 7, 7)) 'Windows 10 の見えない縁 (左右と下に 7) を読む'
  Test-Check (Test-Same (Get-WindowRectForFrame -Frame @(0, 0, 1600, 900) -Insets $insets) @(-7, 0, 1614, 907)) '見える縁から、渡すウィンドウの四角を戻せる'

  $placement = Get-MainPlacement -WorkArea @(0, 0, 1920, 1040) -MainSize @(1600, 900) -Insets @(7, 0, 7, 7)
  Test-Check (Test-Same $placement.frame @(0, 0, 1600, 900)) '主ウィンドウの見える縁は作業領域の左上'
  Test-Check (Test-Same $placement.window @(-7, 0, 1614, 907)) '主ウィンドウに渡すウィンドウの四角は見えない縁の分だけ大きい'
  $placement2 = Get-MainPlacement -WorkArea @(1920, 40, 2560, 1400) -MainSize @(2400, 1350) -Insets @(0, 0, 0, 0)
  Test-Check (Test-Same $placement2.frame @(1920, 40, 2400, 1350)) '2 つ目の画面と作業領域の上の余白にも合わせる'

  Test-Check (Test-Same (Get-CropRect -WindowRect @(-7, 0, 1614, 907) -FrameRect @(0, 0, 1600, 900)) @(7, 0, 1600, 900)) '撮った絵を見える縁で切る'
  Test-Check (Test-Same (Get-CropRect -WindowRect @(0, 0, 100, 50) -FrameRect @(-5, 10, 200, 100)) @(0, 10, 100, 40)) '見える縁がはみ出していたら絵の中に切り詰める'

  $scaled = Get-ScaledSize -Width 2000 -Height 1125 -OutWidthPx 1600
  Test-Check ($scaled.width -eq 1600 -and $scaled.height -eq 900 -and $scaled.scale -eq 0.8) '幅 2000 (125%) を 1600 に縮め、高さも同じ比で縮める'
  $same = Get-ScaledSize -Width 1600 -Height 900 -OutWidthPx 1600
  Test-Check ($same.width -eq 1600 -and $same.height -eq 900 -and $same.scale -eq 1.0) '上限と同じ幅はそのまま'
  $small = Get-ScaledSize -Width 1200 -Height 700 -OutWidthPx 1600
  Test-Check ($small.width -eq 1200 -and $small.scale -eq 1.0) '上限より小さければ広げない'

  $tiles = Get-ReadyLabelTiles -Lines @(
    '12:00:00.000: [cordmux] Composite label ready: tile=2 cell=960x540 paint_ms=3',
    '12:00:00.100: [cordmux] Composite layout canvas=1920x1080 count=4',
    '12:00:00.200: [cordmux] Composite label ready: tile=0 cell=960x540 paint_ms=3',
    '12:00:00.300: [cordmux] Composite label ready: tile=2 cell=960x540 paint_ms=3')
  Test-Check (Test-Same $tiles @(0, 2)) 'ログから名前ラベルを描き終えたタイルの番号を重ねずに読む'
  $none = Get-ReadyLabelTiles -Lines @('12:00:00.000: [cordmux] Composite layout canvas=1920x1080 count=0')
  Test-Check ($none.Count -eq 0) '名前ラベルの行がなければ 0 枚'

  # 100% の浮いたドック: ウィンドウ (-7, 0) から、見える縁 (0, 0)、クライアント領域は左の縁 1 と上の 31 の内側。
  $click = Get-TitleDoubleClickPoint -FrameRect @(500, 300, 440, 700) -ClientRect @(501, 331, 438, 668)
  Test-Check (Test-Same $click @(498, 315)) 'ダブルクリックはタイトルバーの帯の中ほどで、クライアント領域の左の 3 物理画素外を押す'
  Test-Check ($click[1] -gt 300 -and $click[1] -lt 331) '押す高さはタイトルバーの帯の中'
  $click2 = Get-TitleDoubleClickPoint -FrameRect @(500, 300, 660, 1050) -ClientRect @(502, 346, 656, 1002)
  Test-Check ($click2[0] -lt 502 -and ((502 - $click2[0]) / 2.0) -ge 1.5) '200% でも論理の画素で 1.5 以上外 (丸めても範囲の外)'

  # 右の領域のドック: 幅 300 を 420 にするには、左の縁の 1 画素外を押して左へ 120 動かす。
  $drag = Get-SeparatorDrag -DockRect @(1180, 60, 300, 600) -ClientRect @(0, 31, 1600, 869) -TargetWidthPx 420 -Dpi 96
  Test-Check ($drag.edge -eq 'left' -and (Test-Same $drag.from @(1179, 360)) -and (Test-Same $drag.to @(1059, 360))) '右の領域のドックは左の縁の仕切りを左へ引っ張って広げる'
  $narrow = Get-SeparatorDrag -DockRect @(1100, 60, 500, 600) -ClientRect @(0, 31, 1600, 869) -TargetWidthPx 420 -Dpi 96
  Test-Check ((Test-Same $narrow.to @(1179, 360))) '広すぎれば右へ引っ張って狭める'
  $drag150 = Get-SeparatorDrag -DockRect @(1770, 90, 450, 900) -ClientRect @(0, 46, 2400, 1304) -TargetWidthPx 630 -Dpi 144
  Test-Check ((Test-Same $drag150.from @(1768, 540)) -and (Test-Same $drag150.to @(1588, 540))) '150% では縁から 2 物理画素外を押す'
  $dragLeft = Get-SeparatorDrag -DockRect @(0, 31, 300, 600) -ClientRect @(0, 31, 1600, 869) -TargetWidthPx 420 -Dpi 96
  Test-Check ($dragLeft.edge -eq 'right' -and (Test-Same $dragLeft.from @(300, 331)) -and (Test-Same $dragLeft.to @(420, 331))) '左の領域のドックは右の縁の仕切りを右へ引っ張って広げる'

  Test-Check (Test-DockWidth -WidthPx 421 -TargetLogical 420 -Dpi 96) '1 論理画素の差は合っているとみなす'
  Test-Check (-not (Test-DockWidth -WidthPx 423 -TargetLogical 420 -Dpi 96)) '3 論理画素の差は合っていない'
  Test-Check ((Test-DockWidth -WidthPx 527 -TargetLogical 420 -Dpi 120) -and -not (Test-DockWidth -WidthPx 528 -TargetLogical 420 -Dpi 120)) '125% では 525 から 2 物理画素の差まで受ける'

  $elements = @(
    [PSCustomObject]@{ name = 'cordmux'; className = 'QWidget'; rect = @(1180, 60, 420, 600); offscreen = $false },
    [PSCustomObject]@{ name = 'コントロール'; className = 'OBSDock'; rect = @(1200, 700, 400, 200); offscreen = $false },
    [PSCustomObject]@{ name = 'cordmux'; className = 'OBSDock'; rect = @(0, 0, 0, 0); offscreen = $false },
    [PSCustomObject]@{ name = 'cordmux'; className = 'OBSDock'; rect = @(1180, 60, 420, 600); offscreen = $false })
  $picked = Select-DockElement -Elements $elements -Title 'cordmux'
  Test-Check ($picked -and $picked.className -eq 'OBSDock' -and $picked.rect[2] -eq 420) 'ドックは名前とクラス名で選び、大きさのない物と中身のウィジェットは選ばない'
  Test-Check ($null -eq (Select-DockElement -Elements @($elements[0], $elements[1]) -Title 'cordmux')) 'ドッキングしたドックがなければ選ばない'

  $lp = [CordmuxReadmeShotNative]::MakeLParam(-5, 1200)
  $lp2 = [CordmuxReadmeShotNative]::MakeLParam(1600, -20)
  Test-Check ([CordmuxReadmeShotNative]::LParamX($lp) -eq -5 -and [CordmuxReadmeShotNative]::LParamY($lp) -eq 1200 -and [CordmuxReadmeShotNative]::LParamX($lp2) -eq 1600 -and [CordmuxReadmeShotNative]::LParamY($lp2) -eq -20) 'メッセージの座標は負の数も読み戻せる'

  # 左半分が赤、右半分が青の 200 x 100 を 100 x 50 に縮める。
  $src = New-Object System.Drawing.Bitmap(200, 100, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
  $g = [System.Drawing.Graphics]::FromImage($src)
  $g.Clear([System.Drawing.Color]::FromArgb(255, 200, 0, 0))
  $blueBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 0, 0, 200))
  $g.FillRectangle($blueBrush, 100, 0, 100, 100)
  $blueBrush.Dispose()
  $g.Dispose()
  $shrunk = New-ScaledImage -Source $src -Width 100 -Height 50
  $p1 = $shrunk.GetPixel(20, 25)
  $p2 = $shrunk.GetPixel(80, 25)
  $edge = $shrunk.GetPixel(0, 0)
  $corner = $shrunk.GetPixel(99, 49)
  Test-Check ($shrunk.Width -eq 100 -and $shrunk.Height -eq 50) '縮めた絵は 100 x 50'
  Test-Check ($p1.R -gt 150 -and $p1.B -lt 50 -and $p2.B -gt 150 -and $p2.R -lt 50) '縮めても左右の絵の位置は変わらない'
  Test-Check ($edge.R -gt 150 -and $corner.B -gt 150) '縮めても縁が暗くならない'
  $shrunk.Dispose()
  $src.Dispose()

  # List[object] に @() を使うと Windows PowerShell 5.1 は「引数の型が一致しません」で止まるので、ToArray で配列にする。
  $st = [PSCustomObject]@{ ok = ($failures.Count -eq 0); checks = $checks.Count; failures = $failures.ToArray(); results = $checks.ToArray() }
  Write-JsonResultAndExit -Result $st -ExitCode ($(if ($st.ok) { 0 } else { 1 }))
}

# ---- ウィンドウとドックを探す (ウィンドウに触らない) --------------------------------

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

# Hwnd の UI Automation の要素のうち、Scope と Condition に合う物の名前、クラス名、四角 (物理の画素)、見えていないか。
function Get-UiaElements {
  param([IntPtr]$Hwnd, [System.Windows.Automation.TreeScope]$Scope, [System.Windows.Automation.Condition]$Condition)
  $root = [System.Windows.Automation.AutomationElement]::FromHandle($Hwnd)
  $list = @()
  foreach ($el in $root.FindAll($Scope, $Condition)) {
    try {
      $c = $el.Current
      $b = $c.BoundingRectangle
      $rect = if ($b.IsEmpty) { @(0, 0, 0, 0) } else { @([int][math]::Round($b.X), [int][math]::Round($b.Y), [int][math]::Round($b.Width), [int][math]::Round($b.Height)) }
      $list += [PSCustomObject]@{ name = $c.Name; className = $c.ClassName; rect = $rect; offscreen = [bool]$c.IsOffscreen }
    }
    catch {
      # 読んでいる間に消えた要素 (Qt が並べ直した後など) は数えない。
    }
  }
  return , $list
}

# 主ウィンドウにドッキングしているドック (UI Automation の子のうち、クラス名に Dock を含み、見えていて大きさのある物)。
# 浮いたドックは主ウィンドウの子に出ない (Qt はウィンドウになったウィジェットを子に数えない) ので、ここに出るのはドッキングした物だけ。
function Get-DockedDocks {
  param([IntPtr]$MainHwnd)
  $kids = Get-UiaElements -Hwnd $MainHwnd -Scope ([System.Windows.Automation.TreeScope]::Children) -Condition ([System.Windows.Automation.Condition]::TrueCondition)
  return , @($kids | Where-Object { $_.className -match 'Dock' -and -not $_.offscreen -and $_.rect[2] -gt 0 -and $_.rect[3] -gt 0 })
}

# 主ウィンドウにドッキングした cordmux のドック。子になければ、名前で子孫も探す (タブにまとめられたドックは子の下にある)。
function Find-DockedDock {
  param([IntPtr]$MainHwnd)
  $hit = Select-DockElement -Elements (Get-DockedDocks -MainHwnd $MainHwnd) -Title $DockTitle
  if ($hit) { return $hit }
  $byName = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty, $DockTitle)
  return Select-DockElement -Elements (Get-UiaElements -Hwnd $MainHwnd -Scope ([System.Windows.Automation.TreeScope]::Descendants) -Condition $byName) -Title $DockTitle
}

# 浮いたドックのウィンドウ (タイトルが -DockTitle の見えているトップレベルウィンドウ)。なければ IntPtr.Zero。
function Find-FloatingDock {
  $hit = [CordmuxReadmeShotNative]::TopLevelWindows([uint32]$TargetPid) | Where-Object { $_.Value -eq $DockTitle } | Select-Object -First 1
  if ($hit) { return $hit.Key }
  return [IntPtr]::Zero
}

# ---- 撮る ----------------------------------------------------------------------

$result = [ordered]@{ ok = $false; pid = $null; method = $Method; error = $null }
try {
  $physical = [CordmuxReadmeShotNative]::UsePhysicalPixels()
  $result.physicalPixels = $physical

  $info = $null
  if ($HoldInfo) {
    # 前の実行の hold-<tag>.json が残っていることがあるので、書かれていて、その OBS が動いているまで待つ (hold と同時に起動してよい)。
    $deadline = (Get-Date).AddSeconds($WaitSeconds)
    while ($true) {
      if (Test-Path -LiteralPath $HoldInfo) {
        $info = Get-Content -LiteralPath $HoldInfo -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($info.obsPid -and (Get-Process -Id ([int]$info.obsPid) -ErrorAction SilentlyContinue)) { break }
      }
      if ((Get-Date) -gt $deadline) { throw "動いている OBS の hold-<tag>.json が $WaitSeconds 秒たっても書かれない: $HoldInfo" }
      Start-Sleep -Milliseconds 1000
    }
    $HoldInfo = (Resolve-Path -LiteralPath $HoldInfo).ProviderPath
    if ($TargetPid -le 0) { $TargetPid = [int]$info.obsPid }
    if (-not $OutDir) { $OutDir = Split-Path -Parent $HoldInfo }
  }
  $result.pid = $TargetPid
  if ($TargetPid -le 0) { throw '-HoldInfo か -Pid が要る' }
  if (-not $OutDir) { throw '-HoldInfo を渡さないときは -OutDir が要る' }
  if (-not (Get-Process -Id $TargetPid -ErrorAction SilentlyContinue)) { throw "pid $TargetPid のプロセスが見つからない" }
  if (-not (Test-Path -LiteralPath $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
  # Bitmap.Save は .NET のカレントディレクトリで相対の場所を読むので、PowerShell の場所から絶対の場所にしておく。
  $OutDir = (Resolve-Path -LiteralPath $OutDir).ProviderPath

  # 声に音を乗せる (中身が空なら全部の声。test/tools/harness の README の「声に音を乗せる」)。
  $result.voiceTone = $null
  if ($VoiceOn) {
    $toneFile = if ($info) { $info.voiceToneFile } else { $null }
    if (-not $toneFile) { throw '-VoiceOn には hold を --voice-users で起動した hold-<tag>.json が要る (voiceToneFile がない)' }
    [System.IO.File]::WriteAllText($toneFile, '')
    $result.voiceTone = [PSCustomObject]@{ file = $toneFile; written = $true }
  }

  # 名前ラベルを描き終えるまで待つ (OBS が書いているログを、書き込みを妨げずに読む)。
  $result.labelsReady = $null
  $result.labelTiles = @()
  $logDir = if ($info -and $info.obsConfigDir) { Join-Path $info.obsConfigDir 'logs' } else { $null }
  if ($ExpectLabels -gt 0 -and $logDir) {
    $deadline = (Get-Date).AddSeconds($WaitSeconds)
    $ready = @()
    while ($true) {
      $newest = Get-ChildItem -LiteralPath $logDir -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
      if ($newest) {
        $stream = [System.IO.File]::Open($newest.FullName, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        try {
          $reader = New-Object System.IO.StreamReader($stream, [System.Text.Encoding]::UTF8)
          $ready = Get-ReadyLabelTiles -Lines ($reader.ReadToEnd() -split "`r?`n")
          $reader.Dispose()
        }
        finally {
          $stream.Dispose()
        }
      }
      if (@($ready).Count -ge $ExpectLabels -or (Get-Date) -gt $deadline) { break }
      Start-Sleep -Milliseconds 1000
    }
    $result.labelTiles = @($ready)
    $result.labelsReady = (@($ready).Count -ge $ExpectLabels)
  }

  $windows = [CordmuxReadmeShotNative]::TopLevelWindows([uint32]$TargetPid)
  $mainHit = $windows | Where-Object { $_.Value.StartsWith('OBS ') } | Select-Object -First 1
  if (-not $mainHit) {
    $titles = @($windows | ForEach-Object { $_.Value }) -join ' | '
    throw ("OBS の主ウィンドウが見つからない。見えているウィンドウ: " + $titles)
  }
  $main = $mainHit.Key
  if ([CordmuxReadmeShotNative]::IsZoomed($main) -or [CordmuxReadmeShotNative]::IsIconic($main)) {
    [void][CordmuxReadmeShotNative]::ShowWindow($main, [CordmuxReadmeShotNative]::SW_SHOWNOACTIVATE)
    Start-Sleep -Milliseconds 300
  }
  $result.before = [PSCustomObject]@{ main = [CordmuxReadmeShotNative]::FrameRect($main) }

  # ドッキングする (浮いていれば)。
  $docking = [ordered]@{ wasFloating = $false; attempts = 0; points = @() }
  $result.docking = $docking
  $floating = Find-FloatingDock
  if ($floating -ne [IntPtr]::Zero) {
    $docking.wasFloating = $true
    while ($floating -ne [IntPtr]::Zero -and $docking.attempts -lt $dockTries) {
      $docking.attempts++
      $client = [CordmuxReadmeShotNative]::ClientRectOnScreen($floating)
      if (-not $client) { throw '浮いたドックのクライアント領域を読めない' }
      $point = Get-TitleDoubleClickPoint -FrameRect ([CordmuxReadmeShotNative]::FrameRect($floating)) -ClientRect $client
      $docking.points += , $point
      $wm = [CordmuxReadmeShotNative]::WM_NCLBUTTONDBLCLK
      $ht = [CordmuxReadmeShotNative]::HTCAPTION
      $posted = [CordmuxReadmeShotNative]::Post($floating, $wm, $ht, $point[0], $point[1])
      Start-Sleep -Milliseconds $doubleClickGapMs
      $posted = [CordmuxReadmeShotNative]::Post($floating, $wm, $ht, $point[0], $point[1]) -and $posted
      if (-not $posted) { throw '浮いたドックにメッセージを置けない (PostMessage が失敗した)' }
      $until = (Get-Date).AddMilliseconds($dockWaitMs)
      do {
        Start-Sleep -Milliseconds 200
        $floating = Find-FloatingDock
      } while ($floating -ne [IntPtr]::Zero -and (Get-Date) -lt $until)
    }
    if ($floating -ne [IntPtr]::Zero) {
      throw ("浮いたドック (" + $DockTitle + ") のタイトルバーを " + $dockTries + " 回ダブルクリックしても、ドッキングしない (浮いたウィンドウが残っている)。" +
        "撮る間に OBS の上でマウスを動かすと Qt がダブルクリックと数えないので、触らずにもう一度実行する。続くときは OBS の [ドック] のメニューで直す")
    }
  }
  $dockEl = $null
  $until = (Get-Date).AddMilliseconds($dockWaitMs)
  while ($true) {
    $dockEl = Find-DockedDock -MainHwnd $main
    if ($dockEl -or (Get-Date) -gt $until) { break }
    Start-Sleep -Milliseconds 200
  }
  if (-not $dockEl) {
    $names = @(Get-DockedDocks -MainHwnd $main | ForEach-Object { $_.name }) -join ' | '
    $state = if ($docking.wasFloating) { '浮いたウィンドウは消えたが' } else { '浮いたウィンドウもなく' }
    throw ("ドック (" + $DockTitle + ") が主ウィンドウの中に見つからない (" + $state + "、UI Automation の子にない)。" +
      "閉じているときは OBS の [ドック] のメニューで表示する。主ウィンドウの中のドック: " + $names)
  }

  # 主ウィンドウを置く。見えない縁の幅は置いた後の状態で読み直し、2 回置く (縁の幅は DPI と枠の形で決まるので、2 回目でそろう)。
  $dpi = [int][CordmuxReadmeShotNative]::GetDpiForWindow($main)
  $work = [CordmuxReadmeShotNative]::WorkArea($main)
  if (-not $work) { throw 'ウィンドウのある画面の作業領域を読めない' }
  $mainSize = @((ConvertTo-PhysicalPx -Logical $Width -Dpi $dpi), (ConvertTo-PhysicalPx -Logical $Height -Dpi $dpi))
  function Set-MainPlacement {
    param([int]$Passes)
    for ($pass = 0; $pass -lt $Passes; $pass++) {
      $insets = Get-FrameInsets -WindowRect ([CordmuxReadmeShotNative]::Rect($main)) -FrameRect ([CordmuxReadmeShotNative]::FrameRect($main))
      $placement = Get-MainPlacement -WorkArea $work -MainSize $mainSize -Insets $insets
      [CordmuxReadmeShotNative]::Place($main, $placement.window)
      Start-Sleep -Milliseconds 300
    }
  }
  Set-MainPlacement -Passes 2
  Start-Sleep -Milliseconds 500

  # ドックの幅を合わせる。
  $dockInfo = [ordered]@{ name = $DockTitle; requested = $null; rect = $null; widthLogical = $null; widthOk = $null; drags = @() }
  $result.dock = $dockInfo
  $dockEl = Find-DockedDock -MainHwnd $main
  if (-not $dockEl) { throw ("主ウィンドウを置いた後に、ドック (" + $DockTitle + ") が見つからない") }
  if ($DockWidth -gt 0) {
    $dockInfo.requested = $DockWidth
    $targetPx = ConvertTo-PhysicalPx -Logical $DockWidth -Dpi $dpi
    while (-not (Test-DockWidth -WidthPx $dockEl.rect[2] -TargetLogical $DockWidth -Dpi $dpi) -and $dockInfo.drags.Count -lt $dockWidthTries) {
      $client = [CordmuxReadmeShotNative]::ClientRectOnScreen($main)
      if (-not $client) { throw '主ウィンドウのクライアント領域を読めない' }
      $drag = Get-SeparatorDrag -DockRect $dockEl.rect -ClientRect $client -TargetWidthPx $targetPx -Dpi $dpi
      $from = [CordmuxReadmeShotNative]::ToClient($main, $drag.from[0], $drag.from[1])
      $to = [CordmuxReadmeShotNative]::ToClient($main, $drag.to[0], $drag.to[1])
      $mk = [CordmuxReadmeShotNative]::MK_LBUTTON
      $posted = [CordmuxReadmeShotNative]::Post($main, [CordmuxReadmeShotNative]::WM_LBUTTONDOWN, $mk, $from[0], $from[1])
      Start-Sleep -Milliseconds 150
      $posted = [CordmuxReadmeShotNative]::Post($main, [CordmuxReadmeShotNative]::WM_MOUSEMOVE, $mk, $to[0], $to[1]) -and $posted
      Start-Sleep -Milliseconds 250
      $posted = [CordmuxReadmeShotNative]::Post($main, [CordmuxReadmeShotNative]::WM_LBUTTONUP, 0, $to[0], $to[1]) -and $posted
      if (-not $posted) { throw '主ウィンドウにメッセージを置けない (PostMessage が失敗した)' }
      Start-Sleep -Milliseconds 500
      $after = Find-DockedDock -MainHwnd $main
      if (-not $after) { throw ("仕切りを引っ張った後に、ドック (" + $DockTitle + ") が見つからない") }
      $dockInfo.drags += [PSCustomObject]@{ edge = $drag.edge; from = $drag.from; to = $drag.to; widthBefore = $dockEl.rect[2]; widthAfter = $after.rect[2] }
      if ($after.rect[2] -eq $dockEl.rect[2]) {
        $dockInfo.rect = $after.rect
        throw ("ドックの幅が変わらない (" + $drag.edge + " の縁の仕切りを画面の (" + ($drag.from -join ', ') + ") で押して (" + ($drag.to -join ', ') +
          ") まで引っ張った。幅は物理の画素で " + $after.rect[2] + ")。押した所が仕切りでないか、Qt が最小か最大の幅で止めている。" +
          "-DockWidth を変えるか、0 (幅を変えない) で撮る")
      }
      $dockEl = $after
    }
  }
  # ドッキングした直後のドックの幅で主ウィンドウの最小の大きさが -Width を超えていると、上で置いた大きさは Qt に止められている。
  # 幅を合わせた後にもう一度置き、ドックを読み直す。
  Set-MainPlacement -Passes 1
  $dockEl = Find-DockedDock -MainHwnd $main
  if (-not $dockEl) { throw ("主ウィンドウを置き直した後に、ドック (" + $DockTitle + ") が見つからない") }
  if ($DockWidth -gt 0) { $dockInfo.widthOk = (Test-DockWidth -WidthPx $dockEl.rect[2] -TargetLogical $DockWidth -Dpi $dpi) }
  $dockInfo.rect = $dockEl.rect
  $dockInfo.widthLogical = ConvertTo-LogicalPx -Physical $dockEl.rect[2] -Dpi $dpi
  if ($dockInfo.widthOk -eq $false) {
    throw ("ドックの幅が " + $DockWidth + " にならない (" + $dockWidthTries + " 回引っ張って、論理の画素で " + $dockInfo.widthLogical + ")。" +
      "Qt がドックかプレビューの最小の幅で止めている。-DockWidth を変えるか、0 (幅を変えない) で撮る")
  }
  # 主ウィンドウの中のドックの並び (コントロールのドックとの上下と左右を、撮った後に数で確かめるため)。
  $result.docks = Get-DockedDocks -MainHwnd $main
  Start-Sleep -Milliseconds $SettleMs

  # 撮る。
  if ($Method -eq 'screen') {
    [CordmuxReadmeShotNative]::SetTopmost($main, $true)
    Start-Sleep -Milliseconds 500
  }
  try {
    $windowRect = [CordmuxReadmeShotNative]::Rect($main)
    $frameRect = [CordmuxReadmeShotNative]::FrameRect($main)
    if ($Method -eq 'screen') {
      $bitmap = New-Object System.Drawing.Bitmap($frameRect[2], $frameRect[3], [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
      $g = [System.Drawing.Graphics]::FromImage($bitmap)
      try { $g.CopyFromScreen($frameRect[0], $frameRect[1], 0, 0, (New-Object System.Drawing.Size($frameRect[2], $frameRect[3]))) }
      finally { $g.Dispose() }
    }
    else {
      # 24 bit で撮る (32 bit で撮ると、GDI が書かないアルファの値で透明になる所が出うる)。
      $full = New-Object System.Drawing.Bitmap($windowRect[2], $windowRect[3], [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
      $g = [System.Drawing.Graphics]::FromImage($full)
      try {
        $hdc = $g.GetHdc()
        try { [void][CordmuxReadmeShotNative]::PrintWindow($main, $hdc, [CordmuxReadmeShotNative]::PW_RENDERFULLCONTENT) }
        finally { $g.ReleaseHdc($hdc) }
      }
      finally { $g.Dispose() }
      $crop = Get-CropRect -WindowRect $windowRect -FrameRect $frameRect
      $bitmap = $full.Clone((New-Object System.Drawing.Rectangle($crop[0], $crop[1], $crop[2], $crop[3])), $full.PixelFormat)
      $full.Dispose()
    }
  }
  finally {
    if ($Method -eq 'screen') { [CordmuxReadmeShotNative]::SetTopmost($main, $false) }
  }

  $mainPath = Join-Path $OutDir ($Prefix + '-obs.png')
  $shotPath = Join-Path $OutDir ($Prefix + '-shot.png')
  $bitmap.Save($mainPath, [System.Drawing.Imaging.ImageFormat]::Png)
  $size = Get-ScaledSize -Width $bitmap.Width -Height $bitmap.Height -OutWidthPx $OutWidth
  if ($size.scale -lt 1.0) {
    $shot = New-ScaledImage -Source $bitmap -Width $size.width -Height $size.height
    $shot.Save($shotPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $shot.Dispose()
  }
  else {
    $bitmap.Save($shotPath, [System.Drawing.Imaging.ImageFormat]::Png)
  }

  $result.ok = $true
  $result.dpi = $dpi
  $result.workArea = $work
  # 主ウィンドウが求めた大きさになったか (物理の画素で 2 まで。Qt か Windows が最小や画面の大きさで止めると偽)。
  $sizeOk = ([math]::Abs($frameRect[2] - $mainSize[0]) -le 2) -and ([math]::Abs($frameRect[3] - $mainSize[1]) -le 2)
  $result.main = [PSCustomObject]@{ window = $windowRect; frame = $frameRect; requested = $mainSize; sizeOk = $sizeOk; png = $mainPath; width = $bitmap.Width; height = $bitmap.Height }
  $result.shot = [PSCustomObject]@{ png = $shotPath; width = $size.width; height = $size.height; scale = [math]::Round($size.scale, 4) }
  # 作業領域の外にはみ出した部分があるか (-Method screen では、はみ出した所は撮れていない)。
  $result.beyondWorkArea = (($frameRect[0] + $frameRect[2]) -gt ($work[0] + $work[2])) -or (($frameRect[1] + $frameRect[3]) -gt ($work[1] + $work[3]))
  $bitmap.Dispose()
  Write-JsonResultAndExit -Result ([PSCustomObject]$result) -ExitCode 0
}
catch {
  $result.ok = $false
  $result.error = $_.Exception.Message
  Write-JsonResultAndExit -Result ([PSCustomObject]$result) -ExitCode 1
}
