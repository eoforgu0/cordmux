#requires -Version 5.1
<#
  harness-suspend.ps1 - テスト用の送信側 (test/tools/harness の electron.exe) の自分のプロセスを数秒止めて戻すツール。

  ハートビートが切れる経路 (レンダラが凍った・スリープから戻った場合。TCP は切れないまま返事が来ない) を測る。製品 (cordmux.dll) は 5 秒ごとに ping を送り、
  返事のない ping が 2 つたまった次の刻みで切れたものとして扱い (src-obs/ws/listener.h の ping_interval_ms 5000・
  max_missed_pongs 2、listener.cpp の on_ping_tick)、2 秒たったら枠を空にする。pong を返すのは送信側のレンダラの
  JavaScript (sender.ts) なので、レンダラを止めると TCP を開いたまま返事だけが止まる。利用者の Vesktop は止めない。
  Vesktop を閉じる操作は接続が閉じる経路で、この経路の代わりにならない。

  止める長さの既定は 20 秒: 止め始めから切れるまで 10 秒 (刻みの直前に止めた) から 15 秒 (刻みの直後に返事をした直後に
  止めた)、その後 2 秒で枠が空く。17 秒より長く止めないと、刻みの位置によっては切れずに戻る (余裕は 3 秒)。

  -Part で止める物を選ぶ:
    renderer  (既定) 送信側のレンダラ (コマンド行に --type=renderer) だけ。レンダラが凍った場合と同じ形。
              ネットワークの部分 (utility) と主プロセスは動いたまま。
    all       送信側のプロセスの木 (主プロセスとその子の全部)。アプリ全体が止まった形 (スリープから戻った形に近い)。

  止めてよいのは送信側の自分のプロセスだけである。-Pid の木 (Win32_Process の ParentProcessId をたどる) のうち、
  実行ファイルの場所が test/tools/harness の下の物だけを対象にし、木の根 (-Pid) がそうでなければ何も止めずに断る
  (refused)。利用者の Vesktop・Discord・OBS・ブラウザは場所が違うので、pid を取り違えても止まらない。

  戻し方: 止めた後は try / finally で必ず戻す (Ctrl+C でも finally は走る)。このツールのプロセスが外から殺されると
  止まったまま残るので、そのときは -Resume で戻すか、実行スクリプトの harness-stop (taskkill /T /F) で木ごと閉じる。

  結果は 1 行の JSON を標準出力に書く (screenshot.ps1 と同じ UTF-8)。-Out を渡せば同じ JSON をそのファイルにも書く。
    { "ok": true|false, "pid", "part", "seconds", "targets": [ { "pid", "type", "path" } ],
      "suspendedAt", "suspendedAtMs", "resumedAt", "resumedAtMs", "heldMs", "refused": [...], "error" }
  時刻は ISO 8601 (UTC) と Unix のミリ秒。notice_count.py の faults.tsv の at にコピーする。

  ほかの使い方:
    -List      木と、止めてよいかの見分けを出すだけ (止めない)。
    -Resume    木の対象を戻すだけ (止まったまま残ったとき)。
    -SelfTest  見えない子のプロセス (自分で起こす powershell.exe。ウィンドウを作らない) を止めて戻し、止まっている間に動かない
               こと、場所の外の pid を断ることを確認する。作業の保存先は呼び出し元の TEMP の下に作って消す
               (_work の下は実機の実行の保存先なので、そこに書かない)。
#>

