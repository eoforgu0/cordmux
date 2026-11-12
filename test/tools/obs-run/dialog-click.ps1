#requires -Version 5.1
<#
  dialog-click.ps1 - 実行スクリプト (run-window.mjs) が、OBS のプロセスのウィンドウ (ドック、cordmux の設定、確認と
  結果のウィンドウ、メニュー) のコンポーネントを押す、またはウィンドウの中のコンポーネントと文の一覧を読むためのツール。

  ウィンドウの探し方:
    - -Title を渡す: 対象のプロセス (-Pid) の見えているトップレベルウィンドウのうち、タイトルが -Title と
      完全一致するウィンドウを全部見る (確認のウィンドウと結果のウィンドウは同じタイトルで、確認のウィンドウが閉じ切る前に
      結果のウィンドウが開くことがある)。
    - -Popup を渡す: タイトルを問わず、対象のプロセスの見えているトップレベルウィンドウを全部見る
      (QMenu のウィンドウはタイトルを持たない)。メニューの項目を押すときに使う。

  コンポーネントの種類 (-ControlType): Button (既定)、CheckBox、MenuItem、ListItem、ComboBox、TabItem。Name (読み上げの名前) が
  -Button と完全一致するコンポーネントを探す (同名が複数あれば -Index 番目)。-NamePrefix を渡すと、-Button で始まる名前も探す。
  押し方はコンポーネントが持つ形で決める:
    - ListItem: SelectionItemPattern の Select (設定のウィンドウのページの一覧。押してもウィンドウは開かない)。
    - ComboBox: -Select の文字列と名前が完全一致する項目を選ぶ。ExpandCollapsePattern の Expand で一覧を開き、一覧のウィンドウへ
      矢印キーと Enter を PostMessage で送る。一覧の項目の SelectionItemPattern の Select は QComboBox の現在の項目を変えず、
      ComboBox は SetFocus を受けない (OBS 33.0.1 で確認した) ので使わない。キーは一覧のウィンドウにだけ届くので、前面のウィンドウに入力が
      漏れない。一覧の項目を items に、選ぶ前と後の値 (ValuePattern か SelectionPattern) を valueBefore と valueAfter に残し、
      -Select と違えば ok:false。-ListOnly では何も開かず選ばず、ComboBox の現在の値を comboValue に残す。
    - -Desired on / off と TogglePattern: 現在の状態が行き先と同じなら押さない (pressed:false)。
      メニューの項目で押さなかったときは、メニューのウィンドウへ Esc を送って閉じる (開いたまま残さない)。
    - MenuItem で、下にメニューを持つ項目 (ExpandCollapsePattern があり葉でない。OBS のメニューバーの
      [ツール]): Expand で開く (expanded:true)。cordmux のアンインストールの
      入口が OBS の [ツール] メニューにもあるため、メニューを順にたどる。
    - それ以外: InvokePattern の Invoke。無ければ TogglePattern の Toggle。
  -NoWait: Invoke (と Expand) を別のスレッドで始め、最長 1 秒だけ戻りを待つ。モーダルのウィンドウ (QMenu::exec や
  QDialog::exec) を開くコンポーネントは、ウィンドウが閉じるまで Invoke が戻らない。戻らなかったことは returned:false で残し、ok は true (押すこと自体はできた)。

  読む物 (どの形でも結果に入れる): ボタンの名前 (buttons)、文 (texts。Text の名前と、Edit の名前と値。
  場所の行は長いと読むだけの入力欄になる = src-obs/dock/ui/message_dialog.cpp の path_widget)、
  Edit だけの一覧 (edits)、見つけたコンポーネントの種類の名前の一覧 (controls。-ControlType の物)。
    - -ListOnly: 何も押さない。ウィンドウがあれば ok:true。-Button を渡したときは、その名前のコンポーネントが現れるまで
      -Timeout の間待ち、buttonFound で答える。
  -Timeout の間は 300 ms 刻みで探し直す (ウィンドウはボタンを押した直後にはまだ開いていないことがある)。

  前面は取らない (SetForegroundWindow も SendKeys も使わない。UI Automation のパターンは対象の
  プロセスの側で動くので、共有 PC で他のアプリが前面にあっても動作する)。メニューを閉じる Esc だけは
  PostMessage でウィンドウへ直に送る (前面に依らない)。

  結果は 1 行の JSON を標準出力に書く (UTF-8、BOM なし)。
    { "ok": bool, "pid": int, "title": str, "popup": bool, "controlType": str, "found": bool,
      "button": str, "index": int, "listOnly": bool, "buttonFound": bool|null, "invoked": bool,
      "pressed": bool, "expanded": bool, "returned": bool|null, "toggleBefore": str|null, "desired": str,
      "select": str, "selected": bool|null, "valueBefore": str|null, "valueAfter": str|null, "comboValue": str|null,
      "items": [str], "buttons": [str], "texts": [str], "edits": [str], "controls": [str], "error": str|null }
  ウィンドウが見つからない・コンポーネントが見つからない、のどちらでも例外を投げずに ok:false を返す (終了コード 1。
  他のツールと同じ「失敗を握って先へ進む」形)。

  BOM 付き UTF-8 で保存する (cleanup.ps1 と同じ理由。BOM を外すと PowerShell 5.1 が cp932 として
  読み、ヒアストリングの境界がずれる)。ヒアストリングの中の C# のコメントは英語で書く。

  確認していないこと (実機の実行で確認する): Qt 6 が QMenu の項目を MenuItem として出し InvokePattern を
  持つこと、チェックの付く項目が TogglePattern を持つこと、QListWidget の項目が SelectionItemPattern を
  持つこと、Esc の PostMessage でメニューが閉じること。OBS のメニューバーの [ツール] が MenuItem として
  出て、読み上げの名前が文字の & を外した形 (ja「ツール(T)」、en「Tools」) で、ExpandCollapsePattern の
  Expand でメニューが開くこと (Invoke しか持たなければ Invoke で開くかも)。
  Qt 6 の QComboBox が ComboBox として出て、読み上げの名前が横のラベル (buddy) になり、ExpandCollapsePattern の Expand で
  一覧が開き、開いた一覧のウィンドウへ PostMessage した矢印キーと Enter で現在の項目が変わり、ValuePattern か SelectionPattern で値が読めること
  (実際の OBS の設定のウィンドウでだけ確認できる)。

  セルフテスト (OBS を起こさない。既に動いている explorer.exe に対してだけ、何も押さずに行う):
    -Pid <explorer.exe の pid> -Title "Program Manager" -ListOnly           -> ok:true, found:true
    -Pid <explorer.exe の pid> -Title "no such window" -ListOnly -Timeout 0 -> ok:false, found:false
    -Pid <explorer.exe の pid> -Title "Program Manager" -ListOnly -Button "no such button" -Timeout 0
                                                                            -> ok:true, buttonFound:false
