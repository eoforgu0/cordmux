#requires -Version 5.1
<#
  screenshot.ps1 - 実行スクリプト (run-window.mjs) が OBS の見えているウィンドウを PNG に撮るためのツール。

  実行スクリプトが撮った PNG を人と機械が見られるようにし、実行中に OBS の画面を人が見張らなくてよいようにする。

  対象のプロセス (-Pid) が持つ、見えているトップレベルウィンドウを全部 (EnumWindows +
  GetWindowThreadProcessId) 撮る。ドックが浮いた別のトップレベルウィンドウになっていても撮れる
  ようにするため (メインウィンドウ 1 枚だけに決め打ちしない)。1 ウィンドウにつき PNG 1 枚を -OutDir の下に
  "<Prefix>-<連番>[-<ウィンドウのタイトルの英数字>].png" という名前で書く (連番は列挙順の 1 始まり。
  タイトルの英数字は無ければ付けない)。

  撮り方は PrintWindow (PW_RENDERFULLCONTENT を指定) を使う。DWM の合成描画やハードウェア
  描画のあるウィンドウでも、他のウィンドウの裏や画面の外にあっても撮れるため。GetWindowRect + 画面からの
  切り出し (Graphics.CopyFromScreen) は、ウィンドウが画面の手前に出ていないと撮れない欠点がある
  ので採らない。

  結果は 1 行の JSON を標準出力に書く。
    成功: { "ok": true, "pid": <int>, "shots": [ { "path", "title", "width", "height" }, ... ] }
    失敗: { "ok": false, "pid": <int>, "error": "<文言>" } (終了コード 1)
  -Pid で渡した pid のプロセスが見つからない、またはウィンドウが 1 つも撮れなかった (0x0 サイズ等で
  全部除かれた) 場合でも、0 件の shots で ok:true を返す (「プロセスがない」だけは throw して
  ok:false にする。呼び出し側が理由を区別できるようにするため)。
#>

[CmdletBinding()]
param(
  # PowerShell の自動変数 $PID (実行中の powershell.exe 自身の pid。読み取り専用) と
  # 名前が衝突するため、実体は $TargetPid に持たせ、呼び出し側の引数名だけ -Pid になるよう
  # Alias で合わせる。
  [Parameter(Mandatory = $true)]
  [Alias('Pid')]
  [int]$TargetPid,

  [Parameter(Mandatory = $true)]
  [string]$OutDir,

  [Parameter(Mandatory = $true)]
  [string]$Prefix
)

$ErrorActionPreference = 'Stop'

function Write-JsonResultAndExit {
  # PowerShell の既定の標準出力は実行環境の既定コードページ (例: 日本語 Windows の cp932) を
  # 通ることがあり、Write-Output で日本語混じりの JSON を出すと呼び出し側 (run-window.mjs の
  # execFileSync encoding:"utf8") で文字化けする (実測して確認した。summarize-window.py に
  # PYTHONIOENCODING=utf-8 を渡す判断と同じ理由)。標準出力のストリームに UTF-8 (BOM なし) の
  # バイト列を直接書き、コードページに依存しない形にする。
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
  Add-Type -AssemblyName System.Drawing

  # user32 のウィンドウの列挙・矩形取得・PrintWindow の薄い包み。英語のコメントだけを持つ
  # (comments_japanese.py の対象は外側の .ps1 のコメント。ここは文字列リテラルの中身で
  # あり、外側の字句解析ではコメントとして分解されない)。
  $nativeSource = @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class CordmuxScreenshotNative
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
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);

    [DllImport("user32.dll")]
    public static extern IntPtr GetAncestor(IntPtr hWnd, uint gaFlags);

    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);

    public const uint GA_ROOT = 2;
    public const uint PW_RENDERFULLCONTENT = 2;

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    // Visible top-level windows (their own root ancestor) owned by targetProcessId.
    public static List<IntPtr> FindTopLevelWindows(uint targetProcessId)
    {
        var found = new List<IntPtr>();
        EnumWindows(delegate (IntPtr hWnd, IntPtr lParam)
        {
            uint windowProcessId;
            GetWindowThreadProcessId(hWnd, out windowProcessId);
            if (windowProcessId == targetProcessId
                && IsWindowVisible(hWnd)
                && GetAncestor(hWnd, GA_ROOT) == hWnd)
            {
                found.Add(hWnd);
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    public static string GetTitle(IntPtr hWnd)
    {
        int len = GetWindowTextLength(hWnd);
        if (len == 0) return "";
        var sb = new StringBuilder(len + 1);
        GetWindowText(hWnd, sb, sb.Capacity);
        return sb.ToString();
    }
}
"@
  Add-Type -TypeDefinition $nativeSource -ReferencedAssemblies System.Drawing

  if (-not (Test-Path -LiteralPath $OutDir)) {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
  }

  # 対象プロセスの生死を先に見る (居なければウィンドウの列挙をしても必ず空になるだけなので、
  # 理由をはっきりさせるためここで早期に落とす)。
  $proc = Get-Process -Id $TargetPid -ErrorAction SilentlyContinue
  if (-not $proc) {
    throw "pid $TargetPid のプロセスが見つからない"
  }

  $windows = [CordmuxScreenshotNative]::FindTopLevelWindows([uint32]$TargetPid)

  $shots = New-Object System.Collections.Generic.List[object]
  $n = 0
  foreach ($hWnd in $windows) {
    $n += 1
    $rect = New-Object CordmuxScreenshotNative+RECT
    [void][CordmuxScreenshotNative]::GetWindowRect($hWnd, [ref]$rect)
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    if ($width -le 0 -or $height -le 0) {
      continue
    }

    $title = [CordmuxScreenshotNative]::GetTitle($hWnd)
    # ファイル名には連番とウィンドウのタイトルの英数字だけを使う (日本語や記号を含むタイトルが
    # そのままファイル名に出ないようにする)。
    $titleAlnum = ($title -replace '[^A-Za-z0-9]', '')
    if ($titleAlnum.Length -gt 40) {
      $titleAlnum = $titleAlnum.Substring(0, 40)
    }
    $suffix = ''
    if ($titleAlnum) {
      $suffix = "-$titleAlnum"
    }
    $fileName = "$Prefix-$n$suffix.png"
    $outPath = Join-Path $OutDir $fileName

    $bitmap = New-Object System.Drawing.Bitmap($width, $height)
    try {
      $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
      try {
        $hdc = $graphics.GetHdc()
        try {
          [void][CordmuxScreenshotNative]::PrintWindow($hWnd, $hdc, [CordmuxScreenshotNative]::PW_RENDERFULLCONTENT)
        }
        finally {
          $graphics.ReleaseHdc($hdc)
        }
      }
      finally {
        $graphics.Dispose()
      }
      $bitmap.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
    }
    finally {
      $bitmap.Dispose()
    }

    $shots.Add([PSCustomObject]@{
        path   = $outPath
        title  = $title
        width  = $width
        height = $height
      })
  }

  Write-JsonResultAndExit -Result ([PSCustomObject]@{ ok = $true; pid = $TargetPid; shots = $shots }) -ExitCode 0
}
catch {
  Write-JsonResultAndExit -Result ([PSCustomObject]@{ ok = $false; pid = $TargetPid; error = $_.Exception.Message }) -ExitCode 1
}
