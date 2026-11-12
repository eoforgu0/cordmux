#requires -Version 5.1
<#
  cleanup.selftest.ps1 - cleanup.ps1 をテストデータだけに対して動かすセルフテスト。

  単独の巻き戻しは、作業用の一時フォルダーに置いた state.json のコピーに対して
  実行する。利用者の実際のファイルには絶対に触れない。

  BOM 付き UTF-8 で保存する (cleanup.ps1 と同じ理由。BOM を外すと PowerShell 5.1 が
  BOM なしの .ps1 を cp932 として読み、日本語が壊れる)。

  安全策: このスクリプトは cleanup.ps1 を実際の %APPDATA%\Vesktop\state.json や
  実際の %LOCALAPPDATA%\cordmux には絶対に向けない。下の呼び出しはすべて
  -StateJsonPath / -BackupPath / -WorkDir を明示して渡し (ベルト)、さらに子プロセス
  向けに APPDATA / LOCALAPPDATA / ProgramData の環境変数を fixture の木へ差し替える
  (サスペンダー) - cleanup.ps1 の (4)/(5) はこれらの変数を -WorkDir 経由ではなく
  直接読むため、実機に触れずにその手順を検査するにはこの差し替えしかない。

  作業場所: 既定は -WorkRoot を省略したとき ([IO.Path]::GetTempPath() の下の
  cordmux-cleanup-selftest)。このセッション固有の絶対パスをハードコードせず、
  -WorkRoot パラメータで受け取る形にしてある。CI (lint.yml の cleanup-selftest job) は
  引数なしで呼ぶので、この既定がそのまま使われる。既定の作業場所はこのセルフテストだけの物なので、全部通ったら消す (失敗したときは
  調べられるよう残す)。-WorkRoot を渡したときは渡した側の物なので消さない。

  実行方法: powershell.exe -NoProfile -ExecutionPolicy Bypass -File cleanup.selftest.ps1
#>

[CmdletBinding()]
param(
  [string]$WorkRoot = (Join-Path ([IO.Path]::GetTempPath()) 'cordmux-cleanup-selftest')
)

$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$cleanupScript = Join-Path $here 'cleanup.ps1'
$root = $WorkRoot
$ownsRoot = -not $PSBoundParameters.ContainsKey('WorkRoot')

$passCount = 0
$failCount = 0

function Assert-True {
  param([bool]$Condition, [string]$Message)
  if ($Condition) {
    Write-Output ("PASS: " + $Message)
    $script:passCount = $script:passCount + 1
  } else {
    Write-Output ("FAIL: " + $Message)
    $script:failCount = $script:failCount + 1
  }
}

function Write-JsonFileNoBom {
  param([string]$Path, $Object, [int]$Depth = 20)
  $json = $Object | ConvertTo-Json -Depth $Depth
  $dir = Split-Path -Parent $Path
  if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
  $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
  [System.IO.File]::WriteAllText($Path, $json, $utf8NoBom)
}

function Read-JsonFile {
  param([string]$Path)
  $text = Get-Content -LiteralPath $Path -Raw -Encoding UTF8
  return $text | ConvertFrom-Json
}