#>

[CmdletBinding()]
param(
  # 他のツールと同じ理由: 読み取り専用の自動変数 $PID と大文字小文字を区別せず衝突するため、
  # 実体は $TargetPid に持たせ、引数名だけ -Pid にする。
  [Parameter(Mandatory = $true)]
  [Alias('Pid')]
  [int]$TargetPid,

  # -Popup でないときは要る (このツールは "cordmux" 以外のウィンドウも探すために存在するので既定を持たない)。
  [AllowEmptyString()]
  [string]$Title = '',

  [switch]$Popup,

  [AllowEmptyString()]
  [string]$Button = '',

  [ValidateSet('Button', 'CheckBox', 'MenuItem', 'ListItem', 'ComboBox', 'TabItem')]
  [string]$ControlType = 'Button',

  # -Button の名前で始まる要素も選ぶ (ドックの [状態] タブは、件数があると「状態 (N)」になる)。
  [switch]$NamePrefix,

  # -ControlType ComboBox で選ぶ項目の名前 (完全一致)。
  [AllowEmptyString()]
  [string]$Select = '',

  [ValidateRange(0, 100)]
  [int]$Index = 0,

  [ValidateRange(0, 120000)]
  [int]$Timeout = 10000,

  [ValidateSet('any', 'on', 'off')]
  [string]$Desired = 'any',

  [switch]$ListOnly,

  [switch]$NoWait
)

$ErrorActionPreference = 'Stop'

