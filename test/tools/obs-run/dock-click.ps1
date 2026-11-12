#requires -Version 5.1
<#
  dock-click.ps1 - 実行スクリプト (run-window.mjs) が浮いたドックのウィンドウのボタン (またはチェック
  ボックス) を押すためのツール。

  キー入力 (SendKeys) は前面のウィンドウへしか届かず、Windows は前面の横取りを原則拒むので、
  前面を取れないと使えない。このツールは UI Automation
  (System.Windows.Automation) でつまみそのものを掴んで押すので、前面を取れていなくても
  押せる (SetForegroundWindow は呼ばない)。使い道は、参加者の行のチェック (人の見えている
  名前で名指す) と、ドックのボタン ([Vesktop に追加]・cordmux の録画の開始と終了ほか)。

  対象のプロセス (-Pid) が持つ、タイトルが -Title (既定 "cordmux") と完全一致する見えている
  トップレベルウィンドウ (screenshot.ps1 と同じ探し方) の子孫から、Name が -Name と
  完全一致する要素を探す。探す対象は Button と CheckBox の両方である。
  参加者の行のチェックは QCheckBox を継ぐ RowCheck (src-obs/dock/person_row.h) で、読み上げの
  名前は人の見えている名前 (person_row.cpp の check_->setAccessibleName) なので、
  ControlType.Button だけに絞ると行のチェックを見つけられない。

  押し方は見つかった要素が持つパターンと -Desired で決める:
    - -Desired on/off で TogglePattern があれば、ToggleState が行き先と違うときだけ押す (既に行き先なら
      押し直さない。「入れる」「切る」操作は何度実行しても同じ結果になる)。押すのは Invoke (Qt の pressAction = animateClick。
      製品がつないでいる clicked が出る)。Invoke がないコンポーネントだけ Toggle()。押した後に行き先に着いたかを
      最長 -DesiredSettleMs 見て reachedDesired に書く (着かなくても ok は真。下の本体のコメントを見よ)。
      Qt は checkable なコンポーネント (QCheckBox、checkable の QPushButton) に TogglePattern と InvokePattern の
      両方を付け、TogglePattern の Toggle() は QAbstractButton::toggle() になって clicked を出さない
      (qtbase 6.8 の qwindowsuiamainprovider.cpp・qwindowsuiatoggleprovider.cpp・simplewidgets.cpp を読んだ)。
      TogglePattern を先に使って Toggle() で押す方法は採らない (行のチェックも録画のボタンも
      clicked で動くので、見た目だけ変わって製品は何もしない)。
    - それ以外 (-Desired any、または TogglePattern のないコンポーネント) は、InvokePattern があれば Invoke()
      (QPushButton はこちら)、なく TogglePattern があれば On でなければ Toggle()。
    - どちらも無ければ「押せるコンポーネントではない」として ok:false にする。

  未確認: 参加者の行のチェックを、このツールが名前で見つけて押せるかは実機でまだ確認していない
  (行は表ではない)。
  押し方と行き先は結果の invokedVia・toggleStateBefore・toggleState・reachedDesired に出る。
  見つからなければほかの場合と同じく ok:false を返す。

  -Timeout の間、要素が見つかるまで 300 ms 刻みで探し直す (前の操作の直後は Qt の描き直しが
  まだ済んでいないことがあるため。キーを送るツールが前面化の直後に一呼吸置くのと同じ考え方)。

  結果は 1 行の JSON を標準出力に書く。
    成功: { "ok": true, "pid": <int>, "name": "<string>", "index": <int>,
            "controlType": "<string>", "invokedVia": "Invoke"|"Toggle"|"None",
            "toggleState": "<string|null>", "desired": "any"|"on"|"off" }
          (-Desired on/off で TogglePattern のあるコンポーネントは toggleStateBefore と reachedDesired も。
           行き先に着かなくても ok は真で、note に残る)
    失敗: { "ok": false, "pid": <int>, "name": "<string>", "index": <int>, "error": "<string>" }
          (終了コード 1)
  ウィンドウが見つからない・要素が見つからない・押せるコンポーネントではない、のどれでも例外を投げずに
  ok:false を返す (screenshot.ps1 と同じ「失敗を握って先へ進む」形)。