function New-Fixture {
  # {caseDir}\appdata, \localappdata, \programdata, \workdir, \workdir\obs-33.0.1 を持つ、
  # 独立した 1 つの木を作る。ここに出てくるものはどれも実際の Windows のパスではない。
  #
  # バックアップは製品が書く場所とキーの名前にする: ポータブル OBS の
  # <obs>\config\obs-studio\plugin_config\cordmux\vencord_dir_backup.json に
  # previous / written / written_at / dll_dir。実行スクリプトだけが知る値 (一時 Vencord ビルド・
  # ブラウザのプロファイル・共有の中身の一時フォルダ) は別の 1 ファイル
  # workdir\call-window-artifacts.json に置く。
  param([string]$CaseDir, [string]$VencordDirValue, [bool]$WithBackup, $PreviousValue, [string]$BackupText)

  if (Test-Path -LiteralPath $CaseDir) { Remove-Item -LiteralPath $CaseDir -Recurse -Force }
  New-Item -ItemType Directory -Path $CaseDir -Force | Out-Null

  $appData = Join-Path $CaseDir 'appdata'
  $localAppData = Join-Path $CaseDir 'localappdata'
  $programData = Join-Path $CaseDir 'programdata'
  $workDir = Join-Path $CaseDir 'workdir'
  $obsDir = Join-Path $workDir 'obs-33.0.1'
  New-Item -ItemType Directory -Path (Join-Path $appData 'Vesktop') -Force | Out-Null
  New-Item -ItemType Directory -Path $localAppData -Force | Out-Null
  New-Item -ItemType Directory -Path (Join-Path $programData 'obs-studio\plugins') -Force | Out-Null
  New-Item -ItemType Directory -Path $workDir -Force | Out-Null
  New-Item -ItemType Directory -Path (Join-Path $workDir 'out') -Force | Out-Null
  New-Item -ItemType Directory -Path (Join-Path $obsDir 'config\obs-studio\plugin_config\cordmux') -Force | Out-Null

  $stateJsonPath = Join-Path $appData 'Vesktop\state.json'
  $backupPath = Join-Path $obsDir 'config\obs-studio\plugin_config\cordmux\vencord_dir_backup.json'
  $artifactsPath = Join-Path $workDir 'call-window-artifacts.json'

  # 他の鍵とネストしたオブジェクトを持つ state.json。往復させても他が壊れないことを
  # 確認するため。書き戻しで他の値を落とすと、利用者の Vesktop の設定を壊すことになる。
  $state = [PSCustomObject]@{
    discordBranch  = 'stable'
    minimizeToTray = $true
    windowBounds   = [PSCustomObject]@{ x = 10; y = 20; width = 1280; height = 720 }
    skipHosts      = @('a.example', 'b.example')
  }
  if ($null -ne $VencordDirValue) {
    $state | Add-Member -NotePropertyName 'vencordDir' -NotePropertyValue $VencordDirValue
  }
  Write-JsonFileNoBom -Path $stateJsonPath -Object $state

  # 実行スクリプトだけが知る値は、バックアップの有無と関係なく作る (どちらのファイルから何を読むかを
  # 取り違えたら失敗するように、バックアップがないケースでもこちらは置いておく)。
  $tempBuild = Join-Path $CaseDir 'temp-vencord-build'
  New-Item -ItemType Directory -Path $tempBuild -Force | Out-Null
  $browserProfile = Join-Path $CaseDir 'browser-profile-1'
  New-Item -ItemType Directory -Path $browserProfile -Force | Out-Null
  $sharedContent = Join-Path $CaseDir 'shared-content-1'
  New-Item -ItemType Directory -Path $sharedContent -Force | Out-Null
  Write-JsonFileNoBom -Path $artifactsPath -Object ([PSCustomObject]@{
      tempVencordBuild  = $tempBuild
      browserProfiles   = @($browserProfile)
      sharedContentDirs = @($sharedContent)
    })

  if ($BackupText) {
    # 壊れたバックアップ / 鍵の足りないバックアップ: 文字列をそのまま書く。
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($backupPath, $BackupText, $utf8NoBom)
  } elseif ($WithBackup) {
    # 製品 (src-obs/install/vencord_dir.cpp の backup_to_json) が書く 4 鍵そのもの。
    # written_at はエポックのマイクロ秒の整数 (C++ の欄の名前だけが written_at_us)。
    $backup = [PSCustomObject]@{
      previous   = $PreviousValue
      written    = $VencordDirValue
      written_at = 1788000000000000
      dll_dir    = (Join-Path $obsDir 'plugins\cordmux')
    }
    Write-JsonFileNoBom -Path $backupPath -Object $backup
  }

  # WorkDir の中身: 削除されるべきものと、残るべきもの。
  Set-Content -LiteralPath (Join-Path $workDir 'recording1.mkv') -Value 'fake-mkv' -Encoding Ascii
  Set-Content -LiteralPath (Join-Path $workDir 'out\stream-test.flv') -Value 'fake-flv' -Encoding Ascii
  Set-Content -LiteralPath (Join-Path $workDir 'out\clip.wav') -Value 'fake-wav' -Encoding Ascii
  Set-Content -LiteralPath (Join-Path $workDir 'out\result.json') -Value '{"ok":true}' -Encoding Ascii
  Set-Content -LiteralPath (Join-Path $workDir 'out\run.log') -Value 'log line' -Encoding Ascii
  Set-Content -LiteralPath (Join-Path $workDir 'out\frame1.png') -Value 'fake-png' -Encoding Ascii
  # 録画ログ (*.jsonl) と録画ファイルの作成の一時ファイル (*.pcm) と、
  # 区間の断片 / 黒のテストデータ / 完成 (*.mov)。-KeepRecordings は
  # 名前が recordings のフォルダの下だけを残す。
  New-Item -ItemType Directory -Path (Join-Path $workDir 'out\recordings\20260101-000000\segments') -Force | Out-Null
  New-Item -ItemType Directory -Path (Join-Path $workDir 'out\recordings\20260101-000000\black') -Force | Out-Null
  Set-Content -LiteralPath (Join-Path $workDir 'out\recordings\20260101-000000\ledger.jsonl') -Value '{}' -Encoding Ascii
  Set-Content -LiteralPath (Join-Path $workDir 'out\recordings\20260101-000000\taro_screen.mov') -Value 'fake-mov' -Encoding Ascii
  Set-Content -LiteralPath (Join-Path $workDir 'out\recordings\20260101-000000\segments\u1_screen_video_000001.mov') -Value 'fake-mov' -Encoding Ascii
  Set-Content -LiteralPath (Join-Path $workDir 'out\recordings\20260101-000000\black\u1_enc1.mov') -Value 'fake-mov' -Encoding Ascii
  Set-Content -LiteralPath (Join-Path $workDir 'out\recordings\20260101-000000\silence.pcm') -Value 'fake-pcm' -Encoding Ascii
  # recordings の外にも録画ファイルの作成の一時ファイルを 1 つ置く (-KeepRecordings でも消えるべきもの)。
  Set-Content -LiteralPath (Join-Path $workDir 'out\scratch.pcm') -Value 'fake-pcm' -Encoding Ascii
  Set-Content -LiteralPath (Join-Path $workDir 'out\stray.jsonl') -Value '{}' -Encoding Ascii

  return [PSCustomObject]@{
    AppData       = $appData
    LocalAppData  = $localAppData
    ProgramData   = $programData
    WorkDir       = $workDir
    ObsDir        = $obsDir
    StateJsonPath = $stateJsonPath
    BackupPath    = $backupPath
    ArtifactsPath = $artifactsPath
  }
}

# どの呼び出しもテスト専用の受け口の URL (どのツールも使わないポート) を渡す。cleanup.ps1 は URL がないと
# 実行スクリプトの実際の URL (1935 番) の受け口を読むので、実機テストの最中にセルフテストを実行すると実際の受け口の判定が結果に混ざる。
# 19354 番はこのセルフテストだけのポート (stream-sink-stale.ps1 のセルフテストは 19351・19352、case 16 は 19353)。
$SelftestStreamSinkUrl = 'rtmp://127.0.0.1:19354/live/cordmux-cleanup-selftest-none'
# 渡した URL と報告の URL を保存しておく (最後に「1935 番を 1 度も見ていない」ことを確認する)。
$script:streamSinkUrlsSeen = New-Object System.Collections.ArrayList