# 一覧の上限 (結果の JSON を小さく保つ)。
$maxItems = 200
$maxTextLength = 500
# -NoWait で Invoke の戻りを待つ上限 (モーダルを開かないコンポーネントは数十 ms で戻る)。
$noWaitReturnMs = 1000

function Write-JsonResultAndExit {
  # dock-click.ps1 と同じ理由: 標準出力へ UTF-8 (BOM なし) のバイト列を直接書く。
  param($Result, [int]$ExitCode)
  $json = $Result | ConvertTo-Json -Depth 5 -Compress
  $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
  $bytes = $utf8NoBom.GetBytes($json)
  $stdout = [System.Console]::OpenStandardOutput()
  $stdout.Write($bytes, 0, $bytes.Length)
  $stdout.Flush()
  exit $ExitCode
}

$script:state = [ordered]@{
  found        = $false
  buttonFound  = $null
  invoked      = $false
  pressed      = $false
  expanded     = $false
  returned     = $null
  toggleBefore = $null
  selected     = $null
  valueBefore  = $null
  valueAfter   = $null
  comboValue   = $null
  items        = @()
  buttons      = @()
  texts        = @()
  edits        = @()
  controls     = @()
}

function New-Result {
  param([bool]$Ok, $ErrorText)
  return [PSCustomObject]@{
    ok           = $Ok
    pid          = $TargetPid
    title        = $Title
    popup        = [bool]$Popup
    controlType  = $ControlType
    found        = $script:state.found
    button       = $Button
    index        = $Index
    listOnly     = [bool]$ListOnly
    buttonFound  = $script:state.buttonFound
    invoked      = $script:state.invoked
    pressed      = $script:state.pressed
    expanded     = $script:state.expanded
    returned     = $script:state.returned
    toggleBefore = $script:state.toggleBefore
    desired      = $Desired
    select       = $Select
    selected     = $script:state.selected
    valueBefore  = $script:state.valueBefore
    valueAfter   = $script:state.valueAfter
    comboValue   = $script:state.comboValue
    items        = @($script:state.items)
    buttons      = @($script:state.buttons)
    texts        = @($script:state.texts)
    edits        = @($script:state.edits)
    controls     = @($script:state.controls)
    error        = $ErrorText
  }
}

function Limit-Text {
  param([string]$Text)
  if ($Text.Length -gt $maxTextLength) { return $Text.Substring(0, $maxTextLength) }
  return $Text
}

function Find-Elements {
  # ウィンドウの根から、指定の種類の要素を (根そのものも含めて) 集める。空の名前は捨てる。
  param($Roots, $UiaControlType)
  $condition = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty, $UiaControlType)
  $names = New-Object System.Collections.Generic.List[string]
  $elements = New-Object System.Collections.Generic.List[object]
  $owners = New-Object System.Collections.Generic.List[object]
  foreach ($pair in $Roots) {
    $found = @()
    try {
      $found = $pair.Root.FindAll([System.Windows.Automation.TreeScope]::Subtree, $condition)
    } catch {
      # 探している間に閉じたウィンドウは飛ばす。
      continue
    }
    foreach ($element in $found) {
      $name = $element.Current.Name
      if ([string]::IsNullOrEmpty($name)) { continue }
      if ($names.Count -ge $maxItems) { break }
      $names.Add((Limit-Text $name))
      $elements.Add($element)
      $owners.Add($pair.Handle)
    }
  }
  return [PSCustomObject]@{ Names = $names; Elements = $elements; Owners = $owners }
}

function Read-Edits {
  # 入力欄の名前と値 (場所の行。名前は場所そのもの = message_dialog.cpp の setAccessibleName(path))。
  param($Roots)
  $editType = [System.Windows.Automation.ControlType]::Edit
  $condition = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty, $editType)
  $values = New-Object System.Collections.Generic.List[string]
  foreach ($pair in $Roots) {
    $found = @()
    try {
      $found = $pair.Root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $condition)
    } catch {
      continue
    }
    foreach ($element in $found) {
      if ($values.Count -ge $maxItems) { break }
      $name = $element.Current.Name
      if (-not [string]::IsNullOrEmpty($name)) { $values.Add((Limit-Text $name)) }
      $valuePattern = $null
      if ($element.TryGetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern, [ref]$valuePattern)) {
        $value = $valuePattern.Current.Value
        if (-not [string]::IsNullOrEmpty($value) -and $value -ne $name) { $values.Add((Limit-Text $value)) }
      }
    }
  }
  return $values
}

