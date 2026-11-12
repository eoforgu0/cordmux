#requires -Version 5.1
<#
  cleanup.ps1 - 実機テストの実行 (作業セッション) の後始末。

  巻き戻しは OBS や cordmux.dll を使わず、バックアップファイルに対して単独で動く。
  cleanup.selftest.ps1 は、巻き戻しをコピーに対してだけ実行し、実際のファイルには
  触れないことを確認する。

  BOM 付き UTF-8 で保存する。BOM を外すと PowerShell 5.1 が cp932 として
  読んで日本語が文字化けする。

  このスクリプトが触れる範囲 (これだけ):
    - 実行ファイルのパスが -WorkDir の下にある obs64 / electron / ffmpeg と
      いう名前のプロセス (それ以外は絶対に触らない。Get-OwnTargetProcesses を見よ)
    - 親のいない配信の受け口: 実行ファイルが同梱の ffmpeg (src-obs/.deps の obs-deps。
      -WorkDir の外) で、コマンド行に -listen と受け口の URL (rtmp://127.0.0.1:1935/live/test) があり、
      親の pid に誰もいないプロセスを stream-sink-stale.ps1 (子の powershell。-Stop なし) で
      見つけ、問題として書くだけ。止めない (止めるのは、メッセージを読んだ人が
      stream-sink-stale.ps1 -Stop を手で実行するとき)。読むだけなので -DryRun でも同じ
    - -WorkDir の下にある、録画・配信用の拡張子に一致するファイル
      (*.jsonl (録画ログ) と *.pcm (録画ファイルの作成の一時ファイル) を含む。
       -KeepRecordings のときは、名前が recordings のフォルダーの下だけ残す)
    - -WorkDir\appdata (run-window.mjs が APPDATA を差し替える先そのもの。
      フォルダーごと消してよい)
    - %APPDATA%\cordmux-harness、%LOCALAPPDATA%\electron (フォルダーごと。
      -RealProfile のときは触らない。下の -RealProfile の欄を見よ)
    - %LOCALAPPDATA%\cordmux\session.json という 1 ファイルだけ
    - -CallWindow のときだけ: -StateJsonPath の中の vencordDir の値を
      -BackupPath (製品が書くバックアップ) から復元し、-ArtifactsPath (実行スクリプトが書く
      1 ファイル) に記録された一時 Vencord ビルド・ブラウザのプロファイル・
      共有の中身に使った一時フォルダーも削除する
  -WatchProgramData のときだけ読むもの (VM の往復だけ):
    - %ProgramData%\obs-studio\plugins の一覧 (漏れの監視。読むだけで消さない)
  絶対に触れないもの: 利用者の実際の OBS と Vesktop、環境変数 CORDMUX_PROTECTED_PROCESSES の
  ファイルで指定したプロセスとその設定、cmake/MSBuild/node、npm のキャッシュ、利用者のアプリのキャッシュ。

  バックアップの出所: vencordDir のバックアップは製品が書く 1 ファイル
  vencord_dir_backup.json (obs_module_config_path 配下 =
  ポータブル OBS では <obs>\config\obs-studio\plugin_config\cordmux\) である。
  キーは previous / written / written_at / dll_dir で、
  src-obs/install/vencord_dir.cpp の backup_to_json がこの名前で書く
  (C++ のフィールド名だけが written_at_us で、JSON のキーは written_at)。
  previous が JSON の null なら「書く前はキーがなかった」という意味である。

  終了コード: 0 = すべて確認済みでクリーン、1 = 何か残っている
  (一覧つき)、2 = 引数が不正、またはバックアップファイルがない/壊れている。
#>

[CmdletBinding()]
param(
  # 作業用のフォルダー。空なら、param の後でこのスクリプトの場所から決める (リポジトリの直下の _work)。
  # powershell -File で起動したとき、param の既定の式の中では $PSScriptRoot が空になるため、ここでは決めない。
  [string]$WorkDir = '',
  # 製品 (cordmux.dll) が読み書きする state.json の場所と同じ決め方にする: 環境変数 VENCORD_USER_DATA_DIR があればその下、
  # なければ %APPDATA%\Vesktop (製品は RoamingAppData の既知のフォルダーを読むが、差し替えていない実行では同じ場所)。
  # run-window.mjs は、差し替えた実行で VENCORD_USER_DATA_DIR を作業のフォルダーの下に向けて渡す。
  [string]$StateJsonPath = $(if ($env:VENCORD_USER_DATA_DIR) { Join-Path $env:VENCORD_USER_DATA_DIR 'state.json' } else { "$env:APPDATA\Vesktop\state.json" }),
  # ポータブル OBS のフォルダー (製品のバックアップの保存先を組み立てるため)。空なら
  # 作業用のフォルダーの下のポータブル OBS 31.1.1。
  [string]$ObsDir = '',
  # 空なら $ObsDir から決める。
  [string]$BackupPath = '',
  # 実行スクリプトだけが知る値 (製品は知らない) の保存先。空なら作業用のフォルダーの下。
  [string]$ArtifactsPath = '',
  # 実際の %APPDATA% / %LOCALAPPDATA% のまま実行した場合 (run-window.mjs の --real-profile) の
  # 後始末。この 2 つは差し替えていないので、この実行が作ったものではない。
  # %APPDATA%\cordmux-harness と %LOCALAPPDATA%\electron には手を出さない。
  # %LOCALAPPDATA%\cordmux\session.json は cordmux.dll がこの実行で書いたものなので消す
  # (どの実行でも増える共通の後始末)。
  [switch]$RealProfile,
  [switch]$CallWindow,
  # --step record の完成ファイルは、あとで別の確認に使う。その step だけ、
  # 録画のフォルダー (名前が recordings のフォルダーの下) を残す。
  # 残したものは、確認が終わった後に、このスクリプトを素で 1 回実行して消す (README.md の「後片付け (`cleanup.ps1`)」)。
  [switch]$KeepRecordings,
  # ProgramData の漏れの監視 ((5) の %ProgramData%\obs-studio\plugins を読む) はこの
  # フラグの後ろに置き、VM の往復 (run-window.mjs --layout programdata) でだけ指定する。
  # フラグがなければ %ProgramData%\obs-studio\plugins を読みもしない (VM でない PC では利用者の
  # 実際の保存先で、実機テストの規則が読み書きを禁じている)。
  [switch]$WatchProgramData,
  # 残った配信の受け口を探す URL。空なら stream-sink-stale.ps1 の既定 (実行スクリプトの STREAM_SINK_URL)。
  # セルフテストがテスト専用の URL を渡すための引数で、rtmp://127.0.0.1:<port>/ の形でなければ stream-sink-stale.ps1 が断る。
  [string]$StreamSinkUrl = '',
  [switch]$DryRun,
  [switch]$Json
)

# 渡されなかったパスを、このスクリプトの場所から決める ($PSScriptRoot が空のときは呼び出しの情報から)。
$scriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Definition }
if (-not $WorkDir) { $WorkDir = Join-Path $scriptDir '..\..\..\_work' }
if (-not $ObsDir) { $ObsDir = Join-Path $WorkDir 'obs-33.0.1' }
if (-not $BackupPath) { $BackupPath = Join-Path $ObsDir 'config\obs-studio\plugin_config\cordmux\vencord_dir_backup.json' }
if (-not $ArtifactsPath) { $ArtifactsPath = Join-Path $WorkDir 'call-window-artifacts.json' }

