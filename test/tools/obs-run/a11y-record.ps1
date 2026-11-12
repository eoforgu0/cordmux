#requires -Version 5.1
<#
  a11y-record.ps1 - 読み上げの元になる値を、音を出さずに集めるツール。
  ナレーターを使わずに、ナレーターが読む元の値 (UI Automation の木の値) と、読み上げを起こす出来事を書き出す。
  読み上げの聞こえ方そのものは確認しない。
  Qt 6.8.3 の Windows の読み上げのコンポーネント (qtbase v6.8.3 の src/plugins/platforms/windows/uiautomation/qwindowsuiamainprovider.cpp)
  は、QAccessible の Name を Name に、Help を HelpText に、Description (cordmux が setAccessibleDescription で入れる説明) を
  FullDescription (UIA_FullDescriptionPropertyId = 30159) に出す。Announcement (QAccessibleAnnouncementEvent) は
  UiaRaiseNotificationEvent、名前の変化は UIA_NamePropertyId の PropertyChanged、ウィンドウの出入りは WindowOpened・WindowClosed、
  フォーカスは FocusChanged で渡す。LiveRegionChanged は Qt 6.8.3 では出さない (出れば記録する)。
  .NET Framework の UI Automation のクライアント (System.Windows.Automation) は FullDescription を読む API を持たず、LiveSetting も
  「サポートされていないプロパティ」を値として返すので、この 2 つはクライアントの内部の
  生の呼び出し (UiaCoreApi.RawUiaGetPropertyValue。番号をそのまま UI Automation へ渡す) で読む。読めることは -SelfTest で
  確認する (値の入った見本で往復させる)。.NET の更新で内部の名前が変わって読めなくなったら、-SelfTest が失敗になる。

  形は 3 つ (どれか 1 つだけ):
    -Tree -Pid <n> [-Title <題>] [-Out <file>] [-RawView] [-IncludeHidden] [-MaxDepth 40] [-MaxNodes 5000]
        対象のプロセスのトップレベルウィンドウ (見えている物。-Title を渡せば題が完全に一致する物だけ) を根から木で書き出す。
        各要素: name、controlType (と番号)、localizedControlType、helpText、fullDescription、itemStatus、liveSetting、
        isEnabled、isKeyboardFocusable、hasKeyboardFocus、isOffscreen、automationId、className、accessKey、acceleratorKey、
        labeledBy、runtimeId、rect、型の値 (toggle、expandCollapse、value と readOnly、selected、grid の行と列の数、
        table の行と列の見出しの名前、window の isModal)。既定は Control の見え方 (ナレーターがたどる木)、-RawView で全部。
        -Out が無ければ木の JSON を標準出力へ。
    -Record -Pid <n> -Out <file.jsonl> [-Seconds <n>] [-StopFile <path>] [-IncludeHidden]
        出来事を 1 行 1 件の JSON で書き足す。止まるのは、-Seconds が過ぎた・-StopFile ができた・対象のプロセスが
        終了した、のどれか早い時 (-Seconds と -StopFile のどちらかは要る)。記録する出来事 (kind):
          focus             フォーカスの移り (対象のプロセスの物だけ)。要素の値と、それを含むトップレベルウィンドウの名前と説明
          notification      UiaRaiseNotificationEvent (displayString、activityId、notificationKind、notificationProcessing)
          liveRegionChanged ライブリージョンの変化
          windowOpened / windowClosed  ウィンドウの出入り (開いたウィンドウの name と fullDescription)
          menuOpened        メニューが開いた
          propertyChanged   name・helpText・itemStatus・isEnabled・toggleState・expandCollapseState・value の変化 (old と new。
                            同じ要素の runtimeId で、1 回の切り替えで何回出たかを数えられる)
          windowWatched     変化の監視を始めたウィンドウ (始めにあったウィンドウと、後から開いたウィンドウ)
          start             記録の始め (1 行目。-Out のファイルへは書き足すので、実行の区切りに使う)
        行には t (始めからの ms) と wall (時刻) が付く。終わりに 1 行の要約 (種類ごとの数と止まった理由) を標準出力へ。
        t と wall は出来事がこのツールに届いた時刻 (出た時刻ではない)。-SelfTest での測定では、ビルドの有無に
        かかわらず、登録の後の最初の 1 件が最長約 10 秒止められ、その間の物が後でまとめて届いた (失われはしなかった)。見たい操作より
        十分前に起こしておき、始めの数秒の時刻は遅れうるものとして読む。
        登録: フォーカスはデスクトップ全体 (対象のプロセスの物だけを書く)、ウィンドウの出入りはデスクトップのルートに Subtree、それ以外は
        対象のプロセスのトップレベルウィンドウごとに Subtree (始めと、ウィンドウが開いた後の 200 ms 以内と、1 秒ごとの再走査で足す)。デスクトップの
        ルートへの登録は、デスクトップの木の下にないウィンドウ (-SelfTest の非表示ウィンドウ) の notification を届けなかったので使わない。
        1 つの変化は、それを持つウィンドウの登録からだけ書く (同じ変化が 2 つの登録に届いても 1 行)。
    -SelfTest
        OBS を起こさずに、このツールの読み方と出来事の受け方を確認する。このツール自身を子のプロセス (powershell.exe、
        コンソールウィンドウなし) で起動し、子の中に非表示のウィンドウ (WS_VISIBLE を付けず、表示も前面化もしない) を作って、値を
        決めた UI Automation のコンポーネント (名前・HelpText・FullDescription・LiveSetting) を持たせる。-Tree と同じ読み方で値が
        往復するか、-Record と同じ登録 (ウィンドウごとに Subtree) で notification・liveRegionChanged・propertyChanged を 1 回ずつ
        受けるか、focus を受けるか (記録だけ) を確認する。子は終わりに閉じる (閉じなければ止める)。
        コンポーネントを別のプロセスに置くのは OBS と同じ形にするためである。同じプロセスの中に置くと、1 回の名前の変化が
        同じ登録で 2 回届いた。1 回の切り替えで名前の変化が何回出るかを数えるので、ツールの側で増えてはいけない。
        結果は 1 行の JSON (checks の各項目、待った時間の waits、受けた出来事の行)。
        待ち方: 決まった長さのスリープではなく、届くまで待つ。
          (1) 準備の確認: 子にマーカーの出来事 (MenuOpened。判定する 3 種のどれでもなく、ウィンドウごとの登録の最後の種類) を 500 ms
              ごとに出させ、記録の側に 1 件届くまで待つ (最長 30 秒)。届いてから (届かなければ 30 秒で) 本番を 1 回だけ出させる。
          (2) 本番: notification・liveRegionChanged・propertyChanged の 3 種が全部届くまで待つ (出させてから最長 10 秒)。
          (3) 落ち着き (「ちょうど 1 回」の確認を弱めない): 3 種が届いた後、子にもう 1 回マーカー (柵) を出させ、それが届くまで
              (最長 10 秒) と、最後の 1 件が届いてから 1.5 秒 (前の決まった待ちと同じ長さ) の、遅いほうまで待ってから数える。
              子の出来事はこれまで全部、出した順に届いたので、柵が届けばその前に出た物 (2 回目の名前の変化も) は届いている。
              1.5 秒は、順序が入れ替わって届く場合への備えである。決まった長さだけ待つ方法は採らない (1 秒後に出した 2 回目が 1.0-2.3 秒後に
              届き、1.5 秒では取りこぼした)。いちばん遅く届いた 1 件の時間だけ待つ方法も採らない (経路が通った後は多くが 20 ms 未満で
              届くので待ちが短くなり、同じく取りこぼした)。
          届かなければ 10 秒で失敗のままにする (失敗は、出来事が届かなかったか、.NET の内部の名前が変わったかのどちらかで、checks のどれが失敗かで分かる)。
          waits に、マーカーを出した回数 (probeRaises)・マーカーが届くまで (probeSeenMs、届かなければ -1)・本番の各種が届くまで
          (notificationMs など、出させてからの ms。届かなければ -1)・柵が届くまで (fenceSeenMs、届かなければ -1)・
          落ち着きの長さ (settleMs)・全体 (totalMs) を書く。
          背景: ビルドと並行した実行で、メッセージが 0 件になったことがある (記録の登録に 4.2 秒。静かなときは 2.1 秒で、
          出させてから約 0.5 秒で届いた)。届くのが 1.5 秒より遅かったのか、子が出した時に登録がまだ子の側へ着いて
          いなかった (その出来事は二度と届かない) のかは、その記録からは区別できないので、両方を防ぐ ((1) が後者、(2) が前者)。
          測定 (ビルドと並行した場合も、ビルドのない場合も): 登録の後の最初の 1 件が最長約 10 秒 (9.7 秒) 止められ、
          その間に出させたマーカーは後で全部まとめて届いた (失われた物は 0)。1 件届いた後の本番は多くが 20 ms 未満で届いた
          (1 回だけ 415 ms)。
          長さ: マーカーの 30 秒は、見た最長の止まり (9.7 秒) の約 3 倍。本番の 10 秒は、経路が通った後の届き (最長 415 ms) にも、
          遅れて出す故障 (late の 3.5 秒) にも足りる。長くして損をするのは本当に届かない失敗の場合だけ (約 45 秒で終わる)。
          決まったスリープを延ばす方法は採らない (静かな PC でも毎回その分遅く、混み方がそれを超えればまた失敗する。登録が着く前に
          出した物はスリープしても戻らない)。失敗したら -SelfTest を再実行する方法も採らない (-Record は再実行できないので、届かないことがある事実を
          成功で隠してしまう。待つ形なら、本当に届かない場合だけが失敗になる)。
    -SelfTest -SelfTestFault silent|noLiveRegion|nameTwice|late
        セルフテストそのものが失敗になれることを確認する故障 (実機での事前確認では使わない。結果に fault が付く)。子が:
          silent        「出した」と答えるが何も出さない (本当に届かない場合)。失敗: 3 種とも。マーカーと本番の待ちを使い切る (約 45 秒)
          noLiveRegion  liveRegionChanged だけ出さない。赤: liveRegionChangedReceived だけ
          nameTwice     「出した」と答えた 1 秒後に名前の変化をもう 1 回出す (遅れて届く 2 回目)。失敗: nameChangedReceivedOnce だけ
          late          「出した」と答えてから 3.5 秒後に出す (1.5 秒の固定待ちなら失敗する遅さ)。成功のまま (ok:true)
        -SelfTest のほかで渡すと断る。

  してはいけないこと (守る): プロセスやウィンドウを起動しない。押さない・フォーカスを動かさない・前面を取らない (読むだけ)。
  音を出さない。書くのは -Out のファイルだけ (-SelfTest はファイルを書かず、起こすのは自分の子のプロセス 1 つだけ)。

  結果は UTF-8 (BOM なし) の 1 行の JSON を標準出力に書く (-Tree で -Out のないときは木の JSON)。
  失敗しても例外を投げずに ok:false を返す (終了コード 1。ほかのツールと同じ形)。
  BOM 付き UTF-8 で保存する (cleanup.ps1 と同じ理由。BOM を外すと PowerShell 5.1 が cp932 として読む)。
  ヒアストリングの中の C# のコメントは英語で書く。

  確認していないこと (実機で確認する): OBS (Qt 6.8.3) のウィンドウで FullDescription に cordmux の説明が出ること、
  Qt の Announcement が notification として届くこと (ウィンドウごとの Subtree の登録で)、メニューのウィンドウ (QMenu) が
  windowOpened か menuOpened で出ること、OBS の見えるウィンドウの windowOpened がデスクトップのルートへの登録に届くこと (届かなくても
  1 秒ごとの再走査でウィンドウの監視を始める)。-SelfTest の非表示ウィンドウは Qt ではなく .NET のコンポーネントなので、Qt の出し方は実機で見る。
  フォーカスの出来事は非表示ウィンドウでは起こせないことがある (-SelfTest の focus は記録だけで合否に入れない)。