function Invoke-Cleanup {
  param($Fixture, [switch]$CallWindow, [switch]$DryRun, [switch]$RealProfile, [switch]$KeepRecordings, [switch]$WatchProgramData, [string]$StreamSinkUrl = $SelftestStreamSinkUrl)
  if (-not $StreamSinkUrl -or $StreamSinkUrl -match ':1935/') { throw ('the self-test must pass a test-only stream receiver URL: ' + $StreamSinkUrl) }

  $origAppData = $env:APPDATA
  $origLocalAppData = $env:LOCALAPPDATA
  $origProgramData = $env:ProgramData
  $origVencordDir = $env:VENCORD_USER_DATA_DIR
  try {
    # cleanup.ps1 の (4)/(5) が直接読む 3 つの環境変数を差し替え、子プロセスが
    # 既定値を使ってでも実機に届かないようにする。-StateJsonPath の既定は VENCORD_USER_DATA_DIR を先に読むので、
    # 実機の値が残らないよう空にする。
    $env:VENCORD_USER_DATA_DIR = $null
    $env:APPDATA = $Fixture.AppData
    $env:LOCALAPPDATA = $Fixture.LocalAppData
    $env:ProgramData = $Fixture.ProgramData

    $cleanupArgs = @(
      '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $cleanupScript,
      '-WorkDir', $Fixture.WorkDir,
      '-StateJsonPath', $Fixture.StateJsonPath,
      '-ObsDir', $Fixture.ObsDir,
      '-BackupPath', $Fixture.BackupPath,
      '-ArtifactsPath', $Fixture.ArtifactsPath,
      '-Json'
    )
    if ($CallWindow) { $cleanupArgs += '-CallWindow' }
    if ($DryRun) { $cleanupArgs += '-DryRun' }
    if ($RealProfile) { $cleanupArgs += '-RealProfile' }
    if ($KeepRecordings) { $cleanupArgs += '-KeepRecordings' }
    if ($WatchProgramData) { $cleanupArgs += '-WatchProgramData' }
    $cleanupArgs += @('-StreamSinkUrl', $StreamSinkUrl)
    [void]$script:streamSinkUrlsSeen.Add($StreamSinkUrl)

    $output = & powershell.exe @cleanupArgs 2>&1
    $exitCode = $LASTEXITCODE
  } finally {
    $env:APPDATA = $origAppData
    $env:LOCALAPPDATA = $origLocalAppData
    $env:ProgramData = $origProgramData
    $env:VENCORD_USER_DATA_DIR = $origVencordDir
  }

  $jsonLine = $output | Where-Object { $_ -like 'CORDMUX_CLEANUP_JSON:*' } | Select-Object -Last 1
  $report = $null
  if ($jsonLine) {
    $report = ($jsonLine -replace '^CORDMUX_CLEANUP_JSON:', '') | ConvertFrom-Json
    if ($null -ne $report.streamSinkCheck -and $report.streamSinkCheck.url) { [void]$script:streamSinkUrlsSeen.Add([string]$report.streamSinkCheck.url) }
  }

  return [PSCustomObject]@{
    ExitCode = $exitCode
    Output   = $output
    Report   = $report
  }
}

Write-Output "=== cleanup.selftest.ps1 ==="
Write-Output ("cleanup.ps1: " + $cleanupScript)
Write-Output ("fixture root: " + $root)
Write-Output ""

# --- (a)+(b)+(c を兼ねた確認): null でない元の値に復元する ----
Write-Output "-- case 1: restore to a real previous value, other keys untouched --"
$case1Dir = Join-Path $root 'case1'
$fx1 = New-Fixture -CaseDir $case1Dir -VencordDirValue 'C:\fake\plugins\cordmux\data\vencord' -WithBackup $true -PreviousValue 'C:\fake\previous\vencord'
$before1 = Read-JsonFile -Path $fx1.StateJsonPath
$result1 = Invoke-Cleanup -Fixture $fx1 -CallWindow
Assert-True ($result1.ExitCode -eq 0) ("case1 exit code is 0 (was " + $result1.ExitCode + ")")
$after1 = Read-JsonFile -Path $fx1.StateJsonPath
Assert-True ($after1.vencordDir -eq 'C:\fake\previous\vencord') "case1 (a) vencordDir reverted to the previous value"
Assert-True ($after1.discordBranch -eq $before1.discordBranch) "case1 (b) discordBranch untouched"
Assert-True ($after1.minimizeToTray -eq $before1.minimizeToTray) "case1 (b) minimizeToTray untouched"
Assert-True ($after1.windowBounds.width -eq $before1.windowBounds.width) "case1 (b) nested windowBounds.width untouched"
Assert-True (($after1.skipHosts -join ',') -eq ($before1.skipHosts -join ',')) "case1 (b) skipHosts array untouched"
$rawBytes1 = [System.IO.File]::ReadAllBytes($fx1.StateJsonPath)
$hasBom1 = ($rawBytes1.Length -ge 3 -and $rawBytes1[0] -eq 0xEF -and $rawBytes1[1] -eq 0xBB -and $rawBytes1[2] -eq 0xBF)
Assert-True (-not $hasBom1) "case1 state.json was written without a BOM"
Assert-True (-not (Test-Path -LiteralPath (Join-Path $fx1.WorkDir 'recording1.mkv'))) "case1 (f) recording1.mkv deleted"
Assert-True (-not (Test-Path -LiteralPath (Join-Path $fx1.WorkDir 'out\stream-test.flv'))) "case1 (f) stream-test.flv deleted"
Assert-True (-not (Test-Path -LiteralPath (Join-Path $fx1.WorkDir 'out\clip.wav'))) "case1 (f) clip.wav deleted"
Assert-True (Test-Path -LiteralPath (Join-Path $fx1.WorkDir 'out\result.json')) "case1 (f) result.json kept"
Assert-True (Test-Path -LiteralPath (Join-Path $fx1.WorkDir 'out\run.log')) "case1 (f) run.log kept"
Assert-True (Test-Path -LiteralPath (Join-Path $fx1.WorkDir 'out\frame1.png')) "case1 (f) frame1.png kept"
$backupAfter1 = Read-JsonFile -Path $fx1.BackupPath
Assert-True ($null -ne $backupAfter1.written_at) "case1 backup keeps the product spelling (written_at)"
Assert-True ($null -ne $backupAfter1.dll_dir) "case1 backup keeps the product spelling (dll_dir)"
$artifactsAfter1 = Read-JsonFile -Path $fx1.ArtifactsPath
Assert-True (-not (Test-Path -LiteralPath $artifactsAfter1.tempVencordBuild)) "case1 temp Vencord build deleted (from the artifacts file)"
Assert-True (-not (Test-Path -LiteralPath $artifactsAfter1.browserProfiles[0])) "case1 browser profile deleted (from the artifacts file)"
Assert-True (-not (Test-Path -LiteralPath $artifactsAfter1.sharedContentDirs[0])) "case1 shared content dir deleted (from the artifacts file)"