$scriptStart = Get-Date
$problems = New-Object System.Collections.Generic.List[string]
$actions = New-Object System.Collections.Generic.List[string]
$userAppsSeen = New-Object System.Collections.Generic.List[string]
$buildToolsSeen = New-Object System.Collections.Generic.List[string]
$restoreBranch = 'not-applicable'

function Get-FullPath {
  param([string]$Path)
  return [System.IO.Path]::GetFullPath($Path)
}

function Get-ProcessPathSafe {
  param($Proc)
  try { return $Proc.Path } catch { return $null }
}

function Test-UnderDir {
  param([string]$Candidate, [string]$DirFull)
  if (-not $Candidate) { return $false }
  return $Candidate.StartsWith(($DirFull + '\'), [System.StringComparison]::OrdinalIgnoreCase)
}

function Get-OwnTargetProcesses {
  # 「自分の」= 実行パスが WorkDir の下にある obs64 / electron / ffmpeg。
  # パスで判定する (コマンドラインでは判定しない)。このスクリプトの実行中に
  # 自分で呼んだものであっても、システムに入っている ffmpeg.exe には意図的に一致させない。
  # WorkDir の外にあるものには絶対に手を出さないためである。
  # 下の「親のいない配信の受け口」は WorkDir の外の同梱の ffmpeg を読むが、
  # 伝えるだけで止めない (このスクリプトが止めるのは、ここで選んだ WorkDir の下の物だけ)。
  param([string]$WorkDirFull)
  $names = 'obs64', 'electron', 'ffmpeg'
  Get-Process -ErrorAction SilentlyContinue |
    Where-Object { $names -contains $_.ProcessName } |
    Where-Object { Test-UnderDir -Candidate (Get-ProcessPathSafe $_) -DirFull $WorkDirFull }
}

function Test-JsonPropertyExists {
  param($Obj, [string]$Name)
  if ($null -eq $Obj) { return $false }
  return $null -ne $Obj.PSObject.Properties[$Name]
}

function Get-JsonPropertyValueOrNull {
  param($Obj, [string]$Name)
  if (-not (Test-JsonPropertyExists $Obj $Name)) { return $null }
  return $Obj.PSObject.Properties[$Name].Value
}

function Set-OrRemoveVencordDir {
  # バックアップの previous が null なら、鍵そのものを削除する (書く前は存在しなかった、
  # という意味なので)。JSON の null として書き戻してはいけない。cleanup.selftest.ps1
  # の (c) を見よ。
  param($Obj, $Value)
  if ($null -eq $Value) {
    if (Test-JsonPropertyExists $Obj 'vencordDir') {
      [void]$Obj.PSObject.Properties.Remove('vencordDir')
    }
  } else {
    if (Test-JsonPropertyExists $Obj 'vencordDir') {
      $Obj.vencordDir = $Value
    } else {
      [void](Add-Member -InputObject $Obj -NotePropertyName 'vencordDir' -NotePropertyValue $Value -Force)
    }
  }
}

function Read-JsonFile {
  # BOM のないファイルでも -Encoding UTF8 が要る: PS 5.1 の Get-Content は
  # BOM がないと既定で ANSI コードページとして読むため、改行の誤読どころか
  # 非 ASCII 文字そのものが壊れる。
  param([string]$Path)
  $text = Get-Content -LiteralPath $Path -Raw -Encoding UTF8
  return $text | ConvertFrom-Json
}

function Write-JsonFileNoBom {
  # state.json は Node (Vesktop) が読む。PS 5.1 の Set-Content -Encoding UTF8 は
  # EF BB BF の BOM を書いてしまい、Node の JSON.parse はそれを取り除かないため、
  # バイト列を自分で書く。
  param([string]$Path, $Object, [int]$Depth = 20)
  $json = $Object | ConvertTo-Json -Depth $Depth
  $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
  [System.IO.File]::WriteAllText($Path, $json, $utf8NoBom)
}

# ---- 引数の健全性チェック (異常なら exit 2) --------------------------------

if ([string]::IsNullOrWhiteSpace($WorkDir) -or [string]::IsNullOrWhiteSpace($StateJsonPath) -or [string]::IsNullOrWhiteSpace($BackupPath) -or [string]::IsNullOrWhiteSpace($ArtifactsPath)) {
  Write-Output "RESULT: ERROR"
  Write-Output "  - WorkDir / StateJsonPath / BackupPath / ArtifactsPath must not be empty"
  exit 2
}

$workDirFull = (Get-FullPath $WorkDir).TrimEnd('\')

# ---- (1)+(2) ポータブル OBS / 自分自身のプロセス ---------------------------

$ownProcs = @(Get-OwnTargetProcesses -WorkDirFull $workDirFull)
foreach ($p in $ownProcs) {
  Write-Output ("own-process: " + $p.ProcessName + " " + $p.Id + " " + (Get-ProcessPathSafe $p))
}

if ($ownProcs.Count -gt 0 -and -not $DryRun) {
  foreach ($p in $ownProcs) {
    try { $p.CloseMainWindow() | Out-Null } catch {}
  }
  Start-Sleep -Seconds 5
  foreach ($p in $ownProcs) {
    $still = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
    if ($still) {
      Write-Output ("force-stop: " + $p.ProcessName + " " + $p.Id)
      Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    }
  }
  Start-Sleep -Seconds 2
}

if ($DryRun) {
  foreach ($p in $ownProcs) {
    $actions.Add("would stop: " + $p.ProcessName + " " + $p.Id + " " + (Get-ProcessPathSafe $p))
  }
} else {
  $remainingOwn = @(Get-OwnTargetProcesses -WorkDirFull $workDirFull)
  foreach ($p in $remainingOwn) {
    $problems.Add("own process still running after stop: " + $p.ProcessName + " " + $p.Id + " " + (Get-ProcessPathSafe $p))
  }
}

# ---- 親のいない配信の受け口 ------------------------------------------------------------------
# 同梱の ffmpeg の受け口 (-listen) で、親の pid に誰もいない物を探して問題として書く。止めない。
# 実行スクリプト (node) を外から打ち切っても、受け口は node のジョブごと 5-17 ms で終わる
# (libuv が detached でない子を「閉じたら中を全部終わらせる」ジョブに入れる)。だから実行スクリプトの受け口は残らず、
# 親のいない受け口は、閉じたシェルから手で起動した物だけである。止める相手がないのに自動で止めると、
# 動いている実行の受け口や、使い回された pid を誤って止めるおそれだけが残る。
# そのため、止めるのは、メッセージを読んだ人が stream-sink-stale.ps1 -Stop を手で実行するときだけにする。
# 同梱の ffmpeg は WorkDir の外なので、上の自分のプロセスには入らない。判定は stream-sink-stale.ps1 の 1 か所にあり、
# 実行スクリプトの stream-sink-start も同じ物を呼ぶ。子の powershell で呼ぶのは、このスクリプトの変数 ($DryRun など) を混ぜないためである。
# 受け口の pid を WorkDir に保存して照合する方法は、バックアップを書く前に落ちた実行を拾えないので採らない。
# 判定をここにコピーする方法は、実行スクリプトと 2 か所になるので採らない。
$streamSinkCheck = $null
$staleScript = Join-Path $scriptDir 'stream-sink-stale.ps1'
$staleArgs = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $staleScript)
if ($StreamSinkUrl) { $staleArgs += @('-StreamSinkUrl', $StreamSinkUrl) }
try {
  $staleOut = & (Join-Path $PSHOME 'powershell.exe') @staleArgs 2>$null
  $staleExit = $LASTEXITCODE
  $staleLine = [string](@($staleOut) | Where-Object { $_ -match '^\s*\{' } | Select-Object -Last 1)
  if (-not $staleLine) { throw ("no result from stream-sink-stale.ps1 (exit " + $staleExit + ")") }
  $streamSinkCheck = $staleLine | ConvertFrom-Json
  if ($streamSinkCheck.refused -or ($staleExit -eq 2)) {
    $problems.Add("could not check for a stale stream receiver: " + [string]$streamSinkCheck.error)
  } else {
    if ($staleExit -eq 3) {
      # 親の読みかプロセスの一覧の読みが失敗した (決められない)。何も止めていない。
      $undecidedPids = @(@($streamSinkCheck.undecided) | Where-Object { $null -ne $_ } | ForEach-Object { $_.pid }) -join ','
      $problems.Add("could not decide whether a stream receiver is stale (nothing was stopped): " + [string]$streamSinkCheck.error + $(if ($undecidedPids) { " (pid " + $undecidedPids + ")" } else { "" }))
    }
    foreach ($s in @($streamSinkCheck.stale)) {
      if ($null -eq $s) { continue }
      $problems.Add("stale stream receiver: pid " + $s.pid + " (its parent process " + $s.parentPid + " is gone) " + $streamSinkCheck.exe + " " + $streamSinkCheck.url + " is still running; cleanup.ps1 does not stop it (stop it with stream-sink-stale.ps1 -Stop)")
    }
    foreach ($o in @($streamSinkCheck.owned)) {
      if ($null -eq $o) { continue }
      Write-Output ("stream receiver whose parent process is running (not touched): pid " + $o.pid + " parent " + $o.parentPid)
    }
  }
} catch {
  $problems.Add("could not check for a stale stream receiver: " + $_.Exception.Message)
}

# 利用者本人のアプリと他人のビルドツール: 報告するだけで絶対に触らず、問題としても
# 数えない (この後始末の仕事はそれらを裁くことではない)。
$obsOutside = Get-Process -Name 'obs64' -ErrorAction SilentlyContinue |
  Where-Object { -not (Test-UnderDir -Candidate (Get-ProcessPathSafe $_) -DirFull $workDirFull) }
foreach ($p in $obsOutside) { $userAppsSeen.Add("obs64 " + $p.Id + " " + (Get-ProcessPathSafe $p)) }

# 守るプロセスは、Vesktop と、利用者が環境変数 CORDMUX_PROTECTED_PROCESSES のファイルに書いたもの
# (1 行に 1 つのプロセス名。例 foo.exe。空行と # で始まる行は読まない。run-window.mjs の起動前の守りと同じ読み方)。
# 報告するだけなので、ファイルが読めなくても後始末は止めず、読めなかったことだけを出す。
$protectedNames = New-Object System.Collections.Generic.List[string]
$protectedNames.Add('vesktop')
if ($env:CORDMUX_PROTECTED_PROCESSES) {
  try {
    foreach ($line in (Get-Content -LiteralPath $env:CORDMUX_PROTECTED_PROCESSES -Encoding UTF8 -ErrorAction Stop)) {
      $entry = $line.Trim().TrimStart([char]0xFEFF)
      if ($entry.Length -eq 0 -or $entry.StartsWith('#')) { continue }
      if ($entry.EndsWith('.exe', [System.StringComparison]::OrdinalIgnoreCase)) { $entry = $entry.Substring(0, $entry.Length - 4) }
      $protectedNames.Add($entry)
    }
  } catch {
    Write-Output ("could not read CORDMUX_PROTECTED_PROCESSES file (reporting only vesktop): " + $_.Exception.Message)
  }
}
foreach ($name in $protectedNames) {
  Get-Process -Name $name -ErrorAction SilentlyContinue | ForEach-Object {
    $userAppsSeen.Add($name + " " + $_.Id)
  }
}

foreach ($line in $userAppsSeen) { Write-Output ("user-app (not touched): " + $line) }

Get-Process -ErrorAction SilentlyContinue |
  Where-Object { $_.ProcessName -in 'cmake', 'MSBuild', 'node', 'cl', 'link', 'ninja' } |
  ForEach-Object { $buildToolsSeen.Add($_.ProcessName + " " + $_.Id) }
foreach ($line in $buildToolsSeen) { Write-Output ("build-tool (not touched): " + $line) }

# ---- (3) WorkDir 配下の一時ファイル -----------------------------------------
# 削除するのは録画 (*.mkv *.mov *.mp4 *.h264 *.wav) とローカルの配信の受け口
# (*.flv) だけ。結果の JSON・ログ・*.png は、
# この一覧に入れていないだけで残る。
#
# 次の 2 つの拡張子も一覧に入れている:
#   *.jsonl  cordmux の録画ログ (src-obs/record/README.md の ledger.jsonl)
#   *.pcm    録画ファイルの作成が作る生 PCM の一時ファイル (全長ぶんの無音を
#            作って区間を実際のオフセットへ書き込む形なので、1 時間で数 GB になる)
# 区間の断片 (segments/*.mov) と黒のテストデータ (black/*.mov) と完成 (*_screen.mov /
# *_voice.mov) は、いずれも既に一覧にある *.mov に一致する。

$deleteExtensions = '.mkv', '.mov', '.mp4', '.h264', '.wav', '.flv', '.jsonl', '.pcm'
# -KeepRecordings のときだけ、名前が recordings のフォルダーの下は消さない。
$recordingsDirName = 'recordings'
function Test-UnderRecordings {
  param([string]$FullName, [string]$WorkDirFull)
  $rel = $FullName.Substring($WorkDirFull.Length).TrimStart('\', '/')
  foreach ($part in ($rel -split '[\\/]')) {
    if ($part -ieq $recordingsDirName) { return $true }
  }
  return $false
}
if (Test-Path -LiteralPath $workDirFull) {
  # 注記: Windows PowerShell 5.1 では、`\*` で終わる -Path に -Recurse -Include を
  # 付けても最上位しか一致しない (昔からある PS のバグ)。素の再帰一覧を取ってから
  # 拡張子で絞り込むほうが、_work\out\stream-test.flv のようなネストした
  # ファイルにも確実に届く。
  $tempFiles = Get-ChildItem -Path $workDirFull -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $deleteExtensions -contains $_.Extension.ToLowerInvariant() }
  if ($KeepRecordings) {
    $kept = @($tempFiles | Where-Object { Test-UnderRecordings -FullName $_.FullName -WorkDirFull $workDirFull })
    foreach ($f in $kept) { $actions.Add("kept (recordings): " + $f.FullName) }
    $tempFiles = @($tempFiles | Where-Object { -not (Test-UnderRecordings -FullName $_.FullName -WorkDirFull $workDirFull) })
  }
  foreach ($f in $tempFiles) {
    if ($DryRun) {
      $actions.Add("would delete: " + $f.FullName)
    } else {
      try {
        Remove-Item -LiteralPath $f.FullName -Force -ErrorAction Stop
      } catch {
        $problems.Add("could not delete: " + $f.FullName + " (" + $_.Exception.Message + ")")
      }
    }
  }
} else {
  Write-Output ("WorkDir not found, step 3 skipped: " + $workDirFull)
}

# ---- (4) リポジトリの外に増えたもの -----------------------------------------
# npm の通常のキャッシュと利用者のアプリのキャッシュは、意図してこの一覧に
# 入れていない (どちらも普通の、作り直せるキャッシュである)。

$harnessHarness = Join-Path $env:APPDATA 'cordmux-harness'
$electronCache = Join-Path $env:LOCALAPPDATA 'electron'
$sessionJson = Join-Path (Join-Path $env:LOCALAPPDATA 'cordmux') 'session.json'
# run-window.mjs が APPDATA を差し替える先
# (_work/appdata。テスト用の Vesktop の state.json と、テスト用にインストールしたファイルの data/vencord /
# data/vencord-src へコピーした Vencord のビルド成果が残る)。$env:APPDATA 経由ではなく
# -WorkDir 経由で参照する: このプロセス自身の $env:APPDATA は (差し替えの外で
# cleanup.ps1 を直接呼んだ場合) 利用者の実際の %APPDATA% を指しうるため、そちらは
# 絶対に触らない。
$appDataOverride = Join-Path $workDirFull 'appdata'

# -RealProfile のときは、差し替えていない %APPDATA% /
# %LOCALAPPDATA% の下にある 2 つのフォルダーを消さない。この実行はテスト用の送信側
# (Electron) を 1 度も起動していないので、そこにあるものはこの実行が作ったものではない。
$dirTargets = @($appDataOverride)
if ($RealProfile) {
  foreach ($skipped in @($harnessHarness, $electronCache)) {
    Write-Output ("kept (real profile, not created by this window): " + $skipped)
  }
} else {
  $dirTargets = @($harnessHarness, $electronCache, $appDataOverride)
}

foreach ($dirTarget in $dirTargets) {
  if (Test-Path -LiteralPath $dirTarget) {
    if ($DryRun) {
      $actions.Add("would delete: " + $dirTarget)
    } else {
      try {
        Remove-Item -LiteralPath $dirTarget -Recurse -Force -ErrorAction Stop
      } catch {
        $problems.Add("could not delete: " + $dirTarget + " (" + $_.Exception.Message + ")")
      }
    }
  }
}

if (Test-Path -LiteralPath $sessionJson -PathType Leaf) {
  if ($DryRun) {
    $actions.Add("would delete: " + $sessionJson)
  } else {
    try {
      Remove-Item -LiteralPath $sessionJson -Force -ErrorAction Stop
    } catch {
      $problems.Add("could not delete: " + $sessionJson + " (" + $_.Exception.Message + ")")
    }
  }
}

# ---- (5) 利用者の OBS/Vesktop/Discord の設定に触れない。ファイアウォール規則も増やさない --
# ここで使う実機のパスはすべて %APPDATA% / %LOCALAPPDATA% / %ProgramData% 由来
# (-WorkDir 由来ではない) なので、cleanup.selftest.ps1 はこのスクリプトを呼ぶ前に
# その 3 つの環境変数をテストデータのフォルダーへ差し替える。

$pluginsDir = Join-Path $env:ProgramData 'obs-studio\plugins'
if (-not $WatchProgramData) {
  Write-Output "ProgramData plugin watch skipped (pass -WatchProgramData in the VM only)"
} elseif (Test-Path -LiteralPath $pluginsDir) {
  # "cordmux" そのもの (接尾辞なし) は、既にインストールされている cordmux のプラグインで、
  # 利用者自身のものなのでそのままにする。
  # 新規の cordmux* (例えば "cordmux-other") があれば、この実行の作業が
  # 共有の plugins フォルダーへ漏れ出たことを意味し、それは絶対に起きてはならない
  # (テストの作業から ProgramData\obs-studio\plugins の
  # 配下には何も置かない)。
  Get-ChildItem -LiteralPath $pluginsDir -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -like 'cordmux*' -and $_.Name -ne 'cordmux' } |
    ForEach-Object { $problems.Add("unexpected plugin folder: " + $_.FullName) }
}

$obsStudioDir = Join-Path $env:APPDATA 'obs-studio'
if (Test-Path -LiteralPath $obsStudioDir) {
  Get-ChildItem -LiteralPath $obsStudioDir -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -gt $scriptStart } |
    ForEach-Object { $problems.Add("The real OBS config touched since this run started: " + $_.FullName) }
}

