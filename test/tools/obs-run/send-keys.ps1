#requires -Version 5.1
<#
  send-keys.ps1 - 実機の OBS の名指したウィンドウへキー (Tab・Shift+Tab・Esc など) を送り、1 つ送るごとにフォーカスがどこへ動いたかを
  読むツール。

  フォーカスの行き先・アクティブなウィンドウ・フォーカスの巡回は人のキーボードの操作で起きる形を見るもので、
  UI Automation の Invoke ではフォーカスが動かない。
  dialog-sendkeys.ps1 は文字列を送るツールで、{TAB} も文字として送る (特殊文字をエスケープする) ので代わりにならない。

  する事:
    1. -Pid のプロセスの、タイトルが -Title と同じ (または -TitlePrefix で始まる) 見えているトップレベルウィンドウを探す (-Timeout の間)。
    2. そのウィンドウを前面に出し、前面になったことを確認する。前面を取れなければキーを 1 つも送らずに ok:false にする
       (dialog-sendkeys.ps1 と同じ守り。利用者が開いた別のアプリへキーが入らないため)。
       Windows は、ほかのアプリへの最後の入力から ForegroundLockTimeout (既定 200000 ms) の間は前面の横取りを断る。
       断られたら、その値を error に添える (利用者が触らずに待てば通る)。
    3. -Keys を順に (-Repeat 回くり返して) SendKeys の書き方で送る。1 つの項目は 1 つのキー ({TAB}・+{TAB}・{ESC}・{F6}・
       %t・{DOWN 3} など。文字を並べた文字列は断る。フォーカスの履歴と送ったキーが 1 対 1 になるように)。送る前に毎回、前面のウィンドウが
       まだ -Pid のプロセスの物かを確認し、違えばそこで止める (Esc でウィンドウが閉じて主ウィンドウへ前面が移るのは同じプロセスなので続ける)。
    4. 送るごとに -DelayMs 待ち、UI Automation のフォーカスの要素 (名前・種類・クラス・AutomationId・四角・プロセス) と前面のウィンドウのタイトルを
       読む。-ShotDir を渡せば、そのたびに screenshot.ps1 で -Pid のウィンドウを全部撮る (接頭は <ShotPrefix>-<連番>)。

  結果は 1 行の JSON を標準出力に書く (screenshot.ps1 と同じ UTF-8)。
    { "ok", "pid", "title", "sent": <int>, "trail": [ { "n", "key", "at", "foreground": { "title", "pid" },
      "focus": { "name", "controlType", "className", "automationId", "rect": [x,y,w,h], "pid" }|null, "shots": [...]|null } ],
      "foregroundLockTimeoutMs", "error" }
  失敗を握って先へ進む形 (ok:false でも trail は送れた所まで書く)。

  -SelfTest: キーの書き方の見分け、見つからないタイトルと見えないウィンドウ (自分で作る、表に出さない WinForms のウィンドウ) を断ること、
  前面のウィンドウのプロセスの見分けを確認する。前面を動かす呼び出し (SetForegroundWindow) とキーの送りはセルフテストでは行わない。
#>