# --- (c): 元の値が null -> 鍵は null として書かず、消えていなければならない -
Write-Output ""
Write-Output "-- case 2: previous value is null, key must disappear --"
$case2Dir = Join-Path $root 'case2'
$fx2 = New-Fixture -CaseDir $case2Dir -VencordDirValue 'C:\fake\plugins\cordmux\data\vencord' -WithBackup $true -PreviousValue $null
$result2 = Invoke-Cleanup -Fixture $fx2 -CallWindow
Assert-True ($result2.ExitCode -eq 0) ("case2 exit code is 0 (was " + $result2.ExitCode + ")")
$after2Text = Get-Content -LiteralPath $fx2.StateJsonPath -Raw -Encoding UTF8
$after2 = $after2Text | ConvertFrom-Json
Assert-True (-not ($after2.PSObject.Properties['vencordDir'])) "case2 (c) vencordDir key removed entirely"
Assert-True ($after2Text -notmatch '"vencordDir"') "case2 (c) the literal key text is gone from the file"

# --- (d): バックアップファイルがない -> 何も書かず、終了コード 2 --------------------
Write-Output ""
Write-Output "-- case 3: no backup file --"
$case3Dir = Join-Path $root 'case3'
$fx3 = New-Fixture -CaseDir $case3Dir -VencordDirValue 'C:\fake\plugins\cordmux\data\vencord' -WithBackup $false -PreviousValue $null
$before3Text = Get-Content -LiteralPath $fx3.StateJsonPath -Raw -Encoding UTF8
$result3 = Invoke-Cleanup -Fixture $fx3 -CallWindow
Assert-True ($result3.ExitCode -eq 2) ("case3 (d) exit code is 2 (was " + $result3.ExitCode + ")")
$after3Text = Get-Content -LiteralPath $fx3.StateJsonPath -Raw -Encoding UTF8
Assert-True ($after3Text -eq $before3Text) "case3 (d) state.json byte-for-byte unchanged"
Assert-True (-not (Test-Path -LiteralPath $fx3.BackupPath)) "case3 (d) no backup file was created"

# --- (e): -DryRun は何も変えない ---------------------------------------------
Write-Output ""
Write-Output "-- case 4: -DryRun leaves everything as-is --"
$case4Dir = Join-Path $root 'case4'
$fx4 = New-Fixture -CaseDir $case4Dir -VencordDirValue 'C:\fake\plugins\cordmux\data\vencord' -WithBackup $true -PreviousValue 'C:\fake\previous\vencord'
$before4StateText = Get-Content -LiteralPath $fx4.StateJsonPath -Raw -Encoding UTF8
$before4BackupText = Get-Content -LiteralPath $fx4.BackupPath -Raw -Encoding UTF8
$before4Artifacts = Read-JsonFile -Path $fx4.ArtifactsPath
$result4 = Invoke-Cleanup -Fixture $fx4 -CallWindow -DryRun
Assert-True ($result4.ExitCode -eq 0) ("case4 (e) exit code is 0 (was " + $result4.ExitCode + ")")
$after4StateText = Get-Content -LiteralPath $fx4.StateJsonPath -Raw -Encoding UTF8
$after4BackupText = Get-Content -LiteralPath $fx4.BackupPath -Raw -Encoding UTF8
Assert-True ($after4StateText -eq $before4StateText) "case4 (e) state.json unchanged"
Assert-True ($after4BackupText -eq $before4BackupText) "case4 (e) backup file unchanged"
Assert-True (Test-Path -LiteralPath (Join-Path $fx4.WorkDir 'recording1.mkv')) "case4 (e) recording1.mkv NOT deleted"
Assert-True (Test-Path -LiteralPath (Join-Path $fx4.WorkDir 'out\stream-test.flv')) "case4 (e) stream-test.flv NOT deleted"
Assert-True (Test-Path -LiteralPath $before4Artifacts.tempVencordBuild) "case4 (e) temp Vencord build NOT deleted"
Assert-True (Test-Path -LiteralPath $before4Artifacts.browserProfiles[0]) "case4 (e) browser profile NOT deleted"
Assert-True (Test-Path -LiteralPath $before4Artifacts.sharedContentDirs[0]) "case4 (e) shared content dir NOT deleted"

# --- 追加: -CallWindow なしでも WorkDir の後始末は動き、vencordDir は変わらない
Write-Output ""
Write-Output "-- case 5: common cleanup without -CallWindow leaves vencordDir alone --"
$case5Dir = Join-Path $root 'case5'
$fx5 = New-Fixture -CaseDir $case5Dir -VencordDirValue 'C:\fake\plugins\cordmux\data\vencord' -WithBackup $true -PreviousValue 'C:\fake\previous\vencord'
$result5 = Invoke-Cleanup -Fixture $fx5
Assert-True ($result5.ExitCode -eq 0) ("case5 exit code is 0 (was " + $result5.ExitCode + ")")
$after5 = Read-JsonFile -Path $fx5.StateJsonPath
Assert-True ($after5.vencordDir -eq 'C:\fake\plugins\cordmux\data\vencord') "case5 vencordDir left at the written value (no -CallWindow)"
Assert-True (-not (Test-Path -LiteralPath (Join-Path $fx5.WorkDir 'recording1.mkv'))) "case5 recording1.mkv still deleted by the common steps"