try {
  Get-NetFirewallRule -ErrorAction Stop |
    Where-Object { $_.DisplayName -match 'cordmux|obs64|electron' } |
    ForEach-Object { $problems.Add("unexpected firewall rule: " + $_.DisplayName) }
} catch {
  Write-Output ("firewall check skipped: " + $_.Exception.Message)
}

# vencordDir の整合性 (ここでは読むだけ。実際の復元は下の (6)-(8) で行う)。
# この確認を通るのは、値が「最後に自分が書いた値」(実行の途中は常にこれで正常)
# か「バックアップの元の値に戻っている」(手順 (6) の後、または一度も切り替えていない
# 場合に正常) のどちらかであるとき。それ以外なら、このスクリプトの規律以外の
# 何かがこの値を変えたということである。
if ((Test-Path -LiteralPath $StateJsonPath) -and (Test-Path -LiteralPath $BackupPath)) {
  try {
    $stateNow = Read-JsonFile -Path $StateJsonPath
    $backupNow = Read-JsonFile -Path $BackupPath
    $currentVal = Get-JsonPropertyValueOrNull $stateNow 'vencordDir'
    $writtenVal = Get-JsonPropertyValueOrNull $backupNow 'written'
    $previousVal = Get-JsonPropertyValueOrNull $backupNow 'previous'
    if ($null -ne $writtenVal -and $currentVal -eq $writtenVal) {
      Write-Output "vencordDir check: at the written (switched) value"
    } elseif ($currentVal -eq $previousVal) {
      Write-Output "vencordDir check: at the backed-up previous value"
    } else {
      $problems.Add("vencordDir is neither the backed-up previous nor the written value: current=" + [string]$currentVal)
    }
  } catch {
    $problems.Add("could not check vencordDir consistency: " + $_.Exception.Message)
  }
} else {
  Write-Output "vencordDir consistency check skipped (state.json or backup file not found)"
}