#>

[CmdletBinding()]
param(
  # 他のツールと同じ理由: 読み取り専用の自動変数 $PID と大文字小文字を区別せず衝突するため、実体は $TargetPid に持たせる。
  [Alias('Pid')]
  [int]$TargetPid = 0,

  [switch]$Tree,
  [switch]$Record,
  [switch]$SelfTest,
  # -SelfTest が自分を子のプロセスとして起動するときだけ使う (非表示ウィンドウのコンポーネントの側)。人は使わない。
  [switch]$SelfTestProvider,

  [AllowEmptyString()]
  [string]$Title = '',

  [AllowEmptyString()]
  [string]$Out = '',

  [ValidateRange(0, 14400)]
  [int]$Seconds = 0,

  [AllowEmptyString()]
  [string]$StopFile = '',

  [ValidateRange(1, 200)]
  [int]$MaxDepth = 40,

  [ValidateRange(1, 200000)]
  [int]$MaxNodes = 5000,

  [switch]$RawView,
  [switch]$IncludeHidden,

  # -SelfTest のセルフテストが失敗になれることを確認するための故障 (先頭のコメントの「-SelfTestFault」)。実機での事前確認では使わない。
  # 既定の '' は検証されない (PowerShell は既定値を ValidateSet に当てない)。
  [ValidateSet('silent', 'noLiveRegion', 'nameTwice', 'late')]
  [string]$SelfTestFault = ''
)

$ErrorActionPreference = 'Stop'

function Write-Utf8AndExit {
  # dock-click.ps1 と同じ理由: 標準出力へ UTF-8 (BOM なし) のバイト列を直接書く。
  param([string]$Text, [int]$ExitCode)
  $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
  $bytes = $utf8NoBom.GetBytes($Text)
  $stdout = [System.Console]::OpenStandardOutput()
  $stdout.Write($bytes, 0, $bytes.Length)
  $stdout.Flush()
  exit $ExitCode
}

function Write-ErrorAndExit {
  # 引数の誤りと例外を 1 行の JSON にする。
  param([string]$Mode, [string]$Message)
  $json = [PSCustomObject]@{ ok = $false; mode = $Mode; pid = $TargetPid; error = $Message } | ConvertTo-Json -Compress
  Write-Utf8AndExit -Text $json -ExitCode 1
}

$modes = @()
if ($Tree) { $modes += 'tree' }
if ($Record) { $modes += 'record' }
if ($SelfTest) { $modes += 'selftest' }
if ($SelfTestProvider) { $modes += 'provider' }
if ($modes.Count -ne 1) {
  Write-ErrorAndExit -Mode '' -Message 'give exactly one of -Tree, -Record, -SelfTest'
}
$mode = $modes[0]
if (-not [string]::IsNullOrEmpty($SelfTestFault) -and $mode -ne 'selftest' -and $mode -ne 'provider') {
  Write-ErrorAndExit -Mode $mode -Message '-SelfTestFault is only for -SelfTest'
}
if (($mode -eq 'tree' -or $mode -eq 'record') -and $TargetPid -le 0) {
  Write-ErrorAndExit -Mode $mode -Message '-Pid is required'
}
if ($mode -eq 'record') {
  if ([string]::IsNullOrEmpty($Out)) { Write-ErrorAndExit -Mode $mode -Message '-Out is required for -Record' }
  if ($Seconds -le 0 -and [string]::IsNullOrEmpty($StopFile)) {
    Write-ErrorAndExit -Mode $mode -Message 'give -Seconds or -StopFile for -Record'
  }
}

$source = @"
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Automation;
using System.Windows.Automation.Provider;

