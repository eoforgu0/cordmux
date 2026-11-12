#requires -Version 5.1
<#
  window-size.ps1 - 実機の OBS の名指したウィンドウ (設定のウィンドウ・確認のウィンドウ・主ウィンドウなど) の大きさを替え、Windows と Qt が止めた大きさを
  読むツール。

  設定のウィンドウを最小の高さに縮めたときのフォーカスの行き先や、主ウィンドウの幅を替えた直後の押下の守りを見るために使う。
  dock-width.ps1 は主ウィンドウと浮いたドックの幅だけを縮めるツールで、ほかのウィンドウと
  高さを扱えない。人がウィンドウの縁をつまむと、どこで止まったかを数で読めない。Windows に大きさを依頼し、Qt が許した最小
  (WM_GETMINMAXINFO) で止まった大きさを読む。

  する事:
    1. -Pid のプロセスの、タイトルが -Title と同じ (または -TitlePrefix で始まる) 見えているトップレベルウィンドウを探す (-Timeout の間)。
       見えていないウィンドウは扱わない。-ProcessName を渡したときは、その名前のプロセス (chrome なら全部) の、見えていて最小化されて
       いないトップレベルウィンドウのうち一番大きいウィンドウを探す (Chrome の主ウィンドウのタイトルは前面のタブのタイトルなのでタイトルでは探せない)。-Title と
       -TitlePrefix は、渡せば絞りに使う (どちらも渡さなくてよい)。
    2. 現在の四角を保存し、依頼する大きさを決める: -Min height | width | both はその向きを 1 px に (Qt の最小で止まる)、-Width / -Height は
       その値 (物理の画素)。渡さなかった向きは現在のまま。
    3. SetWindowPos で大きさだけを替える (動かさない・前面に出さない = SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)。
       最大化されているウィンドウは、先に ShowWindow(SW_SHOWNOACTIVATE) で通常の大きさへ戻してから替える (結果の zoomed が真)。
       -Maximize は大きさを替えずに、ウィンドウを最大化へ戻す (最大化していたウィンドウを縮めた後の戻し。SW_MAXIMIZE はウィンドウを前面に出しうる)。
    4. -SettleMs 待って、止まった四角とクライアントの大きさを、物理の画素と論理の画素 (GetDpiForWindow で 96 DPI 換算) で読む。
       依頼した大きさと違えば clamped が真 (Qt か Windows が止めた)。
    5. -NoRestore が無ければ保存した四角へ戻し、戻った四角も書く。縮めたまま Tab で回るときは -NoRestore で縮め、終えたら
       結果の before の幅と高さを -Width / -Height に渡してもう 1 回呼ぶ (-NoRestore つき)。

  結果は 1 行の JSON を標準出力に書く (screenshot.ps1 と同じ UTF-8)。
    { "ok", "pid", "title", "zoomed", "maximized", "dpi", "before": [x,y,w,h], "requested": [w,h], "after": [x,y,w,h], "client": [w,h],
      "afterLogical": [w,h], "clamped": { "width", "height" }, "restored": [x,y,w,h]|null, "error" }

  -SelfTest: 表に出さない WinForms のウィンドウ (最小の大きさを決めたもの) を自分で作り、同じ縮め方で最小に止まることと、見えないウィンドウは
  名指しても扱わないことを確認する。画面にウィンドウは出ない。
#>

