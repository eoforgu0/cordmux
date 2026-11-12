#requires -Version 5.1
<#
  dialog-sendkeys.ps1 - 実行スクリプト (run-window.mjs) が、ドックのメニューの
  [診断情報をエクスポート...] などが開く保存ダイアログ (ドックとは別のトップレベルウィンドウ) へ
  文字を入れて確定するためのツール。

  [診断情報をエクスポート...] を選んだ後に
  開く保存ダイアログは、タイトルが "cordmux" ではない別のトップレベルウィンドウ
  (src-obs/data/locale/ja-JP.ini の cordmux.dialog.export.title = "診断情報のエクスポート") で、
  Windows の標準の保存ダイアログ (OS のウィンドウ) なので、Qt 自身のウィンドウのコンポーネントを名前で探す dock-click.ps1 /
  dialog-click.ps1 では扱えない。このツールはタイトルを引数で指定する汎用の、
  ウィンドウを見つけて前面へ出し、文字を入れて Enter を送るツールである。

  Windows の保存ダイアログ (Qt の QFileDialog::getSaveFileName は既定でネイティブの
  IFileSaveDialog を使う) はファイル名の欄が既定でフォーカスを持つので、UI Automation で
  欄を探さずそのまま SendKeys で打ち込む (OS 標準ダイアログの欄の Name や
  構成は Windows のバージョンによって変わりうるため、UI Automation の ValuePattern では狙わない)。フル
  パス (ドライブ文字から) を打ち込めば、ダイアログはその場所への保存として受け付ける
  (Windows の保存ダイアログの既知の挙動)。

  対象のプロセス (-Pid) が持つ、タイトルが -Title と完全一致する見えているトップレベルウィンドウを
  探す (screenshot.ps1 と同じ探し方だが、既定タイトルは持たない = 呼び出し側が必ず指定する)。
  保存ダイアログは項目を選んだ直後にはまだ開いていないことがある (製品は中身を詰め終えてから
  開く) ので、-Timeout の間は 300 ms 刻みで探し直す。

  前面を取れたときしかキーを送らない。SendKeys は「現在前面にあるウィンドウ」へ送るツールで、送り先を
  指定できない。Windows は前面の横取りを原則拒む (呼び出し元が前面のプロセスでない・最後の入力を
  受けていないときは、タスクバーを光らせるだけで前面は変わらない) ので、SetForegroundWindow の
  後に GetForegroundWindow がこのウィンドウを返すことを確認し、返さなければキーを 1 つも送らずに
  ok:false を返す (共有 PC で、利用者が開いたままの別のアプリへキーが入らないため)。

  -Text は SendKeys の特殊文字 (`+^%~(){}[]`) を自動でエスケープしてから送る (呼び出し側は
  素のファイルパスをそのまま渡してよい)。既定では打ち込んだ後に {ENTER} も送って確定する
  (-NoEnter で省ける)。

  結果は 1 行の JSON を標準出力に書く。
    成功: { "ok": true, "pid": <int>, "title": "<string>", "textLength": <int>,
            "pressedEnter": true|false }
    失敗: { "ok": false, "pid": <int>, "title": "<string>", "error": "<string>" } (終了コード 1)
  ウィンドウが見つからない・前面を取れない、のどちらでも例外を投げずに ok:false を返す
  (他のツールと同じ「失敗を握って先へ進む」形)。
#>

[CmdletBinding()]
param(
  # 他のツールと同じ理由: PowerShell の読み取り専用の自動変数 $PID と大文字小文字を区別せず
  # 衝突するため、実体は $TargetPid に持たせ、呼び出し側の引数名だけ -Pid になるよう
  # Alias で合わせる。
  [Parameter(Mandatory = $true)]
  [Alias('Pid')]
  [int]$TargetPid,

  # 既定タイトルは持たない (dock-click.ps1 等と違い、このツールは "cordmux" 以外のウィンドウを探すために
  # 存在する)。
  [Parameter(Mandatory = $true)]
  [string]$Title,

  [Parameter(Mandatory = $true)]
  [AllowEmptyString()]
  [string]$Text,

  [ValidateRange(0, 60000)]
  [int]$Timeout = 10000,

  # 打ち込んだ後の {ENTER} を省く (セルフテスト・呼び出し側が別に確定させたい場合)。
  [switch]$NoEnter
)

$ErrorActionPreference = 'Stop'

function Write-JsonResultAndExit {
  # 他のツールと同じ理由: 既定の標準出力は実行環境の既定コードページ (このマシンでは cp932) を
  # 通ることがあり、日本語混じりの JSON をそのまま出すと呼び出し側で文字化けする。標準出力の
  # ストリームに UTF-8 (BOM なし) のバイト列を直接書く。
  param($Result, [int]$ExitCode)
  $json = $Result | ConvertTo-Json -Depth 5 -Compress
  $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
  $bytes = $utf8NoBom.GetBytes($json)
  $stdout = [System.Console]::OpenStandardOutput()
  $stdout.Write($bytes, 0, $bytes.Length)
  $stdout.Flush()
  exit $ExitCode
}