namespace CordmuxA11y
{
    // Minimal JSON writer (no external assemblies).
    public sealed class JObj
    {
        private readonly StringBuilder sb = new StringBuilder("{");
        private bool first = true;
        private void Key(string k) { if (!first) sb.Append(','); first = false; sb.Append(Json.Str(k)).Append(':'); }
        public JObj S(string k, string v) { Key(k); sb.Append(Json.Str(v)); return this; }
        public JObj N(string k, long v) { Key(k); sb.Append(v.ToString(CultureInfo.InvariantCulture)); return this; }
        public JObj B(string k, bool v) { Key(k); sb.Append(v ? "true" : "false"); return this; }
        public JObj Raw(string k, string json) { Key(k); sb.Append(json ?? "null"); return this; }
        public override string ToString() { return sb.ToString() + "}"; }
    }

    public static class Json
    {
        public static string Str(string s)
        {
            if (s == null) return "null";
            var sb = new StringBuilder("\"");
            foreach (char c in s)
            {
                switch (c)
                {
                    case '"': sb.Append("\\\""); break;
                    case '\\': sb.Append("\\\\"); break;
                    case '\n': sb.Append("\\n"); break;
                    case '\r': sb.Append("\\r"); break;
                    case '\t': sb.Append("\\t"); break;
                    default:
                        if (c < 0x20) sb.Append("\\u").Append(((int)c).ToString("x4")); else sb.Append(c);
                        break;
                }
            }
            return sb.Append('"').ToString();
        }

        public static string Arr(IEnumerable<string> rawItems)
        {
            var sb = new StringBuilder("[");
            bool first = true;
            foreach (string item in rawItems) { if (!first) sb.Append(','); first = false; sb.Append(item); }
            return sb.Append(']').ToString();
        }
    }

    public static class Win
    {
        public delegate bool EnumProc(IntPtr hwnd, IntPtr lParam);
        [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lParam);
        [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
        [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr hwnd, StringBuilder text, int max);

        public static string TitleOf(IntPtr hwnd)
        {
            var sb = new StringBuilder(1024);
            GetWindowTextW(hwnd, sb, sb.Capacity);
            return sb.ToString();
        }

        // Top-level windows of a process (visible ones unless includeHidden; exact title match when title is not empty).
        public static List<IntPtr> TopLevels(int pid, string title, bool includeHidden)
        {
            var list = new List<IntPtr>();
            EnumWindows((h, l) =>
            {
                uint p;
                GetWindowThreadProcessId(h, out p);
                if ((int)p != pid) return true;
                if (!includeHidden && !IsWindowVisible(h)) return true;
                if (!string.IsNullOrEmpty(title) && TitleOf(h) != title) return true;
                list.Add(h);
                return true;
            }, IntPtr.Zero);
            return list;
        }
    }

    public static class Uia
    {
        public const int FullDescriptionId = 30159;
        public const int LiveSettingId = 30135;

        // The managed client has no schema entry for FullDescription and rejects LiveSetting ("unsupported property"
        // inside the returned value), so these two are read through the client's own raw call
        // (MS.Internal.Automation.UiaCoreApi.RawUiaGetPropertyValue on the element's node), which passes the id to UIA as is.
        private static readonly MethodInfo RawGet = typeof(AutomationElement).Assembly
            .GetType("MS.Internal.Automation.UiaCoreApi")
            .GetMethod("RawUiaGetPropertyValue", BindingFlags.NonPublic | BindingFlags.Public | BindingFlags.Static);
        private static readonly MethodInfo RawNode = typeof(AutomationElement)
            .GetMethod("get_RawNode", BindingFlags.NonPublic | BindingFlags.Instance);

        public static bool RawAvailable { get { return RawGet != null && RawNode != null; } }

        public static object RawProp(AutomationElement e, int id)
        {
            if (!RawAvailable || e == null) return null;
            try
            {
                object node = RawNode.Invoke(e, null);
                object[] args = { node, id, null };
                int hr = (int)RawGet.Invoke(null, args);
                if (hr < 0) return null;
                object v = args[2];
                // UIA answers "not supported" with a reserved COM object; treat it (and any COM object) as no value.
                if (v != null && Marshal.IsComObject(v)) return null;
                return v;
            }
            catch (Exception) { return null; }
        }

        public static string FullDescriptionOf(AutomationElement e)
        {
            return ValueText(RawProp(e, FullDescriptionId));
        }

        public static string LiveSettingOf(AutomationElement e)
        {
            object v = RawProp(e, LiveSettingId);
            if (v is int)
            {
                switch ((int)v)
                {
                    case 0: return "Off";
                    case 1: return "Polite";
                    case 2: return "Assertive";
                }
            }
            return ValueText(v);
        }

        public static object Prop(AutomationElement e, AutomationProperty p)
        {
            if (p == null) return null;
            try
            {
                object v = e.GetCurrentPropertyValue(p, true);
                // The managed client returns some failures as an exception object instead of throwing.
                return v == AutomationElement.NotSupported || v is Exception ? null : v;
            }
            catch (Exception) { return null; }
        }

        public static string PropText(AutomationElement e, AutomationProperty p)
        {
            return ValueText(Prop(e, p));
        }

        public static string ValueText(object v)
        {
            if (v == null) return null;
            if (v == AutomationElement.NotSupported || v is Exception) return null;
            var ints = v as int[];
            if (ints != null) return string.Join(".", Array.ConvertAll(ints, i => i.ToString(CultureInfo.InvariantCulture)));
            var ct = v as ControlType;
            if (ct != null) return ct.ProgrammaticName.Replace("ControlType.", "");
            var ae = v as AutomationElement;
            if (ae != null) { try { return ae.Current.Name; } catch (Exception) { return "(element)"; } }
            if (v is double) return ((double)v).ToString(CultureInfo.InvariantCulture);
            return Convert.ToString(v, CultureInfo.InvariantCulture);
        }

        public static string RuntimeId(AutomationElement e)
        {
            try { return ValueText(e.GetRuntimeId()); } catch (Exception) { return null; }
        }

        // The values Narrator reads for one element (and the pattern states it announces).
        public static JObj Snapshot(AutomationElement e)
        {
            var o = new JObj();
            o.S("name", PropText(e, AutomationElement.NameProperty));
            var ct = Prop(e, AutomationElement.ControlTypeProperty) as ControlType;
            o.S("controlType", ct != null ? ct.ProgrammaticName.Replace("ControlType.", "") : null);
            if (ct != null) o.N("controlTypeId", ct.Id);
            o.S("localizedControlType", PropText(e, AutomationElement.LocalizedControlTypeProperty));
            o.S("helpText", PropText(e, AutomationElement.HelpTextProperty));
            o.S("fullDescription", FullDescriptionOf(e));
            o.S("itemStatus", PropText(e, AutomationElement.ItemStatusProperty));
            o.S("liveSetting", LiveSettingOf(e));
            o.S("isEnabled", PropText(e, AutomationElement.IsEnabledProperty));
            o.S("isKeyboardFocusable", PropText(e, AutomationElement.IsKeyboardFocusableProperty));
            o.S("hasKeyboardFocus", PropText(e, AutomationElement.HasKeyboardFocusProperty));
            o.S("isOffscreen", PropText(e, AutomationElement.IsOffscreenProperty));
            o.S("automationId", PropText(e, AutomationElement.AutomationIdProperty));
            o.S("className", PropText(e, AutomationElement.ClassNameProperty));
            o.S("accessKey", PropText(e, AutomationElement.AccessKeyProperty));
            o.S("acceleratorKey", PropText(e, AutomationElement.AcceleratorKeyProperty));
            o.S("labeledBy", PropText(e, AutomationElement.LabeledByProperty));
            o.S("runtimeId", RuntimeId(e));
            object rect = Prop(e, AutomationElement.BoundingRectangleProperty);
            if (rect is System.Windows.Rect)
            {
                var r = (System.Windows.Rect)rect;
                if (!r.IsEmpty)
                    o.Raw("rect", Json.Arr(new[] { ((long)r.X).ToString(), ((long)r.Y).ToString(), ((long)r.Width).ToString(), ((long)r.Height).ToString() }));
            }
            Patterns(e, o);
            return o;
        }

