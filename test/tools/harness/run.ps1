# ハーネス起動スクリプト。
# nvidia-smi の情報を環境変数 (HARNESS_NVIDIA_SMI) に入れてから Electron を起動し、終了を待つ。
# 実行例: powershell -NoProfile -ExecutionPolicy Bypass -File run.ps1

$ErrorActionPreference = 'Stop'
Set-Location -Path $PSScriptRoot

$gpuInfo = & nvidia-smi --query-gpu=name,driver_version,utilization.encoder --format=csv,noheader
$env:HARNESS_NVIDIA_SMI = ($gpuInfo -join ' | ')

Write-Host "GPU: $($env:HARNESS_NVIDIA_SMI)"
Write-Host 'Electron を起動します...'

npx electron .
$exitCode = $LASTEXITCODE

Write-Host "Electron 終了 (exit code: $exitCode)"

$outDir = Join-Path $PSScriptRoot '..\..\..\_work\out'
$latest = Get-ChildItem -Path $outDir -Filter 'harness-*.json' -ErrorAction SilentlyContinue |
  Sort-Object LastWriteTime -Descending |
  Select-Object -First 1

if ($latest) {
  Write-Host "結果 JSON: $($latest.FullName)"
} else {
  Write-Host '結果 JSON が見つかりませんでした。'
}