# ---- (6)-(8) 通話のときだけ: 巻き戻し -----------------------------------------

$callWindowArgError = $false
$artifacts = $null
if ($CallWindow) {
  $backup = $null
  $backupValid = $false

  if (Test-Path -LiteralPath $BackupPath) {
    try {
      $backup = Read-JsonFile -Path $BackupPath
      # 製品が書く 4 つのキー。復元に要るのは previous と written の 2 つで、
      # written_at と dll_dir は読むだけ (報告へ載せる)。
      if ((Test-JsonPropertyExists $backup 'written') -and (Test-JsonPropertyExists $backup 'previous')) {
        $backupValid = $true
      } else {
        $problems.Add("backup file is missing 'written' or 'previous': " + $BackupPath)
        $callWindowArgError = $true
      }
    } catch {
      $problems.Add("backup file is not valid JSON: " + $BackupPath + " (" + $_.Exception.Message + ")")
      $callWindowArgError = $true
    }
  } else {
    $problems.Add("backup file not found, cannot restore vencordDir: " + $BackupPath)
    $callWindowArgError = $true
  }

  # 実行スクリプトだけが知る値。無ければ何もしない (ブラウザも一時ビルドも
  # 使わなかった実行では書かれない)。あるのに壊れているときは問題として数える。
  if (Test-Path -LiteralPath $ArtifactsPath) {
    try {
      $artifacts = Read-JsonFile -Path $ArtifactsPath
      Write-Output ("artifacts file: " + $ArtifactsPath)
    } catch {
      $problems.Add("artifacts file is not valid JSON: " + $ArtifactsPath + " (" + $_.Exception.Message + ")")
      $callWindowArgError = $true
    }
  } else {
    Write-Output ("artifacts file not found, nothing of its own to delete: " + $ArtifactsPath)
  }

  if ($backupValid -and -not (Test-Path -LiteralPath $StateJsonPath)) {
    $problems.Add("state.json not found, cannot restore vencordDir: " + $StateJsonPath)
    $callWindowArgError = $true
    $backupValid = $false
  }

  if ($backupValid) {
    try {
      $state = Read-JsonFile -Path $StateJsonPath
      $currentVal = Get-JsonPropertyValueOrNull $state 'vencordDir'
      $writtenVal = Get-JsonPropertyValueOrNull $backup 'written'
      $previousVal = Get-JsonPropertyValueOrNull $backup 'previous'

      if ($null -ne $writtenVal -and $currentVal -eq $writtenVal) {
        $restoreBranch = 'restore'
      } elseif ($currentVal -eq $previousVal) {
        $restoreBranch = 'already-restored'
      } else {
        $restoreBranch = 'mismatch'
        $problems.Add("vencordDir does not match the backup, leaving it alone: current=" + [string]$currentVal)
      }

      if ($restoreBranch -eq 'restore') {
        if ($DryRun) {
          $actions.Add("would restore vencordDir to: " + [string]$previousVal)
        } else {
          $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
          $stateCopyName = (Split-Path -Leaf $StateJsonPath) + '.' + $stamp + '.bak'
          $stateCopyPath = Join-Path (Split-Path -Parent $BackupPath) $stateCopyName
          Copy-Item -LiteralPath $StateJsonPath -Destination $stateCopyPath -Force
          Set-OrRemoveVencordDir -Obj $state -Value $previousVal
          Write-JsonFileNoBom -Path $StateJsonPath -Object $state
          $actions.Add("restored vencordDir to: " + [string]$previousVal)
        }
      } elseif ($restoreBranch -eq 'already-restored') {
        Write-Output "vencordDir is already at the previous value, nothing to restore"
      }
    } catch {
      $problems.Add("could not read or write state.json: " + $_.Exception.Message)
    }
  }

  # (7) 一時 Vencord ビルド: vencordDir がもうそこを指していないと確認できてから
  # だけ削除する (先に消すと Vesktop の
  # ensureVencordFiles() がそのパスを作り直し、公式ビルドを取りに行ってしまう)。
  # 出所は実行スクリプトの 1 ファイル: 製品のバックアップは previous / written /
  # written_at / dll_dir の 4 つのキーしか持たない。
  $tempBuild = $null
  if ($artifacts) { $tempBuild = Get-JsonPropertyValueOrNull $artifacts 'tempVencordBuild' }
  if ($tempBuild) {
    $safeToDeleteBuild = ($restoreBranch -eq 'restore' -or $restoreBranch -eq 'already-restored')
    if (-not $safeToDeleteBuild) {
      Write-Output ("temp Vencord build kept, restore not confirmed: " + $tempBuild)
    } elseif (-not (Test-Path -LiteralPath $tempBuild)) {
      Write-Output ("temp Vencord build already gone: " + $tempBuild)
    } elseif ($DryRun) {
      $actions.Add("would delete temp Vencord build: " + $tempBuild)
    } else {
      try {
        Remove-Item -LiteralPath $tempBuild -Recurse -Force -ErrorAction Stop
      } catch {
        $problems.Add("could not delete temp Vencord build: " + $tempBuild + " (" + $_.Exception.Message + ")")
      }
    }
  }

  # (8) ブラウザのプロファイルと、共有の中身に使った一時フォルダ: vencordDir とは
  # 無関係で、いつ消しても安全。どちらも実行スクリプトの 1 ファイルから読む。
  # sharedContentDirs というキーの名前は、実行スクリプトが置いた共有の中身用の
  # 一時フォルダーを指す。
  $browserProfiles = $null
  $sharedContentDirs = $null
  if ($artifacts) {
    $browserProfiles = Get-JsonPropertyValueOrNull $artifacts 'browserProfiles'
    $sharedContentDirs = Get-JsonPropertyValueOrNull $artifacts 'sharedContentDirs'
  }
  foreach ($bp in @($browserProfiles)) {
    if (-not $bp) { continue }
    if (-not (Test-Path -LiteralPath $bp)) {
      Write-Output ("browser profile already gone: " + $bp)
    } elseif ($DryRun) {
      $actions.Add("would delete browser profile: " + $bp)
    } else {
      try {
        Remove-Item -LiteralPath $bp -Recurse -Force -ErrorAction Stop
      } catch {
        $problems.Add("could not delete browser profile: " + $bp + " (" + $_.Exception.Message + ")")
      }
    }
  }
  foreach ($sc in @($sharedContentDirs)) {
    if (-not $sc) { continue }
    if (-not (Test-Path -LiteralPath $sc)) {
      Write-Output ("shared content dir already gone: " + $sc)
    } elseif ($DryRun) {
      $actions.Add("would delete shared content dir: " + $sc)
    } else {
      try {
        Remove-Item -LiteralPath $sc -Recurse -Force -ErrorAction Stop
      } catch {
        $problems.Add("could not delete shared content dir: " + $sc + " (" + $_.Exception.Message + ")")
      }
    }
  }
}