# --- 追加: 想定外の plugin フォルダは問題として報告される ---------------
Write-Output ""
Write-Output "-- case 6: an unexpected cordmux-other plugin folder is flagged --"
$case6Dir = Join-Path $root 'case6'
$fx6 = New-Fixture -CaseDir $case6Dir -VencordDirValue 'C:\fake\plugins\cordmux\data\vencord' -WithBackup $true -PreviousValue 'C:\fake\previous\vencord'
New-Item -ItemType Directory -Path (Join-Path $fx6.ProgramData 'obs-studio\plugins\cordmux') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $fx6.ProgramData 'obs-studio\plugins\cordmux-other') -Force | Out-Null
$result6 = Invoke-Cleanup -Fixture $fx6 -WatchProgramData
$legacyPluginDir = Join-Path $fx6.ProgramData 'obs-studio\plugins\cordmux'
Assert-True ($result6.ExitCode -eq 1) ("case6 exit code is 1 (was " + $result6.ExitCode + ")")
Assert-True (($result6.Report.problems -join ' ') -match 'cordmux-other') "case6 problem list names cordmux-other"
Assert-True (-not ($result6.Report.problems -contains ("unexpected plugin folder: " + $legacyPluginDir))) "case6 the legacy 'cordmux' folder itself is not flagged"

# --- 監視は -WatchProgramData の後ろ (フラグがなければ読まない) ------
Write-Output ""
Write-Output "-- case 13: without -WatchProgramData the ProgramData plugins folder is not read --"
$case13Dir = Join-Path $root 'case13'
$fx13 = New-Fixture -CaseDir $case13Dir -VencordDirValue 'C:\fake\plugins\cordmux\data\vencord' -WithBackup $true -PreviousValue 'C:\fake\previous\vencord'
New-Item -ItemType Directory -Path (Join-Path $fx13.ProgramData 'obs-studio\plugins\cordmux-other') -Force | Out-Null
$result13 = Invoke-Cleanup -Fixture $fx13
Assert-True ($result13.ExitCode -eq 0) ("case13 exit code is 0 (was " + $result13.ExitCode + ")")
Assert-True (-not (($result13.Report.problems -join ' ') -match 'cordmux-other')) "case13 the leak folder is not reported without the flag"
Assert-True ((($result13.Output | Out-String) -match 'ProgramData plugin watch skipped')) "case13 the skip is written to the output"
Assert-True ($result13.Report.watchProgramData -eq $false) "case13 the report says the watch was off"

# --- 壊れたバックアップ -> 何も書かず、終了コード 2 ----------
Write-Output ""
Write-Output "-- case 7: a broken backup file is refused --"
$case7Dir = Join-Path $root 'case7'
$fx7 = New-Fixture -CaseDir $case7Dir -VencordDirValue 'C:\fake\plugins\cordmux\data\vencord' -WithBackup $true -PreviousValue 'C:\fake\previous\vencord' -BackupText '{"previous": "C:\fake\previous\vencord", '
$before7Text = Get-Content -LiteralPath $fx7.StateJsonPath -Raw -Encoding UTF8
$result7 = Invoke-Cleanup -Fixture $fx7 -CallWindow
$after7Text = Get-Content -LiteralPath $fx7.StateJsonPath -Raw -Encoding UTF8
Assert-True ($result7.ExitCode -eq 2) ("case7 exit code is 2 (was " + $result7.ExitCode + ")")
Assert-True ($after7Text -eq $before7Text) "case7 state.json byte-for-byte unchanged"
$artifacts7 = Read-JsonFile -Path $fx7.ArtifactsPath
Assert-True (Test-Path -LiteralPath $artifacts7.tempVencordBuild) "case7 temp Vencord build kept (restore was not confirmed)"

# --- キーの足りないバックアップ (古いキー名) -> 終了コード 2 --------
# 古いキー名 (original / writtenAt) をそのまま置いたら受け付けないことを確認する:
# 製品が書くのは previous / written / written_at / dll_dir である。
Write-Output ""
Write-Output "-- case 8: a backup with the old key names is refused --"
$case8Dir = Join-Path $root 'case8'
$oldStyleBackup = '{"original": "C:/fake/previous/vencord", "written": "C:/fake/plugins/cordmux/data/vencord", "writtenAt": "yesterday"}'
$fx8 = New-Fixture -CaseDir $case8Dir -VencordDirValue 'C:\fake\plugins\cordmux\data\vencord' -WithBackup $true -PreviousValue 'C:\fake\previous\vencord' -BackupText $oldStyleBackup
$before8Text = Get-Content -LiteralPath $fx8.StateJsonPath -Raw -Encoding UTF8
$result8 = Invoke-Cleanup -Fixture $fx8 -CallWindow
$after8Text = Get-Content -LiteralPath $fx8.StateJsonPath -Raw -Encoding UTF8
Assert-True ($result8.ExitCode -eq 2) ("case8 exit code is 2 (was " + $result8.ExitCode + ")")
Assert-True ($after8Text -eq $before8Text) "case8 state.json byte-for-byte unchanged"
Assert-True (($result8.Report.problems -join ' ') -match 'previous') "case8 the problem names the missing key"