function Read-ComboValue {
  # ComboBox の現在の値。ValuePattern があればその値、無ければ SelectionPattern で選ばれている項目の名前。読めなければ $null。
  param($Element)
  try {
    $valuePattern = $null
    if ($Element.TryGetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern, [ref]$valuePattern)) {
      return [string]$valuePattern.Current.Value
    }
    $selectionPattern = $null
    if ($Element.TryGetCurrentPattern([System.Windows.Automation.SelectionPattern]::Pattern, [ref]$selectionPattern)) {
      $picked = $selectionPattern.Current.GetSelection()
      if ($picked.Count -gt 0) { return [string]$picked[0].Current.Name }
    }
  } catch {
    return $null
  }
  return $null
}

function Find-ComboPopup {
  # 開いた一覧のウィンドウを探す。対象プロセスの見えているトップレベルウィンドウのうち、OwnerHandle 以外で、名前が ItemName の ListItem を
  # 持つウィンドウである。そのウィンドウの ListItem の名前を上から順に返す (ウィンドウが見つからなければ Handle は $null)。
  param([IntPtr]$OwnerHandle, [string]$ItemName, [uint32]$ProcessId)
  $condition = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::ListItem)
  foreach ($handle in [CordmuxDialogClickNative]::FindTopLevelWindows($ProcessId, '', $true)) {
    if ($handle -eq $OwnerHandle) { continue }
    $names = New-Object System.Collections.Generic.List[string]
    try {
      foreach ($listItem in [System.Windows.Automation.AutomationElement]::FromHandle($handle).FindAll([System.Windows.Automation.TreeScope]::Subtree, $condition)) {
        $names.Add([string]$listItem.Current.Name)
      }
    } catch {
      continue
    }
    if ($names.Contains($ItemName)) { return [PSCustomObject]@{ Handle = $handle; Names = $names.ToArray() } }
  }
  return [PSCustomObject]@{ Handle = $null; Names = @() }
}