        private static void Patterns(AutomationElement e, JObj o)
        {
            object p;
            try
            {
                if (e.TryGetCurrentPattern(TogglePattern.Pattern, out p))
                    o.S("toggle", ((TogglePattern)p).Current.ToggleState.ToString());
                if (e.TryGetCurrentPattern(ExpandCollapsePattern.Pattern, out p))
                    o.S("expandCollapse", ((ExpandCollapsePattern)p).Current.ExpandCollapseState.ToString());
                if (e.TryGetCurrentPattern(ValuePattern.Pattern, out p))
                {
                    o.S("value", ((ValuePattern)p).Current.Value);
                    o.B("readOnly", ((ValuePattern)p).Current.IsReadOnly);
                }
                if (e.TryGetCurrentPattern(SelectionItemPattern.Pattern, out p))
                    o.B("selected", ((SelectionItemPattern)p).Current.IsSelected);
                if (e.TryGetCurrentPattern(GridPattern.Pattern, out p))
                {
                    o.N("gridRows", ((GridPattern)p).Current.RowCount);
                    o.N("gridColumns", ((GridPattern)p).Current.ColumnCount);
                }
                if (e.TryGetCurrentPattern(TablePattern.Pattern, out p))
                {
                    var t = (TablePattern)p;
                    o.Raw("columnHeaders", Names(t.Current.GetColumnHeaders()));
                    o.Raw("rowHeaders", Names(t.Current.GetRowHeaders()));
                    o.S("rowOrColumnMajor", t.Current.RowOrColumnMajor.ToString());
                }
                if (e.TryGetCurrentPattern(WindowPattern.Pattern, out p))
                    o.B("isModal", ((WindowPattern)p).Current.IsModal);
            }
            catch (Exception ex)
            {
                o.S("patternError", ex.GetType().Name);
            }
        }

        private static string Names(AutomationElement[] items)
        {
            var list = new List<string>();
            if (items != null)
                foreach (AutomationElement a in items) list.Add(Json.Str(PropText(a, AutomationElement.NameProperty)));
            return Json.Arr(list);
        }

        [DllImport("user32.dll")] private static extern IntPtr GetAncestor(IntPtr hwnd, uint flags);

        // The top-level window handle that holds e: the nearest element with a window handle, then its root ancestor.
        public static long TopHwndOf(AutomationElement e)
        {
            try
            {
                TreeWalker walker = TreeWalker.RawViewWalker;
                AutomationElement cur = e;
                for (int i = 0; i < 100 && cur != null; ++i)
                {
                    object hv = Prop(cur, AutomationElement.NativeWindowHandleProperty);
                    if (hv is int && (int)hv != 0)
                        return GetAncestor(new IntPtr((int)hv), 2 /* GA_ROOT */).ToInt64();
                    cur = walker.GetParent(cur);
                }
            }
            catch (Exception) { }
            return 0;
        }

        // The top-level window that contains e (its name and description are what Narrator reads on entering a window).
        public static AutomationElement WindowOf(AutomationElement e)
        {
            try
            {
                TreeWalker walker = TreeWalker.ControlViewWalker;
                AutomationElement root = AutomationElement.RootElement;
                AutomationElement cur = e;
                for (int i = 0; i < 100 && cur != null; ++i)
                {
                    AutomationElement parent = walker.GetParent(cur);
                    if (parent == null || Automation.Compare(parent, root)) return cur;
                    cur = parent;
                }
            }
            catch (Exception) { }
            return null;
        }
    }

    public static class TreeDump
    {
        public static string Dump(int pid, string title, bool rawView, bool includeHidden, int maxDepth, int maxNodes)
        {
            TreeWalker walker = rawView ? TreeWalker.RawViewWalker : TreeWalker.ControlViewWalker;
            int count = 0;
            bool truncated = false;
            var windows = new List<string>();
            foreach (IntPtr h in Win.TopLevels(pid, title, includeHidden))
            {
                AutomationElement root;
                try { root = AutomationElement.FromHandle(h); } catch (Exception) { continue; }
                var w = new JObj();
                w.N("hwnd", h.ToInt64());
                w.S("title", Win.TitleOf(h));
                w.Raw("root", Node(walker, root, 0, maxDepth, maxNodes, ref count, ref truncated));
                windows.Add(w.ToString());
            }
            var o = new JObj();
            o.B("ok", true).S("mode", "tree").N("pid", pid).S("title", title).S("view", rawView ? "raw" : "control");
            o.N("windowCount", windows.Count).N("nodeCount", count).B("truncated", truncated);
            o.Raw("windows", Json.Arr(windows));
            return o.ToString();
        }

        private static string Node(TreeWalker walker, AutomationElement e, int depth, int maxDepth, int maxNodes, ref int count, ref bool truncated)
        {
            ++count;
            JObj o = Uia.Snapshot(e);
            if (depth >= maxDepth) { truncated = true; return o.ToString(); }
            var children = new List<string>();
            AutomationElement c = null;
            try { c = walker.GetFirstChild(e); } catch (Exception) { }
            while (c != null)
            {
                if (count >= maxNodes) { truncated = true; break; }
                children.Add(Node(walker, c, depth + 1, maxDepth, maxNodes, ref count, ref truncated));
                try { c = walker.GetNextSibling(c); } catch (Exception) { c = null; }
            }
            if (children.Count > 0) o.Raw("children", Json.Arr(children));
            return o.ToString();
        }
    }

    public sealed class Recorder : IDisposable
    {
        private readonly int pid;
        private readonly bool includeHidden;
        private readonly StreamWriter writer;
        private readonly Stopwatch clock = Stopwatch.StartNew();
        private readonly object gate = new object();
        private readonly Dictionary<string, int> counts = new Dictionary<string, int>();
        private readonly HashSet<long> watchedHwnds = new HashSet<long>();
        private readonly HashSet<string> knownWindowIds = new HashSet<string>();
        private readonly AutomationEventHandler onEvent;
        private readonly AutomationFocusChangedEventHandler onFocus;
        private bool disposed;
        private volatile bool rescan;

        // True once after a window opened (consumed by the caller's loop).
        public bool TakeRescan() { bool r = rescan; rescan = false; return r; }
        public readonly List<string> Lines = new List<string>(); // kept for -SelfTest
        public bool KeepLines;

        private static readonly AutomationProperty[] Watched = new[]
        {
            AutomationElement.NameProperty, AutomationElement.HelpTextProperty, AutomationElement.ItemStatusProperty,
            AutomationElement.IsEnabledProperty, TogglePattern.ToggleStateProperty,
            ExpandCollapsePattern.ExpandCollapseStateProperty, ValuePattern.ValueProperty,
        };

        public Recorder(int pid, string outPath, bool includeHidden)
        {
            this.pid = pid;
            this.includeHidden = includeHidden;
            if (!string.IsNullOrEmpty(outPath))
            {
                string dir = Path.GetDirectoryName(Path.GetFullPath(outPath));
                if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
                writer = new StreamWriter(outPath, true, new UTF8Encoding(false));
            }
            onEvent = OnEvent;
            onFocus = OnFocus;
        }

        public void Start()
        {
            AutomationElement root = AutomationElement.RootElement;
            Automation.AddAutomationFocusChangedEventHandler(onFocus);
            // Window open and close need the desktop root (the window is not known before it opens). Everything else is
            // window of -SelfTest), while a registration on the window itself did.
            Automation.AddAutomationEventHandler(WindowPattern.WindowOpenedEvent, root, TreeScope.Subtree, onEvent);
            Automation.AddAutomationEventHandler(WindowPattern.WindowClosedEvent, root, TreeScope.Subtree, onEvent);
            Scan();
        }