# SendKeys の特殊文字 (+^%~(){}[]) を {x} の形でエスケープする。呼び出し側は素の文字列
# (ファイルパスなど) をそのまま渡せるようにするため。
function Format-SendKeysText {
  param([string]$Text)
  $special = '+^%~(){}[]'
  $sb = New-Object System.Text.StringBuilder
  foreach ($ch in $Text.ToCharArray()) {
    if ($special.IndexOf($ch) -ge 0) {
      [void]$sb.Append('{').Append($ch).Append('}')
    } else {
      [void]$sb.Append($ch)
    }
  }
  return $sb.ToString()
}

try {
  Add-Type -AssemblyName System.Windows.Forms

  # user32 のウィンドウの列挙・前面化の薄い包み。他のツールと同じ理由でヒアストリングの中だけ英語の
  # コメント。
  $nativeSource = @"
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class CordmuxDialogSendKeysNative
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

    // Visible top-level window (its own root ancestor) owned by targetProcessId
    // whose title matches wantedTitle exactly. Returns IntPtr.Zero if none found.
    public static IntPtr FindTitledTopLevelWindow(uint targetProcessId, string wantedTitle)
    {
        IntPtr found = IntPtr.Zero;
        EnumWindows(delegate (IntPtr hWnd, IntPtr lParam)
        {
            uint windowProcessId;
            GetWindowThreadProcessId(hWnd, out windowProcessId);
            if (windowProcessId != targetProcessId
                || !IsWindowVisible(hWnd)
                || GetAncestor(hWnd, GA_ROOT) != hWnd)
            {
                return true;
            }
            int len = GetWindowTextLength(hWnd);
            if (len == 0) return true;
            var sb = new StringBuilder(len + 1);
            GetWindowText(hWnd, sb, sb.Capacity);
            if (sb.ToString() == wantedTitle)
            {
                found = hWnd;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
"@
  Add-Type -TypeDefinition $nativeSource

  $proc = Get-Process -Id $TargetPid -ErrorAction SilentlyContinue
  if (-not $proc) {
    throw "process not found for pid $TargetPid"
  }

  # 保存ダイアログは [診断情報をエクスポート...] を選んだ直後にはまだ開いていないことがあるので、
  # -Timeout の間は探し直す。
  $deadline = (Get-Date).AddMilliseconds($Timeout)
  $hWnd = [IntPtr]::Zero
  while ((Get-Date) -lt $deadline) {
    $hWnd = [CordmuxDialogSendKeysNative]::FindTitledTopLevelWindow([uint32]$TargetPid, $Title)
    if ($hWnd -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 300
  }

  if ($hWnd -eq [IntPtr]::Zero) {
    Write-JsonResultAndExit -Result ([PSCustomObject]@{
        ok    = $false
        pid   = $TargetPid
        title = $Title
        error = "no visible top-level window titled '$Title' for pid $TargetPid within $Timeout ms"
      }) -ExitCode 1
  }

  [void][CordmuxDialogSendKeysNative]::SetForegroundWindow($hWnd)
  # 前面化した直後はまだ入力を受け付けられないことがあるため、送る前に一呼吸置く。
  Start-Sleep -Milliseconds 300
  # **前面を取れたことを確認してからしかキーを送らない** (冒頭の注記)。
  if ([CordmuxDialogSendKeysNative]::GetForegroundWindow() -ne $hWnd) {
    Write-JsonResultAndExit -Result ([PSCustomObject]@{
        ok    = $false
        pid   = $TargetPid
        title = $Title
        error = "window titled '$Title' did not become foreground; no key was sent"
      }) -ExitCode 1
  }

  $escaped = Format-SendKeysText -Text $Text
  [System.Windows.Forms.SendKeys]::SendWait($escaped)
  $pressedEnter = $false
  if (-not $NoEnter) {
    Start-Sleep -Milliseconds 300
    [System.Windows.Forms.SendKeys]::SendWait('{ENTER}')
    $pressedEnter = $true
  }

  Write-JsonResultAndExit -Result ([PSCustomObject]@{
      ok           = $true
      pid          = $TargetPid
      title        = $Title
      textLength   = $Text.Length
      pressedEnter = $pressedEnter
    }) -ExitCode 0
}
catch {
  Write-JsonResultAndExit -Result ([PSCustomObject]@{ ok = $false; pid = $TargetPid; title = $Title; error = $_.Exception.Message }) -ExitCode 1
}