try {
  if (-not $Popup -and [string]::IsNullOrEmpty($Title)) {
    Write-JsonResultAndExit -Result (New-Result -Ok $false -ErrorText "-Title is required unless -Popup is given") -ExitCode 1
  }
  if (-not $ListOnly -and [string]::IsNullOrEmpty($Button)) {
    Write-JsonResultAndExit -Result (New-Result -Ok $false -ErrorText "-Button is required unless -ListOnly is given") -ExitCode 1
  }
  if (-not $ListOnly -and $ControlType -eq 'ComboBox' -and [string]::IsNullOrEmpty($Select)) {
    Write-JsonResultAndExit -Result (New-Result -Ok $false -ErrorText "-Select is required for -ControlType ComboBox unless -ListOnly is given") -ExitCode 1
  }

  Add-Type -AssemblyName UIAutomationClient
  Add-Type -AssemblyName UIAutomationTypes

  $nativeSource = @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Automation;

public static class CordmuxDialogClickNative
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
    public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    public const uint GA_ROOT = 2;
    public const uint WM_KEYDOWN = 0x0100;
    public const uint WM_KEYUP = 0x0101;
    public const int VK_ESCAPE = 0x1B;
    public const int VK_UP = 0x26;
    public const int VK_DOWN = 0x28;
    public const int VK_RETURN = 0x0D;

    // Qt は lParam からスキャンコードと拡張の印を読む (矢印キーは拡張キー)。
    public static void PostKey(IntPtr hWnd, int vk)
    {
        long scan = vk == VK_UP ? 0x48 : vk == VK_DOWN ? 0x50 : 0x1C;
        long extended = vk == VK_RETURN ? 0L : 1L << 24;
        long down = 1L | (scan << 16) | extended;
        long up = down | (1L << 30) | (1L << 31);
        PostMessage(hWnd, WM_KEYDOWN, (IntPtr)vk, (IntPtr)down);
        PostMessage(hWnd, WM_KEYUP, (IntPtr)vk, (IntPtr)up);
    }

    // Visible top-level windows (their own root ancestor) owned by targetProcessId.
    // When anyTitle is false, only windows whose title equals wantedTitle exactly.
    public static IntPtr[] FindTopLevelWindows(uint targetProcessId, string wantedTitle, bool anyTitle)
    {
        var found = new List<IntPtr>();
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
            if (anyTitle)
            {
                found.Add(hWnd);
                return true;
            }
            int len = GetWindowTextLength(hWnd);
            if (len == 0) return true;
            var sb = new StringBuilder(len + 1);
            GetWindowText(hWnd, sb, sb.Capacity);
            if (sb.ToString() == wantedTitle) found.Add(hWnd);
            return true;
        }, IntPtr.Zero);
        return found.ToArray();
    }

    // Close a popup menu window without choosing an item.
    public static void PostEscape(IntPtr hWnd)
    {
        PostMessage(hWnd, WM_KEYDOWN, (IntPtr)VK_ESCAPE, IntPtr.Zero);
        PostMessage(hWnd, WM_KEYUP, (IntPtr)VK_ESCAPE, IntPtr.Zero);
    }

    // Start Invoke on a background thread and wait up to waitMs for it to return.
    // A control that opens a modal window (QMenu::exec, QDialog::exec) does not return
    // until that window closes. Returns true when Invoke returned within waitMs.
    public static bool InvokeWithoutWaiting(InvokePattern pattern, int waitMs)
    {
        var thread = new Thread(delegate () { try { pattern.Invoke(); } catch (Exception) { } });
        thread.IsBackground = true;
        thread.Start();
        return thread.Join(waitMs);
    }

    // Same as InvokeWithoutWaiting, for a menu item that owns a submenu (ExpandCollapsePattern.Expand).
    public static bool ExpandWithoutWaiting(ExpandCollapsePattern pattern, int waitMs)
    {
        var thread = new Thread(delegate () { try { pattern.Expand(); } catch (Exception) { } });
        thread.IsBackground = true;
        thread.Start();
        return thread.Join(waitMs);
    }
}
"@
  Add-Type -TypeDefinition $nativeSource -ReferencedAssemblies UIAutomationClient, UIAutomationTypes

  $proc = Get-Process -Id $TargetPid -ErrorAction SilentlyContinue
  if (-not $proc) {
    Write-JsonResultAndExit -Result (New-Result -Ok $false -ErrorText "process not found for pid $TargetPid") -ExitCode 1
  }

  $wantedType = [System.Windows.Automation.ControlType]::$ControlType
  $buttonType = [System.Windows.Automation.ControlType]::Button
  $textType = [System.Windows.Automation.ControlType]::Text
  $deadline = (Get-Date).AddMilliseconds($Timeout)
  $where = if ($Popup) { "any visible top-level window" } else { "a visible top-level window titled '$Title'" }
  $lastError = "no $where for pid $TargetPid within $Timeout ms"

  while ($true) {
    $handles = [CordmuxDialogClickNative]::FindTopLevelWindows([uint32]$TargetPid, $Title, [bool]$Popup)
    if ($handles.Count -gt 0) {
      $script:state.found = $true
      $roots = @()
      foreach ($h in $handles) {
        try {
          $roots += [PSCustomObject]@{ Handle = $h; Root = [System.Windows.Automation.AutomationElement]::FromHandle($h) }
        } catch {
          # 探している間に閉じたウィンドウは飛ばす。
        }
      }
      $buttons = Find-Elements -Roots $roots -UiaControlType $buttonType
      $texts = Find-Elements -Roots $roots -UiaControlType $textType
      $edits = Read-Edits -Roots $roots
      $wanted = if ($ControlType -eq 'Button') { $buttons } else { Find-Elements -Roots $roots -UiaControlType $wantedType }
      $script:state.buttons = @($buttons.Names)
      $script:state.edits = @($edits)
      $script:state.texts = @($texts.Names) + @($edits)
      $script:state.controls = @($wanted.Names)

      $matched = @()
      $matchedOwners = @()
      if (-not [string]::IsNullOrEmpty($Button)) {
        for ($i = 0; $i -lt $wanted.Names.Count; $i++) {
          if ($wanted.Names[$i] -eq $Button -or ($NamePrefix -and $wanted.Names[$i].StartsWith($Button, [System.StringComparison]::Ordinal))) {
            $matched += $wanted.Elements[$i]
            $matchedOwners += $wanted.Owners[$i]
          }
        }
      }

      if ($ListOnly) {
        if ([string]::IsNullOrEmpty($Button)) {
          Write-JsonResultAndExit -Result (New-Result -Ok $true -ErrorText $null) -ExitCode 0
        }
        if ($matched.Count -gt 0) {
          $script:state.buttonFound = $true
          if ($ControlType -eq 'ComboBox') { $script:state.comboValue = Read-ComboValue $matched[0] }
          Write-JsonResultAndExit -Result (New-Result -Ok $true -ErrorText $null) -ExitCode 0
        }
        $lastError = "$ControlType '$Button' did not appear within $Timeout ms"
      } elseif ($Index -lt $matched.Count) {
        $script:state.buttonFound = $true
        $target = $matched[$Index]
        $owner = $matchedOwners[$Index]

        if ($ControlType -eq 'ListItem' -or $ControlType -eq 'TabItem') {
          $selectionPattern = $null
          if ($target.TryGetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern, [ref]$selectionPattern)) {
            $selectionPattern.Select()
            $script:state.invoked = $true
            $script:state.pressed = $true
            $script:state.returned = $true
            Write-JsonResultAndExit -Result (New-Result -Ok $true -ErrorText $null) -ExitCode 0
          }
          $lastError = "the list item does not support SelectionItemPattern"
        } elseif ($ControlType -eq 'ComboBox') {
          # ComboBox は項目を選ぶ。ExpandCollapsePattern の Expand で一覧を開き、一覧のウィンドウへ矢印キーと Enter を送る。開いた一覧は
          # フォーカスがなくてもキーを受け、現在の項目から動かして Enter で決める。設定のウィンドウは [OK] か [適用] を押すまで値を使わない。
          $lastError = $null
          $script:state.valueBefore = Read-ComboValue $target
          if ($null -eq $script:state.valueBefore) {
            $lastError = "the ComboBox value could not be read"
          } elseif ($script:state.valueBefore -ne $Select) {
            $expandPattern = $null
            if (-not $target.TryGetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern, [ref]$expandPattern)) {
              $lastError = "the ComboBox has no ExpandCollapsePattern"
            } else {
              try {
                $expandPattern.Expand()
                $script:state.expanded = $true
                Start-Sleep -Milliseconds 300
                $opened = Find-ComboPopup -OwnerHandle ([IntPtr]$owner) -ItemName $Select -ProcessId ([uint32]$TargetPid)
                $script:state.items = @($opened.Names)
                if ($null -eq $opened.Handle) {
                  $lastError = "the open list of the ComboBox has no item '$Select'"
                  $expandPattern.Collapse()
                } else {
                  $from = [array]::IndexOf([string[]]$opened.Names, [string]$script:state.valueBefore)
                  $to = [array]::IndexOf([string[]]$opened.Names, [string]$Select)
                  if ($from -lt 0) {
                    $lastError = "the open list of the ComboBox has no item for the current value"
                    $expandPattern.Collapse()
                  } else {
                    $arrow = if ($to -gt $from) { [CordmuxDialogClickNative]::VK_DOWN } else { [CordmuxDialogClickNative]::VK_UP }
                    for ($step = 0; $step -lt [Math]::Abs($to - $from); $step++) {
                      [CordmuxDialogClickNative]::PostKey($opened.Handle, $arrow)
                      Start-Sleep -Milliseconds 40
                    }
                    [CordmuxDialogClickNative]::PostKey($opened.Handle, [CordmuxDialogClickNative]::VK_RETURN)
                    Start-Sleep -Milliseconds 200
                  }
                }
              } catch {
                $lastError = "the ComboBox could not be changed: " + $_.Exception.Message
              }
            }
          }
          $script:state.valueAfter = Read-ComboValue $target
          if ($script:state.valueAfter -eq $Select) {
            $script:state.selected = $true
            $script:state.invoked = $true
            $script:state.pressed = ($script:state.valueBefore -ne $Select)
            $script:state.returned = $true
            Write-JsonResultAndExit -Result (New-Result -Ok $true -ErrorText $null) -ExitCode 0
          }
          $script:state.selected = $false
          if ([string]::IsNullOrEmpty($lastError)) { $lastError = "the ComboBox value after selecting is '$($script:state.valueAfter)', not '$Select'" }
          Write-JsonResultAndExit -Result (New-Result -Ok $false -ErrorText $lastError) -ExitCode 1
        } else {
          $togglePattern = $null
          $hasToggle = $target.TryGetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern, [ref]$togglePattern)
          if ($hasToggle) { $script:state.toggleBefore = [string]$togglePattern.Current.ToggleState }
          $wantOn = [System.Windows.Automation.ToggleState]::On
          $wantOff = [System.Windows.Automation.ToggleState]::Off
          $alreadyThere = $false
          if ($hasToggle -and $Desired -eq 'on') { $alreadyThere = ($togglePattern.Current.ToggleState -eq $wantOn) }
          if ($hasToggle -and $Desired -eq 'off') { $alreadyThere = ($togglePattern.Current.ToggleState -eq $wantOff) }
          if ($alreadyThere) {
            # 行き先と同じなので押さない。メニューの項目なら、開いたままにしないよう閉じる。
            if ($ControlType -eq 'MenuItem') { [CordmuxDialogClickNative]::PostEscape($owner) }
            $script:state.returned = $true
            Write-JsonResultAndExit -Result (New-Result -Ok $true -ErrorText $null) -ExitCode 0
          }
          # 下にメニューを持つ項目 (OBS のメニューバーの [ツール] など) は Expand で開く。Qt は子のメニューを持つ
          # 項目に ExpandCollapsePattern を出し、Invoke を出さないことがある (確認していないこと)。葉の項目は下の Invoke。
          $expandPattern = $null
          if ($ControlType -eq 'MenuItem' -and
              $target.TryGetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern, [ref]$expandPattern) -and
              $expandPattern.Current.ExpandCollapseState -ne [System.Windows.Automation.ExpandCollapseState]::LeafNode) {
            $script:state.invoked = $true
            $script:state.pressed = $true
            $script:state.expanded = $true
            if ($NoWait) {
              $script:state.returned = [CordmuxDialogClickNative]::ExpandWithoutWaiting($expandPattern, $noWaitReturnMs)
            } else {
              $expandPattern.Expand()
              $script:state.returned = $true
            }
            Write-JsonResultAndExit -Result (New-Result -Ok $true -ErrorText $null) -ExitCode 0
          }
          $invokePattern = $null
          if ($target.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$invokePattern)) {
            $script:state.invoked = $true
            $script:state.pressed = $true
            if ($NoWait) {
              $script:state.returned = [CordmuxDialogClickNative]::InvokeWithoutWaiting($invokePattern, $noWaitReturnMs)
            } else {
              $invokePattern.Invoke()
              $script:state.returned = $true
            }
            Write-JsonResultAndExit -Result (New-Result -Ok $true -ErrorText $null) -ExitCode 0
          }
          if ($hasToggle) {
            $togglePattern.Toggle()
            $script:state.invoked = $true
            $script:state.pressed = $true
            $script:state.returned = $true
            Write-JsonResultAndExit -Result (New-Result -Ok $true -ErrorText $null) -ExitCode 0
          }
          $lastError = "the $ControlType supports neither InvokePattern nor TogglePattern"
        }
      } else {
        $lastError = "$ControlType '$Button' (index $Index) not found in $where"
      }
    }
    if ((Get-Date) -ge $deadline) { break }
    Start-Sleep -Milliseconds 300
  }

  if ($ListOnly -and -not [string]::IsNullOrEmpty($Button)) { $script:state.buttonFound = $false }
  if (-not $ListOnly -and $script:state.found -and $null -eq $script:state.buttonFound) { $script:state.buttonFound = $false }
  # -ListOnly でウィンドウはあったがコンポーネントが出なかったときは、ウィンドウがあったこと自体は ok (答えは buttonFound)。
  $ok = $ListOnly -and $script:state.found
  $exitCode = 1
  if ($ok) { $exitCode = 0 }
  $errorText = $lastError
  if ($ok) { $errorText = $null }
  Write-JsonResultAndExit -Result (New-Result -Ok $ok -ErrorText $errorText) -ExitCode $exitCode
}
catch {
  Write-JsonResultAndExit -Result (New-Result -Ok $false -ErrorText $_.Exception.Message) -ExitCode 1
}