        // Property changes are watched per window of the target process (not on the desktop root) to keep the load on
        // other applications low; new windows are picked up by WindowOpened and by this periodic scan.
        public void Scan()
        {
            foreach (IntPtr h in Win.TopLevels(pid, null, includeHidden))
            {
                lock (gate) { if (disposed || !watchedHwnds.Add(h.ToInt64())) continue; }
                try
                {
                    AutomationElement w = AutomationElement.FromHandle(h);
                    string rid = Uia.RuntimeId(w);
                    lock (gate) { if (rid != null) knownWindowIds.Add(rid); }
                    // One handler per window that drops events from elements of other windows, so that one change is
                    // written once even if UIA hands it to more than one registration (row 372 compares counts).
                    long own = h.ToInt64();
                    AutomationPropertyChangedEventHandler handler = (sender, args) =>
                    {
                        long top = Uia.TopHwndOf(sender as AutomationElement);
                        if (top != 0 && top != own) return;
                        OnProp(sender, args, own, top);
                    };
                    Automation.AddAutomationPropertyChangedEventHandler(w, TreeScope.Subtree, handler, Watched);
                    AutomationEventHandler events = (sender, args) =>
                    {
                        long top = Uia.TopHwndOf(sender as AutomationElement);
                        if (top != 0 && top != own) return;
                        OnEvent(sender, args);
                    };
                    Automation.AddAutomationEventHandler(AutomationElementIdentifiers.NotificationEvent, w, TreeScope.Subtree, events);
                    Automation.AddAutomationEventHandler(AutomationElementIdentifiers.LiveRegionChangedEvent, w, TreeScope.Subtree, events);
                    Automation.AddAutomationEventHandler(AutomationElement.MenuOpenedEvent, w, TreeScope.Subtree, events);
                    Write(new JObj().S("kind", "windowWatched").N("hwnd", h.ToInt64()).S("title", Win.TitleOf(h)).Raw("element", Uia.Snapshot(w).ToString()));
                }
                catch (Exception ex)
                {
                    Write(new JObj().S("kind", "error").S("where", "scan").S("error", ex.GetType().Name + ": " + ex.Message));
                }
            }
        }

        public bool ProcessAlive()
        {
            try { return !Process.GetProcessById(pid).HasExited; } catch (Exception) { return false; }
        }

        private bool Ours(AutomationElement e)
        {
            try { return e != null && e.Current.ProcessId == pid; } catch (Exception) { return false; }
        }

        private void OnFocus(object sender, AutomationFocusChangedEventArgs args)
        {
            var e = sender as AutomationElement;
            if (!Ours(e)) return;
            var o = new JObj().S("kind", "focus").Raw("element", Uia.Snapshot(e).ToString());
            AutomationElement w = Uia.WindowOf(e);
            if (w != null)
                o.Raw("window", new JObj().S("name", Uia.PropText(w, AutomationElement.NameProperty))
                                         .S("fullDescription", Uia.FullDescriptionOf(w))
                                         .S("helpText", Uia.PropText(w, AutomationElement.HelpTextProperty)).ToString());
            Write(o);
        }

        private void OnEvent(object sender, AutomationEventArgs args)
        {
            var e = sender as AutomationElement;
            int id = args.EventId.Id;
            if (id == WindowPattern.WindowClosedEvent.Id)
            {
                var closed = args as WindowClosedEventArgs;
                string rid = closed != null ? Uia.ValueText(closed.GetRuntimeId()) : null;
                bool known;
                lock (gate) { known = rid != null && knownWindowIds.Contains(rid); }
                if (known) Write(new JObj().S("kind", "windowClosed").S("runtimeId", rid));
                return;
            }
            if (!Ours(e)) return;
            if (id == AutomationElementIdentifiers.NotificationEvent.Id)
            {
                var o = new JObj().S("kind", "notification");
                var n = args as NotificationEventArgs;
                if (n != null)
                    o.S("displayString", n.DisplayString).S("activityId", n.ActivityId)
                     .S("notificationKind", n.NotificationKind.ToString()).S("notificationProcessing", n.NotificationProcessing.ToString());
                Write(o.Raw("element", Uia.Snapshot(e).ToString()));
            }
            else if (id == AutomationElementIdentifiers.LiveRegionChangedEvent.Id)
                Write(new JObj().S("kind", "liveRegionChanged").Raw("element", Uia.Snapshot(e).ToString()));
            else if (id == WindowPattern.WindowOpenedEvent.Id)
            {
                string rid = Uia.RuntimeId(e);
                lock (gate) { if (rid != null) knownWindowIds.Add(rid); }
                Write(new JObj().S("kind", "windowOpened").Raw("element", Uia.Snapshot(e).ToString()));
                // Do not register handlers from inside a UIA callback (the managed event manager holds its lock here);
                // the loop in RunUntil scans on its next 200 ms tick.
                rescan = true;
            }
            else if (id == AutomationElement.MenuOpenedEvent.Id)
                Write(new JObj().S("kind", "menuOpened").Raw("element", Uia.Snapshot(e).ToString()));
        }

        // via = the window whose registration delivered the change; top = the window that holds the element (0 = unknown).
        private void OnProp(object sender, AutomationPropertyChangedEventArgs args, long via, long top)
        {
            var e = sender as AutomationElement;
            var o = new JObj().S("kind", "propertyChanged").S("property", Automation.PropertyName(args.Property))
                              .S("old", Uia.ValueText(args.OldValue)).S("new", Uia.ValueText(args.NewValue))
                              .N("via", via).N("top", top);
            o.S("runtimeId", e != null ? Uia.RuntimeId(e) : null);
            o.S("name", e != null ? Uia.PropText(e, AutomationElement.NameProperty) : null);
            Write(o);
        }

        public void Write(JObj body)
        {
            string text = body.ToString();
            // Prefix t (ms since start) and wall (local time) to the object.
            string line = "{\"t\":" + clock.ElapsedMilliseconds.ToString(CultureInfo.InvariantCulture)
                        + ",\"wall\":" + Json.Str(DateTime.Now.ToString("yyyy-MM-ddTHH:mm:ss.fff", CultureInfo.InvariantCulture))
                        + (text.Length > 2 ? "," + text.Substring(1) : "}");
            string kind = ExtractKind(text);
            lock (gate)
            {
                if (disposed) return;
                int n;
                counts.TryGetValue(kind, out n);
                counts[kind] = n + 1;
                if (writer != null) { writer.WriteLine(line); writer.Flush(); }
                if (KeepLines) Lines.Add(line);
            }
        }

        private static string ExtractKind(string json)
        {
            const string key = "\"kind\":\"";
            int i = json.IndexOf(key, StringComparison.Ordinal);
            if (i < 0) return "?";
            int j = json.IndexOf('"', i + key.Length);
            return j < 0 ? "?" : json.Substring(i + key.Length, j - i - key.Length);
        }

        public string Counts()
        {
            var o = new JObj();
            lock (gate) { foreach (var kv in counts) o.N(kv.Key, kv.Value); }
            return o.ToString();
        }

        public long ElapsedMs { get { return clock.ElapsedMilliseconds; } }

        public List<string> CopyLines() { lock (gate) { return new List<string>(Lines); } }

        public void Dispose()
        {
            try { Automation.RemoveAllEventHandlers(); } catch (Exception) { }
            lock (gate)
            {
                disposed = true;
                if (writer != null) writer.Dispose();
            }
        }

        // Runs until seconds pass, the stop file appears, or the target process ends. Returns the summary JSON.
        public static string RunUntil(int pid, string outPath, int seconds, string stopFile, bool includeHidden)
        {
            string result = null;
            var t = new Thread(() =>
            {
                string reason = "seconds";
                using (var rec = new Recorder(pid, outPath, includeHidden))
                {
                    rec.Start();
                    rec.Write(new JObj().S("kind", "start").N("pid", pid).S("out", outPath));
                    long lastScan = 0;
                    while (true)
                    {
                        Thread.Sleep(200);
                        if (seconds > 0 && rec.ElapsedMs >= seconds * 1000L) { reason = "seconds"; break; }
                        if (!string.IsNullOrEmpty(stopFile) && File.Exists(stopFile)) { reason = "stopFile"; break; }
                        if (!rec.ProcessAlive()) { reason = "processExited"; break; }
                        if (rec.TakeRescan() || rec.ElapsedMs - lastScan >= 1000) { rec.Scan(); lastScan = rec.ElapsedMs; }
                    }
                    long elapsed = rec.ElapsedMs;
                    string counts = rec.Counts();
                    rec.Dispose();
                    result = new JObj().B("ok", true).S("mode", "record").N("pid", pid).S("out", outPath)
                                       .S("stopReason", reason).N("durationMs", elapsed).Raw("counts", counts).ToString();
                }
            });
            t.SetApartmentState(ApartmentState.MTA);
            t.Start();
            t.Join();
            return result;
        }
    }