# --- -RealProfile は利用者の 2 つのフォルダーに触らない -----
Write-Output ""
Write-Output "-- case 9: -RealProfile keeps cordmux-harness and the electron cache --"
$case9Dir = Join-Path $root 'case9'
$fx9 = New-Fixture -CaseDir $case9Dir -VencordDirValue 'C:\fake\plugins\cordmux\data\vencord' -WithBackup $true -PreviousValue 'C:\fake\previous\vencord'
$harnessDir = Join-Path $fx9.AppData 'cordmux-harness'
$electronDir = Join-Path $fx9.LocalAppData 'electron'
$sessionJson = Join-Path $fx9.LocalAppData 'cordmux\session.json'
New-Item -ItemType Directory -Path $harnessDir -Force | Out-Null
New-Item -ItemType Directory -Path $electronDir -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $fx9.LocalAppData 'cordmux') -Force | Out-Null
Set-Content -LiteralPath $sessionJson -Value '{"port":4470}' -Encoding Ascii
$result9 = Invoke-Cleanup -Fixture $fx9 -RealProfile
Assert-True ($result9.ExitCode -eq 0) ("case9 exit code is 0 (was " + $result9.ExitCode + ")")
Assert-True (Test-Path -LiteralPath $harnessDir) "case9 cordmux-harness kept (not created by a real-profile window)"
Assert-True (Test-Path -LiteralPath $electronDir) "case9 electron cache kept (not created by a real-profile window)"
Assert-True (-not (Test-Path -LiteralPath $sessionJson)) "case9 session.json still deleted (common cleanup, 4.4 (4))"

# --- 追加: -RealProfile なしなら既定どおり 2 つとも消える ------------------------
Write-Output ""
Write-Output "-- case 10: without -RealProfile the two folders are deleted as before --"
$case10Dir = Join-Path $root 'case10'
$fx10 = New-Fixture -CaseDir $case10Dir -VencordDirValue 'C:\fake\plugins\cordmux\data\vencord' -WithBackup $true -PreviousValue 'C:\fake\previous\vencord'
$harnessDir10 = Join-Path $fx10.AppData 'cordmux-harness'
$electronDir10 = Join-Path $fx10.LocalAppData 'electron'
New-Item -ItemType Directory -Path $harnessDir10 -Force | Out-Null
New-Item -ItemType Directory -Path $electronDir10 -Force | Out-Null
$result10 = Invoke-Cleanup -Fixture $fx10
Assert-True ($result10.ExitCode -eq 0) ("case10 exit code is 0 (was " + $result10.ExitCode + ")")
Assert-True (-not (Test-Path -LiteralPath $harnessDir10)) "case10 cordmux-harness deleted"
Assert-True (-not (Test-Path -LiteralPath $electronDir10)) "case10 electron cache deleted"

# --- 録画ログと録画ファイルの作成の一時ファイルの削除と、-KeepRecordings ---
Write-Output ""
Write-Output "-- case 11: *.jsonl and *.pcm are deleted, recordings go too (no -KeepRecordings) --"
$case11Dir = Join-Path $root 'case11'
$fx11 = New-Fixture -CaseDir $case11Dir -VencordDirValue 'C:\fake\plugins\cordmux\data\vencord' -WithBackup $true -PreviousValue 'C:\fake\previous\vencord'
$sessionDir11 = Join-Path $fx11.WorkDir 'out\recordings\20260101-000000'
$result11 = Invoke-Cleanup -Fixture $fx11
Assert-True ($result11.ExitCode -eq 0) ("case11 exit code is 0 (was " + $result11.ExitCode + ")")
Assert-True (-not (Test-Path -LiteralPath (Join-Path $sessionDir11 'ledger.jsonl'))) "case11 ledger.jsonl deleted"
Assert-True (-not (Test-Path -LiteralPath (Join-Path $sessionDir11 'taro_screen.mov'))) "case11 finalized mov deleted"
Assert-True (-not (Test-Path -LiteralPath (Join-Path $sessionDir11 'segments\u1_screen_video_000001.mov'))) "case11 segment mov deleted"
Assert-True (-not (Test-Path -LiteralPath (Join-Path $sessionDir11 'black\u1_enc1.mov'))) "case11 black mov deleted"
Assert-True (-not (Test-Path -LiteralPath (Join-Path $sessionDir11 'silence.pcm'))) "case11 finalize pcm deleted"
Assert-True (-not (Test-Path -LiteralPath (Join-Path $fx11.WorkDir 'out\scratch.pcm'))) "case11 stray pcm deleted"
Assert-True (-not (Test-Path -LiteralPath (Join-Path $fx11.WorkDir 'out\stray.jsonl'))) "case11 stray jsonl deleted"
Assert-True (Test-Path -LiteralPath (Join-Path $fx11.WorkDir 'out\result.json')) "case11 result.json still kept"

Write-Output ""
Write-Output "-- case 12: -KeepRecordings keeps only what is under a folder named recordings --"
$case12Dir = Join-Path $root 'case12'
$fx12 = New-Fixture -CaseDir $case12Dir -VencordDirValue 'C:\fake\plugins\cordmux\data\vencord' -WithBackup $true -PreviousValue 'C:\fake\previous\vencord'
$sessionDir12 = Join-Path $fx12.WorkDir 'out\recordings\20260101-000000'
$result12 = Invoke-Cleanup -Fixture $fx12 -KeepRecordings
Assert-True ($result12.ExitCode -eq 0) ("case12 exit code is 0 (was " + $result12.ExitCode + ")")
Assert-True (Test-Path -LiteralPath (Join-Path $sessionDir12 'ledger.jsonl')) "case12 ledger.jsonl kept"
Assert-True (Test-Path -LiteralPath (Join-Path $sessionDir12 'taro_screen.mov')) "case12 finalized mov kept"
Assert-True (Test-Path -LiteralPath (Join-Path $sessionDir12 'segments\u1_screen_video_000001.mov')) "case12 segment mov kept"
Assert-True (Test-Path -LiteralPath (Join-Path $sessionDir12 'black\u1_enc1.mov')) "case12 black mov kept"
Assert-True (Test-Path -LiteralPath (Join-Path $sessionDir12 'silence.pcm')) "case12 finalize pcm kept"
Assert-True (-not (Test-Path -LiteralPath (Join-Path $fx12.WorkDir 'out\scratch.pcm'))) "case12 stray pcm outside recordings still deleted"
Assert-True (-not (Test-Path -LiteralPath (Join-Path $fx12.WorkDir 'out\stray.jsonl'))) "case12 stray jsonl outside recordings still deleted"
Assert-True (-not (Test-Path -LiteralPath (Join-Path $fx12.WorkDir 'recording1.mkv'))) "case12 mkv outside recordings still deleted"

