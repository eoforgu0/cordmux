#requires -Version 5.1
<#
  explorer-windows.ps1 - 場所が _work の下のエクスプローラーのウィンドウだけを読み、製品のボタンが開いたウィンドウだけを閉じるツール。

  製品の [フォルダーの場所を開く]・[ファイルの場所を開く]・[フォルダーを開く] が開いたエクスプローラーの行き先
  (どのフォルダーが開いたか、項目が選ばれているか) を数で残し、こちらが開かせたウィンドウだけを閉じる。
  a11y-record.ps1 -Tree でも選ばれた項目は読める (SelectionItemPattern) が、ウィンドウを閉じることはできない。
  利用者が開いているウィンドウを閉じない守りをツールの側に持たせる。

  読み方: Shell.Application の Windows() (開いているエクスプローラーのウィンドウ。Internet Explorer のウィンドウも入る) の 1 つずつについて、
  実行ファイル (FullName) が explorer.exe で、場所 (LocationURL から求めたパス) が -Root の下 (-Root そのものを含む) のウィンドウだけを、
  題 (LocationName)・場所・選ばれた項目 (Document.SelectedItems() の Path)・開いているフォルダー (Document.Folder.Self.Path) とともに
  返す。-Root の外のウィンドウは場所 (LocationURL) だけを見て、ほかは読まず閉じず、数 (outsideCount) だけを返す。読めなかったウィンドウ
  (閉じかけのウィンドウは COM の読みで例外を出す) は unreadableCount に数える。前面は取らない (SetForegroundWindow もウィンドウの操作も呼ばない。
  Quit() はウィンドウを前に出さずに閉じる)。

  -Close -Before <before.json> -Expect <フォルダー>: 閉じるウィンドウはツールが自分で決める。
  -Before は製品のボタンを押す前にこのツールで読んだ結果の JSON (-Out で書いた物)、-Expect は押したボタンが開くはずのフォルダー。
  閉じるのは「現在 -Root の下にあり、HWND が -Before のウィンドウになく、場所が -Expect と同じ」ウィンドウがちょうど 1 つのときのその 1 つだけ。
  次のときは何も閉じずに断る (終了コード 2):
    - -Before が読めない・このツールの読みでない (close が偽・ok が真・断りでない、の形でない)・-Root が現在と違う。
    - -Before に読めなかったウィンドウ (unreadableCount が 0 でない) か HWND のないウィンドウがある (そのウィンドウが押した後に読めると「新しいウィンドウ」に見える)。
    - 場所が -Expect と同じウィンドウに、現在 HWND を読めない物がある (前にあったかを照合できない)。
    - 一致するウィンドウが 2 つ以上 (どれがボタンの開いたウィンドウか分からない)。
    - 一致するウィンドウが 1 つでも、Shell のウィンドウの総数 (下のウィンドウ・外のウィンドウ・読めないウィンドウの数の和) が -Before より増えていない
      (新しいウィンドウが開いていない。外を見ていた利用者のウィンドウが -Expect へ移ってきた形)。
  一致するウィンドウが 0 なら何も閉じずに ok (エクスプローラーが新しいウィンドウを開かず、前のウィンドウを前に出しただけの形)。閉じたウィンドウは -CloseWaitMs
  (既定 3000) の間にウィンドウの一覧から消えたかを closed に書く。
  このように決める理由: 場所の一致は「こちらが開かせたウィンドウ」の証明にならない (利用者が前のウィンドウの録画を見るために _work の下を開いていれば、
  場所は同じ)。押す前と後の差から呼び出し元が -Hwnd で名指しする方法では、守りが差の取り方に依存する
  (押す前に読めなかったウィンドウ・HWND のないウィンドウ・外から移ってきたウィンドウが差に入る)。そのため差の取り方と確認をツールの中に持たせる。
  次の方法は採らない。-Hwnd の名指しのまま手で確認する。-Before のない -Close で下のウィンドウを全部閉じる (利用者のウィンドウを閉じる)。
  読みに全部の Shell のウィンドウの HWND を足して差を全部のウィンドウと取る (読みが外のウィンドウの HWND まで広がる。総数の比べで
  同じ穴 (外から移ってきたウィンドウ) を、読みを広げずに塞げる)。-Before の古さを時刻で切る (総数の比べと -Expect と「ちょうど 1 つ」で、
  古い -Before でも利用者のウィンドウを選ぶには、利用者が同じ時に同じフォルダーを新しく開く必要があり、時刻のしきい値に根拠がない)。
  -Expect を題 (LocationName) で比べる (同じ名前のフォルダーはどこにでもある)。
  -Hwnd は受けて断る (終了コード 2 と理由の JSON。引数を消すと、古い呼び出しが JSON でない誤りで落ちる)。

  -Root の守り: -Root は読むときも閉じるときも <repo>\_work と同じかその下でなければ断る
  (終了コード 2。Shell.Application も作らない)。-Expect も -Root の下でなければ断る。
  -Root の守りを -Close のときだけにする方法は採らない (読みは閉じないが、広い -Root では利用者のウィンドウの題と選ばれたファイルの名前を
  -Out の JSON に書く。このツールの相手は製品が _work の下に開かせたウィンドウだけで、外を読む呼び出し元はない)。
  前提: Windows 10 のエクスプローラーにはタブがなく、1 つのウィンドウに 1 つの HWND。Windows 11 のタブでは同じ枠の
  HWND を複数の項目が持つので、この前提が崩れる (配布しないテストのツールなので、Windows 10 の前提として書く)。

  場所の判定 (Test-LocationUnderRoot): LocationURL の file:///C:/... を UnescapeDataString でパスに変換 (%20 の空白、%E9.. の UTF-8 の文字)、
  / を \ にして GetFullPath で正規化 (.. を畳む) し、大文字小文字を区別せずに -Root と同じか、-Root + '\' で始まるかを見る
  (-Root の名前で始まる別のフォルダー (_work2) は外)。file://server/share は \server\share に変換する。file: でない場所 (空、
  ::{GUID} の特別なフォルダー、http) は外。-Expect との一致も同じ変換で「同じパス」かを見る。Document.Folder.Self.Path で
  判定する方法は採らない (判定のために外のウィンドウの Document まで読むことになる。LocationURL はウィンドウの場所だけを返す)。

  結果は 1 行の JSON を標準出力に書く (harness-suspend.ps1 と同じ UTF-8)。-Out を渡せば同じ JSON をそのファイルにも書く。
    読み: { "ok", "root", "close": false, "windows": [ { "hwnd", "title", "location", "url", "folderPath", "selected": [...] } ],
            "outsideCount", "unreadableCount", "error" }
    閉じ: { "ok", "root", "close": true, "before", "expect", "windows": [ { ..., "isNew", "atExpect", "quitCalled", "closed" } ],
            "matched": [hwnd...], "target", "closed", "beforeTotal", "total", "outsideCount", "unreadableCount", "refused", "error", "note" }
  終了コード: 0 = 読めた (-Close なら決めた 1 つを閉じたか、閉じるウィンドウが無かった)、1 = 決めたウィンドウが閉じなかった・読みの例外、
  2 = 断った (何も閉じていない)。

  -SelfTest: ウィンドウを開かず、Shell.Application も作らない。場所の判定、-Root の守り、引数の守り、偽のウィンドウの組で「下のウィンドウだけを読み、外の
  ウィンドウの Document を読まない」ことと、-Before と -Expect の閉じの決め (新しいウィンドウ 1 つだけを閉じる・前からあったウィンドウと -Expect でないウィンドウは
  閉じない・断りの形を全部) を、このツールの読みの JSON を往復させた -Before で当てる。

  使い方:
    powershell -NoProfile -ExecutionPolicy Bypass -File test/tools/obs-run/explorer-windows.ps1 [-Root <dir>] [-Out <before.json>]
    powershell -NoProfile -ExecutionPolicy Bypass -File test/tools/obs-run/explorer-windows.ps1 [-Root <dir>] -Close -Before <before.json> -Expect <dir> [-CloseWaitMs 3000] [-Out <close.json>]
    powershell -NoProfile -ExecutionPolicy Bypass -File test/tools/obs-run/explorer-windows.ps1 -SelfTest