    // In-process provider on a window that is never shown, used only by -SelfTest.
    public sealed class HiddenProvider : IRawElementProviderSimple
    {
        public IntPtr Hwnd;
        public volatile string Name = "cordmux-a11y-selftest";
        public const string Help = "selftest-help-text";
        public const string Description = "selftest-full-description";

        public ProviderOptions ProviderOptions { get { return ProviderOptions.ServerSideProvider; } }
        public object GetPatternProvider(int patternId) { return null; }
        public object GetPropertyValue(int id)
        {
            if (id == AutomationElementIdentifiers.NameProperty.Id) return Name;
            if (id == AutomationElementIdentifiers.ControlTypeProperty.Id) return ControlType.Pane.Id;
            if (id == AutomationElementIdentifiers.HelpTextProperty.Id) return Help;
            if (id == Uia.FullDescriptionId) return Description;
            if (id == AutomationElementIdentifiers.LiveSettingProperty.Id) return 1; // Polite
            if (id == AutomationElementIdentifiers.AutomationIdProperty.Id) return "cordmuxA11ySelfTest";
            if (id == AutomationElementIdentifiers.IsKeyboardFocusableProperty.Id) return true;
            return null;
        }
        public IRawElementProviderSimple HostRawElementProvider
        {
            get { return Hwnd == IntPtr.Zero ? null : AutomationInteropProvider.HostProviderFromHandle(Hwnd); }
        }
    }

    public static class SelfTest
    {
        private delegate IntPtr WndProc(IntPtr h, uint msg, IntPtr w, IntPtr l);

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct WNDCLASSEX
        {
            public uint cbSize; public uint style; public WndProc lpfnWndProc; public int cbClsExtra; public int cbWndExtra;
            public IntPtr hInstance; public IntPtr hIcon; public IntPtr hCursor; public IntPtr hbrBackground;
            public string lpszMenuName; public string lpszClassName; public IntPtr hIconSm;
        }
        [StructLayout(LayoutKind.Sequential)]
        private struct MSG { public IntPtr hwnd; public uint message; public IntPtr wParam; public IntPtr lParam; public uint time; public int x; public int y; }