# --- 親のいない配信の受け口 ----------------------------------
# 判定そのもの (場所・コマンド行・親) は stream-sink-stale.ps1 -SelfTest が偽のプロセスの組と実際のプロセスで確認する。ここでは cleanup.ps1 が
# それを -Stop なしで呼び、-DryRun でも素の呼び出しでも止めずに問題として書くことと、手で実行する -Stop が止めることを
# 実際の ffmpeg で確認する。テスト専用の URL (1935 番は使わない) で、親を先に終了させた ffmpeg -listen を 1 つ起動する
# (同梱の ffmpeg がない PC (CI) では飛ばす)。
Write-Output ""
Write-Output "-- case 14: the stale stream receiver check runs and reports --"
$case14Dir = Join-Path $root 'case14'
$fx14 = New-Fixture -CaseDir $case14Dir -VencordDirValue 'C:\fake\plugins\cordmux\data\vencord' -WithBackup $true -PreviousValue 'C:\fake\previous\vencord'
$result14 = Invoke-Cleanup -Fixture $fx14 -DryRun
Assert-True ($null -ne $result14.Report -and $null -ne $result14.Report.streamSinkCheck -and $result14.Report.streamSinkCheck.refused -eq $false) "case14 the JSON report carries the stream receiver check"
Assert-True ($null -ne $result14.Report -and $result14.Report.streamSinkCheck.url -eq $SelftestStreamSinkUrl -and $result14.Report.streamSinkCheck.stop -eq $false) "case14 -DryRun looks at the test-only URL and does not stop"
$result14b = Invoke-Cleanup -Fixture $fx14
Assert-True ($null -ne $result14b.Report -and $result14b.Report.streamSinkCheck.stop -eq $false -and $result14b.ExitCode -eq 0) ("case14 the plain run does not pass -Stop either (it only reports; exit " + $result14b.ExitCode + ")")

Write-Output ""
Write-Output "-- case 15: a refused stream receiver URL is a problem (exit 1), nothing is stopped --"
$case15Dir = Join-Path $root 'case15'
$fx15 = New-Fixture -CaseDir $case15Dir -VencordDirValue 'C:\fake\plugins\cordmux\data\vencord' -WithBackup $true -PreviousValue 'C:\fake\previous\vencord'
$result15 = Invoke-Cleanup -Fixture $fx15 -StreamSinkUrl 'rtmp://0.0.0.0:19354/live/cordmux-cleanup-selftest-none'
Assert-True ($result15.ExitCode -eq 1) ("case15 exit code is 1 (was " + $result15.ExitCode + ")")
Assert-True ((@($result15.Report.problems) | Where-Object { $_ -like 'could not check for a stale stream receiver:*' }).Count -eq 1) "case15 the refusal is listed as a problem"

Write-Output ""
Write-Output "-- case 16: a real receiver without a parent (test URL): -DryRun and the plain run report it and keep it, -Stop by hand stops it --"
$bundled16 = $null
try {
  $spec16 = Get-Content -LiteralPath (Join-Path $here '..\..\..\src-obs\buildspec.json') -Raw -Encoding UTF8 | ConvertFrom-Json
  $bundled16 = [IO.Path]::GetFullPath((Join-Path $here ('..\..\..\src-obs\.deps\obs-studio-' + $spec16.dependencies.'obs-studio'.version + '\.deps\obs-deps-' + $spec16.dependencies.prebuilt.version + '-x64\bin\ffmpeg.exe')))
} catch { }
if (-not $bundled16 -or -not (Test-Path -LiteralPath $bundled16 -PathType Leaf)) {
  Write-Output ("SKIP: case16 needs the bundled ffmpeg (" + $bundled16 + ")")
} else {
  $url16 = 'rtmp://127.0.0.1:19353/live/cordmux-cleanup-selftest'
  $orphan16 = [int64]0
  # 起動した ffmpeg のハンドルを直後に保持する (finally はこのハンドルで止める。pid では止めない。使い回された pid を止めないためである)。
  $held16 = $null
  try {
    # 間の powershell が ffmpeg を起こして pid を書き、すぐ終わる (ffmpeg の親が居ない = 閉じた殻から手で起こした形)。
    $argText16 = '-hide_banner -loglevel quiet -listen 1 -i ' + $url16 + ' -f null -'
    $inner16 = "`$p = Start-Process -FilePath '" + $bundled16.Replace("'", "''") + "' -ArgumentList '" + $argText16 + "' -WindowStyle Hidden -PassThru; `$p.Id"
    $pidText16 = & (Join-Path $PSHOME 'powershell.exe') -NoProfile -NonInteractive -Command $inner16
    [void][int64]::TryParse(([string]($pidText16 | Select-Object -Last 1)).Trim(), [ref]$orphan16)
    if ($orphan16 -gt 0) {
      $gp16 = Get-Process -Id $orphan16 -ErrorAction SilentlyContinue
      if ($null -ne $gp16) {
        [void]$gp16.Handle
        $path16 = $null
        try { $path16 = $gp16.Path } catch { }
        if ($path16 -and ([IO.Path]::GetFullPath($path16) -ieq $bundled16)) { $held16 = $gp16 }
      }
    }
    Start-Sleep -Milliseconds 800
    Assert-True ($null -ne $held16 -and -not $held16.HasExited) ("case16 the receiver without a parent is running (pid " + $orphan16 + ")")
    $case16Dir = Join-Path $root 'case16'
    $fx16 = New-Fixture -CaseDir $case16Dir -VencordDirValue 'C:\fake\plugins\cordmux\data\vencord' -WithBackup $true -PreviousValue 'C:\fake\previous\vencord'
    $staleLine16 = 'stale stream receiver: pid ' + $orphan16 + ' (its parent process '
    $dry16 = Invoke-Cleanup -Fixture $fx16 -DryRun -StreamSinkUrl $url16
    Assert-True ($dry16.ExitCode -eq 1 -and (@($dry16.Report.problems) | Where-Object { $_ -like ($staleLine16 + '*is still running; cleanup.ps1 does not stop it*') }).Count -eq 1) ("case16 -DryRun reports it as a problem (exit " + $dry16.ExitCode + ")")
    Assert-True ((@($dry16.Report.actions) | Where-Object { $_ -like '*stream receiver*' }).Count -eq 0) "case16 -DryRun does not write it as an action (nothing would be stopped)"
    Assert-True (-not $held16.HasExited) "case16 -DryRun does not stop it"
    $run16 = Invoke-Cleanup -Fixture $fx16 -StreamSinkUrl $url16
    Assert-True ($run16.ExitCode -eq 1 -and (@($run16.Report.problems) | Where-Object { $_ -like ($staleLine16 + '*') }).Count -eq 1) ("case16 the plain run reports it as a problem too (exit " + $run16.ExitCode + ")")
    $held16.Refresh()
    Assert-True (-not $held16.HasExited) "case16 the plain run does not stop it (cleanup.ps1 stops nothing outside the work folder)"
    # テストを実行する人がメッセージを読んで手で打つ形: stream-sink-stale.ps1 -Stop。
    $stopOut16 = & (Join-Path $PSHOME 'powershell.exe') -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $here 'stream-sink-stale.ps1') -Stop -StreamSinkUrl $url16
    $stopExit16 = $LASTEXITCODE
    $stop16 = ([string](@($stopOut16) | Where-Object { $_ -match '^\s*\{' } | Select-Object -Last 1)) | ConvertFrom-Json
    $held16.Refresh()
    Assert-True ($stopExit16 -eq 0 -and (@($stop16.stale | Where-Object { $_.pid -eq $orphan16 -and $_.outcome -eq 'stopped' })).Count -eq 1 -and $held16.HasExited) ("case16 stream-sink-stale.ps1 -Stop by hand stops it (exit " + $stopExit16 + ")")
  } finally {
    # 自分で起こした物だけ、起こした直後に開いた手で止める。
    if ($null -ne $held16) {
      try { $held16.Refresh(); if (-not $held16.HasExited) { $held16.Kill(); [void]$held16.WaitForExit(3000) } } catch { }
    }
  }
}