# ---- 結果 --------------------------------------------------------------------

$exitCode = 0
if ($CallWindow -and $callWindowArgError) {
  $exitCode = 2
} elseif ($problems.Count -gt 0) {
  $exitCode = 1
}

if ($exitCode -eq 0) {
  Write-Output "RESULT: OK"
} elseif ($exitCode -eq 1) {
  Write-Output "RESULT: REMAINING"
  foreach ($p in $problems) { Write-Output ("  - " + $p) }
} else {
  Write-Output "RESULT: ERROR"
  foreach ($p in $problems) { Write-Output ("  - " + $p) }
}
$actionPrefix = if ($DryRun) { "  (dry-run) " } else { "  " }
foreach ($a in $actions) { Write-Output ($actionPrefix + $a) }

if ($Json) {
  $report = [PSCustomObject]@{
    exitCode      = $exitCode
    dryRun        = [bool]$DryRun
    callWindow    = [bool]$CallWindow
    workDir       = $workDirFull
    stateJsonPath = $StateJsonPath
    obsDir        = $ObsDir
    backupPath    = $BackupPath
    artifactsPath = $ArtifactsPath
    realProfile   = [bool]$RealProfile
    watchProgramData = [bool]$WatchProgramData
    streamSinkCheck = $streamSinkCheck
    restoreBranch = $restoreBranch
    problems      = @($problems)
    actions       = @($actions)
    userAppsSeen    = @($userAppsSeen)
    buildToolsSeen = @($buildToolsSeen)
  }
  $reportJson = $report | ConvertTo-Json -Depth 10 -Compress
  Write-Output ("CORDMUX_CLEANUP_JSON:" + $reportJson)
}

exit $exitCode