        [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern ushort RegisterClassExW(ref WNDCLASSEX c);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern bool UnregisterClassW(string name, IntPtr inst);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr CreateWindowExW(uint ex, string cls, string title, uint style, int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);
        [DllImport("user32.dll")] private static extern IntPtr DefWindowProcW(IntPtr h, uint msg, IntPtr w, IntPtr l);
        [DllImport("user32.dll")] private static extern int GetMessageW(out MSG m, IntPtr h, uint min, uint max);
        [DllImport("user32.dll")] private static extern bool TranslateMessage(ref MSG m);
        [DllImport("user32.dll")] private static extern IntPtr DispatchMessageW(ref MSG m);
        [DllImport("user32.dll")] private static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
        [DllImport("user32.dll")] private static extern bool DestroyWindow(IntPtr h);
        [DllImport("user32.dll")] private static extern void PostQuitMessage(int code);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr GetModuleHandleW(string name);

        private const uint WM_GETOBJECT = 0x003D, WM_CLOSE = 0x0010, WM_DESTROY = 0x0002;
        private const uint WS_EX_TOOLWINDOW = 0x00000080, WS_EX_NOACTIVATE = 0x08000000;
        private const int UiaRootObjectId = -25;

        // Child side (-SelfTestProvider): owns a window that is never shown and raises events when the parent asks.
        // The provider runs in another process than the recorder, as OBS does (an in-process provider delivered the same
        // Name change twice through one registration in the first version of this test).
        // fault (only to prove that the checks can go red; empty in real use):
        //   silent       acknowledges "probed" and "raised" but raises nothing (the events really do not arrive)
        //   noLiveRegion raises everything except LiveRegionChanged (one kind missing)
        //   nameTwice    raises the Name change a second time DuplicateDelayMs after acknowledging (a late duplicate)
        //   late         acknowledges "raised" first and raises LateMs later (must stay green: the wait is deadline based)
        public const int LateMs = 3500;
        public const int DuplicateDelayMs = 1000;

        public static int Provide(string fault)
        {
            fault = fault ?? "";
            var provider = new HiddenProvider();
            IntPtr hwnd = IntPtr.Zero;
            var ready = new ManualResetEvent(false);
            string cls = "CordmuxA11ySelfTest" + Guid.NewGuid().ToString("N");
            WndProc proc = (h, msg, w, l) =>
            {
                if (msg == WM_GETOBJECT && (long)l == UiaRootObjectId)
                    return AutomationInteropProvider.ReturnRawElementProvider(h, w, l, provider);
                if (msg == WM_CLOSE) { DestroyWindow(h); return IntPtr.Zero; }
                if (msg == WM_DESTROY) { PostQuitMessage(0); return IntPtr.Zero; }
                return DefWindowProcW(h, msg, w, l);
            };
            IntPtr inst = GetModuleHandleW(null);
            var ui = new Thread(() =>
            {
                var wc = new WNDCLASSEX();
                wc.cbSize = (uint)Marshal.SizeOf(typeof(WNDCLASSEX));
                wc.lpfnWndProc = proc;
                wc.hInstance = inst;
                wc.lpszClassName = cls;
                RegisterClassExW(ref wc);
                // No WS_VISIBLE and never ShowWindow: the window is never drawn, never activated, never in the taskbar.
                hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls, "cordmux-a11y-selftest", 0, -32000, -32000, 10, 10,
                                       IntPtr.Zero, IntPtr.Zero, inst, IntPtr.Zero);
                provider.Hwnd = hwnd;
                ready.Set();
                MSG m;
                while (GetMessageW(out m, IntPtr.Zero, 0, 0) > 0) { TranslateMessage(ref m); DispatchMessageW(ref m); }
                UnregisterClassW(cls, inst);
                GC.KeepAlive(proc);
            });
            ui.SetApartmentState(ApartmentState.STA);
            ui.IsBackground = true;
            ui.Start();
            ready.WaitOne(5000);
            Console.Out.WriteLine("hwnd " + hwnd.ToInt64().ToString(CultureInfo.InvariantCulture));
            Console.Out.Flush();
            string line;
            bool silent = fault == "silent";
            while ((line = Console.In.ReadLine()) != null)
            {
                if (line == "quit") break;
                if (line == "probe")
                {
                    // Readiness probe: MenuOpened is none of the three judged kinds and is the last kind Recorder.Scan
                    // registers, so seeing it means the path from this process to the recorder is live.
                    if (!silent)
                        AutomationInteropProvider.RaiseAutomationEvent(AutomationElement.MenuOpenedEvent, provider,
                            new AutomationEventArgs(AutomationElement.MenuOpenedEvent));
                    Console.Out.WriteLine("probed");
                    Console.Out.Flush();
                    continue;
                }
                if (line != "raise") continue;
                if (fault == "late")
                {
                    Console.Out.WriteLine("raised");
                    Console.Out.Flush();
                    Thread.Sleep(LateMs);
                }
                if (!silent)
                {
                    AutomationInteropProvider.RaiseAutomationEvent(AutomationElementIdentifiers.NotificationEvent, provider,
                        new NotificationEventArgs(AutomationNotificationKind.Other, AutomationNotificationProcessing.All, "selftest-display", "selftest-activity"));
                    if (fault != "noLiveRegion")
                        AutomationInteropProvider.RaiseAutomationEvent(AutomationElementIdentifiers.LiveRegionChangedEvent, provider,
                            new AutomationEventArgs(AutomationElementIdentifiers.LiveRegionChangedEvent));
                    string old = provider.Name;
                    provider.Name = "cordmux-a11y-selftest-renamed";
                    AutomationInteropProvider.RaiseAutomationPropertyChangedEvent(provider,
                        new AutomationPropertyChangedEventArgs(AutomationElementIdentifiers.NameProperty, old, provider.Name));
                    AutomationInteropProvider.RaiseAutomationEvent(AutomationElementIdentifiers.AutomationFocusChangedEvent, provider,
                        new AutomationFocusChangedEventArgs(0, 0));
                }
                if (fault != "late")
                {
                    Console.Out.WriteLine("raised");
                    Console.Out.Flush();
                }
                if (fault == "nameTwice")
                {
                    Thread.Sleep(DuplicateDelayMs);
                    AutomationInteropProvider.RaiseAutomationPropertyChangedEvent(provider,
                        new AutomationPropertyChangedEventArgs(AutomationElementIdentifiers.NameProperty, "cordmux-a11y-selftest", provider.Name));
                }
            }
            if (hwnd != IntPtr.Zero) PostMessageW(hwnd, WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
            ui.Join(3000);
            return 0;
        }

        // Parent side (-SelfTest): starts the child (this script with -SelfTestProvider, no console window), reads its
        // element and records its events with the same code as -Tree and -Record.
        public static string Run(string scriptPath, string fault)
        {
            string result = null;
            var t = new Thread(() => { result = RunMta(scriptPath, fault ?? ""); });
            t.SetApartmentState(ApartmentState.MTA);
            t.Start();
            t.Join();
            return result;
        }

        // Waits (deadline based, not fixed sleeps). The old fixed waits (500 ms before the raise, 1500 ms after it)
        // saw 0 events once while builds kept the PC busy: the recorder's registration had taken 4.2 s
        // instead of 2.1 s, and either the delivery was slower than 1.5 s or the subscription was not live yet when the
        // child raised (then the events are lost and no later wait brings them back). The probe covers the second case,
        // the arrival deadline the first. Measured with and without builds running: the first
        // delivery after the registration was held back for up to 9.7 s (every probe raised meanwhile arrived at once
        // afterwards, none was lost); once one had arrived, the measured events mostly took under 20 ms (one run 415 ms).
        // The probe deadline leaves about 3 times
        // the longest hold seen; a red run (nothing ever arrives) ends after about 45 s.
        public const int ProbeDeadlineMs = 30000;   // keep re-raising the probe until the recorder sees one
        public const int ProbeIntervalMs = 500;     // between probe raises
        public const int EventDeadlineMs = 10000;   // from sending "raise" until all three judged kinds have arrived
        public const int SettleFloorMs = 1500;      // after the last arrival, so that a duplicate is still counted
        public const int AckTimeoutMs = 10000;      // for each "probed" / "raised" line of the child
        private const int PollMs = 50;

        // Reads the child's stdout on one thread into a queue, so that a timed-out wait does not leave a pending
        // ReadLineAsync behind (the next ReadLineAsync on the same stream would throw).
        private sealed class LineReader
        {
            private readonly System.Collections.Concurrent.BlockingCollection<string> queue =
                new System.Collections.Concurrent.BlockingCollection<string>();

            public LineReader(StreamReader reader)
            {
                var t = new Thread(() =>
                {
                    try
                    {
                        string line;
                        while ((line = reader.ReadLine()) != null) queue.Add(line);
                    }
                    catch (Exception) { }
                    finally { queue.CompleteAdding(); }
                });
                t.IsBackground = true;
                t.Start();
            }

            // null on timeout or when the child closed its stdout.
            public string Next(int timeoutMs)
            {
                string line;
                try { return queue.TryTake(out line, timeoutMs) ? line : null; }
                catch (Exception) { return null; }
            }
        }

        private static int CountKind(List<string> lines, string kind)
        {
            int n = 0;
            foreach (string l in lines) if (l.Contains("\"kind\":\"" + kind + "\"")) ++n;
            return n;
        }

        // t (ms on the recorder's clock) of the first line of a kind, or -1.
        private static long FirstT(List<string> lines, string kind)
        {
            foreach (string l in lines)
            {
                if (!l.Contains("\"kind\":\"" + kind + "\"")) continue;
                const string key = "{\"t\":";
                if (!l.StartsWith(key, StringComparison.Ordinal)) return -1;
                int end = l.IndexOf(',', key.Length);
                long t;
                if (end > 0 && long.TryParse(l.Substring(key.Length, end - key.Length), NumberStyles.Integer, CultureInfo.InvariantCulture, out t))
                    return t;
                return -1;
            }
            return -1;
        }

        private static string RunMta(string scriptPath, string fault)
        {
            var checks = new List<string>();
            string events = "[]";
            var waits = new JObj();
            bool allOk = true;
            int childPid = 0;
            var total = Stopwatch.StartNew();
            var psi = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "WindowsPowerShell\\v1.0\\powershell.exe"),
                "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + scriptPath + "\" -SelfTestProvider"
                + (fault.Length > 0 ? " -SelfTestFault " + fault : ""));
            psi.UseShellExecute = false;
            psi.CreateNoWindow = true;
            psi.RedirectStandardInput = true;
            psi.RedirectStandardOutput = true;
            psi.RedirectStandardError = true;
            using (Process child = Process.Start(psi))
            {
                childPid = child.Id;
                var stdout = new LineReader(child.StandardOutput);
                try
                {
                    string first = stdout.Next(60000);
                    long hv = 0;
                    bool created = first != null && first.StartsWith("hwnd ") && long.TryParse(first.Substring(5), out hv) && hv != 0;
                    allOk &= Add(checks, "hiddenWindowCreated", created, first);
                    if (created)
                    {
                        var hwnd = new IntPtr(hv);
                        // 1. Values round-trip through the same reader as -Tree.
                        AutomationElement e = AutomationElement.FromHandle(hwnd);
                        string name = Uia.PropText(e, AutomationElement.NameProperty);
                        string help = Uia.PropText(e, AutomationElement.HelpTextProperty);
                        string full = Uia.FullDescriptionOf(e);
                        string live = Uia.LiveSettingOf(e);
                        allOk &= Add(checks, "nameRoundTrip", name == "cordmux-a11y-selftest", name);
                        allOk &= Add(checks, "helpTextRoundTrip", help == HiddenProvider.Help, help);
                        allOk &= Add(checks, "fullDescriptionRoundTrip", full == HiddenProvider.Description, full);
                        allOk &= Add(checks, "liveSettingRoundTrip", live == "Polite", live);
                        string tree = TreeDump.Dump(childPid, "cordmux-a11y-selftest", false, true, 5, 50);
                        allOk &= Add(checks, "treeHasFullDescription", tree.Contains(HiddenProvider.Description), tree.Length.ToString() + " chars");

                        // 2. Events through the same registration as -Record (desktop root, Subtree, filtered by process id).
                        using (var rec = new Recorder(childPid, null, true))
                        {
                            rec.KeepLines = true;
                            rec.Start();

                            // 2a. Readiness: re-raise the probe until the recorder sees one (or ProbeDeadlineMs).
                            var probeClock = Stopwatch.StartNew();
                            int probeRaises = 0;
                            long probeSeenMs = -1;
                            while (probeSeenMs < 0 && probeClock.ElapsedMilliseconds < ProbeDeadlineMs)
                            {
                                child.StandardInput.WriteLine("probe");
                                child.StandardInput.Flush();
                                if (stdout.Next(AckTimeoutMs) != "probed") break; // the child is stuck; the checks below show it
                                ++probeRaises;
                                long next = probeClock.ElapsedMilliseconds + ProbeIntervalMs;
                                while (probeClock.ElapsedMilliseconds < next)
                                {
                                    if (CountKind(rec.CopyLines(), "menuOpened") > 0) { probeSeenMs = probeClock.ElapsedMilliseconds; break; }
                                    Thread.Sleep(PollMs);
                                }
                            }
                            waits.N("probeRaises", probeRaises).N("probeSeenMs", probeSeenMs).N("probeDeadlineMs", ProbeDeadlineMs);

                            // 2b. One measured raise; wait until all three judged kinds arrived (or EventDeadlineMs).
                            long raiseT = rec.ElapsedMs;
                            child.StandardInput.WriteLine("raise");
                            child.StandardInput.Flush();
                            string raised = stdout.Next(AckTimeoutMs);
                            allOk &= Add(checks, "childRaised", raised == "raised", raised);
                            string[] judged = { "notification", "liveRegionChanged", "propertyChanged" };
                            List<string> lines;
                            while (true)
                            {
                                lines = rec.CopyLines();
                                bool allArrived = true;
                                foreach (string k in judged) if (FirstT(lines, k) < 0) { allArrived = false; break; }
                                if (allArrived || rec.ElapsedMs - raiseT >= EventDeadlineMs) break;
                                Thread.Sleep(PollMs);
                            }
                            long last = -1;
                            bool every = true;
                            foreach (string k in judged)
                            {
                                long t = FirstT(lines, k);
                                long ms = t < 0 ? -1 : Math.Max(0, t - raiseT);
                                waits.N(k + "Ms", ms);
                                if (ms < 0) every = false; else last = Math.Max(last, ms);
                            }
                            waits.N("eventDeadlineMs", EventDeadlineMs);
                            // 2c. Settle, so that a duplicate is still counted. A fixed wait was not enough: a Name change
                            // raised 1.0 s after the first arrived 1.0 to 2.3 s after it (round 18). Instead the child
                            // raises one more probe (a fence) after everything it raised for "raise"; every run so far
                            // delivered the child's events in the order raised, so the fence arriving means the rest has
                            // arrived. The floor (the old fixed wait, from the last judged arrival) guards against a
                            // reordered delivery. Not needed when a kind is missing: the deadline already passed.
                            long fenceSeenMs = -1;
                            var settleClock = Stopwatch.StartNew();
                            if (every)
                            {
                                var fenceClock = Stopwatch.StartNew();
                                child.StandardInput.WriteLine("probe");
                                child.StandardInput.Flush();
                                if (stdout.Next(AckTimeoutMs) == "probed")
                                {
                                    ++probeRaises;
                                    while (fenceClock.ElapsedMilliseconds < EventDeadlineMs)
                                    {
                                        if (CountKind(rec.CopyLines(), "menuOpened") >= probeRaises) { fenceSeenMs = fenceClock.ElapsedMilliseconds; break; }
                                        Thread.Sleep(PollMs);
                                    }
                                }
                                long floorEnd = raiseT + last + SettleFloorMs;
                                long rest = floorEnd - rec.ElapsedMs;
                                if (rest > 0) Thread.Sleep((int)rest);
                            }
                            waits.N("fenceSeenMs", fenceSeenMs).N("settleMs", every ? settleClock.ElapsedMilliseconds : 0);
                            lines = rec.CopyLines();
                            events = Json.Arr(lines);
                            string all = string.Join("\n", lines);
                            allOk &= Add(checks, "notificationReceived",
                                Count(lines, "notification") == "1" && all.Contains("selftest-display") && all.Contains("selftest-activity"),
                                Count(lines, "notification"));
                            allOk &= Add(checks, "liveRegionChangedReceived", Count(lines, "liveRegionChanged") == "1", Count(lines, "liveRegionChanged"));
                            // Exactly once: row 372 counts how many Name changes one toggle raises.
                            allOk &= Add(checks, "nameChangedReceivedOnce",
                                Count(lines, "propertyChanged") == "1" && all.Contains("cordmux-a11y-selftest-renamed"),
                                Count(lines, "propertyChanged"));
                            allOk &= Add(checks, "windowWatched", all.Contains("\"kind\":\"windowWatched\""), Count(lines, "windowWatched"));
                            // Focus cannot be guaranteed on a window that is never shown; recorded, not judged.
                            checks.Add(new JObj().S("name", "focusReceived (info)").B("ok", all.Contains("\"kind\":\"focus\""))
                                                 .S("detail", Count(lines, "focus")).ToString());
                            rec.Dispose();
                        }
                    }
                }
                catch (Exception ex)
                {
                    allOk = false;
                    checks.Add(Check("exception", false, ex.GetType().Name + ": " + ex.Message));
                }
                finally
                {
                    try { child.StandardInput.WriteLine("quit"); child.StandardInput.Close(); } catch (Exception) { }
                    bool exited = child.WaitForExit(10000);
                    if (!exited) { try { child.Kill(); } catch (Exception) { } }
                    allOk &= Add(checks, "childExited", exited, exited ? "exit " + child.ExitCode.ToString() : "killed");
                }
            }
            waits.N("totalMs", total.ElapsedMilliseconds);
            var o = new JObj().B("ok", allOk).S("mode", "selftest").N("childPid", childPid);
            // A run with a fault is never a pass of the check before the live window; say so in the result itself.
            if (fault.Length > 0) o.S("fault", fault);
            return o.Raw("checks", Json.Arr(checks)).Raw("waits", waits.ToString()).Raw("events", events).ToString();
        }