# --- 既定のパス: -WorkDir などを渡さずに powershell -File で起動しても、作業用のフォルダーを決められる -----------
# 実行スクリプトは -WorkDir を渡さない。param の既定の式で $PSScriptRoot を使うと空になり、Join-Path の誤りで何もせずに
# 終わっていた。-DryRun なので、作業用のフォルダーを読むだけで消さない。環境変数は ほかの場合と同じく差し替える。
Write-Output ""
Write-Output "-- default paths: no -WorkDir, -ObsDir, -BackupPath, -ArtifactsPath --"
$caseDefaultDir = Join-Path $root 'case-default-paths'
$fxDefault = New-Fixture -CaseDir $caseDefaultDir -VencordDirValue 'C:\fake\plugins\cordmux\data\vencord' -WithBackup $false -PreviousValue $null
$origAppDataDefault = $env:APPDATA
$origLocalAppDataDefault = $env:LOCALAPPDATA
$origProgramDataDefault = $env:ProgramData
$origVencordDirDefault = $env:VENCORD_USER_DATA_DIR
try {
  $env:VENCORD_USER_DATA_DIR = $null
  $env:APPDATA = $fxDefault.AppData
  $env:LOCALAPPDATA = $fxDefault.LocalAppData
  $env:ProgramData = $fxDefault.ProgramData
  $defaultOutput = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $cleanupScript -DryRun -Json -StreamSinkUrl $SelftestStreamSinkUrl 2>&1
  $defaultExit = $LASTEXITCODE
} finally {
  $env:APPDATA = $origAppDataDefault
  $env:LOCALAPPDATA = $origLocalAppDataDefault
  $env:ProgramData = $origProgramDataDefault
  $env:VENCORD_USER_DATA_DIR = $origVencordDirDefault
}
[void]$script:streamSinkUrlsSeen.Add($SelftestStreamSinkUrl)
$defaultJson = $defaultOutput | Where-Object { $_ -like 'CORDMUX_CLEANUP_JSON:*' } | Select-Object -Last 1
Assert-True ($null -ne $defaultJson) ("default paths: cleanup.ps1 prints its JSON line (exit code " + $defaultExit + ")")
if ($defaultJson) {
  $defaultReport = ($defaultJson -replace '^CORDMUX_CLEANUP_JSON:', '') | ConvertFrom-Json
  $expectedWork = [System.IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $cleanupScript) '..\..\..\_work'))
  Assert-True ([System.IO.Path]::GetFullPath([string]$defaultReport.workDir) -eq $expectedWork) ("default paths: workDir is the _work folder at the repository root (" + $defaultReport.workDir + ")")
}

# --- どの呼び出しもテスト専用の受け口の URL だけを見た ------------------------
Write-Output ""
Write-Output "-- the stream receiver URLs the self-test passed and saw --"
$seen1935 = @($script:streamSinkUrlsSeen | Where-Object { $_ -match ':1935/' })
Assert-True ($script:streamSinkUrlsSeen.Count -gt 0 -and $seen1935.Count -eq 0) ("every cleanup.ps1 call passed and looked at a test-only receiver URL, never port 1935 (" + $script:streamSinkUrlsSeen.Count + " seen, " + $seen1935.Count + " on 1935)")

Write-Output ""
Write-Output ("=== " + $passCount + " passed, " + $failCount + " failed ===")
if ($failCount -eq 0) {
  if ($ownsRoot -and (Test-Path -LiteralPath $root)) { Remove-Item -LiteralPath $root -Recurse -Force }
  Write-Output "SELFTEST: PASS"
  exit 0
} else {
  Write-Output ("work folder kept for inspection: " + $root)
  Write-Output "SELFTEST: FAIL"
  exit 1
}