#>

[CmdletBinding()]
param(
  # screenshot.ps1 と同じ理由: PowerShell の読み取り専用の自動変数 $PID と
  # 大文字小文字を区別せず衝突するため、実体は $TargetPid に持たせ、呼び出し側の引数名だけ
  # -Pid になるよう Alias で合わせる。
  [Parameter(Mandatory = $true)]
  [Alias('Pid')]
  [int]$TargetPid,

  [Parameter(Mandatory = $true)]
  [string]$Name,

  # セルフテスト (README.md のセルフテストの 2 系統) のために公開している。実際の呼び出しでは渡さない
  # (既定がドックの実際のタイトルそのもの)。
  [string]$Title = 'cordmux',

  [ValidateRange(0, 100)]
  [int]$Index = 0,

  [ValidateRange(0, 60000)]
  [int]$Timeout = 5000,

  # 行き先を明示する (録画の ON / OFF を押すため)。
  # 'any' (既定) はこれまでどおり: 押せるコンポーネントなら押す (CheckBox は On でなければ Toggle)。
  # 'on' / 'off' は入り切りのつまみ (CheckBox) の行き先を決める。
  #   - TogglePattern を持つなら、現在の状態が行き先と違うときだけ押す (押すのは Invoke。Invoke がない
  #     コンポーネントだけ Toggle。下の本体の注記。これがないと OFF へ戻せない: 既定は「On でなければ Toggle」
  #     なので、On のものを Off にする道がない)。押した後に行き先に着いたかを確認する (DesiredSettleMs)。
  #   - InvokePattern だけのコンポーネント (QPushButton) は押すと切り替わるだけで、現在の状態を
  #     読む手段がない。行き先が指定されていても押すが、結果の desired と
  #     toggleState:null を見れば「行き先は確認していない」と分かる形にする
  #     (押さない選択にすると、ラベルがボタンだった場合に
  #     録画を 1 度も ON にできない)。
  [ValidateSet('any', 'on', 'off')]
  [string]$Desired = 'any',

  # -Desired on/off で押した後、製品が checked を描き直して行き先に着くまで待つ上限 (ミリ秒)。
  # ドックの周期 500 ms の 4 周期 + Qt の animateClick の 100 ms + 余裕 (run-window-plan.mjs の UI_APPLY_SETTLE_MS と同じ 3 秒)。
  [ValidateRange(0, 60000)]
  [int]$DesiredSettleMs = 3000,

  # Name が -Name で始まる要素を探す (完全一致でなく前方一致)。録画中の [録画を終了] は、ボタンのテキストの後ろに
  # 経過時間が付く (cordmux.dock.record_stop_elapsed.button) ので、完全一致では見つからない。
  [switch]$NamePrefix
)

$ErrorActionPreference = 'Stop'

