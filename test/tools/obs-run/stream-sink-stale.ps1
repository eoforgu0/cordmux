#requires -Version 5.1
<#
  stream-sink-stale.ps1 - 親のいない配信の受け口 (同梱の ffmpeg -listen) を見つけるツール。-Stop は手で実行するときだけ止める。

  実行スクリプト (node) の受け口は、実行スクリプトの spawn (stream-sink.mjs。detached なし) で起動し、libuv が
  「閉じたら中を全部終わらせる」ジョブに入れるので、実行スクリプトを外から打ち切っても (TerminateProcess・taskkill /F) 受け口は 5-17 ms で
  一緒に終わる (5 回測定した)。PC の異常終了は再起動で何も残らない。だから実行スクリプトの受け口は残らない。親のいない受け口が
  ありうるのは、閉じたシェル (Start-Process をして終わった powershell など) から同梱の ffmpeg を手で起動したときだけである。
  そのため、実行スクリプト (stream-sink-start) と cleanup.ps1 は見つけて伝えるだけで止めない。止めるのは、メッセージを
  読んだ人が -Stop を手で実行するときだけである。
  実行スクリプトと cleanup.ps1 が -Stop で自動で止める方法は採らない (止める相手がなく、動いている実行の受け口や使い回された pid を
  誤って止めるおそれだけが残る)。ツールごと消す方法も採らない (実行の最中に手で起動した受け口が 1935 番を塞いだとき、同じ守りで見つけて止める物がなくなる)。

  親のいない受け口の判定 (全部を満たす物だけ。ほかの ffmpeg には触らない):
    (1) 実行ファイル (Win32_Process の ExecutablePath) が同梱の ffmpeg と同じパス (GetFullPath で正規化、大文字小文字を区別しない)。
        場所の読めないプロセスは一致しない。
    (2) コマンド行 (CommandLine) に -listen と受け口の URL が語として入っている (URL の後ろに文字が続く別の URL は一致しない)。
        コマンド行の読めないプロセスは一致しない。
    (3) 親の pid (ParentProcessId) を現在どのプロセスも持っていないことを、確実に読めた (Win32_Process の問い合わせがエラーなしに 0 件で、
        Get-Process も「その pid のプロセスはない」のエラーを返した)。親の pid を誰かが持っていれば owned (触らない。実行スクリプトか、
        pid を使い回した別のプロセスかは問わない)。どちらかの読みが失敗したら undecided (決められない。止めない)。
        pid が 0 以下の親は存在するものとして扱う (System Idle Process。親がいないとは言えない)。
  次の方法は採らない。親の読みの失敗を「親がいない」と同じに扱う (WMI の一時のエラーで、動いている実行の受け口を止めうる)。
  親の pid を現在使っているプロセスが子より後に起動したら pid の使い回しとして止める (システム時刻で比べるので、時計が戻ると動いている実行スクリプトの受け口を
  止めうる。使い回しは元の親が終了した後にしか起きず、実行スクリプトの受け口は親と一緒に終わるので、この分岐で拾える物はない)。
  Win32_Process だけで「いない」と決める (2 つの別の読みが同じ答えのときだけ確実とする)。
  実行ファイルだけで決める (同じ ffmpeg を使う判定ツールにも一致する)、コマンド行だけで決める (利用者が WinGet で入れた ffmpeg にも一致する)、
  親を見ない、1935 番の待ち受けているプロセスで決める、実行スクリプトが pid を保存する、という方法も採らない。

  -Stop (手で実行するときだけ): 止める直前に、その pid の Process を開いてハンドル (Handle) を持ち (持っている間はその pid は使い回されない)、
  ハンドルで読んだ起動時刻が見つけたときの時刻とマイクロ秒まで同じこと、同じ pid の Win32_Process (ハンドルを持っているので同じプロセス) の場所・
  コマンド行・起動時刻、親の pid に誰もいないことをもう 1 度確認してから、同じハンドルで止める (Process.Kill は開いたハンドルを使い、もう終わって
  いれば例外になって別のプロセスを止めない)。読み直してから Stop-Process -Id で止める方法は採らない (読み直しと止めの間に
  pid が使い回されると別のプロセスを止める)。時刻はマイクロ秒で比べる (Win32_Process の CreationDate はマイクロ秒で切られ、
  Process.StartTime は 100 ns。そのままでは同じプロセスでも合わない)。
  1 つでも undecided があれば、-Stop でも何も止めない (読みが揺れている間は止めない側に倒す)。

  同梱の ffmpeg の場所: 既定は src-obs/buildspec.json の obs-studio と prebuilt のバージョンから組み立てる (実行スクリプトの
  obsDepsBinRelFromBuildspec と同じ形。読めなければ同じ既定の場所)。-StreamSinkExe で渡すときも、<repo>\src-obs\.deps の下の
  ffmpeg.exe でなければ断る。-StreamSinkUrl は rtmp://127.0.0.1:<port>/<path> でなければ断る (既定は実行スクリプトの STREAM_SINK_URL と
  同じ文字列。vitest が突き合わせる)。

  結果は 1 行の JSON を標準出力に書く (UTF-8)。-Out を渡せば同じ JSON をそのファイルにも書く。
    { "ok", "stop", "exe", "url",
      "stale": [ { "pid", "parentPid", "createdAt", "outcome", "stopped", "error" } ],
      "owned": [ { "pid", "parentPid" } ], "undecided": [ { "pid", "parentPid", "error" } ], "remaining": [pid...], "refused", "error" }
  outcome: found (-Stop なしで見つけた)、stopped (このツールが止めて存在しなくなった)、already-gone (止める前に存在しなくなっていた。止めていない)、
  left-alone (見つけた後に変わっていた = 起動時刻が違う・親の pid に誰かが入った。止めていない)、failed (止めようとして止まらなかった)、
  not-stopped-undecided (-Stop だが決められない受け口があったので止めていない)。stopped は outcome が stopped のときだけ真。
  remaining は stopped と already-gone でない物。
  終了コード: 0 = 親のいない受け口がない (-Stop なら全部止めたか存在しなくなっていた)、1 = 親のいない受け口が残っている、
  2 = 断った (引数)、3 = 決められなかった (プロセスの一覧か親の読みが失敗した。何も止めていない)。

  -SelfTest: 偽のプロセスの組で判定 (1)-(3)・親の読みの 3 つの答え・止め方 (開いたハンドルで確認して同じハンドルで止める) を確認する。同梱の
  ffmpeg がある PC では、テスト専用の URL (rtmp://127.0.0.1:19351/ と 19352/、1935 番は使わない) で実際の ffmpeg -listen を 2 つ起動し
  (1 つは親を先に終了させた形、1 つはこのツールの子 = 動いている親)、親のいない形だけを止め、子は止めないことを確認する。起動した 2 つは
  起動した直後にハンドルを開き、finally でそのハンドルで止める (pid では止めない)。出力はどちらも -f null (ファイルを書かない)。ウィンドウは出さない。

  使い方:
    powershell -NoProfile -ExecutionPolicy Bypass -File test/tools/obs-run/stream-sink-stale.ps1            # 見つけるだけ (実行スクリプトと cleanup.ps1 もこの形)
    powershell -NoProfile -ExecutionPolicy Bypass -File test/tools/obs-run/stream-sink-stale.ps1 -Stop      # 手で実行する: 見つけて止める
    powershell -NoProfile -ExecutionPolicy Bypass -File test/tools/obs-run/stream-sink-stale.ps1 -SelfTest
#>

[CmdletBinding()]
param(
  [switch]$Stop,
  [string]$StreamSinkExe = '',
  [string]$StreamSinkUrl = '',
  [ValidateRange(0, 60000)]
  [int]$StopWaitMs = 3000,
  [string]$Out = '',
  [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'

# 実行スクリプトの STREAM_SINK_URL (run-window-plan.mjs) と同じ字 (stream-sink.test.ts が突き合わせる)。
$DefaultStreamSinkUrl = 'rtmp://127.0.0.1:1935/live/test'
# buildspec.json を読めないときの既定 (run-window.mjs の OBS_DEPS_BIN の既定と同じ。同じテストが突き合わせる)。
$FallbackDepsBinRel = 'src-obs\.deps\obs-studio-31.1.1\.deps\obs-deps-2025-07-11-x64\bin'

function Write-JsonResultAndExit {
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

function Get-RepoRoot {
  return [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
}

# 同梱の ffmpeg の場所 (buildspec.json から。読めなければ既定)。
function Get-BundledFfmpegPath {
  param([string]$RepoRoot)
  $rel = $FallbackDepsBinRel
  try {
    $spec = Get-Content -LiteralPath (Join-Path $RepoRoot 'src-obs\buildspec.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $obsVersion = [string]$spec.dependencies.'obs-studio'.version
    $prebuilt = [string]$spec.dependencies.prebuilt.version
    if ($obsVersion -and $prebuilt) {
      $rel = 'src-obs\.deps\obs-studio-' + $obsVersion + '\.deps\obs-deps-' + $prebuilt + '-x64\bin'
    }
  }
  catch { }
  return [System.IO.Path]::GetFullPath((Join-Path (Join-Path $RepoRoot $rel) 'ffmpeg.exe'))
}

function Test-SamePath {
  param([string]$A, [string]$B)
  if (-not $A -or -not $B) { return $false }
  try {
    return [System.IO.Path]::GetFullPath($A).Equals([System.IO.Path]::GetFullPath($B), [System.StringComparison]::OrdinalIgnoreCase)
  }
  catch {
    return $false
  }
}

# 時刻をマイクロ秒に切った tick (UTC)。Win32_Process の CreationDate (マイクロ秒) と Process.StartTime (100 ns) を同じ物として比べる。
# 整数のまま切る (double の割り算は 17 桁の tick の下の桁を落とす)。読めなければ $null。
function Get-MicrosecondTicks {
  param($Time)
  if ($null -eq $Time) { return $null }
  try {
    $t = ([datetime]$Time).ToUniversalTime().Ticks
    return [int64]($t - ($t % 10))
  }
  catch {
    return $null
  }
}

# 引数の守り。exe は <repo>\src-obs\.deps の下の ffmpeg.exe だけ、URL は rtmp://127.0.0.1:<port>/ で始まる物だけ。
function Resolve-StaleArguments {
  param([string]$ExeText, [string]$UrlText, [string]$RepoRoot)
  $res = [ordered]@{ exe = $null; url = $null; error = $null }
  $exe = if ($ExeText) { $ExeText } else { Get-BundledFfmpegPath -RepoRoot $RepoRoot }
  try { $exe = [System.IO.Path]::GetFullPath($exe) } catch { $res.error = 'the ffmpeg path is not valid: ' + $ExeText; return [PSCustomObject]$res }
  $depsRoot = [System.IO.Path]::GetFullPath((Join-Path $RepoRoot 'src-obs\.deps')).TrimEnd('\') + '\'
  if (-not $exe.StartsWith($depsRoot, [System.StringComparison]::OrdinalIgnoreCase) -or -not ([System.IO.Path]::GetFileName($exe) -ieq 'ffmpeg.exe')) {
    $res.error = 'only the bundled ffmpeg.exe under ' + $depsRoot + ' is looked at: ' + $exe
    return [PSCustomObject]$res
  }
  $url = if ($UrlText) { $UrlText } else { $DefaultStreamSinkUrl }
  if ($url -notmatch '^rtmp://127\.0\.0\.1:[0-9]{1,5}/[A-Za-z0-9/_.-]+$') {
    $res.error = 'the stream receiver URL must be rtmp://127.0.0.1:<port>/<path>: ' + $url
    return [PSCustomObject]$res
  }
  $res.exe = $exe
  $res.url = $url
  return [PSCustomObject]$res
}

# コマンド行に -listen と URL が語として入っているか (前後は行の端・空白・引用符)。
function Test-ReceiverCommandLine {
  param([string]$CommandLine, [string]$Url)
  if (-not $CommandLine -or -not $Url) { return $false }
  $edgeL = '(?:^|[\s"])'
  $edgeR = '(?:$|[\s"])'
  if ($CommandLine -notmatch ($edgeL + '-listen' + $edgeR)) { return $false }
  return ($CommandLine -match ($edgeL + [regex]::Escape($Url) + $edgeR))
}

# 1 つのプロセスを分ける: $null = 受け口ではない、'stale' = 親のいない受け口、'owned' = 親の pid を誰かが持っている受け口、
# 'undecided' = 親の読みが失敗した受け口 (止めない)。ParentState は Get-ParentState の state (absent・present・unknown)。
function Get-ReceiverClass {
  param($Proc, [string]$ParentState, [string]$Exe, [string]$Url)
  if ($null -eq $Proc) { return $null }
  if (-not (Test-SamePath -A ([string]$Proc.ExecutablePath) -B $Exe)) { return $null }
  if (-not (Test-ReceiverCommandLine -CommandLine ([string]$Proc.CommandLine) -Url $Url)) { return $null }
  if ($ParentState -eq 'absent') { return 'stale' }
  if ($ParentState -eq 'present') { return 'owned' }
  return 'undecided'
}

# 親の pid を今持っているプロセスがあるか: absent (確かに居ない) / present (居る) / unknown (読めなかった)。
# CimQuery: pid -> Win32_Process の行の並び (失敗は例外)。ProcessProbe: pid -> 'present' か 'absent' (Get-Process の「ない」の誤りだけを
# absent にし、ほかの誤りは例外)。2 つの読みが「居ない」で揃ったときだけ absent。セルフテストは偽の読みを渡す。
function Get-ParentState {
  param([int64]$ParentPid, [scriptblock]$CimQuery, [scriptblock]$ProcessProbe)
  if ($ParentPid -le 0) { return [PSCustomObject]@{ state = 'present'; error = $null } }
  try {
    $rows = @(& $CimQuery $ParentPid)
  }
  catch {
    return [PSCustomObject]@{ state = 'unknown'; error = ('Win32_Process could not be read for the parent ' + $ParentPid + ': ' + $_.Exception.Message) }
  }
  if ($rows.Count -gt 0) { return [PSCustomObject]@{ state = 'present'; error = $null } }
  try {
    $probe = [string](& $ProcessProbe $ParentPid)
  }
  catch {
    return [PSCustomObject]@{ state = 'unknown'; error = ('Get-Process could not be read for the parent ' + $ParentPid + ': ' + $_.Exception.Message) }
  }
  if ($probe -eq 'absent') { return [PSCustomObject]@{ state = 'absent'; error = $null } }
  if ($probe -eq 'present') { return [PSCustomObject]@{ state = 'present'; error = $null } }
  return [PSCustomObject]@{ state = 'unknown'; error = ('unexpected answer for the parent ' + $ParentPid + ': ' + $probe) }
}

# 実際の読み (セルフテストの実際の部と main が使う)。
$LiveCimQuery = { param($Id) return @(Get-CimInstance -ClassName Win32_Process -Filter ('ProcessId=' + [int64]$Id) -ErrorAction Stop) }
$LiveProcessProbe = {
  param($Id)
  try {
    [void](Get-Process -Id ([int]$Id) -ErrorAction Stop)
    return 'present'
  }
  catch {
    if ([string]$_.FullyQualifiedErrorId -like 'NoProcessFoundForGivenId*') { return 'absent' }
    throw
  }
}
$LiveParentStateOf = { param($ParentPid) return (Get-ParentState -ParentPid $ParentPid -CimQuery $LiveCimQuery -ProcessProbe $LiveProcessProbe) }
# 止める直前の 1 回の読み直し (pid -> Win32_Process の行か $null。失敗は例外)。
$LiveReadProc = { param($Id) $r = @(& $LiveCimQuery $Id); if ($r.Count -eq 0) { return $null }; return $r[0] }
# 止める相手を開く: Process を取り、手を開く (以後その pid は使い回されない)。居なければ $null。
$LiveOpenProc = {
  param($Id)
  try {
    $p = Get-Process -Id ([int]$Id) -ErrorAction Stop
  }
  catch {
    if ([string]$_.FullyQualifiedErrorId -like 'NoProcessFoundForGivenId*') { return $null }
    throw
  }
  [void]$p.Handle
  return $p
}

function Get-FfmpegProcesses {
  return @(Get-CimInstance -ClassName Win32_Process -Filter "Name='ffmpeg.exe'" -ErrorAction Stop)
}

# 受け口を探して分ける。Procs と ParentStateOf (親の pid -> Get-ParentState の答え) を渡せば偽の組で回せる (セルフテスト)。
function Find-Receivers {
  param($Procs, [scriptblock]$ParentStateOf, [string]$Exe, [string]$Url)
  $stale = New-Object System.Collections.ArrayList
  $owned = New-Object System.Collections.ArrayList
  $undecided = New-Object System.Collections.ArrayList
  foreach ($p in @($Procs)) {
    if ($null -eq $p) { continue }
    # 先に (1) と (2) を見て、当たる物だけ親を読む (ほかの ffmpeg の親は読まない)。
    if (-not (Test-SamePath -A ([string]$p.ExecutablePath) -B $Exe)) { continue }
    if (-not (Test-ReceiverCommandLine -CommandLine ([string]$p.CommandLine) -Url $Url)) { continue }
    $ps = $null
    try { $ps = & $ParentStateOf ([int64]$p.ParentProcessId) } catch { $ps = [PSCustomObject]@{ state = 'unknown'; error = $_.Exception.Message } }
    if ($null -eq $ps) { $ps = [PSCustomObject]@{ state = 'unknown'; error = 'no answer for the parent' } }
    $class = Get-ReceiverClass -Proc $p -ParentState ([string]$ps.state) -Exe $Exe -Url $Url
    $createdUs = Get-MicrosecondTicks $p.CreationDate
    # 起動時刻を読めない受け口は、止める直前に同じプロセスかを確認できないので決めない。
    if ($class -eq 'stale' -and $null -eq $createdUs) { $class = 'undecided'; $ps = [PSCustomObject]@{ state = 'unknown'; error = 'the start time of the receiver cannot be read' } }
    if ($class -eq 'stale') {
      [void]$stale.Add([PSCustomObject][ordered]@{
          pid       = [int64]$p.ProcessId
          parentPid = [int64]$p.ParentProcessId
          createdAt = ([datetime]::new($createdUs, [System.DateTimeKind]::Utc)).ToString('o')
          createdUs = $createdUs
          outcome   = 'found'
          stopped   = $false
          error     = $null
        })
    }
    elseif ($class -eq 'owned') { [void]$owned.Add([PSCustomObject]@{ pid = [int64]$p.ProcessId; parentPid = [int64]$p.ParentProcessId }) }
    elseif ($class -eq 'undecided') { [void]$undecided.Add([PSCustomObject]@{ pid = [int64]$p.ProcessId; parentPid = [int64]$p.ParentProcessId; error = [string]$ps.error }) }
  }
  return [PSCustomObject]@{ stale = $stale.ToArray(); owned = $owned.ToArray(); undecided = $undecided.ToArray() }
}

# 親のいない受け口を止める (-Stop のときだけ呼ぶ)。1 つずつ: 開いて手を持つ -> 手で読んだ起動時刻が見つけたときと同じ -> 同じ pid の
# Win32_Process (ハンドルを持っているので同じプロセス) の起動時刻・場所・コマンド行・親のいないことをもう 1 度 -> 同じハンドルで Kill。
# OpenProc・ReadProc・ParentStateOf は差し替えられる (セルフテストは偽の物で「同じ手で止める」を当てる)。
function Stop-StaleReceivers {
  param($Stale, [string]$Exe, [string]$Url, [int]$WaitMs, [scriptblock]$OpenProc, [scriptblock]$ReadProc, [scriptblock]$ParentStateOf)
  $killed = New-Object System.Collections.ArrayList
  foreach ($s in @($Stale)) {
    $h = $null
    try {
      $h = & $OpenProc $s.pid
      if ($null -eq $h -or $h.HasExited) { $s.outcome = 'already-gone'; continue }
      if ((Get-MicrosecondTicks $h.StartTime) -ne [int64]$s.createdUs) {
        $s.outcome = 'left-alone'; $s.error = 'the process at this pid started at another time (the pid was reused)'; continue
      }
      $now = & $ReadProc $s.pid
      if ($null -eq $now -or (Get-MicrosecondTicks $now.CreationDate) -ne [int64]$s.createdUs) {
        $s.outcome = 'left-alone'; $s.error = 'the process changed before stopping'; continue
      }
      $ps = & $ParentStateOf ([int64]$now.ParentProcessId)
      $again = Get-ReceiverClass -Proc $now -ParentState ([string]$ps.state) -Exe $Exe -Url $Url
      if ($again -ne 'stale') {
        $s.outcome = 'left-alone'; $s.error = 'it is no longer a receiver without a parent (now ' + $(if ($null -eq $again) { 'not a receiver' } else { $again }) + ')'; continue
      }
      $h.Kill()
      [void]$killed.Add([PSCustomObject]@{ rec = $s; proc = $h })
    }
    catch {
      if ($null -ne $h -and $h.HasExited -and $s.outcome -eq 'found') { $s.outcome = 'already-gone' }
      else { $s.outcome = 'failed'; $s.error = $_.Exception.Message }
    }
  }
  $deadline = [DateTime]::UtcNow.AddMilliseconds($WaitMs)
  foreach ($k in $killed) {
    $left = [int][Math]::Max(0, ($deadline - [DateTime]::UtcNow).TotalMilliseconds)
    $gone = $false
    try { $gone = [bool]$k.proc.WaitForExit($left) } catch { $gone = $false }
    if ($gone) { $k.rec.outcome = 'stopped'; $k.rec.stopped = $true }
    else { $k.rec.outcome = 'failed'; $k.rec.error = 'still running ' + $WaitMs + ' ms after the stop' }
  }
}

# 外へ出す形 (createdUs は内側だけ)。
function ConvertTo-StaleOutput {
  param($Stale)
  return @(@($Stale) | Where-Object { $null -ne $_ } | ForEach-Object { [PSCustomObject][ordered]@{ pid = $_.pid; parentPid = $_.parentPid; createdAt = $_.createdAt; outcome = $_.outcome; stopped = $_.stopped; error = $_.error } })
}

# 1 回の確認 (main とセルフテストが使う)。返り値: 結果の形と終了コード。
function Invoke-StaleCheck {
  param($Procs, [string]$Exe, [string]$Url, [bool]$DoStop, [int]$WaitMs, [scriptblock]$ParentStateOf, [scriptblock]$OpenProc, [scriptblock]$ReadProc)
  $found = Find-Receivers -Procs $Procs -ParentStateOf $ParentStateOf -Exe $Exe -Url $Url
  $err = $null
  if ($DoStop) {
    if (@($found.undecided).Count -gt 0) {
      foreach ($s in @($found.stale)) { $s.outcome = 'not-stopped-undecided' }
      $err = 'some receivers could not be decided; nothing was stopped'
    }
    else {
      Stop-StaleReceivers -Stale $found.stale -Exe $Exe -Url $Url -WaitMs $WaitMs -OpenProc $OpenProc -ReadProc $ReadProc -ParentStateOf $ParentStateOf
    }
  }
  $remaining = @(@($found.stale) | Where-Object { $_.outcome -ne 'stopped' -and $_.outcome -ne 'already-gone' } | ForEach-Object { $_.pid })
  $code = 0
  if (@($found.undecided).Count -gt 0) {
    $code = 3
    if (-not $err) { $err = 'some receivers could not be decided (a read of the parent failed)' }
  }
  elseif ($remaining.Count -gt 0) { $code = 1 }
  $result = [PSCustomObject][ordered]@{
    ok        = ($code -eq 0)
    stop      = $DoStop
    exe       = $Exe
    url       = $Url
    stale     = @(ConvertTo-StaleOutput $found.stale)
    owned     = @($found.owned)
    undecided = @($found.undecided)
    remaining = $remaining
    refused   = $false
    error     = $err
  }
  return [PSCustomObject]@{ result = $result; exitCode = $code; stale = $found.stale }
}

# ---- セルフテスト ------------------------------------------------------------------------------------
function Invoke-SelfTest {
  $failures = New-Object System.Collections.ArrayList
  $checks = New-Object System.Collections.ArrayList
  function Check {
    param([bool]$Cond, [string]$Label)
    [void]$checks.Add([PSCustomObject]@{ ok = $Cond; label = $Label })
    if (-not $Cond) { [void]$failures.Add($Label) }
  }
  $repo = Get-RepoRoot
  $bundled = Get-BundledFfmpegPath -RepoRoot $repo
  $url = $DefaultStreamSinkUrl

  # (1) 引数の守り。
  $a = Resolve-StaleArguments -ExeText '' -UrlText '' -RepoRoot $repo
  Check ($null -eq $a.error -and $a.exe -eq $bundled -and $a.url -eq $url) 'args: the defaults are the bundled ffmpeg and the driver URL'
  Check ($bundled.EndsWith('\bin\ffmpeg.exe', [System.StringComparison]::OrdinalIgnoreCase) -and $bundled.StartsWith((Join-Path $repo 'src-obs\.deps\obs-studio-'), [System.StringComparison]::OrdinalIgnoreCase)) ('args: the bundled path is built from buildspec.json (' + $bundled + ')')
  foreach ($bad in @('C:\Tools\WinGet\Links\ffmpeg.exe', 'C:\ffmpeg\bin\ffmpeg.exe', (Join-Path $repo 'src-obs\.deps\x\ffprobe.exe'), (Join-Path $repo 'src-obs\.deps2\ffmpeg.exe'), (Join-Path $repo 'src-obs\.deps\..\ffmpeg.exe'))) {
    $a = Resolve-StaleArguments -ExeText $bad -UrlText '' -RepoRoot $repo
    Check ($null -ne $a.error) ('args: another ffmpeg is refused (' + $bad + ')')
  }
  foreach ($bad in @('rtmp://0.0.0.0:1935/live/test', 'rtmp://example.com:1935/live/test', 'rtmp://127.0.0.1/live/test', 'rtmp://127.0.0.1:1935/live/test x', 'rtmp://127.0.0.1:1935/')) {
    $a = Resolve-StaleArguments -ExeText '' -UrlText $bad -RepoRoot $repo
    Check ($null -ne $a.error) ('args: the URL "' + $bad + '" is refused')
  }

  # (2) 判定 (偽のプロセス)。並び: 実行ファイル・コマンド行・親の読み (absent・present・unknown)・期待・見出し。
  $t0 = [datetime]::new(2000, 1, 1, 10, 0, 0)
  $q = [char]34
  $sinkCmd = $q + $bundled + $q + ' -y -hide_banner -loglevel warning -listen 1 -i ' + $url + ' -c copy C:\w\stream-c6.flv'
  $winget = 'C:\Tools\WinGet\Links\ffmpeg.exe'
  $cases = @(
    @($bundled, $sinkCmd, 'absent', 'stale', 'the bundled receiver whose parent pid is held by no process'),
    @($bundled, $sinkCmd, 'present', 'owned', 'the receiver whose parent pid is held (a running driver, or a reused pid) is not touched'),
    @($bundled, $sinkCmd, 'unknown', 'undecided', 'a failed read of the parent is undecided, never stale'),
    @($bundled.ToUpperInvariant(), $sinkCmd, 'absent', 'stale', 'the path is compared without case'),
    @($bundled, ($bundled + ' -listen 1 -i "' + $url + '" -c copy x.flv'), 'absent', 'stale', 'a quoted URL'),
    @($winget, $sinkCmd, 'absent', $null, 'another ffmpeg with the same command line is not touched'),
    @($null, $sinkCmd, 'absent', $null, 'a process whose path cannot be read is not touched'),
    @($bundled, $null, 'absent', $null, 'a process whose command line cannot be read is not touched'),
    @($bundled, ($bundled + ' -i C:\w\rec.mov -f framemd5 -'), 'absent', $null, 'the bundled ffmpeg used by a judge is not touched'),
    @($bundled, ($bundled + ' -i ' + $url + ' -c copy x.flv'), 'absent', $null, 'without -listen it is not a receiver'),
    @($bundled, ($bundled + ' -listen 1 -i ' + $url + '2 -c copy x.flv'), 'absent', $null, 'a longer URL is another receiver'),
    @($bundled, ($bundled + ' -listen 1 -i rtmp://127.0.0.1:19350/live/test -c copy x.flv'), 'absent', $null, 'another port is another receiver'),
    @($bundled, ($bundled + ' -nolisten 1 -i ' + $url), 'absent', $null, '-nolisten is not -listen')
  )
  $i = 0
  foreach ($c in $cases) {
    $i++
    $proc = [PSCustomObject]@{ ProcessId = 1001 + 2 * $i; ParentProcessId = 2001 + 2 * $i; ExecutablePath = $c[0]; CommandLine = $c[1]; CreationDate = $t0 }
    $got = Get-ReceiverClass -Proc $proc -ParentState $c[2] -Exe $bundled -Url $url
    Check ($got -eq $c[3]) ('class: ' + $c[4] + ' -> ' + $(if ($null -eq $c[3]) { 'none' } else { $c[3] }) + ' (got ' + $(if ($null -eq $got) { 'none' } else { $got }) + ')')
  }

  # (3) 親の読み (Get-ParentState): 2 つの読みが「居ない」で揃ったときだけ absent。失敗は unknown。偽の pid は奇数 (Windows の pid は 4 の倍数で、
  # 奇数の pid のプロセスはない。読みを誤って本物に向けても何も当たらない)。
  $cimNone = { param($Id) return @() }
  $cimOne = { param($Id) return @([PSCustomObject]@{ ProcessId = $Id }) }
  $cimFail = { param($Id) throw 'WMI quota violation' }
  $probeAbsent = { param($Id) return 'absent' }
  $probePresent = { param($Id) return 'present' }
  $probeFail = { param($Id) throw 'Access is denied' }
  $psCases = @(
    @(3001, $cimNone, $probeAbsent, 'absent', 'both reads say no process'),
    @(3003, $cimOne, $probeAbsent, 'present', 'Win32_Process sees the parent pid'),
    @(3005, $cimNone, $probePresent, 'present', 'Get-Process sees the parent pid'),
    @(3007, $cimFail, $probeAbsent, 'unknown', 'a failed Win32_Process read'),
    @(3009, $cimNone, $probeFail, 'unknown', 'a failed Get-Process read (not the not-found error)'),
    @(0, $cimNone, $probeAbsent, 'present', 'a parent pid of 0 counts as present (never stale)')
  )
  foreach ($c in $psCases) {
    $st = Get-ParentState -ParentPid $c[0] -CimQuery $c[1] -ProcessProbe $c[2]
    Check ($st.state -eq $c[3]) ('parent: ' + $c[4] + ' -> ' + $c[3] + ' (got ' + $st.state + ')')
  }
  # 実際の読み: 奇数の pid は居ない (Get-Process の「ない」の誤りの名前が合っている)、自分の pid は居る。
  $live = Get-ParentState -ParentPid 4000001 -CimQuery $LiveCimQuery -ProcessProbe $LiveProcessProbe
  Check ($live.state -eq 'absent') ('parent (live): an odd pid that no process can hold is absent (got ' + $live.state + ' ' + $live.error + ')')
  $live = Get-ParentState -ParentPid $PID -CimQuery $LiveCimQuery -ProcessProbe $LiveProcessProbe
  Check ($live.state -eq 'present') ('parent (live): this powershell is present (got ' + $live.state + ')')

  # (4) Find-Receivers と Invoke-StaleCheck: 当たらない ffmpeg の親は読まない。親の読みが 1 つでも失敗すれば -Stop でも止めない (終了コード 3)。
  $script:SsParentReads = New-Object System.Collections.ArrayList
  $procs = @(
    [PSCustomObject]@{ ProcessId = 11; ParentProcessId = 21; ExecutablePath = $bundled; CommandLine = $sinkCmd; CreationDate = $t0 },
    [PSCustomObject]@{ ProcessId = 13; ParentProcessId = 23; ExecutablePath = $winget; CommandLine = $sinkCmd; CreationDate = $t0 },
    [PSCustomObject]@{ ProcessId = 15; ParentProcessId = 25; ExecutablePath = $bundled; CommandLine = $sinkCmd; CreationDate = $t0 },
    [PSCustomObject]@{ ProcessId = 17; ParentProcessId = 27; ExecutablePath = $bundled; CommandLine = ($bundled + ' -i a.mov -f null -'); CreationDate = $t0 },
    [PSCustomObject]@{ ProcessId = 19; ParentProcessId = 29; ExecutablePath = $bundled; CommandLine = $sinkCmd; CreationDate = $null }
  )
  $parentOf = {
    param($ParentPid)
    [void]$script:SsParentReads.Add($ParentPid)
    if ($ParentPid -eq 25) { return [PSCustomObject]@{ state = 'present'; error = $null } }
    return [PSCustomObject]@{ state = 'absent'; error = $null }
  }
  $found = Find-Receivers -Procs $procs -ParentStateOf $parentOf -Exe $bundled -Url $url
  Check ((@($found.stale | ForEach-Object { $_.pid }) -join ',') -eq '11' -and (@($found.owned | ForEach-Object { $_.pid }) -join ',') -eq '15') ('find: 11 is stale and 15 is owned (stale ' + (@($found.stale | ForEach-Object { $_.pid }) -join ',') + ', owned ' + (@($found.owned | ForEach-Object { $_.pid }) -join ',') + ')')
  Check ((@($found.undecided | ForEach-Object { $_.pid }) -join ',') -eq '19') 'find: a receiver whose start time cannot be read is undecided (it could not be checked again before a stop)'
  Check ((@($script:SsParentReads) -join ',') -eq '21,25,29') ('find: only the parents of receivers are read (' + (@($script:SsParentReads) -join ',') + ')')
  $script:SsKills = New-Object System.Collections.ArrayList
  $openNever = { param($Id) [void]$script:SsKills.Add('open ' + $Id); return $null }
  $parentFails = { param($ParentPid) if ($ParentPid -eq 25) { return [PSCustomObject]@{ state = 'unknown'; error = 'WMI quota violation' } }; return [PSCustomObject]@{ state = 'absent'; error = $null } }
  $chk = Invoke-StaleCheck -Procs @($procs[0], $procs[2]) -Exe $bundled -Url $url -DoStop $true -WaitMs 0 -ParentStateOf $parentFails -OpenProc $openNever -ReadProc { param($Id) return $null }
  Check ($chk.exitCode -eq 3 -and $script:SsKills.Count -eq 0 -and $chk.result.stale[0].outcome -eq 'not-stopped-undecided' -and -not $chk.result.ok) ('stop: one undecided receiver means nothing is stopped and the exit code is 3 (exit ' + $chk.exitCode + ', opened ' + $script:SsKills.Count + ')')
  $chk = Invoke-StaleCheck -Procs @($procs[0]) -Exe $bundled -Url $url -DoStop $false -WaitMs 0 -ParentStateOf $parentOf -OpenProc $openNever -ReadProc { param($Id) return $null }
  Check ($chk.exitCode -eq 1 -and $chk.result.stale[0].outcome -eq 'found' -and $script:SsKills.Count -eq 0 -and (@($chk.result.remaining) -join ',') -eq '11') 'find only: a receiver without a parent is found (exit 1), nothing is opened'

  # (5) 止め方 (偽のプロセス): 開いた手の起動時刻が違えば止めない (使い回された pid)、止めるのは開いた物そのもの (Kill)、
  # 開いたら居なかった物は already-gone で stopped にしない。偽の pid は奇数。
  function New-FakeOpened {
    param([int64]$Id, [datetime]$Start, [bool]$Exited = $false)
    $o = New-Object PSObject
    $o | Add-Member -MemberType NoteProperty -Name Id -Value $Id
    $o | Add-Member -MemberType NoteProperty -Name HasExited -Value $Exited
    $o | Add-Member -MemberType NoteProperty -Name StartTime -Value $Start
    $o | Add-Member -MemberType ScriptMethod -Name Kill -Value { [void]$script:SsKills.Add('kill ' + $this.Id); $this.HasExited = $true }
    $o | Add-Member -MemberType ScriptMethod -Name WaitForExit -Value { param($Ms) return [bool]$this.HasExited }
    return $o
  }
  $fakeProcs = @(
    [PSCustomObject]@{ ProcessId = 101; ParentProcessId = 201; ExecutablePath = $bundled; CommandLine = $sinkCmd; CreationDate = $t0 },
    [PSCustomObject]@{ ProcessId = 103; ParentProcessId = 203; ExecutablePath = $bundled; CommandLine = $sinkCmd; CreationDate = $t0 },
    [PSCustomObject]@{ ProcessId = 105; ParentProcessId = 205; ExecutablePath = $bundled; CommandLine = $sinkCmd; CreationDate = $t0 },
    [PSCustomObject]@{ ProcessId = 107; ParentProcessId = 207; ExecutablePath = $bundled; CommandLine = $sinkCmd; CreationDate = $t0 }
  )
  $script:SsOpened = @{
    101 = (New-FakeOpened -Id 101 -Start $t0.AddTicks(7))
    103 = (New-FakeOpened -Id 103 -Start $t0.AddSeconds(30))
    105 = $null
    107 = (New-FakeOpened -Id 107 -Start $t0)
  }
  $script:SsKills.Clear()
  $openFake = { param($Id) return $script:SsOpened[[int]$Id] }
  $readFake = { param($Id) foreach ($fp in $fakeProcs) { if ($fp.ProcessId -eq $Id) { return $fp } }; return $null }
  # 107 は止める直前の読み直しで親の pid に誰かが入っていた形。
  $absentButLate = { param($ParentPid) if ($script:SsRecheck -and $ParentPid -eq 207) { return [PSCustomObject]@{ state = 'present'; error = $null } }; return [PSCustomObject]@{ state = 'absent'; error = $null } }
  $script:SsRecheck = $false
  $found = Find-Receivers -Procs $fakeProcs -ParentStateOf $absentButLate -Exe $bundled -Url $url
  $script:SsRecheck = $true
  Stop-StaleReceivers -Stale $found.stale -Exe $bundled -Url $url -WaitMs 0 -OpenProc $openFake -ReadProc $readFake -ParentStateOf $absentButLate
  $by = @{}
  foreach ($s in $found.stale) { $by[[int]$s.pid] = $s }
  Check ((@($script:SsKills) -join ',') -eq 'kill 101') ('stop: only the opened process whose start time matches is killed, through the opened object (' + (@($script:SsKills) -join ',') + ')')
  Check ($by[101].outcome -eq 'stopped' -and $by[101].stopped) 'stop: the start time is compared at microseconds (100 ns ticks below that do not matter)'
  Check ($by[103].outcome -eq 'left-alone' -and -not $by[103].stopped) ('stop: a pid now held by a process that started at another time is left alone (' + $by[103].outcome + ')')
  Check ($by[105].outcome -eq 'already-gone' -and -not $by[105].stopped) ('stop: a receiver that is gone before the stop is already-gone, not stopped (' + $by[105].outcome + ')')
  Check ($by[107].outcome -eq 'left-alone' -and -not $by[107].stopped) ('stop: a parent pid that is held again at the last check is left alone (' + $by[107].outcome + ')')
  $outRows = ConvertTo-StaleOutput $found.stale
  Check (@($outRows | Where-Object { $_.stopped }).Count -eq 1 -and $null -eq ($outRows[0].PSObject.Properties['createdUs'])) 'stop: the output says stopped only for the stopped one and does not carry the inner tick'

  # (6) 実際の ffmpeg (同梱の物がある PC だけ)。テスト専用の URL で、親のいない形 (親が先に終わる) と動いている親の形を 1 つずつ。
  $real = [ordered]@{ ran = $false; reason = $null }
  if (-not (Test-Path -LiteralPath $bundled -PathType Leaf)) {
    $real.reason = 'the bundled ffmpeg is not here (skipped): ' + $bundled
  }
  else {
    $real.ran = $true
    $orphanUrl = 'rtmp://127.0.0.1:19351/live/cordmux-selftest'
    $ownedUrl = 'rtmp://127.0.0.1:19352/live/cordmux-selftest'
    # 起こした物は直後に手を開いて持つ (finally はこの手で止める。pid では止めない)。
    $held = New-Object System.Collections.ArrayList
    try {
      # 親のいない形: 間の powershell が ffmpeg を起こして pid を書き、すぐ終わる (ffmpeg の親は居なくなる)。
      $argText = '-hide_banner -loglevel quiet -listen 1 -i ' + $orphanUrl + ' -f null -'
      $inner = "`$p = Start-Process -FilePath '" + $bundled.Replace("'", "''") + "' -ArgumentList '" + $argText + "' -WindowStyle Hidden -PassThru; `$p.Id"
      $orphanPidText = & (Join-Path $PSHOME 'powershell.exe') -NoProfile -NonInteractive -Command $inner
      $orphanPid = [int64]0
      [void][int64]::TryParse(([string]($orphanPidText | Select-Object -Last 1)).Trim(), [ref]$orphanPid)
      $orphanProc = $null
      if ($orphanPid -gt 0) {
        $orphanProc = Get-Process -Id $orphanPid -ErrorAction SilentlyContinue
        if ($null -ne $orphanProc) {
          [void]$orphanProc.Handle
          if (Test-SamePath -A $orphanProc.Path -B $bundled) { [void]$held.Add($orphanProc) } else { $orphanProc = $null }
        }
      }
      # 動いている親の形: このツールの子。
      $ownedProc = Start-Process -FilePath $bundled -ArgumentList ('-hide_banner -loglevel quiet -listen 1 -i ' + $ownedUrl + ' -f null -') -WindowStyle Hidden -PassThru
      [void]$ownedProc.Handle
      [void]$held.Add($ownedProc)
      Start-Sleep -Milliseconds 800
      Check ($null -ne $orphanProc -and -not $orphanProc.HasExited) ('real: the receiver without a parent is running (pid ' + $orphanPid + ')')
      $all = Get-FfmpegProcesses
      $c1 = Invoke-StaleCheck -Procs $all -Exe $bundled -Url $orphanUrl -DoStop $false -WaitMs 0 -ParentStateOf $LiveParentStateOf -OpenProc $LiveOpenProc -ReadProc $LiveReadProc
      Check ((@($c1.result.stale | ForEach-Object { $_.pid }) -join ',') -eq [string]$orphanPid -and @($c1.result.owned).Count -eq 0 -and $c1.exitCode -eq 1) ('real: the receiver without a parent is found (' + (@($c1.result.stale | ForEach-Object { $_.pid }) -join ',') + ', exit ' + $c1.exitCode + ')')
      Check (-not $orphanProc.HasExited) 'real: finding does not stop it'
      $c2 = Invoke-StaleCheck -Procs $all -Exe $bundled -Url $ownedUrl -DoStop $true -WaitMs 3000 -ParentStateOf $LiveParentStateOf -OpenProc $LiveOpenProc -ReadProc $LiveReadProc
      Check (@($c2.result.stale).Count -eq 0 -and (@($c2.result.owned | ForEach-Object { $_.pid }) -join ',') -eq [string]$ownedProc.Id -and $c2.exitCode -eq 0) ('real: the receiver whose parent is running is owned (' + (@($c2.result.owned | ForEach-Object { $_.pid }) -join ',') + ')')
      # 使い回された pid の形: 動いている子の pid を、起動時刻の違う情報で止めさせても止めない。
      $reused = @([PSCustomObject]@{ pid = [int64]$ownedProc.Id; parentPid = 0; createdAt = 'x'; createdUs = [int64](Get-MicrosecondTicks $ownedProc.StartTime) - 10; outcome = 'found'; stopped = $false; error = $null })
      Stop-StaleReceivers -Stale $reused -Exe $bundled -Url $ownedUrl -WaitMs 0 -OpenProc $LiveOpenProc -ReadProc $LiveReadProc -ParentStateOf $LiveParentStateOf
      Check ($reused[0].outcome -eq 'left-alone' -and -not $ownedProc.HasExited) ('real: a record whose start time differs from the process at that pid is left alone (' + $reused[0].outcome + ')')
      $c3 = Invoke-StaleCheck -Procs (Get-FfmpegProcesses) -Exe $bundled -Url $orphanUrl -DoStop $true -WaitMs 3000 -ParentStateOf $LiveParentStateOf -OpenProc $LiveOpenProc -ReadProc $LiveReadProc
      $orphanProc.Refresh()
      Check ($c3.exitCode -eq 0 -and $c3.result.stale[0].outcome -eq 'stopped' -and $orphanProc.HasExited) ('real: -Stop stops the receiver without a parent (exit ' + $c3.exitCode + ', ' + (@($c3.result.stale | ForEach-Object { $_.outcome }) -join ',') + ')')
      $ownedProc.Refresh()
      Check (-not $ownedProc.HasExited) 'real: the owned receiver is still running after the stop'
      # 居なくなった後の記録をもう 1 度渡しても止めず、stopped と書かない。
      $again = @([PSCustomObject]@{ pid = $orphanPid; parentPid = 0; createdAt = 'x'; createdUs = [int64]0; outcome = 'found'; stopped = $false; error = $null })
      Stop-StaleReceivers -Stale $again -Exe $bundled -Url $orphanUrl -WaitMs 0 -OpenProc $LiveOpenProc -ReadProc $LiveReadProc -ParentStateOf $LiveParentStateOf
      Check ($again[0].outcome -eq 'already-gone' -and -not $again[0].stopped) ('real: a pid that is already gone is already-gone, not stopped (' + $again[0].outcome + ')')
    }
    finally {
      # 自分で起こした物だけ、起こした直後に開いた手で止める。
      foreach ($p in $held) {
        try { $p.Refresh(); if (-not $p.HasExited) { $p.Kill(); [void]$p.WaitForExit(3000) } } catch { }
      }
    }
    $leftover = @($held | Where-Object { $_.Refresh(); -not $_.HasExited } | ForEach-Object { $_.Id })
    Check ($leftover.Count -eq 0) ('real: nothing started by the self-test is left (' + ($leftover -join ',') + ')')
  }
  return [PSCustomObject]@{ ok = ($failures.Count -eq 0); selftest = $true; real = [PSCustomObject]$real; checks = $checks.ToArray(); failures = $failures.ToArray() }
}

try {
  if ($SelfTest) {
    $r = Invoke-SelfTest
    Write-JsonResultAndExit -Result $r -ExitCode ($(if ($r.ok) { 0 } else { 1 }))
  }
  $parsed = Resolve-StaleArguments -ExeText $StreamSinkExe -UrlText $StreamSinkUrl -RepoRoot (Get-RepoRoot)
  if ($parsed.error) {
    Write-JsonResultAndExit -Result ([PSCustomObject]@{ ok = $false; stop = [bool]$Stop; refused = $true; error = $parsed.error }) -ExitCode 2
  }
  $procs = $null
  try {
    $procs = Get-FfmpegProcesses
  }
  catch {
    Write-JsonResultAndExit -Result ([PSCustomObject]@{ ok = $false; stop = [bool]$Stop; exe = $parsed.exe; url = $parsed.url; refused = $false; error = ('the process list could not be read; nothing was stopped: ' + $_.Exception.Message) }) -ExitCode 3
  }
  $chk = Invoke-StaleCheck -Procs $procs -Exe $parsed.exe -Url $parsed.url -DoStop ([bool]$Stop) -WaitMs $StopWaitMs -ParentStateOf $LiveParentStateOf -OpenProc $LiveOpenProc -ReadProc $LiveReadProc
  Write-JsonResultAndExit -Result $chk.result -ExitCode $chk.exitCode
}
catch {
  Write-JsonResultAndExit -Result ([PSCustomObject]@{ ok = $false; stop = [bool]$Stop; refused = $false; error = ('could not decide; nothing more was stopped: ' + $_.Exception.Message) }) -ExitCode 3
}