[CmdletBinding()]
param(
  [Alias('Pid')]
  [int]$TargetPid = 0,

  # -Pid の代わりに、プロセスの名前 (拡張子なし。chrome など) でウィンドウを探す。そのプロセスの全部の中の一番大きいウィンドウを扱う。
  [string]$ProcessName = '',

  [string]$Title = '',
  [string]$TitlePrefix = '',

  [ValidateSet('', 'height', 'width', 'both')]
  [string]$Min = '',

  [ValidateRange(0, 16000)]
  [int]$Width = 0,

  [ValidateRange(0, 16000)]
  [int]$Height = 0,

  # 大きさを替えてから読むまでの待ち (ミリ秒)。Qt が並べ直して描き直すまで (dock-width.ps1 と同じ)。
  [ValidateRange(0, 10000)]
  [int]$SettleMs = 500,

  [ValidateRange(0, 60000)]
  [int]$Timeout = 5000,

  [switch]$NoRestore,

  # 大きさを替えずにウィンドウを最大化へ戻す (最大化していたウィンドウを縮めた後の戻し)。
  [switch]$Maximize,
  [switch]$SelfTest
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
using System.Runtime.InteropServices;
using System.Text;

public static class CordmuxWindowSizeNative
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
    public static extern bool GetClientRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("user32.dll")]
    public static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter, int X, int Y, int cx, int cy, uint uFlags);
    [DllImport("user32.dll")]
    public static extern uint GetDpiForWindow(IntPtr hwnd);
    [DllImport("user32.dll")]
    public static extern bool IsIconic(IntPtr hWnd);
    [DllImport("user32.dll")]
    public static extern bool IsZoomed(IntPtr hWnd);
    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

    public const int SW_MAXIMIZE = 3;
    public const int SW_SHOWNOACTIVATE = 4;
    public const uint GA_ROOT = 2;
    public const uint SWP_NOMOVE = 0x0002;
    public const uint SWP_NOZORDER = 0x0004;
    public const uint SWP_NOACTIVATE = 0x0010;

    public static string TitleOf(IntPtr hWnd)
    {
        int len = GetWindowTextLength(hWnd);
        if (len <= 0) return "";
        var sb = new StringBuilder(len + 1);
        GetWindowText(hWnd, sb, sb.Capacity);
        return sb.ToString();
    }

    // Visible top-level window of the process whose title equals (prefix == false) or starts with (prefix == true) the text.
    public static IntPtr FindTopLevel(uint targetProcessId, string text, bool prefix)
    {
        IntPtr found = IntPtr.Zero;
        EnumWindows(delegate (IntPtr hWnd, IntPtr lParam)
        {
            uint pid;
            GetWindowThreadProcessId(hWnd, out pid);
            if (pid != targetProcessId || !IsWindowVisible(hWnd) || GetAncestor(hWnd, GA_ROOT) != hWnd) return true;
            string t = TitleOf(hWnd);
            bool hit = prefix ? t.StartsWith(text, StringComparison.Ordinal) : t == text;
            if (hit) { found = hWnd; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    // Largest visible, not minimized top-level window among the processes. A non-empty text narrows it by title
    // (equal, or starts with when prefix is true).
    public static IntPtr FindLargestOfProcesses(uint[] processIds, string text, bool prefix)
    {
        IntPtr found = IntPtr.Zero;
        long best = 0;
        EnumWindows(delegate (IntPtr hWnd, IntPtr lParam)
        {
            uint pid;
            GetWindowThreadProcessId(hWnd, out pid);
            if (Array.IndexOf(processIds, pid) < 0 || !IsWindowVisible(hWnd) || IsIconic(hWnd) || GetAncestor(hWnd, GA_ROOT) != hWnd) return true;
            if (!string.IsNullOrEmpty(text))
            {
                string t = TitleOf(hWnd);
                bool hit = prefix ? t.StartsWith(text, StringComparison.Ordinal) : t == text;
                if (!hit) return true;
            }
            RECT r;
            GetWindowRect(hWnd, out r);
            long area = (long)(r.Right - r.Left) * (r.Bottom - r.Top);
            if (area > best) { best = area; found = hWnd; }
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

    public static int[] Client(IntPtr hWnd)
    {
        RECT r;
        GetClientRect(hWnd, out r);
        return new int[] { r.Right - r.Left, r.Bottom - r.Top };
    }

    // Size only: no move, no z-order change, no activation.
    public static bool Resize(IntPtr hWnd, int width, int height)
    {
        return SetWindowPos(hWnd, IntPtr.Zero, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    public static bool Place(IntPtr hWnd, int[] rect)
    {
        return SetWindowPos(hWnd, IntPtr.Zero, rect[0], rect[1], rect[2], rect[3], SWP_NOZORDER | SWP_NOACTIVATE);
    }
}
"@
Add-Type -TypeDefinition $nativeSource

function Get-Logical {
  param([double]$Px, [int]$Dpi)
  if ($Dpi -le 0) { return $null }
  return [math]::Round($Px * 96.0 / $Dpi, 1)
}

# 依頼する大きさ (物理のピクセル)。Min が優先し、次に Width / Height、どれも無ければ現在のまま。
function Get-RequestedSize {
  param([int[]]$Before, [string]$MinMode, [int]$W, [int]$H)
  $rw = $Before[2]
  $rh = $Before[3]
  if ($W -gt 0) { $rw = $W }
  if ($H -gt 0) { $rh = $H }
  if ($MinMode -eq 'width' -or $MinMode -eq 'both') { $rw = 1 }
  if ($MinMode -eq 'height' -or $MinMode -eq 'both') { $rh = 1 }
  return @($rw, $rh)
}

# 1 つのウィンドウの大きさを替えて読み、(既定で) 戻す。セルフテストと本番で同じ道を通す。
function Invoke-Resize {
  param([IntPtr]$Hwnd, [int[]]$Requested, [int]$Settle, [bool]$Restore)
  $dpi = [int][CordmuxWindowSizeNative]::GetDpiForWindow($Hwnd)
  $before = [CordmuxWindowSizeNative]::Rect($Hwnd)
  $applied = [CordmuxWindowSizeNative]::Resize($Hwnd, $Requested[0], $Requested[1])
  Start-Sleep -Milliseconds $Settle
  $after = [CordmuxWindowSizeNative]::Rect($Hwnd)
  $client = [CordmuxWindowSizeNative]::Client($Hwnd)
  $restored = $null
  if ($Restore) {
    [void][CordmuxWindowSizeNative]::Place($Hwnd, $before)
    Start-Sleep -Milliseconds $Settle
    $restored = [CordmuxWindowSizeNative]::Rect($Hwnd)
  }
  return [PSCustomObject]@{
    dpi          = $dpi
    before       = $before
    requested    = $Requested
    applied      = $applied
    after        = $after
    client       = $client
    afterLogical = @((Get-Logical -Px $after[2] -Dpi $dpi), (Get-Logical -Px $after[3] -Dpi $dpi))
    clientLogical = @((Get-Logical -Px $client[0] -Dpi $dpi), (Get-Logical -Px $client[1] -Dpi $dpi))
    clamped      = [PSCustomObject]@{ width = ($after[2] -ne $Requested[0]); height = ($after[3] -ne $Requested[1]) }
    restored     = $restored
  }
}

function Find-Window {
  # Largest が真なら、ProcIds の全部のプロセスの中の一番大きいウィンドウ (タイトルは、渡されていれば絞りに使う)。偽なら ProcIds[0] のタイトルの一致するウィンドウ。
  param([int[]]$ProcIds, [string]$ExactTitle, [string]$Prefix, [int]$WaitMs, [bool]$Largest = $false)
  $deadline = (Get-Date).AddMilliseconds($WaitMs)
  do {
    if ($Largest) {
      $filter = if ($Prefix) { $Prefix } else { $ExactTitle }
      $ids = [uint32[]]@($ProcIds | ForEach-Object { [uint32]$_ })
      $h = [CordmuxWindowSizeNative]::FindLargestOfProcesses($ids, $filter, [bool]$Prefix)
    } elseif ($Prefix) {
      $h = [CordmuxWindowSizeNative]::FindTopLevel([uint32]$ProcIds[0], $Prefix, $true)
    } else {
      $h = [CordmuxWindowSizeNative]::FindTopLevel([uint32]$ProcIds[0], $ExactTitle, $false)
    }
    if ($h -ne [IntPtr]::Zero) { return $h }
    Start-Sleep -Milliseconds 200
  } while ((Get-Date) -lt $deadline)
  return [IntPtr]::Zero
}

# ---- セルフテスト --------------------------------------------------------------------------------
function Invoke-SelfTest {
  Add-Type -AssemblyName System.Windows.Forms
  $failures = New-Object System.Collections.ArrayList
  $checks = New-Object System.Collections.ArrayList
  function Check {
    param([bool]$Cond, [string]$Label)
    [void]$checks.Add([PSCustomObject]@{ ok = $Cond; label = $Label })
    if (-not $Cond) { [void]$failures.Add($Label) }
  }
  $form = $null
  try {
    $r = Get-RequestedSize -Before @(0, 0, 800, 600) -MinMode 'height' -W 0 -H 0
    Check ($r[0] -eq 800 -and $r[1] -eq 1) 'min height keeps the width'
    $r = Get-RequestedSize -Before @(0, 0, 800, 600) -MinMode 'both' -W 0 -H 0
    Check ($r[0] -eq 1 -and $r[1] -eq 1) 'min both'
    $r = Get-RequestedSize -Before @(0, 0, 800, 600) -MinMode '' -W 1024 -H 0
    Check ($r[0] -eq 1024 -and $r[1] -eq 600) 'explicit width keeps the height'
    $r = Get-RequestedSize -Before @(0, 0, 800, 600) -MinMode 'width' -W 1024 -H 700
    Check ($r[0] -eq 1 -and $r[1] -eq 700) 'min wins over an explicit size on its own side'

    # 表に出さないウィンドウ (最小 320x240)。WinForms は WM_GETMINMAXINFO で MinimumSize を返すので、Qt のウィンドウと同じく Windows が止める。
    $form = New-Object System.Windows.Forms.Form
    $form.Text = 'cordmux-window-size-selftest-' + $PID
    $form.ShowInTaskbar = $false
    $form.StartPosition = 'Manual'
    $form.Location = New-Object System.Drawing.Point(-32000, -32000)
    $form.Size = New-Object System.Drawing.Size(640, 480)
    $form.MinimumSize = New-Object System.Drawing.Size(320, 240)
    $h = $form.Handle
    Check (-not [CordmuxWindowSizeNative]::IsWindowVisible($h)) 'the self-test window is not visible'
    $found = Find-Window -ProcIds @($PID) -ExactTitle $form.Text -Prefix '' -WaitMs 300
    Check ($found -eq [IntPtr]::Zero) 'a hidden window is not a target'
    $foundLargest = Find-Window -ProcIds @($PID) -ExactTitle $form.Text -Prefix '' -WaitMs 300 -Largest $true
    Check ($foundLargest -eq [IntPtr]::Zero) 'a hidden window is not the largest window of its process'
    $foundNone = Find-Window -ProcIds @(0) -ExactTitle '' -Prefix '' -WaitMs 100 -Largest $true
    Check ($foundNone -eq [IntPtr]::Zero) 'a process id without windows finds no window'
    Check (-not [CordmuxWindowSizeNative]::IsZoomed($h)) 'a normal window is not zoomed'
    Check (@(Get-Process -Name 'cordmux-no-such-process' -ErrorAction SilentlyContinue).Count -eq 0) 'an unknown process name has no process'

    $res = Invoke-Resize -Hwnd $h -Requested (Get-RequestedSize -Before ([CordmuxWindowSizeNative]::Rect($h)) -MinMode 'both' -W 0 -H 0) -Settle 50 -Restore $true
    Check ($res.after[2] -eq 320 -and $res.after[3] -eq 240) ('stopped at the minimum (' + $res.after[2] + 'x' + $res.after[3] + ')')
    Check ($res.clamped.width -and $res.clamped.height) 'clamped on both sides'
    Check ($res.restored[2] -eq 640 -and $res.restored[3] -eq 480) 'restored to the size before'
    Check (-not [CordmuxWindowSizeNative]::IsWindowVisible($h)) 'still not visible after resizing'
    $res2 = Invoke-Resize -Hwnd $h -Requested @(700, 500) -Settle 50 -Restore $false
    Check ($res2.after[2] -eq 700 -and $res2.after[3] -eq 500 -and -not $res2.clamped.width -and -not $res2.clamped.height) 'an allowed size is applied as asked'
    Check ($null -eq $res2.restored) 'no restore with -NoRestore'
    Check ($res2.dpi -gt 0) 'the DPI is read'
  }
  catch {
    [void]$failures.Add('exception: ' + $_.Exception.Message)
  }
  finally {
    if ($form) { $form.Dispose() }
  }
  return [PSCustomObject]@{ ok = ($failures.Count -eq 0); selftest = $true; checks = $checks.ToArray(); failures = $failures.ToArray() }
}

try {
  if ($SelfTest) {
    $st = Invoke-SelfTest
    Write-JsonResultAndExit -Result $st -ExitCode ($(if ($st.ok) { 0 } else { 1 }))
  }
  if ($TargetPid -le 0 -and -not $ProcessName) { throw '-Pid (obsPid of hold-<tag>.json) or -ProcessName is required' }
  if ($TargetPid -gt 0 -and $ProcessName) { throw 'pass only one of -Pid and -ProcessName' }
  if ($TitlePrefix -and $Title) { throw 'pass at most one of -Title and -TitlePrefix' }
  if (-not $ProcessName -and -not $Title -and -not $TitlePrefix) { throw 'pass exactly one of -Title and -TitlePrefix (with -Pid)' }
  if (-not $Maximize -and -not $Min -and $Width -le 0 -and $Height -le 0) { throw 'pass -Min height|width|both, or -Width / -Height, or -Maximize' }
  $procIds = @()
  if ($ProcessName) {
    $procIds = @(Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
    if ($procIds.Count -eq 0) { throw "process not found for name $ProcessName" }
  } else {
    if (-not (Get-Process -Id $TargetPid -ErrorAction SilentlyContinue)) { throw "process not found for pid $TargetPid" }
    $procIds = @($TargetPid)
  }
  $label = if ($Title) { $Title } elseif ($TitlePrefix) { $TitlePrefix + '*' } else { '(largest window)' }
  $who = if ($ProcessName) { "process '$ProcessName'" } else { "pid $TargetPid" }
  $hWnd = Find-Window -ProcIds $procIds -ExactTitle $Title -Prefix $TitlePrefix -WaitMs $Timeout -Largest ([bool]$ProcessName)
  if ($hWnd -eq [IntPtr]::Zero) {
    Write-JsonResultAndExit -Result ([PSCustomObject]@{ ok = $false; pid = $TargetPid; processName = $ProcessName; title = $label; error = "no visible top-level window '$label' for $who within $Timeout ms" }) -ExitCode 1
  }
  $winPid = [uint32]0
  [void][CordmuxWindowSizeNative]::GetWindowThreadProcessId($hWnd, [ref]$winPid)
  $zoomed = [CordmuxWindowSizeNative]::IsZoomed($hWnd)
  if ($Maximize) {
    $beforeMax = [CordmuxWindowSizeNative]::Rect($hWnd)
    [void][CordmuxWindowSizeNative]::ShowWindow($hWnd, [CordmuxWindowSizeNative]::SW_MAXIMIZE)
    Start-Sleep -Milliseconds $SettleMs
    $afterMax = [CordmuxWindowSizeNative]::Rect($hWnd)
    $okMax = [CordmuxWindowSizeNative]::IsZoomed($hWnd)
    Write-JsonResultAndExit -Result ([PSCustomObject]@{ ok = $okMax; pid = [int]$winPid; processName = $ProcessName; title = [CordmuxWindowSizeNative]::TitleOf($hWnd); zoomed = $zoomed; maximized = $okMax; before = $beforeMax; after = $afterMax; error = $(if ($okMax) { $null } else { 'the window is not maximized after ShowWindow' }) }) -ExitCode ($(if ($okMax) { 0 } else { 1 }))
  }
  if ($zoomed) {
    # 最大化のウィンドウは SetWindowPos で大きさを替えても最大化の状態が残るので、先に通常の大きさへ戻す (前面には出さない)。
    [void][CordmuxWindowSizeNative]::ShowWindow($hWnd, [CordmuxWindowSizeNative]::SW_SHOWNOACTIVATE)
    Start-Sleep -Milliseconds $SettleMs
  }
  $requested = Get-RequestedSize -Before ([CordmuxWindowSizeNative]::Rect($hWnd)) -MinMode $Min -W $Width -H $Height
  $res = Invoke-Resize -Hwnd $hWnd -Requested $requested -Settle $SettleMs -Restore (-not $NoRestore)
  $out = [PSCustomObject]@{ ok = [bool]$res.applied; pid = [int]$winPid; processName = $ProcessName; title = [CordmuxWindowSizeNative]::TitleOf($hWnd); zoomed = $zoomed; error = $(if ($res.applied) { $null } else { 'SetWindowPos failed' }) }
  foreach ($p in $res.PSObject.Properties) { $out | Add-Member -NotePropertyName $p.Name -NotePropertyValue $p.Value }
  Write-JsonResultAndExit -Result $out -ExitCode ($(if ($res.applied) { 0 } else { 1 }))
}
catch {
  Write-JsonResultAndExit -Result ([PSCustomObject]@{ ok = $false; pid = $TargetPid; error = $_.Exception.Message }) -ExitCode 1
}