function Write-JsonResultAndExit {
  # screenshot.ps1 と同じ理由: 既定の標準出力は実行環境の既定コードページ
  # (このマシンでは cp932) を通ることがあり、日本語混じりの JSON をそのまま出すと呼び出し側
  # (execFileSync encoding:"utf8") で文字化けする。標準出力のストリームに UTF-8 (BOM なし) の
  # バイト列を直接書く。
  param($Result, [int]$ExitCode)
  $json = $Result | ConvertTo-Json -Depth 5 -Compress
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

  # user32 のウィンドウの列挙の薄い包み。screenshot.ps1 と同じ理由でヒアストリングの
  # 中だけ英語のコメント (identifiers_english.py /
  # comments_japanese.py は文字列トークンの中身を検査しない)。
  $nativeSource = @"
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class CordmuxDockClickNative
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

  $hWnd = [CordmuxDockClickNative]::FindTitledTopLevelWindow([uint32]$TargetPid, $Title)
  if ($hWnd -eq [IntPtr]::Zero) {
    Write-JsonResultAndExit -Result ([PSCustomObject]@{
        ok    = $false
        pid   = $TargetPid
        name  = $Name
        index = $Index
        error = "no visible top-level window titled '$Title' for pid $TargetPid"
      }) -ExitCode 1
  }

  # AutomationElement.FromHandle は無効な HWND でも例外を投げないことがあるので、
  # 見つかった要素の有無で判断する (下の探索ループ)。
  $root = [System.Windows.Automation.AutomationElement]::FromHandle($hWnd)

  $nameCondition = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::NameProperty, $Name)
  $buttonCondition = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::Button)
  $checkBoxCondition = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::CheckBox)
  $typeCondition = New-Object System.Windows.Automation.OrCondition($buttonCondition, $checkBoxCondition)
  # UI Automation の PropertyCondition は前方一致を持たないので、-NamePrefix のときは種類だけで探して名前で絞る。
  $condition = if ($NamePrefix) { $typeCondition } else { New-Object System.Windows.Automation.AndCondition($nameCondition, $typeCondition) }

  # 前の操作の直後は Qt の描き直しがまだ済んでいないことがあるので、-Timeout の間は
  # 300 ms 刻みで探し直す (冒頭の注記)。
  $deadline = (Get-Date).AddMilliseconds($Timeout)
  $matches = $null
  while ((Get-Date) -lt $deadline) {
    $found = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $condition)
    if ($NamePrefix -and $found) {
      $found = @($found | Where-Object { $_.Current.Name -and $_.Current.Name.StartsWith($Name, [System.StringComparison]::Ordinal) })
    }
    if ($found -and $found.Count -gt 0) {
      $matches = $found
      break
    }
    Start-Sleep -Milliseconds 300
  }

  if (-not $matches -or $matches.Count -eq 0) {
    Write-JsonResultAndExit -Result ([PSCustomObject]@{
        ok    = $false
        pid   = $TargetPid
        name  = $Name
        index = $Index
        error = "no button or checkbox named '$Name' found within $Timeout ms"
      }) -ExitCode 1
  }

  if ($Index -ge $matches.Count) {
    Write-JsonResultAndExit -Result ([PSCustomObject]@{
        ok    = $false
        pid   = $TargetPid
        name  = $Name
        index = $Index
        error = "index $Index out of range (found $($matches.Count) element(s) named '$Name')"
      }) -ExitCode 1
  }

  $element = $matches[$Index]
  $controlTypeName = $element.Current.ControlType.ProgrammaticName

  $invokePattern = $null
  $hasInvoke = $element.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$invokePattern)
  $togglePattern = $null
  $hasToggle = $element.TryGetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern, [ref]$togglePattern)

  # -Desired on/off: 行き先を TogglePattern の状態で先に見て、違うときだけ押す。
  # 押すのは Invoke (Qt の pressAction = animateClick で、製品がつないでいる clicked が出る)。Toggle() は Qt では
  # QAbstractButton::toggle() になり clicked を出さない (qtbase 6.8 の simplewidgets.cpp の QAccessibleButton::doAction と
  # qwindowsuiatoggleprovider.cpp)。行のチェック (person_row.cpp) も録画のボタン (bottom_bar.cpp) も clicked で動くので、
  # Toggle() で押すと見た目だけ変わって製品は何もしない。Toggle() は Invoke がないコンポーネントにだけ使う。
  # Qt は checkable なコンポーネントに TogglePattern (state().checkable) と InvokePattern (actionInterface) の両方を付ける
  # (qwindowsuiamainprovider.cpp の GetPatternProvider) ので、Invoke を先に使うと行き先を見ずに押してしまう。
  # 押した後は、製品の周期 (ドック 500 ms) で checked が描き直されるまで最長 DesiredSettleMs 待ち、行き先に
  # 着いたかを reachedDesired に出す。着かなくても ok は真 (押せたことが ok)。録画のボタンの checked は Recording と
  # Stopping だけ真 (rules_dock.cpp の pressed_look) で、開始の直後の Starting (偽) と終了の直後の Stopping (真。録画ファイルの作成の間
  # 続く) は数秒より長く続きうるので、着いたかで失敗にすると、正しく動いた場合まで失敗にしてしまう。録画が実際に始まった・終わったかは
  # --step record と call-1 の判定 (wait-finalize と判定ツール 3 つ) が見る。行のチェックが動作したかは judge-composite-placement と
  # 配置の行 (composite-layout-snapshot) が見る。
  if ($Desired -ne 'any' -and $hasToggle) {
    $target = if ($Desired -eq 'on') { [System.Windows.Automation.ToggleState]::On } else { [System.Windows.Automation.ToggleState]::Off }
    $before = $togglePattern.Current.ToggleState
    $via = 'None'
    $after = $before
    if ($before -ne $target) {
      if ($hasInvoke) {
        $invokePattern.Invoke()
        $via = 'Invoke'
      }
      else {
        $togglePattern.Toggle()
        $via = 'Toggle'
      }
      $settleUntil = (Get-Date).AddMilliseconds($DesiredSettleMs)
      do {
        Start-Sleep -Milliseconds 100
        $after = $togglePattern.Current.ToggleState
      } while ($after -ne $target -and (Get-Date) -lt $settleUntil)
    }
    $reached = ($after -eq $target)
    Write-JsonResultAndExit -Result ([PSCustomObject]@{
        ok              = $true
        pid             = $TargetPid
        name            = $Name
        index           = $Index
        controlType     = $controlTypeName
        invokedVia      = $via
        toggleStateBefore = $before.ToString()
        toggleState     = $after.ToString()
        desired         = $Desired
        reachedDesired  = $reached
        note            = $(if ($reached) { $null } else { "toggle state stayed $($after.ToString()) (wanted $Desired) within $DesiredSettleMs ms" })
      }) -ExitCode 0
  }

  if ($hasInvoke) {
    $invokePattern.Invoke()
    Write-JsonResultAndExit -Result ([PSCustomObject]@{
        ok           = $true
        pid          = $TargetPid
        name         = $Name
        index        = $Index
        controlType  = $controlTypeName
        invokedVia   = 'Invoke'
        toggleState  = $null
        desired      = $Desired
      }) -ExitCode 0
  }

  if ($hasToggle) {
    $before = $togglePattern.Current.ToggleState
    # 行き先を決める。-Desired 'any' (既定) はこれまでどおり「On でなければ Toggle」。on/off で TogglePattern を
    # 持つコンポーネントは上で済んでいる (ここに来るのは Invoke のないコンポーネントだけ)。
    $target = switch ($Desired) {
      'on'  { [System.Windows.Automation.ToggleState]::On }
      'off' { [System.Windows.Automation.ToggleState]::Off }
      default { [System.Windows.Automation.ToggleState]::On }
    }
    if ($before -ne $target) {
      $togglePattern.Toggle()
    }
    $after = $togglePattern.Current.ToggleState
    Write-JsonResultAndExit -Result ([PSCustomObject]@{
        ok           = $true
        pid          = $TargetPid
        name         = $Name
        index        = $Index
        controlType  = $controlTypeName
        invokedVia   = 'Toggle'
        toggleState  = $after.ToString()
        desired      = $Desired
      }) -ExitCode 0
  }

  Write-JsonResultAndExit -Result ([PSCustomObject]@{
      ok    = $false
      pid   = $TargetPid
      name  = $Name
      index = $Index
      error = "element named '$Name' supports neither InvokePattern nor TogglePattern"
    }) -ExitCode 1
}
catch {
  Write-JsonResultAndExit -Result ([PSCustomObject]@{ ok = $false; pid = $TargetPid; name = $Name; index = $Index; error = $_.Exception.Message }) -ExitCode 1
}