        private static string Count(List<string> lines, string kind)
        {
            int n = 0;
            foreach (string l in lines) if (l.Contains("\"kind\":\"" + kind + "\"")) ++n;
            return n.ToString();
        }

        private static string Check(string name, bool ok, string detail)
        {
            return new JObj().S("name", name).B("ok", ok).S("detail", detail).ToString();
        }

        private static bool Add(List<string> checks, string name, bool ok, string detail)
        {
            checks.Add(Check(name, ok, detail));
            return ok;
        }
    }
}
"@

try {
  Add-Type -AssemblyName UIAutomationClient
  Add-Type -AssemblyName UIAutomationTypes
  Add-Type -AssemblyName UIAutomationProvider
  Add-Type -AssemblyName WindowsBase
  Add-Type -TypeDefinition $source -ReferencedAssemblies UIAutomationClient, UIAutomationTypes, UIAutomationProvider, WindowsBase

  switch ($mode) {
    'tree' {
      $json = [CordmuxA11y.TreeDump]::Dump($TargetPid, $Title, [bool]$RawView, [bool]$IncludeHidden, $MaxDepth, $MaxNodes)
      if ([string]::IsNullOrEmpty($Out)) {
        Write-Utf8AndExit -Text $json -ExitCode 0
      }
      $outDir = Split-Path -Parent ([System.IO.Path]::GetFullPath($Out))
      if (-not [string]::IsNullOrEmpty($outDir)) { [void][System.IO.Directory]::CreateDirectory($outDir) }
      [System.IO.File]::WriteAllText($Out, $json, (New-Object System.Text.UTF8Encoding($false)))
      # 木は大きいので、標準出力には要約だけを書く。
      $parsed = $json | ConvertFrom-Json
      $summary = [PSCustomObject]@{
        ok = $true; mode = 'tree'; pid = $TargetPid; out = $Out
        windowCount = $parsed.windowCount; nodeCount = $parsed.nodeCount; truncated = $parsed.truncated
      } | ConvertTo-Json -Compress
      Write-Utf8AndExit -Text $summary -ExitCode 0
    }
    'record' {
      $json = [CordmuxA11y.Recorder]::RunUntil($TargetPid, $Out, $Seconds, $StopFile, [bool]$IncludeHidden)
      Write-Utf8AndExit -Text $json -ExitCode 0
    }
    'selftest' {
      $json = [CordmuxA11y.SelfTest]::Run($PSCommandPath, $SelfTestFault)
      $code = 1
      if (($json | ConvertFrom-Json).ok) { $code = 0 }
      Write-Utf8AndExit -Text $json -ExitCode $code
    }
    'provider' {
      # 標準出力は親との約束 (hwnd の行と raised の行) にだけ使う。
      exit ([CordmuxA11y.SelfTest]::Provide($SelfTestFault))
    }
  }
} catch {
  Write-ErrorAndExit -Mode $mode -Message ($_.Exception.GetType().Name + ': ' + $_.Exception.Message)
}