[CmdletBinding()]
param(
  [Alias('Pid')]
  [int]$TargetPid = 0,

  # ウィンドウのタイトル (完全一致)。-TitlePrefix とどちらか 1 つ。OBS の主ウィンドウはタイトルがバージョンと状態で変わるので -TitlePrefix 'OBS ' で探す。
  [string]$Title = '',
  [string]$TitlePrefix = '',

  [string[]]$Keys = @(),

  [ValidateRange(1, 200)]
  [int]$Repeat = 1,

  # 1 つ送った後、フォーカスを読むまでの待ち (ミリ秒)。Qt がフォーカスを移して描き直すまで (ドックの周期 500 ms より短い)。
  [ValidateRange(0, 10000)]
  [int]$DelayMs = 300,

  [ValidateRange(0, 60000)]
  [int]$Timeout = 5000,

  [string]$ShotDir = '',
  [string]$ShotPrefix = 'key',

  [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'

function Write-JsonResultAndExit {
  # screenshot.ps1 と同じ理由: 標準出力へ UTF-8 (BOM なし) のバイト列を直接書く。
  param($Result, [int]$ExitCode)
  $json = $Result | ConvertTo-Json -Depth 7 -Compress
  $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
  $bytes = $utf8NoBom.GetBytes($json)
  $stdout = [System.Console]::OpenStandardOutput()
  $stdout.Write($bytes, 0, $bytes.Length)
  $stdout.Flush()
  exit $ExitCode
}

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

# user32 のウィンドウの列挙・前面化の薄い包み。screenshot.ps1 と同じ理由でヒアストリングの中だけ英語のコメント。
$nativeSource = @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class CordmuxSendKeysNative
{
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

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
    public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")]
    public static extern IntPtr GetForegroundWindow();

    public const uint GA_ROOT = 2;

    public static string TitleOf(IntPtr hWnd)
    {
        int len = GetWindowTextLength(hWnd);
        if (len <= 0) return "";
        var sb = new StringBuilder(len + 1);
        GetWindowText(hWnd, sb, sb.Capacity);
        return sb.ToString();
    }

    public static uint PidOf(IntPtr hWnd)
    {
        uint pid;
        GetWindowThreadProcessId(hWnd, out pid);
        return pid;
    }

    // Visible top-level window of the process whose title equals (prefix == false) or starts with (prefix == true) the text.
    public static IntPtr FindTopLevel(uint targetProcessId, string text, bool prefix)
    {
        IntPtr found = IntPtr.Zero;
        EnumWindows(delegate (IntPtr hWnd, IntPtr lParam)
        {
            if (PidOf(hWnd) != targetProcessId || !IsWindowVisible(hWnd) || GetAncestor(hWnd, GA_ROOT) != hWnd) return true;
            string t = TitleOf(hWnd);
            bool hit = prefix ? t.StartsWith(text, StringComparison.Ordinal) : t == text;
            if (hit) { found = hWnd; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
"@
Add-Type -TypeDefinition $nativeSource

# 1 つの項目が 1 つのキー (修飾 + {名前} / {名前 回数} / 逃がした特殊文字 / 1 字 / ~) か。
$KeyChordPattern = '^[+^%]*(\{[A-Za-z][A-Za-z0-9]*( [1-9][0-9]?)?\}|\{[+^%~(){}\[\]]\}|~|[^+^%~(){}\[\]])$'
function Test-KeyChord {
  param([string]$Key)
  return [bool]($Key -cmatch $KeyChordPattern)
}

# 前面のウィンドウが対象のプロセスの物か (Esc でウィンドウが閉じても同じプロセスなら続ける)。
function Test-ForegroundOwnedBy {
  param([uint32]$ForegroundPid, [uint32]$Wanted)
  return ($ForegroundPid -ne 0 -and $ForegroundPid -eq $Wanted)
}

function Get-ForegroundLockTimeoutMs {
  try {
    $v = (Get-ItemProperty -Path 'HKCU:\Control Panel\Desktop' -Name ForegroundLockTimeout -ErrorAction Stop).ForegroundLockTimeout
    return [int64]$v
  } catch {
    return $null
  }
}

function Find-Window {
  param([int]$ProcId, [string]$ExactTitle, [string]$Prefix, [int]$WaitMs)
  $deadline = (Get-Date).AddMilliseconds($WaitMs)
  do {
    $h = if ($Prefix) { [CordmuxSendKeysNative]::FindTopLevel([uint32]$ProcId, $Prefix, $true) } else { [CordmuxSendKeysNative]::FindTopLevel([uint32]$ProcId, $ExactTitle, $false) }
    if ($h -ne [IntPtr]::Zero) { return $h }
    Start-Sleep -Milliseconds 200
  } while ((Get-Date) -lt $deadline)
  return [IntPtr]::Zero
}

function Get-FocusInfo {
  try {
    $el = [System.Windows.Automation.AutomationElement]::FocusedElement
    if (-not $el) { return $null }
    $c = $el.Current
    $b = $c.BoundingRectangle
    $rect = if ($b.IsEmpty) { $null } else { @([int]$b.X, [int]$b.Y, [int]$b.Width, [int]$b.Height) }
    return [PSCustomObject]@{
      name         = $c.Name
      controlType  = $c.ControlType.ProgrammaticName
      className    = $c.ClassName
      automationId = $c.AutomationId
      rect         = $rect
      pid          = $c.ProcessId
    }
  } catch {
    return [PSCustomObject]@{ error = $_.Exception.Message }
  }
}

function Invoke-Shot {
  param([int]$ProcId, [string]$Dir, [string]$Prefix)
  try {
    $ps1 = Join-Path $PSScriptRoot 'screenshot.ps1'
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $ps1 -Pid $ProcId -OutDir $Dir -Prefix $Prefix
    $parsed = ($out | Out-String | ConvertFrom-Json)
    return @($parsed.shots | ForEach-Object { $_.path })
  } catch {
    return @('error: ' + $_.Exception.Message)
  }
}

# ---- セルフテスト --------------------------------------------------------------------------------
function Invoke-SelfTest {
  $failures = New-Object System.Collections.ArrayList
  $checks = New-Object System.Collections.ArrayList
  function Check {
    param([bool]$Cond, [string]$Label)
    [void]$checks.Add([PSCustomObject]@{ ok = $Cond; label = $Label })
    if (-not $Cond) { [void]$failures.Add($Label) }
  }
  $form = $null
  try {
    foreach ($k in @('{TAB}', '+{TAB}', '{ESC}', '{F6}', '%t', '^{F4}', '{DOWN 3}', '~', '{+}', 'a', '+a', '{ENTER}')) {
      Check (Test-KeyChord $k) ('one key: ' + $k)
    }
    foreach ($k in @('', 'ab', '{TAB}{TAB}', 'hello', '{TAB', '{TAB 0}', '+', '{}', '{TAB}x')) {
      Check (-not (Test-KeyChord $k)) ('not one key: "' + $k + '"')
    }
    Check (Test-ForegroundOwnedBy 10 10) 'the foreground of the same process is accepted'
    Check (-not (Test-ForegroundOwnedBy 11 10)) 'the foreground of another process is refused'
    Check (-not (Test-ForegroundOwnedBy 0 0)) 'no foreground is refused'

    # 見つからないタイトルは時限の後に Zero (キーを送る前に止まる)。
    $none = Find-Window -ProcId $PID -ExactTitle ('cordmux-send-keys-selftest-none-' + $PID) -Prefix '' -WaitMs 300
    Check ($none -eq [IntPtr]::Zero) 'an unknown title is not found'
    # 表に出さないウィンドウ (自分で作る。Visible にしない) は、タイトルが合っても見つけない。
    $form = New-Object System.Windows.Forms.Form
    $form.Text = 'cordmux-send-keys-selftest-' + $PID
    $form.ShowInTaskbar = $false
    $handle = $form.Handle
    Check ($handle -ne [IntPtr]::Zero) 'the hidden form has a handle'
    Check (-not [CordmuxSendKeysNative]::IsWindowVisible($handle)) 'the hidden form is not visible'
    $hidden = Find-Window -ProcId $PID -ExactTitle $form.Text -Prefix '' -WaitMs 300
    Check ($hidden -eq [IntPtr]::Zero) 'a hidden window is not a target'
    $hiddenPrefix = Find-Window -ProcId $PID -ExactTitle '' -Prefix 'cordmux-send-keys-selftest-' -WaitMs 300
    Check ($hiddenPrefix -eq [IntPtr]::Zero) 'a hidden window is not a target by prefix either'
    $lock = Get-ForegroundLockTimeoutMs
    Check ($null -eq $lock -or $lock -ge 0) 'ForegroundLockTimeout is readable or absent'
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
    $r = Invoke-SelfTest
    Write-JsonResultAndExit -Result $r -ExitCode ($(if ($r.ok) { 0 } else { 1 }))
  }
  if ($TargetPid -le 0) { throw '-Pid is required (obsPid of hold-<tag>.json)' }
  if ((-not $Title -and -not $TitlePrefix) -or ($Title -and $TitlePrefix)) { throw 'pass exactly one of -Title and -TitlePrefix' }
  if ($Keys.Count -eq 0) { throw '-Keys is required (one key per item, e.g. "{TAB}","+{TAB}")' }
  $bad = @($Keys | Where-Object { -not (Test-KeyChord $_) })
  if ($bad.Count -gt 0) { throw ('each item of -Keys must be one key in SendKeys syntax: ' + ($bad -join ' | ')) }
  if (-not (Get-Process -Id $TargetPid -ErrorAction SilentlyContinue)) { throw "process not found for pid $TargetPid" }
  $label = if ($Title) { $Title } else { $TitlePrefix + '*' }
  $lockMs = Get-ForegroundLockTimeoutMs

  $hWnd = Find-Window -ProcId $TargetPid -ExactTitle $Title -Prefix $TitlePrefix -WaitMs $Timeout
  if ($hWnd -eq [IntPtr]::Zero) {
    Write-JsonResultAndExit -Result ([PSCustomObject]@{ ok = $false; pid = $TargetPid; title = $label; sent = 0; trail = @(); foregroundLockTimeoutMs = $lockMs; error = "no visible top-level window '$label' for pid $TargetPid within $Timeout ms; no key was sent" }) -ExitCode 1
  }
  $windowTitle = [CordmuxSendKeysNative]::TitleOf($hWnd)
  [void][CordmuxSendKeysNative]::SetForegroundWindow($hWnd)
  Start-Sleep -Milliseconds 300
  if ([CordmuxSendKeysNative]::GetForegroundWindow() -ne $hWnd) {
    $fg = [CordmuxSendKeysNative]::GetForegroundWindow()
    Write-JsonResultAndExit -Result ([PSCustomObject]@{
        ok = $false; pid = $TargetPid; title = $windowTitle; sent = 0; trail = @(); foregroundLockTimeoutMs = $lockMs
        foreground = [PSCustomObject]@{ title = [CordmuxSendKeysNative]::TitleOf($fg); pid = [CordmuxSendKeysNative]::PidOf($fg) }
        error = "window '$windowTitle' did not become foreground; no key was sent (Windows refuses for ForegroundLockTimeout ms after the last input to another app)"
      }) -ExitCode 1
  }

  if ($ShotDir) { [void](New-Item -ItemType Directory -Path $ShotDir -Force) }
  $trail = New-Object System.Collections.ArrayList
  $n = 0
  $stopError = $null
  for ($r = 0; $r -lt $Repeat -and -not $stopError; $r++) {
    foreach ($k in $Keys) {
      $fg = [CordmuxSendKeysNative]::GetForegroundWindow()
      $fgPid = [CordmuxSendKeysNative]::PidOf($fg)
      if (-not (Test-ForegroundOwnedBy $fgPid ([uint32]$TargetPid))) {
        $stopError = "the foreground left pid $TargetPid (now pid $fgPid '" + [CordmuxSendKeysNative]::TitleOf($fg) + "'); stopped before key " + ($n + 1)
        break
      }
      [System.Windows.Forms.SendKeys]::SendWait($k)
      $n++
      Start-Sleep -Milliseconds $DelayMs
      $fgAfter = [CordmuxSendKeysNative]::GetForegroundWindow()
      $shots = $null
      if ($ShotDir) { $shots = Invoke-Shot -ProcId $TargetPid -Dir $ShotDir -Prefix ($ShotPrefix + '-' + $n.ToString('000')) }
      [void]$trail.Add([PSCustomObject]@{
          n          = $n
          key        = $k
          at         = [DateTimeOffset]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ss.fffZ')
          foreground = [PSCustomObject]@{ title = [CordmuxSendKeysNative]::TitleOf($fgAfter); pid = [CordmuxSendKeysNative]::PidOf($fgAfter) }
          focus      = (Get-FocusInfo)
          shots      = $shots
        })
    }
  }
  Write-JsonResultAndExit -Result ([PSCustomObject]@{ ok = ($null -eq $stopError); pid = $TargetPid; title = $windowTitle; sent = $n; trail = $trail.ToArray(); foregroundLockTimeoutMs = $lockMs; error = $stopError }) -ExitCode ($(if ($null -eq $stopError) { 0 } else { 1 }))
}
catch {
  Write-JsonResultAndExit -Result ([PSCustomObject]@{ ok = $false; pid = $TargetPid; sent = 0; trail = @(); error = $_.Exception.Message }) -ExitCode 1
}