#>

[CmdletBinding()]
param(
  # 既定は repo の _work (実行スクリプトの --out の保存先。製品の録画の保存先と回の出力はこの下)。
  [string]$Root = '',
  [switch]$Close,
  # -Close の材料: 製品のボタンを押す前にこのツールで読んだ JSON と、ボタンが開くはずのフォルダー (上の「-Close」)。
  [string]$Before = '',
  [string]$Expect = '',
  # 既定では使わない。受けて断る (上の説明)。
  [string]$Hwnd = '',
  [ValidateRange(0, 60000)]
  [int]$CloseWaitMs = 3000,
  [string]$Out = '',
  [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'

function Write-JsonResultAndExit {
  # harness-suspend.ps1 と同じ理由: 標準出力へ UTF-8 (BOM なし) のバイト列を直接書く。
  param($Result, [int]$ExitCode)
  $json = $Result | ConvertTo-Json -Depth 6 -Compress
  $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
  if ($Out) {
    try { [System.IO.File]::WriteAllText($Out, $json, $utf8NoBom) } catch { }
  }
  $bytes = $utf8NoBom.GetBytes($json)
  $stdout = [System.Console]::OpenStandardOutput()
  $stdout.Write($bytes, 0, $bytes.Length)
  $stdout.Flush()
  exit $ExitCode
}

# LocationURL をファイルシステムのパスに修正する。file: でなければ $null。
function ConvertFrom-LocationUrl {
  param([string]$Url)
  if (-not $Url) { return $null }
  if ($Url -match '^(?i)file:///') {
    $rest = [System.Uri]::UnescapeDataString($Url.Substring(8))
    return ($rest -replace '/', '\')
  }
  if ($Url -match '^(?i)file://') {
    $rest = [System.Uri]::UnescapeDataString($Url.Substring(7))
    return ('\\' + ($rest -replace '/', '\'))
  }
  return $null
}

# パスを比べる形に修正する (GetFullPath、末尾の \ を落とす)。修正できなければ $null。
function ConvertTo-ComparablePath {
  param([string]$Path)
  if (-not $Path) { return $null }
  try { return [System.IO.Path]::GetFullPath($Path).TrimEnd('\') } catch { return $null }
}

# パスが -Root と同じか、その下か (大文字小文字を区別しない。.. は GetFullPath で畳む)。修正できないパスは外。
function Test-PathUnderRoot {
  param([string]$Path, [string]$RootPath)
  $full = ConvertTo-ComparablePath $Path
  $r = ConvertTo-ComparablePath $RootPath
  if (-not $full -or -not $r) { return $false }
  if ($full.Equals($r, [System.StringComparison]::OrdinalIgnoreCase)) { return $true }
  return $full.StartsWith($r + '\', [System.StringComparison]::OrdinalIgnoreCase)
}

# 2 つのパスが同じ場所か (同じ変換。大文字小文字を区別しない)。
function Test-SamePath {
  param([string]$A, [string]$B)
  $x = ConvertTo-ComparablePath $A
  $y = ConvertTo-ComparablePath $B
  if (-not $x -or -not $y) { return $false }
  return $x.Equals($y, [System.StringComparison]::OrdinalIgnoreCase)
}

function Test-LocationUnderRoot {
  param([string]$Url, [string]$RootPath)
  return (Test-PathUnderRoot -Path (ConvertFrom-LocationUrl -Url $Url) -RootPath $RootPath)
}

function Test-IsExplorer {
  param([string]$FullName)
  if (-not $FullName) { return $false }
  return ([System.IO.Path]::GetFileName($FullName) -ieq 'explorer.exe')
}

# repo の _work (このツールが見てよい場所の上限)。ツールの保存先 test/tools/obs-run から 3 つ上が repo。
function Get-RepoWorkDir {
  return [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\_work'))
}

# -Root が見てよい場所 (AllowedBase = _work) と同じかその下か。判定はウィンドウの場所と同じ Test-PathUnderRoot
# (大文字小文字を区別しない、.. を畳む、_work2 は外)。
function Test-RootAllowed {
  param([string]$RootPath, [string]$AllowedBase)
  return (Test-PathUnderRoot -Path $RootPath -RootPath $AllowedBase)
}

# ウィンドウの組を読み、-Root の下のウィンドウだけを返す (閉じない)。外のウィンドウは FullName と LocationURL だけを見る。
# 返り値の com は windows と同じ並びの COM のウィンドウ (閉じるときに使う。JSON には出さない)。
function Read-ExplorerWindows {
  param($Windows, [string]$RootPath)
  $matched = New-Object System.Collections.ArrayList
  $coms = New-Object System.Collections.ArrayList
  $outside = 0
  $unreadable = 0
  foreach ($w in @($Windows)) {
    if ($null -eq $w) { continue }
    $fullName = $null
    $url = $null
    try {
      $fullName = [string]$w.FullName
      $url = [string]$w.LocationURL
    }
    catch {
      $unreadable++
      continue
    }
    # 閉じかけのウィンドウは読みが例外を出すか、PowerShell が例外を飲んで空を返す。エクスプローラーのウィンドウなら実行ファイルの名前は必ずあるので、
    # 空は読めなかったウィンドウとして数える (外のウィンドウとも下のウィンドウとも決めない = 閉じない)。
    if (-not $fullName) {
      $unreadable++
      continue
    }
    if (-not (Test-IsExplorer -FullName $fullName) -or -not (Test-LocationUnderRoot -Url $url -RootPath $RootPath)) {
      $outside++
      continue
    }
    $rec = [ordered]@{ hwnd = $null; title = $null; location = (ConvertFrom-LocationUrl -Url $url); url = $url; folderPath = $null; selected = @() }
    try {
      $hv = $w.HWND
      if ($null -ne $hv) { $rec.hwnd = [int64]$hv }
    }
    catch { }
    try { $rec.title = [string]$w.LocationName } catch { }
    try {
      $doc = $w.Document
      try { $rec.folderPath = [string]$doc.Folder.Self.Path } catch { }
      try { $rec.selected = @($doc.SelectedItems() | ForEach-Object { [string]$_.Path }) } catch { }
    }
    catch { }
    [void]$matched.Add([PSCustomObject]$rec)
    [void]$coms.Add($w)
  }
  return [PSCustomObject]@{ windows = $matched.ToArray(); com = $coms.ToArray(); outsideCount = $outside; unreadableCount = $unreadable }
}

# 読みの結果 (JSON に書く形)。
function New-ReadResult {
  param([string]$RootPath, $Read)
  return [PSCustomObject][ordered]@{
    ok              = $true
    root            = $RootPath
    close           = $false
    windows         = @($Read.windows)
    outsideCount    = $Read.outsideCount
    unreadableCount = $Read.unreadableCount
    error           = $null
  }
}

function Get-ShellWindows {
  $shell = New-Object -ComObject Shell.Application
  return @($shell.Windows())
}

function Get-OpenHwnds {
  $hwnds = @{}
  foreach ($w in (Get-ShellWindows)) {
    try { $hwnds[[int64]$w.HWND] = $true } catch { }
  }
  return $hwnds
}

# 引数を読み、守りに当たれば error に理由を入れて返す (ウィンドウの一覧を読む前に呼ぶ。断るときは Shell.Application も作らない)。
#   rootPath: 正規化した -Root (空なら AllowedBase)。beforePath と expectPath: -Close のときだけ。
function Resolve-Arguments {
  param([string]$RootText, [bool]$CloseSwitch, [string]$BeforeText, [string]$ExpectText, [string]$HwndText, [string]$AllowedBase)
  $res = [ordered]@{ rootPath = $null; beforePath = $null; expectPath = $null; error = $null }
  try {
    $res.rootPath = if ($RootText) { [System.IO.Path]::GetFullPath($RootText) } else { [System.IO.Path]::GetFullPath($AllowedBase) }
  }
  catch {
    $res.error = 'the root is not a valid path: ' + $RootText
    return [PSCustomObject]$res
  }
  if (-not (Test-RootAllowed -RootPath $res.rootPath -AllowedBase $AllowedBase)) {
    $res.error = 'the root must be the work folder or a folder under it (' + $AllowedBase + '): ' + $res.rootPath
    return [PSCustomObject]$res
  }
  if ($HwndText -and $HwndText.Trim()) {
    $res.error = '-Hwnd is no longer used: -Close takes -Before <the read taken before pressing the button> and -Expect <the folder the button opens>, and picks the window itself'
    return [PSCustomObject]$res
  }
  $hasBefore = [bool]($BeforeText -and $BeforeText.Trim())
  $hasExpect = [bool]($ExpectText -and $ExpectText.Trim())
  if (-not $CloseSwitch) {
    if ($hasBefore -or $hasExpect) { $res.error = '-Before and -Expect are only used with -Close' }
    return [PSCustomObject]$res
  }
  if (-not $hasBefore -or -not $hasExpect) {
    $res.error = '-Close needs -Before <the read taken before pressing the button> and -Expect <the folder the button opens>'
    return [PSCustomObject]$res
  }
  try { $res.beforePath = [System.IO.Path]::GetFullPath($BeforeText) } catch { $res.error = '-Before is not a valid path: ' + $BeforeText; return [PSCustomObject]$res }
  $exp = ConvertTo-ComparablePath $ExpectText
  if (-not $exp) { $res.error = '-Expect is not a valid path: ' + $ExpectText; return [PSCustomObject]$res }
  if (-not (Test-PathUnderRoot -Path $exp -RootPath $res.rootPath)) {
    $res.error = '-Expect must be the root or a folder under it (' + $res.rootPath + '): ' + $exp
    return [PSCustomObject]$res
  }
  $res.expectPath = $exp
  return [PSCustomObject]$res
}

# -Before の JSON (このツールの読みの結果) を確認し、前にあったウィンドウの HWND と Shell のウィンドウの総数を返す。使えなければ error。
function Read-BeforeSnapshot {
  param([string]$JsonText, [string]$RootPath)
  $res = [ordered]@{ hwnds = @{}; total = 0; error = $null }
  $b = $null
  try { $b = $JsonText | ConvertFrom-Json } catch { $res.error = 'the -Before file is not JSON: ' + $_.Exception.Message; return [PSCustomObject]$res }
  if ($null -eq $b -or $null -eq $b.PSObject.Properties['windows'] -or $null -eq $b.PSObject.Properties['unreadableCount'] -or $null -eq $b.PSObject.Properties['outsideCount']) {
    $res.error = 'the -Before file is not a read of this tool (windows, outsideCount and unreadableCount are needed)'; return [PSCustomObject]$res
  }
  if ($b.close -ne $false -or $b.ok -ne $true -or ($null -ne $b.PSObject.Properties['refused'] -and $b.refused) -or ($null -ne $b.PSObject.Properties['selftest'])) {
    $res.error = 'the -Before file must be a successful read without -Close (not a close result, a refusal or a self-test)'; return [PSCustomObject]$res
  }
  if (-not (Test-SamePath -A ([string]$b.root) -B $RootPath)) {
    $res.error = 'the -Before read was taken with another root (' + [string]$b.root + '), not ' + $RootPath; return [PSCustomObject]$res
  }
  if ([int]$b.unreadableCount -ne 0) {
    $res.error = 'the -Before read had ' + $b.unreadableCount + ' unreadable windows; read again before pressing the button'; return [PSCustomObject]$res
  }
  $wins = @(@($b.windows) | Where-Object { $null -ne $_ })
  foreach ($w in $wins) {
    $h = $null
    if ($null -ne $w.PSObject.Properties['hwnd']) { $h = $w.hwnd }
    if ($null -eq $h -or [int64]$h -le 0) {
      $res.error = 'the -Before read has a window without a handle (' + [string]$w.location + '); read again before pressing the button'; return [PSCustomObject]$res
    }
    $res.hwnds[[int64]$h] = $true
  }
  $res.total = $wins.Count + [int]$b.outsideCount + [int]$b.unreadableCount
  return [PSCustomObject]$res
}

# 閉じるウィンドウを決める (閉じない)。Read は Read-ExplorerWindows の結果、BeforeInfo は Read-BeforeSnapshot の結果。
# 返り値: target (windows の並びの位置。無ければ -1)、matched (当たった HWND)、refused と error、note。windows に isNew・atExpect を付ける。
function Select-CloseTarget {
  param($Read, $BeforeInfo, [string]$ExpectPath)
  $res = [ordered]@{ target = -1; matched = @(); refused = $false; error = $null; note = $null; total = 0 }
  $res.total = @($Read.windows).Count + [int]$Read.outsideCount + [int]$Read.unreadableCount
  $hits = New-Object System.Collections.ArrayList
  $i = 0
  foreach ($w in @($Read.windows)) {
    $atExpect = Test-SamePath -A ([string]$w.location) -B $ExpectPath
    $isNew = $null
    if ($null -ne $w.hwnd) { $isNew = -not $BeforeInfo.hwnds.ContainsKey([int64]$w.hwnd) }
    $w | Add-Member -NotePropertyName isNew -NotePropertyValue $isNew -Force
    $w | Add-Member -NotePropertyName atExpect -NotePropertyValue $atExpect -Force
    if ($atExpect -and $null -eq $w.hwnd) {
      $res.refused = $true
      $res.error = 'a window at the expected folder has no readable handle, so it cannot be told apart from the windows before; nothing is closed'
    }
    if ($atExpect -and $isNew -eq $true) { [void]$hits.Add($i) }
    $i++
  }
  $res.matched = @($hits | ForEach-Object { [int64]$Read.windows[$_].hwnd })
  if ($res.refused) { return [PSCustomObject]$res }
  if ($hits.Count -eq 0) {
    $res.note = 'no new window at the expected folder; nothing is closed (Explorer may have brought an existing window to the front)'
    return [PSCustomObject]$res
  }
  if ($hits.Count -gt 1) {
    $res.refused = $true
    $res.error = 'more than one new window is at the expected folder (' + (($res.matched) -join ',') + '); nothing is closed'
    return [PSCustomObject]$res
  }
  if ($res.total -le $BeforeInfo.total) {
    $res.refused = $true
    $res.error = 'no window was added since the read before the button (' + $BeforeInfo.total + ' then, ' + $res.total + ' now), so the window at the expected folder was not opened by the button; nothing is closed'
    return [PSCustomObject]$res
  }
  $res.target = [int]$hits[0]
  return [PSCustomObject]$res
}

# 決めた 1 つのウィンドウに Quit() を呼ぶ (セルフテストも本物も同じ道)。windows に quitCalled と closed を付ける。
function Invoke-CloseTarget {
  param($Read, $Decision)
  foreach ($w in @($Read.windows)) {
    $w | Add-Member -NotePropertyName quitCalled -NotePropertyValue $false -Force
    $w | Add-Member -NotePropertyName closed -NotePropertyValue $null -Force
  }
  if ($Decision.refused -or $Decision.target -lt 0) { return $null }
  $w = $Read.windows[$Decision.target]
  try {
    $Read.com[$Decision.target].Quit()
    $w.quitCalled = $true
  }
  catch {
    $w | Add-Member -NotePropertyName quitError -NotePropertyValue $_.Exception.Message -Force
  }
  return $w
}

# ---- セルフテスト (ウィンドウを開かない。Shell.Application も作らない) ----------------------------------------------
function Invoke-SelfTest {
  $failures = New-Object System.Collections.ArrayList
  $checks = New-Object System.Collections.ArrayList
  function Check {
    param([bool]$Cond, [string]$Label)
    [void]$checks.Add([PSCustomObject]@{ ok = $Cond; label = $Label })
    if (-not $Cond) { [void]$failures.Add($Label) }
  }
  $root = 'C:\repo\_work'
  $base = 'file:///C:/repo/_work'
  # (1) 場所の判定。並び: LocationURL・下か・見出し。
  $rec = [string]([char]0x9332) + [string]([char]0x753B)
  $cases = @(
    @(($base + '/out/live-a/run-b'), $true, 'a folder under the root'),
    @(($base + '/out/live%20a/run-b'), $true, 'a %20 space is decoded'),
    @(($base + '/out/%E9%8C%B2%E7%94%BB'), $true, 'percent-encoded UTF-8 is decoded'),
    @('file:///c:/REPO/_WORK/x', $true, 'case does not matter'),
    @($base, $true, 'the root itself'),
    @(($base + '/'), $true, 'the root with a trailing slash'),
    @('FILE:///C:/repo/_work/x', $true, 'the scheme in capitals'),
    @('file:///C:/repo/_work2/x', $false, 'a sibling whose name starts with the root name'),
    @('file:///C:/repo/_workX', $false, 'a sibling with a longer name'),
    @('file:///C:/repo', $false, 'the parent of the root'),
    @(($base + '/../secret'), $false, 'a .. that leaves the root'),
    @('file://server/share/_work/x', $false, 'a UNC path (other machine)'),
    @('', $false, 'an empty location (This PC and other shell folders)'),
    @('::{20D04FE0-3AEA-1069-A2D8-08002B30309D}', $false, 'a shell namespace id'),
    @('http://example.com/_work/', $false, 'a web address')
  )
  foreach ($c in $cases) {
    $got = Test-LocationUnderRoot -Url $c[0] -RootPath $root
    Check ($got -eq $c[1]) ('location: ' + $c[2] + ' -> ' + $c[1])
  }
  Check ((ConvertFrom-LocationUrl -Url ($base + '/out/live%20a')) -eq ($root + '\out\live a')) 'the %20 path reads as the real folder name'
  Check ((ConvertFrom-LocationUrl -Url ($base + '/out/%E9%8C%B2%E7%94%BB')) -eq ($root + '\out\' + $rec)) 'the UTF-8 path reads as the real folder name'
  Check ((ConvertFrom-LocationUrl -Url 'file://server/share/x') -eq '\\server\share\x') 'file://server/share reads as a UNC path'
  Check ($null -eq (ConvertFrom-LocationUrl -Url '::{20D04FE0-3AEA-1069-A2D8-08002B30309D}')) 'a non-file location has no path'
  Check (Test-IsExplorer -FullName 'C:\Windows\explorer.exe') 'explorer.exe is an Explorer window'
  Check (-not (Test-IsExplorer -FullName 'C:\Program Files\Internet Explorer\iexplore.exe')) 'iexplore.exe is not an Explorer window'
  Check ((Test-SamePath -A ($root + '\out\X\') -B ($root + '\OUT\x')) -and -not (Test-SamePath -A ($root + '\out\x') -B ($root + '\out\x2'))) 'same path: case and a trailing backslash do not matter, a longer name is another folder'

  # (2) -Root の守り: _work と同じかその下だけ (読みも閉じも)。並び: -Root・よいか・見出し。
  $rootCases = @(
    @($root, $true, 'the work folder itself'),
    @(($root + '\'), $true, 'the work folder with a trailing backslash'),
    @(($root + '\out\live-a'), $true, 'a folder under the work folder'),
    @('c:\REPO\_WORK\out', $true, 'case does not matter'),
    @('C:\home', $false, 'the user folder (the user''s windows)'),
    @('C:\', $false, 'a drive root'),
    @('C:\repo', $false, 'the parent of the work folder'),
    @('C:\repo\_work2', $false, 'a sibling whose name starts with the work folder name'),
    @(($root + '\..\_share'), $false, 'a .. that leaves the work folder'),
    @('\\server\share\_work', $false, 'a UNC path'),
    @('', $false, 'an empty root')
  )
  foreach ($c in $rootCases) {
    Check ((Test-RootAllowed -RootPath $c[0] -AllowedBase $root) -eq $c[1]) ('root guard: ' + $c[2] + ' -> ' + $c[1])
  }
  # 引数の守り (Resolve-Arguments。ウィンドウの一覧を読む前に断る)。
  $exp = $root + '\out\live-a\run-b\recordings'
  $bf = $root + '\out\live-a\explorer\a-before.json'
  $a = Resolve-Arguments -RootText '' -CloseSwitch $false -BeforeText '' -ExpectText '' -HwndText '' -AllowedBase $root
  Check ($null -eq $a.error -and $a.rootPath -eq $root -and $null -eq $a.expectPath) 'args: reading with the default root is allowed and closes nothing'
  $a = Resolve-Arguments -RootText 'C:\home' -CloseSwitch $false -BeforeText '' -ExpectText '' -HwndText '' -AllowedBase $root
  Check ($null -ne $a.error) 'args: reading with a root outside the work folder is refused'
  $a = Resolve-Arguments -RootText '' -CloseSwitch $true -BeforeText '' -ExpectText '' -HwndText '' -AllowedBase $root
  Check ($null -ne $a.error) 'args: -Close without -Before and -Expect is refused (never closes every window under the root)'
  $a = Resolve-Arguments -RootText '' -CloseSwitch $true -BeforeText $bf -ExpectText '' -HwndText '' -AllowedBase $root
  Check ($null -ne $a.error) 'args: -Close without -Expect is refused'
  $a = Resolve-Arguments -RootText '' -CloseSwitch $true -BeforeText '' -ExpectText $exp -HwndText '' -AllowedBase $root
  Check ($null -ne $a.error) 'args: -Close without -Before is refused'
  $a = Resolve-Arguments -RootText '' -CloseSwitch $true -BeforeText $bf -ExpectText 'C:\home\Documents' -HwndText '' -AllowedBase $root
  Check ($null -ne $a.error) 'args: -Expect outside the root is refused'
  $a = Resolve-Arguments -RootText 'C:\home' -CloseSwitch $true -BeforeText $bf -ExpectText $exp -HwndText '' -AllowedBase $root
  Check ($null -ne $a.error) 'args: -Close with a root outside the work folder is refused'
  $a = Resolve-Arguments -RootText '' -CloseSwitch $true -BeforeText $bf -ExpectText ($exp + '\') -HwndText '' -AllowedBase $root
  Check ($null -eq $a.error -and $a.expectPath -eq $exp -and $a.beforePath -eq $bf) 'args: -Close -Before -Expect is taken (the trailing backslash is dropped)'
  $a = Resolve-Arguments -RootText '' -CloseSwitch $true -BeforeText $bf -ExpectText $exp -HwndText '101' -AllowedBase $root
  Check ($null -ne $a.error -and $a.error -like '-Hwnd is no longer used*') 'args: -Hwnd is refused with the new form in the reason'
  $a = Resolve-Arguments -RootText '' -CloseSwitch $false -BeforeText $bf -ExpectText '' -HwndText '' -AllowedBase $root
  Check ($null -ne $a.error) 'args: -Before without -Close is refused (a read that names a before file is a mistake)'

  # (3) 偽のウィンドウの組: 下のウィンドウだけを読み、外のウィンドウの Document を読まない。読みは閉じない。
  # 偽のウィンドウが読まれた・閉じられた記録 (スクリプトの範囲に置く。ScriptProperty と ScriptMethod の中から見える)。記録の鍵は Id
  # (HWND を読めない偽のウィンドウでも記録できるように、HWND とは別に持つ)。
  $script:EwSelfTestLog = @{ quit = New-Object System.Collections.ArrayList; documentRead = New-Object System.Collections.ArrayList }
  $log = $script:EwSelfTestLog
  function New-FakeWindow {
    param([int64]$Hwnd, [string]$FullName, [string]$Url, [string]$Name, [string[]]$Selected, [switch]$Throws, [switch]$NoHwnd)
    $w = New-Object PSObject
    if ($Throws) {
      $w | Add-Member -MemberType ScriptProperty -Name FullName -Value { throw 'the window is closing' }
    }
    else {
      $w | Add-Member -MemberType NoteProperty -Name FullName -Value $FullName
    }
    $w | Add-Member -MemberType NoteProperty -Name LocationURL -Value $Url
    $w | Add-Member -MemberType NoteProperty -Name LocationName -Value $Name
    $w | Add-Member -MemberType NoteProperty -Name Id -Value $Hwnd
    if ($NoHwnd) {
      $w | Add-Member -MemberType ScriptProperty -Name HWND -Value { throw 'the window handle cannot be read' }
    }
    else {
      $w | Add-Member -MemberType NoteProperty -Name HWND -Value $Hwnd
    }
    $items = @($Selected | ForEach-Object { [PSCustomObject]@{ Path = $_ } })
    $folder = [PSCustomObject]@{ Self = [PSCustomObject]@{ Path = (ConvertFrom-LocationUrl -Url $Url) } }
    $doc = New-Object PSObject
    $doc | Add-Member -MemberType NoteProperty -Name Folder -Value $folder
    $doc | Add-Member -MemberType ScriptMethod -Name SelectedItems -Value { return $this.Items }
    $doc | Add-Member -MemberType NoteProperty -Name Items -Value $items
    $w | Add-Member -MemberType NoteProperty -Name Doc -Value $doc
    $w | Add-Member -MemberType ScriptProperty -Name Document -Value { [void]$script:EwSelfTestLog.documentRead.Add($this.Id); return $this.Doc }
    $w | Add-Member -MemberType ScriptMethod -Name Quit -Value { [void]$script:EwSelfTestLog.quit.Add($this.Id) }
    return $w
  }
  $explorer = 'C:\Windows\explorer.exe'
  $recUrl = $base + '/out/live-a/run-b/recordings'
  $w101 = New-FakeWindow -Hwnd 101 -FullName $explorer -Url $recUrl -Name 'recordings' -Selected @(($root + '\out\live-a\run-b\recordings\take 1'))
  $w102 = New-FakeWindow -Hwnd 102 -FullName $explorer -Url 'file:///C:/home/Documents' -Name 'Documents' -Selected @('C:\home\Documents\private.txt')
  $w103 = New-FakeWindow -Hwnd 103 -FullName $explorer -Url 'file:///C:/repo/_work2' -Name '_work2' -Selected @()
  $w104 = New-FakeWindow -Hwnd 104 -FullName 'C:\Program Files\Internet Explorer\iexplore.exe' -Url ($base + '/out') -Name 'out' -Selected @()
  $w105 = New-FakeWindow -Hwnd 105 -FullName $explorer -Url '' -Name 'PC' -Selected @()
  $w106 = New-FakeWindow -Hwnd 106 -FullName $explorer -Url $base -Name '_work' -Selected @()
  $w107 = New-FakeWindow -Hwnd 107 -FullName $explorer -Url ($base + '/x') -Name 'x' -Selected @() -Throws
  $w108 = New-FakeWindow -Hwnd 108 -FullName $explorer -Url ($base + '/y') -Name 'y' -Selected @() -NoHwnd
  $fakes = @($w101, $w102, $w103, $w104, $w105, $w106, $w107, $w108)
  $read = Read-ExplorerWindows -Windows $fakes -RootPath $root
  Check ((@($read.windows | ForEach-Object { $_.title }) -join ',') -eq 'recordings,_work,y') 'only the windows under the root are returned'
  Check ((@($read.windows | ForEach-Object { $_.hwnd }) -join ',') -eq '101,106,') 'a window whose handle cannot be read has no hwnd'
  Check ($read.outsideCount -eq 4 -and $read.unreadableCount -eq 1) ('outside windows are counted only (' + $read.outsideCount + ' outside, ' + $read.unreadableCount + ' unreadable)')
  Check (($read.windows[0].selected -join '|') -eq ($root + '\out\live-a\run-b\recordings\take 1')) 'the selected item of a window under the root is read'
  Check ($read.windows[0].title -eq 'recordings' -and $read.windows[0].folderPath -eq ($root + '\out\live-a\run-b\recordings')) 'the title and the folder of a window under the root are read'
  Check ((@($log.documentRead) -join ',') -eq '101,106,108') ('the Document of outside windows is never read (' + (@($log.documentRead) -join ',') + ')')
  Check ($log.quit.Count -eq 0) 'reading closes nothing'

  # (4) -Before と -Expect の閉じの決め。-Before はこのツールの読みの JSON を往復させた物 (main が -Out に書く形と同じ)。
  $toBefore = { param($Wins) return ((New-ReadResult -RootPath $root -Read (Read-ExplorerWindows -Windows $Wins -RootPath $root)) | ConvertTo-Json -Depth 6 -Compress) }
  $closeWith = {
    param($BeforeJson, $NowWins, [string]$ExpectPath)
    # 場面ごとに閉じた記録を空にする (前の場面の失敗が次の場面へ移らないように)。
    $script:EwSelfTestLog.quit.Clear()
    $info = Read-BeforeSnapshot -JsonText $BeforeJson -RootPath $root
    if ($info.error) { return [PSCustomObject]@{ refused = $true; error = $info.error; matched = @(); target = -1; closedHwnd = $null } }
    $now = Read-ExplorerWindows -Windows $NowWins -RootPath $root
    $d = Select-CloseTarget -Read $now -BeforeInfo $info -ExpectPath $ExpectPath
    $t = Invoke-CloseTarget -Read $now -Decision $d
    return [PSCustomObject]@{ refused = $d.refused; error = $d.error; note = $d.note; matched = $d.matched; target = $d.target; closedHwnd = $(if ($t) { $t.hwnd } else { $null }); windows = $now.windows }
  }
  $recPath = $root + '\out\live-a\run-b\recordings'
  $wNew = New-FakeWindow -Hwnd 109 -FullName $explorer -Url $recUrl -Name 'recordings' -Selected @()
  $wNew2 = New-FakeWindow -Hwnd 110 -FullName $explorer -Url ($recUrl + '/') -Name 'recordings' -Selected @()
  $wOther = New-FakeWindow -Hwnd 111 -FullName $explorer -Url ($base + '/out/live-a/run-b') -Name 'run-b' -Selected @()
  $wNoHwndAtExp = New-FakeWindow -Hwnd 112 -FullName $explorer -Url $recUrl -Name 'recordings' -Selected @() -NoHwnd
  $readable = @($w101, $w102, $w103, $w104, $w105, $w106)
  $before = & $toBefore $readable
  $log.quit.Clear()
  # 前からある 101 (利用者が開いていた形) と同じ場所に新しい 109 が開いた: 109 だけを閉じる。
  $r = & $closeWith $before @($w101, $w102, $w103, $w104, $w105, $w106, $wNew) $recPath
  Check (-not $r.refused -and (@($log.quit) -join ',') -eq '109' -and $r.closedHwnd -eq 109) ('close: only the new window at the expected folder is closed, not the window that was there before (' + (@($log.quit) -join ',') + ')')
  Check (((@($r.windows | Where-Object { $_.quitCalled }) | ForEach-Object { $_.hwnd }) -join ',') -eq '109') 'close: quitCalled is set only on the closed window'
  $log.quit.Clear()
  # 新しいウィンドウがない (エクスプローラーが前のウィンドウを前に出しただけ): 何も閉じず断りでもない。
  $r = & $closeWith $before $readable $recPath
  Check (-not $r.refused -and $log.quit.Count -eq 0 -and $r.note) 'close: no new window at the expected folder closes nothing and is not a refusal'
  # 新しいウィンドウが -Expect と違う場所: 閉じない。
  $r = & $closeWith $before @($w101, $w102, $w103, $w104, $w105, $w106, $wOther) $recPath
  Check (-not $r.refused -and $log.quit.Count -eq 0 -and @($r.matched).Count -eq 0) 'close: a new window at another folder is not closed'
  # 新しいウィンドウが 2 つ -Expect にある: 断って何も閉じない。
  $r = & $closeWith $before @($w101, $w102, $w103, $w104, $w105, $w106, $wNew, $wNew2) $recPath
  Check ($r.refused -and $log.quit.Count -eq 0 -and (@($r.matched) -join ',') -eq '109,110') ('close: two new windows at the expected folder are refused, nothing is closed (' + (@($r.matched) -join ',') + ')')
  # 外を見ていた 102 が -Expect へ移ってきた (総数は増えていない) 形: 断る。
  $w102moved = New-FakeWindow -Hwnd 102 -FullName $explorer -Url $recUrl -Name 'recordings' -Selected @()
  $r = & $closeWith $before @($w101, $w102moved, $w103, $w104, $w105, $w106) $recPath
  Check ($r.refused -and $log.quit.Count -eq 0 -and $r.error -like 'no window was added*') ('close: a window that moved in from outside (no window was added) is refused (' + $r.error + ')')
  # -Expect の場所に HWND を読めないウィンドウが今ある: 断る。
  $r = & $closeWith $before @($w101, $w102, $w103, $w104, $w105, $w106, $wNew, $wNoHwndAtExp) $recPath
  Check ($r.refused -and $log.quit.Count -eq 0) 'close: a window at the expected folder without a handle is refused, nothing is closed'
  # -Before に読めないウィンドウ (107) があった: そのウィンドウが押した後に読めて -Expect にあると新しいウィンドウに見える (ほかにウィンドウが 1 つ増えて総数も増える)。断る。
  $w107now = New-FakeWindow -Hwnd 107 -FullName $explorer -Url $recUrl -Name 'recordings' -Selected @()
  $r = & $closeWith (& $toBefore @($w101, $w106, $w107)) @($w101, $w106, $w107now, $w102) $recPath
  Check ($r.refused -and $log.quit.Count -eq 0 -and $r.error -like '*unreadable*') ('close: a -Before read with an unreadable window is refused, the window is not closed (' + $r.error + ')')
  # -Before に HWND のないウィンドウ (108) があった: 同じく、後で HWND が読めて -Expect にあると新しいウィンドウに見える。断る。
  $w108now = New-FakeWindow -Hwnd 108 -FullName $explorer -Url $recUrl -Name 'recordings' -Selected @()
  $r = & $closeWith (& $toBefore @($w101, $w106, $w108)) @($w101, $w106, $w108now, $w102) $recPath
  Check ($r.refused -and $log.quit.Count -eq 0 -and $r.error -like '*without a handle*') ('close: a -Before read with a window without a handle is refused, the window is not closed (' + $r.error + ')')
  # -Before が閉じの結果・失敗の読み・別の -Root・断り・JSON でない: 断る。現在のウィンドウは新しいウィンドウ 109 を含む (守りが無ければ閉じる形)。
  $nowWithNew = @($w101, $w102, $w103, $w104, $w105, $w106, $wNew)
  $bObj = $before | ConvertFrom-Json
  $bObj.close = $true
  $r = & $closeWith ($bObj | ConvertTo-Json -Depth 6 -Compress) $nowWithNew $recPath
  Check ($r.refused -and $log.quit.Count -eq 0) 'close: a close result as -Before is refused'
  $bObj = $before | ConvertFrom-Json
  $bObj.ok = $false
  $r = & $closeWith ($bObj | ConvertTo-Json -Depth 6 -Compress) $nowWithNew $recPath
  Check ($r.refused -and $log.quit.Count -eq 0) 'close: a failed read as -Before is refused'
  $bObj = $before | ConvertFrom-Json
  $bObj.root = $root + '\out'
  $r = & $closeWith ($bObj | ConvertTo-Json -Depth 6 -Compress) $nowWithNew $recPath
  Check ($r.refused -and $log.quit.Count -eq 0 -and $r.error -like '*another root*') 'close: a -Before read with another root is refused'
  $r = & $closeWith '{"ok":false,"refused":true,"error":"x"}' $nowWithNew $recPath
  Check ($r.refused -and $log.quit.Count -eq 0) 'close: a refusal as -Before is refused'
  $r = & $closeWith 'not json' $nowWithNew $recPath
  Check ($r.refused -and $log.quit.Count -eq 0) 'close: a -Before that is not JSON is refused'
  # ウィンドウが 1 つの -Before (JSON で windows が 1 つ) と 0 の -Before も読める。
  $one = & $toBefore @($w101)
  $info = Read-BeforeSnapshot -JsonText $one -RootPath $root
  Check ($null -eq $info.error -and $info.hwnds.ContainsKey([int64]101) -and $info.total -eq 1) ('before: a read with one window is taken (' + $info.error + ')')
  $none = & $toBefore @($w102)
  $info = Read-BeforeSnapshot -JsonText $none -RootPath $root
  Check ($null -eq $info.error -and $info.hwnds.Count -eq 0 -and $info.total -eq 1) ('before: a read with no window under the root is taken and counts the outside window (' + $info.error + ')')
  $r = & $closeWith $none @($w102, $wNew) $recPath
  Check (-not $r.refused -and (@($log.quit) -join ',') -eq '109') 'close: with no window under the root before, the new window is closed'
  return [PSCustomObject]@{ ok = ($failures.Count -eq 0); selftest = $true; checks = $checks.ToArray(); failures = $failures.ToArray() }
}

try {
  if ($SelfTest) {
    $r = Invoke-SelfTest
    Write-JsonResultAndExit -Result $r -ExitCode ($(if ($r.ok) { 0 } else { 1 }))
  }
  # 守り: ウィンドウの一覧を読む前に引数と -Before を見る。断るときは Shell.Application も作らない。
  $parsed = Resolve-Arguments -RootText $Root -CloseSwitch ([bool]$Close) -BeforeText $Before -ExpectText $Expect -HwndText $Hwnd -AllowedBase (Get-RepoWorkDir)
  if ($parsed.error) {
    Write-JsonResultAndExit -Result ([PSCustomObject]@{ ok = $false; root = $parsed.rootPath; close = [bool]$Close; before = $Before; expect = $Expect; refused = $true; error = $parsed.error }) -ExitCode 2
  }
  $rootPath = $parsed.rootPath
  if (-not $Close) {
    $read = Read-ExplorerWindows -Windows (Get-ShellWindows) -RootPath $rootPath
    Write-JsonResultAndExit -Result (New-ReadResult -RootPath $rootPath -Read $read) -ExitCode 0
  }
  $beforeText = $null
  try { $beforeText = [System.IO.File]::ReadAllText($parsed.beforePath, [System.Text.Encoding]::UTF8) } catch { $beforeText = $null }
  $info = if ($null -eq $beforeText) { [PSCustomObject]@{ error = ('the -Before file cannot be read: ' + $parsed.beforePath) } } else { Read-BeforeSnapshot -JsonText $beforeText -RootPath $rootPath }
  if ($info.error) {
    Write-JsonResultAndExit -Result ([PSCustomObject]@{ ok = $false; root = $rootPath; close = $true; before = $parsed.beforePath; expect = $parsed.expectPath; refused = $true; error = $info.error }) -ExitCode 2
  }
  $read = Read-ExplorerWindows -Windows (Get-ShellWindows) -RootPath $rootPath
  $decision = Select-CloseTarget -Read $read -BeforeInfo $info -ExpectPath $parsed.expectPath
  $target = Invoke-CloseTarget -Read $read -Decision $decision
  $ok = -not $decision.refused
  $err = $decision.error
  $closedFlag = $null
  if ($null -ne $target) {
    if ($target.quitCalled) {
      # 閉じたかはウィンドウの一覧から HWND が消えたかで見る (Quit() は待たずに戻る)。
      $deadline = [DateTime]::UtcNow.AddMilliseconds($CloseWaitMs)
      do {
        $open = Get-OpenHwnds
        if (-not $open.ContainsKey([int64]$target.hwnd)) { break }
        Start-Sleep -Milliseconds 200
      } while ([DateTime]::UtcNow -lt $deadline)
      $target.closed = -not (Get-OpenHwnds).ContainsKey([int64]$target.hwnd)
    }
    else {
      $target.closed = $false
    }
    $closedFlag = $target.closed
    if (-not $target.closed) {
      $ok = $false
      $err = 'the window ' + $target.hwnd + ' was not closed within ' + $CloseWaitMs + ' ms'
    }
  }
  $code = if ($decision.refused) { 2 } elseif ($ok) { 0 } else { 1 }
  Write-JsonResultAndExit -Result ([PSCustomObject][ordered]@{
      ok              = $ok
      root            = $rootPath
      close           = $true
      before          = $parsed.beforePath
      expect          = $parsed.expectPath
      windows         = @($read.windows)
      matched         = @($decision.matched)
      target          = $(if ($null -ne $target) { $target.hwnd } else { $null })
      closed          = $closedFlag
      beforeTotal     = $info.total
      total           = $decision.total
      outsideCount    = $read.outsideCount
      unreadableCount = $read.unreadableCount
      refused         = [bool]$decision.refused
      error           = $err
      note            = $decision.note
    }) -ExitCode $code
}
catch {
  Write-JsonResultAndExit -Result ([PSCustomObject]@{ ok = $false; root = $Root; close = [bool]$Close; error = $_.Exception.Message }) -ExitCode 1
}
