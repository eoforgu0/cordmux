#requires -Version 5.1
<#
  launch-chrome-fake-mic.ps1 - Chrome を「マイクの代わりに音のファイルを読む」指定つきで起こすツール。

  ブラウザ (Chromium 系) の起動時の指定で、音のファイルをマイク入力の代わりに読ませる。PC への設定変更にならない。
  同じ指定を Electron (Chromium 150) に渡した経路で 1 kHz の比 1.0003 を得ている (test/tools/mic-check)。

  やること: chrome.exe が 1 つも動いていないことを確認してから、既定のプロファイルのまま
  (プロファイルの指定はしない = 設定は変えない) 次の 2 つの指定で Chrome を起こす。
    --use-fake-device-for-media-stream
    --use-file-for-fake-audio-capture=<WAV>
  Chrome は起動時の指定を最初のプロセスでしか読まない。既に Chrome が動いていると、新しく起こした
  ものはその Chrome のウィンドウになり、指定は捨てられる。だから動いていれば起こさずに ok:false を返す
  (ウィンドウを全部閉じても通知領域に残る「バックグラウンドで動かす」設定の Chrome も同じ)。

  結果は標準出力に 1 行の JSON (ok / pid / chromePath / wav / args / error)。終了コードは ok:true なら 0、
  それ以外は 1。標準出力は UTF-8 (BOM なし) のバイト列を直接書く (screenshot.ps1 と同じ理由)。
  **このファイル自体は BOM 付き UTF-8 で保存する** (cleanup.ps1 と同じ理由。BOM を
  外すと PowerShell 5.1 が既定コードページで読んで日本語のコメントの所で構文が壊れる)。

  使い方:
    powershell -ExecutionPolicy Bypass -File launch-chrome-fake-mic.ps1 -Wav <PCM WAV> [-Url <開く URL>]
        [-ChromePath <chrome.exe>] [-DryRun]
  -DryRun は chrome.exe の場所・WAV・動いているかの確認だけを行い、起こさずに JSON を出す
  (起こした場合の引数の並びを args に入れる)。
#>
param(
  [Parameter(Mandatory = $true)]
  [string]$Wav,
  [string]$Url = '',
  [string]$ChromePath = '',
  [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

function Write-JsonResultAndExit {
  param($Result, [int]$ExitCode)
  $json = $Result | ConvertTo-Json -Depth 5 -Compress
  $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
  $bytes = $utf8NoBom.GetBytes($json)
  $stdout = [System.Console]::OpenStandardOutput()
  $stdout.Write($bytes, 0, $bytes.Length)
  $stdout.Flush()
  exit $ExitCode
}

$result = [ordered]@{
  ok = $false
  dryRun = [bool]$DryRun
  pid = $null
  chromePath = $null
  wav = $null
  url = $Url
  args = @()
  runningChromePids = @()
  error = $null
}

try {
  # 1. chrome.exe の場所 (既定の 2 か所。無ければ -ChromePath)
  $candidates = @()
  if ($ChromePath -ne '') {
    $candidates += $ChromePath
  } else {
    $candidates += (Join-Path ${env:ProgramFiles} 'Google\Chrome\Application\chrome.exe')
    if (${env:ProgramFiles(x86)}) {
      $candidates += (Join-Path ${env:ProgramFiles(x86)} 'Google\Chrome\Application\chrome.exe')
    }
  }
  $chrome = $null
  foreach ($c in $candidates) {
    if (Test-Path -LiteralPath $c -PathType Leaf) { $chrome = (Resolve-Path -LiteralPath $c).Path; break }
  }
  if ($null -eq $chrome) {
    $result.error = 'chrome.exe が見つかりません。-ChromePath で場所を渡してください: ' + ($candidates -join ' / ')
    Write-JsonResultAndExit $result 1
  }
  $result.chromePath = $chrome

  # 2. WAV (Chrome が読める PCM の WAV であることは呼ぶ側が ffprobe で確認する。ここでは在るかだけ)
  if (-not (Test-Path -LiteralPath $Wav -PathType Leaf)) {
    $result.error = '音のファイルが見つかりません: ' + $Wav
    Write-JsonResultAndExit $result 1
  }
  $wavFull = (Resolve-Path -LiteralPath $Wav).Path
  $result.wav = $wavFull

  # 3. 動いている chrome.exe (1 つでもあれば起こさない)
  $running = @(Get-Process -Name 'chrome' -ErrorAction SilentlyContinue)
  $result.runningChromePids = @($running | ForEach-Object { $_.Id })
  if ($running.Count -gt 0) {
    $result.error = 'Chrome がまだ動いています (' + $running.Count + ' 個)。Chrome のウィンドウを全部閉じ、通知領域に Chrome のアイコンが残っていれば右クリックして終了してから、もう一度実行してください。'
    Write-JsonResultAndExit $result 1
  }

  # 4. 起こす (プロファイルは指定しない = 既定のまま)
  # 空白を含む場所は引数ごと引用符で囲む (Start-Process は配列を空白で繋ぐだけで、囲まない)。
  $wavArg = '--use-file-for-fake-audio-capture=' + $wavFull
  if ($wavArg -match '\s') { $wavArg = '"' + $wavArg + '"' }
  $argList = @('--use-fake-device-for-media-stream', $wavArg)
  if ($Url -ne '') { $argList += $Url }
  $result.args = $argList
  if ($DryRun) {
    $result.ok = $true
    Write-JsonResultAndExit $result 0
  }
  $proc = Start-Process -FilePath $chrome -ArgumentList $argList -PassThru
  $result.pid = $proc.Id
  $result.ok = $true
  Write-JsonResultAndExit $result 0
} catch {
  $result.error = $_.Exception.Message
  Write-JsonResultAndExit $result 1
}