[CmdletBinding()]
param(
  # screenshot.ps1 と同じ理由で、実体は $TargetPid、呼び出し側の名前は -Pid。hold-<tag>.json の harnessPid を渡す。
  [Alias('Pid')]
  [int]$TargetPid = 0,

  [ValidateSet('renderer', 'all')]
  [string]$Part = 'renderer',

  # 止める長さ (秒)。既定の 20 秒の理由は冒頭のコメント。
  [ValidateRange(1, 120)]
  [int]$Seconds = 20,

  [string]$Out = '',

  [switch]$List,
  [switch]$Resume,
  [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'

function Write-JsonResultAndExit {
  # screenshot.ps1 と同じ理由: 標準出力へ UTF-8 (BOM なし) のバイト列を直接書く。
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

# ntdll の NtSuspendProcess / NtResumeProcess の薄い包み。screenshot.ps1 と同じ理由でヒアストリングの中だけ英語のコメント。
$nativeSource = @"
using System;
using System.Runtime.InteropServices;

public static class CordmuxHarnessSuspendNative
{
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr OpenProcess(uint dwDesiredAccess, bool bInheritHandle, uint dwProcessId);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr hObject);

    [DllImport("ntdll.dll")]
    public static extern int NtSuspendProcess(IntPtr processHandle);

    [DllImport("ntdll.dll")]
    public static extern int NtResumeProcess(IntPtr processHandle);

    public const uint PROCESS_SUSPEND_RESUME = 0x0800;

    // Returns the NTSTATUS (0 = success), or -1 when the process cannot be opened.
    public static int Suspend(uint pid)
    {
        IntPtr h = OpenProcess(PROCESS_SUSPEND_RESUME, false, pid);
        if (h == IntPtr.Zero) return -1;
        try { return NtSuspendProcess(h); } finally { CloseHandle(h); }
    }

    public static int Resume(uint pid)
    {
        IntPtr h = OpenProcess(PROCESS_SUSPEND_RESUME, false, pid);
        if (h == IntPtr.Zero) return -1;
        try { return NtResumeProcess(h); } finally { CloseHandle(h); }
    }
}
"@
Add-Type -TypeDefinition $nativeSource

# 送信側の保存先 (このツールの 1 つ上の harness)。止めてよいのは実行ファイルがこの下にある物だけ。
$HarnessRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\harness'))

function Test-UnderRoot {
  param([string]$Path, [string]$Root)
  if (-not $Path -or -not $Root) { return $false }
  $full = [System.IO.Path]::GetFullPath($Path)
  $r = $Root.TrimEnd('\') + '\'
  return $full.StartsWith($r, [System.StringComparison]::OrdinalIgnoreCase)
}

# -Pid の木 (自分と子孫)。Win32_Process を 1 回だけ読み、ParentProcessId をたどる。
function Get-ProcessTree {
  param([int]$RootPid)
  $all = @(Get-CimInstance Win32_Process -Property ProcessId, ParentProcessId, ExecutablePath, CommandLine, Name)
  $byParent = @{}
  foreach ($p in $all) {
    $key = [int]$p.ParentProcessId
    if (-not $byParent.ContainsKey($key)) { $byParent[$key] = New-Object System.Collections.ArrayList }
    [void]$byParent[$key].Add($p)
  }
  $root = $all | Where-Object { [int]$_.ProcessId -eq $RootPid } | Select-Object -First 1
  if (-not $root) { return @() }
  $tree = New-Object System.Collections.ArrayList
  $queue = New-Object System.Collections.Queue
  $queue.Enqueue($root)
  $seen = @{}
  while ($queue.Count -gt 0) {
    $p = $queue.Dequeue()
    if ($seen.ContainsKey([int]$p.ProcessId)) { continue }
    $seen[[int]$p.ProcessId] = $true
    [void]$tree.Add($p)
    # 自分自身を親に持つ項目 (pid の使い回し) で回り続けないよう、見た pid は飛ばす。
    if ($byParent.ContainsKey([int]$p.ProcessId)) {
      foreach ($c in $byParent[[int]$p.ProcessId]) {
        if ([int]$c.ProcessId -ne [int]$p.ProcessId) { $queue.Enqueue($c) }
      }
    }
  }
  return , $tree.ToArray()
}

function Get-ProcessKind {
  param($Proc, [int]$RootPid)
  if ([int]$Proc.ProcessId -eq $RootPid) { return 'main' }
  $cmd = [string]$Proc.CommandLine
  $m = [regex]::Match($cmd, '--type=([A-Za-z0-9_-]+)')
  if ($m.Success) { return $m.Groups[1].Value }
  return 'other'
}

# 止める対象を決める。根が保存先の外なら何も返さず refused に理由を書く。木の中の保存先の外の物は対象から外して refused に出す。
function Resolve-Targets {
  param([int]$RootPid, [string]$WhichPart, [string]$Root)
  $tree = Get-ProcessTree -RootPid $RootPid
  $refused = New-Object System.Collections.ArrayList
  $targets = New-Object System.Collections.ArrayList
  $listed = New-Object System.Collections.ArrayList
  if ($tree.Count -eq 0) {
    return [PSCustomObject]@{ targets = @(); refused = @(); listed = @(); error = "process not found for pid $RootPid" }
  }
  foreach ($p in $tree) {
    $kind = Get-ProcessKind -Proc $p -RootPid $RootPid
    $under = Test-UnderRoot -Path ([string]$p.ExecutablePath) -Root $Root
    [void]$listed.Add([PSCustomObject]@{ pid = [int]$p.ProcessId; type = $kind; name = [string]$p.Name; path = [string]$p.ExecutablePath; allowed = $under })
  }
  $rootEntry = $listed | Where-Object { $_.pid -eq $RootPid } | Select-Object -First 1
  if (-not $rootEntry.allowed) {
    [void]$refused.Add([PSCustomObject]@{ pid = $RootPid; path = $rootEntry.path; reason = "the root is not under $Root (only the test sender may be suspended)" })
    return [PSCustomObject]@{ targets = @(); refused = $refused.ToArray(); listed = $listed.ToArray(); error = 'refused' }
  }
  foreach ($e in $listed) {
    if ($WhichPart -eq 'renderer' -and $e.type -ne 'renderer') { continue }
    if (-not $e.allowed) {
      [void]$refused.Add([PSCustomObject]@{ pid = $e.pid; path = $e.path; reason = "not under $Root" })
      continue
    }
    [void]$targets.Add([PSCustomObject]@{ pid = $e.pid; type = $e.type; path = $e.path })
  }
  $err = $null
  if ($targets.Count -eq 0) { $err = "no $WhichPart process under pid $RootPid" }
  return [PSCustomObject]@{ targets = $targets.ToArray(); refused = $refused.ToArray(); listed = $listed.ToArray(); error = $err }
}

function Get-NowMs { return [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() }

# 止めて、HoldMs だけ待って、必ず戻す。戻した結果も返す。
function Invoke-SuspendCycle {
  param($Targets, [int]$HoldMs)
  $suspended = New-Object System.Collections.ArrayList
  $suspendErrors = New-Object System.Collections.ArrayList
  $resumeErrors = New-Object System.Collections.ArrayList
  $t0 = $null
  $t1 = $null
  try {
    $t0 = Get-NowMs
    foreach ($t in $Targets) {
      $st = [CordmuxHarnessSuspendNative]::Suspend([uint32]$t.pid)
      if ($st -eq 0) { [void]$suspended.Add($t) } else { [void]$suspendErrors.Add([PSCustomObject]@{ pid = $t.pid; status = $st }) }
    }
    # 1 つも止められなければ待たない。
    if ($suspended.Count -gt 0) {
      $until = $t0 + $HoldMs
      while ((Get-NowMs) -lt $until) { Start-Sleep -Milliseconds 100 }
    }
  }
  finally {
    foreach ($t in $suspended) {
      $st = [CordmuxHarnessSuspendNative]::Resume([uint32]$t.pid)
      if ($st -ne 0) { [void]$resumeErrors.Add([PSCustomObject]@{ pid = $t.pid; status = $st }) }
    }
    $t1 = Get-NowMs
  }
  return [PSCustomObject]@{
    suspendedAtMs = $t0
    resumedAtMs   = $t1
    suspended     = $suspended.ToArray()
    suspendErrors = $suspendErrors.ToArray()
    resumeErrors  = $resumeErrors.ToArray()
  }
}

function Format-Iso {
  param($Ms)
  if ($null -eq $Ms) { return $null }
  return [DateTimeOffset]::FromUnixTimeMilliseconds([int64]$Ms).ToString('yyyy-MM-ddTHH:mm:ss.fffZ')
}

# ---- セルフテスト --------------------------------------------------------------------------------
# セルフテストの保存先が守りに合うか。合えば $null、合わなければ理由。保存先は呼び出し元の TEMP
# ([System.IO.Path]::GetTempPath() = 環境変数 TEMP / TMP) の下で、repo の _work の下ではないこと。_work は実機の実行の保存先
# (実行スクリプトが中身を保存し、後片付けが実行ごとに消す場所) で、セルフテストがそこに書くと実行の前後の比較に混じる。
# build_x64 の決まった場所は、呼び出し元が TEMP で決められず、CI とローカルで保存先が違うので採らない。
function Get-SelfTestPlaceProblem {
  param([string]$Path, [string]$TempRoot, [string]$Repo)
  if (-not (Test-UnderRoot -Path $Path -Root $TempRoot)) { return "not under the caller's TEMP ($TempRoot)" }
  if (Test-UnderRoot -Path $Path -Root (Join-Path $Repo '_work')) { return "under _work (the live window's folder)" }
  return $null
}

function Invoke-SelfTest {
  $failures = New-Object System.Collections.ArrayList
  $checks = New-Object System.Collections.ArrayList
  function Check {
    param([bool]$Cond, [string]$Label)
    [void]$checks.Add([PSCustomObject]@{ ok = $Cond; label = $Label })
    if (-not $Cond) { [void]$failures.Add($Label) }
  }
  $repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
  $tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
  $work = Join-Path $tempRoot ('harness-suspend-selftest-' + $PID)
  $child = $null
  $ticks = Join-Path $work 'ticks.txt'
  try {
    # (0) 保存先の守り: 実際の保存先は TEMP の下で _work の外。_work の下と、名前が TEMP で始まる
    # 隣のフォルダーは断る。止める対象の守り (下の (2)) は保存先と関係なく test/tools/harness の下だけを許す。
    Check ($null -eq (Get-SelfTestPlaceProblem -Path $work -TempRoot $tempRoot -Repo $repo)) 'the work folder is under the caller TEMP and outside _work'
    Check ($null -ne (Get-SelfTestPlaceProblem -Path (Join-Path $repo '_work\harness-suspend-selftest-1') -TempRoot $tempRoot -Repo $repo)) 'the old place under _work is refused'
    Check ($null -ne (Get-SelfTestPlaceProblem -Path ($tempRoot.TrimEnd('\') + '2\x') -TempRoot $tempRoot -Repo $repo)) 'a sibling folder whose name starts with TEMP is not under TEMP'
    # 止めてよい保存先は test/tools/harness で、名前がそれで始まる隣のフォルダーの実行ファイルは下に入らない。
    Check ($HarnessRoot.TrimEnd('\').EndsWith('\test\tools\harness', [System.StringComparison]::OrdinalIgnoreCase)) 'the allowed root is test/tools/harness'
    Check (-not (Test-UnderRoot -Path ($HarnessRoot.TrimEnd('\') + '2\electron.exe') -Root $HarnessRoot)) 'a sibling folder of test/tools/harness is outside the allowed root'
    [void](New-Item -ItemType Directory -Path $work -Force)
    # 見えない子: 100 ms ごとに 1 字を足し続ける powershell.exe (ウィンドウを作らない = CreateNoWindow)。
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = (Get-Command powershell.exe).Source
    $loop = "while (`$true) { [System.IO.File]::AppendAllText('" + $ticks + "', 'x'); Start-Sleep -Milliseconds 100 }"
    $psi.Arguments = '-NoProfile -NonInteractive -Command "' + $loop + '"'
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $child = [System.Diagnostics.Process]::Start($psi)
    # 書き始めるまで待つ (起動に 1 秒前後)。
    $deadline = (Get-NowMs) + 15000
    while ((Get-NowMs) -lt $deadline -and -not (Test-Path $ticks)) { Start-Sleep -Milliseconds 100 }
    Check (Test-Path $ticks) 'the hidden child started writing ticks'

    # (1) 自分の木に子が入る。
    $tree = Get-ProcessTree -RootPid $PID
    Check (@($tree | Where-Object { [int]$_.ProcessId -eq $child.Id }).Count -eq 1) 'the process tree of this shell contains the child'

    # (2) 保存先の外 (System32 の powershell.exe) は断る。
    $refusedRun = Resolve-Targets -RootPid $child.Id -WhichPart 'all' -Root $HarnessRoot
    Check ($refusedRun.error -eq 'refused' -and $refusedRun.targets.Count -eq 0 -and $refusedRun.refused.Count -eq 1) 'a pid outside test/tools/harness is refused'
    $missing = Resolve-Targets -RootPid 999999999 -WhichPart 'all' -Root $HarnessRoot
    Check ($missing.targets.Count -eq 0 -and $missing.error -like 'process not found*') 'an unknown pid is not found'
    # renderer を名指して renderer が居なければ対象 0 (子の powershell は --type を持たない)。保存先を子の場所に替えて見る。
    $childRoot = Split-Path -Parent $psi.FileName
    $noRenderer = Resolve-Targets -RootPid $child.Id -WhichPart 'renderer' -Root $childRoot
    Check ($noRenderer.targets.Count -eq 0 -and $noRenderer.error -like 'no renderer*') 'no renderer means no target'

    # (3) 保存先を子の場所に替えると対象になり、止めている間は字が増えず、戻すと増える。
    $allowed = Resolve-Targets -RootPid $child.Id -WhichPart 'all' -Root $childRoot
    Check ($allowed.targets.Count -ge 1 -and $null -eq $allowed.error) 'the child is a target when the root covers it'
    $before = (Get-Item $ticks).Length
    Start-Sleep -Milliseconds 400
    $grew = (Get-Item $ticks).Length -gt $before
    Check $grew 'ticks grow while running'
    # 止めている間の大きさを 2 回読む (止めた 300 ms 後と 1500 ms 後)。止めるのと戻すのはツールと同じ包みを呼ぶ。
    $st = [CordmuxHarnessSuspendNative]::Suspend([uint32]$child.Id)
    try {
      Start-Sleep -Milliseconds 300
      $a = (Get-Item $ticks).Length
      Start-Sleep -Milliseconds 1200
      $b = (Get-Item $ticks).Length
    }
    finally {
      $rs = [CordmuxHarnessSuspendNative]::Resume([uint32]$child.Id)
    }
    Check ($st -eq 0 -and $rs -eq 0) 'suspend and resume returned success'
    Check ($a -eq $b) ('no tick while suspended (' + $a + ' -> ' + $b + ')')
    $afterResume = (Get-Item $ticks).Length
    Start-Sleep -Milliseconds 800
    Check ((Get-Item $ticks).Length -gt $afterResume) 'ticks grow again after resume'
    # 止めて待って戻す一続き (Invoke-SuspendCycle) は、頼んだ長さ以上止め、必ず戻す。
    $cycle = Invoke-SuspendCycle -Targets $allowed.targets -HoldMs 1000
    Check ($cycle.suspended.Count -ge 1 -and $cycle.suspendErrors.Count -eq 0 -and $cycle.resumeErrors.Count -eq 0) 'the cycle suspended and resumed every target'
    Check (($cycle.resumedAtMs - $cycle.suspendedAtMs) -ge 1000) 'the cycle held for at least the requested time'
    $afterCycle = (Get-Item $ticks).Length
    Start-Sleep -Milliseconds 800
    Check ((Get-Item $ticks).Length -gt $afterCycle) 'ticks grow again after the cycle'
  }
  catch {
    [void]$failures.Add('exception: ' + $_.Exception.Message)
  }
  finally {
    if ($child -and -not $child.HasExited) {
      try { [void][CordmuxHarnessSuspendNative]::Resume([uint32]$child.Id) } catch { }
      try { $child.Kill(); $child.WaitForExit(5000) | Out-Null } catch { }
    }
    # 保存先の中は ticks.txt の 1 つだけなので、ファイルを消してから空のフォルダーを消す (再帰の削除を使わない)。
    if (Test-Path -LiteralPath $ticks) { Remove-Item -LiteralPath $ticks -Force -ErrorAction SilentlyContinue }
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Force -ErrorAction SilentlyContinue }
  }
  $left = Test-Path $work
  if ($left) { [void]$failures.Add('the work folder was not removed: ' + $work) }
  return [PSCustomObject]@{ ok = ($failures.Count -eq 0); selftest = $true; checks = $checks.ToArray(); failures = $failures.ToArray() }
}

try {
  if ($SelfTest) {
    $r = Invoke-SelfTest
    Write-JsonResultAndExit -Result $r -ExitCode ($(if ($r.ok) { 0 } else { 1 }))
  }
  if ($TargetPid -le 0) { throw '-Pid is required (harnessPid of hold-<tag>.json)' }

  $resolved = Resolve-Targets -RootPid $TargetPid -WhichPart $Part -Root $HarnessRoot
  if ($List) {
    Write-JsonResultAndExit -Result ([PSCustomObject]@{ ok = $true; pid = $TargetPid; part = $Part; harnessRoot = $HarnessRoot; listed = $resolved.listed; targets = $resolved.targets; refused = $resolved.refused; error = $resolved.error }) -ExitCode 0
  }
  if ($resolved.error) {
    Write-JsonResultAndExit -Result ([PSCustomObject]@{ ok = $false; pid = $TargetPid; part = $Part; seconds = $Seconds; targets = $resolved.targets; refused = $resolved.refused; listed = $resolved.listed; error = $resolved.error }) -ExitCode 1
  }
  if ($Resume) {
    $errors = @()
    foreach ($t in $resolved.targets) {
      $st = [CordmuxHarnessSuspendNative]::Resume([uint32]$t.pid)
      if ($st -ne 0) { $errors += [PSCustomObject]@{ pid = $t.pid; status = $st } }
    }
    $at = Get-NowMs
    Write-JsonResultAndExit -Result ([PSCustomObject]@{ ok = ($errors.Count -eq 0); pid = $TargetPid; part = $Part; resumeOnly = $true; targets = $resolved.targets; resumedAt = (Format-Iso $at); resumedAtMs = $at; resumeErrors = $errors; error = $null }) -ExitCode ($(if ($errors.Count -eq 0) { 0 } else { 1 }))
  }

  $cycle = Invoke-SuspendCycle -Targets $resolved.targets -HoldMs ($Seconds * 1000)
  $ok = ($cycle.suspended.Count -eq $resolved.targets.Count) -and ($cycle.resumeErrors.Count -eq 0)
  Write-JsonResultAndExit -Result ([PSCustomObject]@{
      ok            = $ok
      pid           = $TargetPid
      part          = $Part
      seconds       = $Seconds
      targets       = $resolved.targets
      refused       = $resolved.refused
      suspendedAt   = (Format-Iso $cycle.suspendedAtMs)
      suspendedAtMs = $cycle.suspendedAtMs
      resumedAt     = (Format-Iso $cycle.resumedAtMs)
      resumedAtMs   = $cycle.resumedAtMs
      heldMs        = $(if ($null -ne $cycle.resumedAtMs -and $null -ne $cycle.suspendedAtMs) { $cycle.resumedAtMs - $cycle.suspendedAtMs } else { $null })
      suspendErrors = $cycle.suspendErrors
      resumeErrors  = $cycle.resumeErrors
      error         = $(if ($ok) { $null } else { 'some targets were not suspended or not resumed (see suspendErrors / resumeErrors)' })
    }) -ExitCode ($(if ($ok) { 0 } else { 1 }))
}
catch {
  Write-JsonResultAndExit -Result ([PSCustomObject]@{ ok = $false; pid = $TargetPid; part = $Part; error = $_.Exception.Message }) -ExitCode 1
}
