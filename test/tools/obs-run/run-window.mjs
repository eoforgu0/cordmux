#!/usr/bin/env node
// 実機テストの実行スクリプトの実行側。手順そのもの (何をどの順で行うか) は
// run-window-plan.mjs (純粋関数) が決める。このファイルはその手順を fs / child_process /
// obsws.mjs を使って実際に行うだけの薄い層。
//
// OBS の起動と閉じ方は、まず穏やかに CloseMainWindow を数回送り、閉じなければ強制終了し、
// finally で必ず閉じる。2 回目以降の起動では、OBS のログの新規行だけを logsBefore との差分で拾う。
//
// 使い方:
//   node run-window.mjs --obs <OBS のフォルダ> --out <出力フォルダ> --plugin <cordmux install の
//     フォルダ (cordmux.dll と data を持つ)> --step place|through|integration|guard|second-obs|discord-first|dock-view|mic-check|call-gate-a|call-gate-b|call-gate-c|call-gate-d|call-1|record|webcodecs-check|hold|call-hold
//     [--call-window] [--real-profile] [--allow-state-json-write] [--decoder software|hardware]
//     [--call-minutes 20]
//     [--broadcaster-id <メインアカウントの ID>] (--step call-1 だけ。サブアカウントを使う構成の手順を足す。値は実行時にだけ渡す)
//     [--call-accounts <JSON ファイル>] (--step call-1 だけ。実際の通話の実行では必須。ドライランとリハーサルは省くと例の名前を使う。
//        キーは main / receiver / chrome / firefox で、値は Discord に見えている表示名。README を見る)
//     [--rehearsal] (--step call-1 と call-hold だけ。実機の Vesktop の代わりにテスト用の送信側で新しい手順を一度通す。
//        --real-profile / --call-window と両立しない = 差し替えた APPDATA にテスト用の state.json を作る)
//     [--browser-profile <パス> (繰り返し可)] [--shared-content-dir <パス> (繰り返し可)]
//     [--temp-vencord-build <パス>]
//       (--step call-1 だけ。実行スクリプトが実行のために用意したものの保存先を
//        _work/call-window-artifacts.json へ書き、cleanup.ps1 -CallWindow がそれを消す)
//     [--n-screen 4] [--n-voice 8] [--seconds 180] [--warmup 0|1 (既定 1)]
//     [--hw prefer-hardware|prefer-software]
//     [--integration-config default|one-screen|one-screen-one-voice]
//     [--dock-scenario none|connected|no-scene|degraded|contact-missing|idle-10min|select-four|dummy|export-bundle (--step dock-view だけが見る)]
//     [--dock-max-long-edge 640 (--dock-scenario degraded だけが見る。HARNESS_MAX_LONG_EDGE として渡す)]
//     [--dock-install elsewhere|missing|ours|absent (既定 elsewhere。全 --step の seed-config が見る)]
//     [--assign-mode manual|auto (既定 manual = 製品の既定。全 --step の seed-config が
//       settings.json の assign_mode に書く)]
//     [--stream] [--deadline-min 120 (call-1 の実際の通話の既定は、実行計画からの見積もり deadlineEstimate と 180 の大きいほう)] [--tag <名前>] [--allow-unknown-encoder] [--dry-run]
//   node run-window.mjs --out <出力フォルダ> --step install-roundtrip --tag <名前> --zip <zip ファイルの zip>
//     --obs-zip <OBS のポータブルの zip> [--scenario <場面>] [--layout plugins|programdata --vm]
//     [--old-zip <前のバージョンの zip>] [--test-dll <テスト用の DLL>] [--keep-work] [--dry-run]
//       (--obs と --plugin は取らない)
//
// --dry-run: 実行計画の種類のうち下の switch にないもの (unhandledKinds)、ini から
// 引けないコンポーネントの id (missingTextIds)、seed-config.py が書く値 (seedPreview。同じ引数に --dry-run を足して
// 聞く) を出し、前の 2 つが空でないか seed-config.py が断ったら終了コード 1。
//
// --dry-run: 先頭のガード (tasklist / nvidia-smi。読むだけなので実行してよい) と実行計画を JSON で
// 表示するだけで終わる。OBS も harness も起動せず、--out にも何も書かない。cleanup.ps1 は
// -DryRun -Json で実行する (何もしないことの確認だけ)。**--real-profile のときだけ cleanup.ps1 も
// 呼ばない**: cleanup.ps1 は -DryRun でも %APPDATA%\Vesktop\state.json と
// %LOCALAPPDATA%\cordmux\session.json の存在を見に行くので、準備だけの dry-run が
// 実際の保存先を読みに行くことになる。
//
// 終了コード: 0 = 成功、1 = 引数か実行計画の誤り、または予期しない例外で手順を最後まで
// 進められなかった、2 = 先頭のガードに引っかかって始めなかった、
// 3 = **判定が失敗** (ゲートは自分の終了コードで失敗を返す。実行計画の gate:true の
// 手順のどれかが ok:false だった、または期限で打ち切って判定が付かなかった)。
//
// harness (test/tools/harness) の product モードのパラメータ渡しは HARNESS_* の環境変数を使う
// (既存の sustain/send モードの命名に揃えた)。product モード自体は test/tools/harness (sender.ts) 側で実装している。
import { execFileSync, spawn, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { ObsWs, OBS_EVENT_SUBSCRIPTION_INPUT_VOLUME_METERS } from "./obsws.mjs";
import { createStreamSink, summarizeStaleCheck } from "./stream-sink.mjs";
import {
  APPDATA_OVERRIDE_REL,
  BUILD_MARK_FILE_NAME,
  DEFAULT_GUARDED_PROCESS_NAMES,
  PROTECTED_PROCESSES_ENV,
  INSTALL_ROUNDTRIP_STEP,
  JA_LOCALE_INI_REL,
  REMOVAL_TABLE_REL,
  SCENE_NAMES_HEADER_REL,
  judgeFfmpegModules,
  judgeRoundtrip,
  obsDepsBinRelFromBuildspec,
  roundtripPaths,
  CALL_WINDOW_ARTIFACTS_REL,
  DEFAULT_DECODER,
  DEFAULT_DOCK_ASSIGN_MODE,
  DEFAULT_DOCK_INSTALL,
  HUMAN_GO_FILE_NAME,
  HOLD_INFO_FILE_PREFIX,
  HUMAN_POLL_MS,
  HUMAN_TIMEOUT_MS,
  judgeAvDrift,
  judgeVencordDirRestored,
  judgeBundleZipFresh,
  HARNESS_USER_DATA_DIR_NAME,
  harnessElectronArgs,
  judgeVencordSettingsRestored,
  plannedMinutes,
  deadlineShortfallMinutes,
  HANDSHAKE_DIR_REL,
  LOCAL_APPDATA_OVERRIDE_REL,
  vesktopDirSwapProblem,
  OBSWS_CONTROL_PORT,
  OBS_LAUNCH_ARGS,
  keepsRecordings,
  STREAM_SINK_MAX_REARMS,
  STREAM_SINK_PORT,
  STREAM_SINK_URL,
  SECOND_OBS_EMPTY_SCENE_COLLECTION,
  SESSION_JSON_REL,
  STATE_JSON_BEFORE_NAME,
  VENCORD_DIST_REL,
  blockersFromProcessList,
  loadProtectedProcesses,
  encoderUsedOnlyByCallApps,
  buildGapFrames,
  leadingGapFrames,
  countConnectionEvents,
  deadlineExceeded,
  encodeFactsFromHarnessData,
  encoderBusy,
  filesToPlace,
  guardBlocked,
  parseEncoderSessionPids,
  judgeCompositeCount,
  judgeAc31,
  fillTextArgs,
  judgeBackpressure,
  judgeConnectionHeld,
  judgeModuleLoaded,
  judgeModuleNotLoaded,
  judgeGuideUpdateHint,
  isObsNotReadyError,
  recordMarkOf,
  judgeOperationCount,
  judgeOutputSkipped,
  judgeVencordDirWritten,
  mergeCallWindowArtifacts,
  callAccountsProblem,
  parseArgs,
  parseCallAccounts,
  pickNewResultFiles,
  planSteps,
  // --obs と --out の保存先のガード。
  workPathProblems,
  ALLOW_OUTSIDE_WORK_FLAG,
  // 枠の区画の判定・音の切り分け・映像の数・OBS の録画を残す手順・期限の見積もり。
  cellRegionsForJudge,
  // 画面のテキストの解決 (ini の id と textRef)、cordmux のソースを種類の id と slot で探すツール、
  // 実行ごとのフォルダーの並び。
  CORDMUX_SOURCE_KINDS,
  OBS_TEXT_PREFIX,
  // OBS 自身のテキストを en-US.ini にフォールバックする順序と、Qt のアクセシブル名から & を外す処理。
  OBS_LOCALE_FALLBACK_ORDER,
  lookupObsText,
  stripQtMnemonic,
  RECORD_MATERIALS_DIR_NAME,
  SELF_MIC_INPUT_NAME,
  SELF_MIC_INPUT_KIND,
  TEXT_ID_FILE_NAME_FORMAT,
  TEXT_ID_FILE_SCREEN_SHARE,
  classifyCordmuxInputs,
  expandTextRefsDeep,
  findCordmuxInput,
  screenOutputFileNames,
  sourceRefLabel,
  stepTextIds,
  deadlineEstimate,
  deadlineMinFor,
  deadlineEstimateOptsFor,
  judgeCellArgs,
  obsRecordingMoves,
  unlabeledObsRecordings,
  STEPS_KEEPING_UNLABELED_OBS_RECORDINGS,
  HARNESS_FRAMES_PAUSE_ENV,
  FRAMES_PAUSE_FILE_PREFIX,
  FRAMES_PAUSE_FILE_SUFFIX,
  HARNESS_VOICE_TONE_ENV,
  VOICE_TONE_FILE_PREFIX,
  VOICE_TONE_FILE_SUFFIX,
  screenImageFiles,
  parseVideoRateSamples,
  STEPS_KEEPING_OBS_RECORDINGS,
  VIDEO_RATE_WINDOW_LINES,
  WIRE_SCENE_NAME,
  // 実際の通話の --extra-checks (CALL_ONE_OPTION_FLAGS)。
  judgeConnectionsKept,
  judgeVoiceSourceSwitch,
  judgeVoiceVolume,
  judgeRosterLabels,
  judgeSelfAccount,
  judgeLogLines,
  judgeInputLevel,
  rosterLabelReadyFromLog,
  selfSpeakingStatsFromLog,
  selfSourceEventsFromLog,
  judgeSelfSpeakingLog,
  pickPersonScreenOutput,
  judgeCallLog,
  // 実際の通話の判定、Vencord のフォルダーと設定の復元、遅延の測定。
  AC30_ITEM_LABELS,
  REDACTED_BROADCASTER_ID,
  STOP_FILE_NAME,
  VENCORD_DIR_FINAL_PREFIX,
  VENCORD_SETTINGS_AFTER_NAME,
  VENCORD_SETTINGS_BEFORE_NAME,
  VENCORD_SETTINGS_REAL_REL,
  VESKTOP_STATE_JSON_REAL_REL,
  ac32Windows,
  argsForResult,
  cleanupFlags,
  finallyVencordRestorePlan,
  judgeAc32,
  judgeAvSync,
  judgeAvSyncLines,
  judgeLatency,
  judgeLinesSummary,
  keySendingSteps,
  latencyReferenceRect,
  latencyReferenceTransform,
  pickMonitorItem,
  windowFillsMonitor,
  latencyRegions,
  latencyToolArgs,
  settingsKeyDiff,
  toneScanWindowsArg,
  toneWindowValues,
  vencordDirFinalCheck,
  voiceSourceScanWindow,
  // 合成のソースの手順。
  COMPOSITE_INPUT_NAME,
  audioBufferingFromLog,
  compositeCellRegions,
  judgeAudioBuffering,
  judgeCompositeLayout,
  judgeDisplayDelay,
  harnessPersonsFromSlotStreams,
  lastCompositeLayout,
  screenSlotStreamsFromLines,
  videoLatencyShownFromLog,
  // 遅延の測定で見えたセルの集計、メインアカウントの ID の伏せ字。
  latencyCellsSeen,
  redactBroadcasterId,
  TEXT_ID_LOOK_FIELD_IN_GROUP,
  recordSessionsRoot,
  isRecordSessionDirName,
} from "./run-window-plan.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = path.resolve(HERE, "..", "..", "..");
const HARNESS_DIR = path.resolve(HERE, "..", "harness");
const CLEANUP_PS1 = path.join(HERE, "cleanup.ps1");
// 残った配信の受信側を見つけて止めるツール (stream-sink-start と cleanup.ps1 が呼ぶ)。
const STREAM_SINK_STALE_PS1 = path.join(HERE, "stream-sink-stale.ps1");
const SCREENSHOT_PS1 = path.join(HERE, "screenshot.ps1");
const WINDOW_RECT_PS1 = path.join(HERE, "window-rect.ps1");
const WINDOW_SIZE_PS1 = path.join(HERE, "window-size.ps1");
// ドックの確認 (自動配置・ダミー映像・診断情報の zip) を、人の操作なしで実行スクリプトが
// 行うためのツール (dock-click.ps1 の README.md を見よ)。
const DOCK_CLICK_PS1 = path.join(HERE, "dock-click.ps1");
const DIALOG_SENDKEYS_PS1 = path.join(HERE, "dialog-sendkeys.ps1");
const DUMMY_TILE_PY = path.join(HERE, "dummy_tile.py");
const CHECK_BUNDLE_ZIP_PY = path.join(HERE, "check_bundle_zip.py");
const TONE_SCAN_PY = path.join(HERE, "tone_scan.py");
const PLATE_MEASURE_PY = path.join(HERE, "plate_measure.py");
// --step mic-check が呼ぶツール。test/tools/mic-check の README と
// test/tools/obs-run/README.md を見よ。
const MIC_CHECK_RUN_MJS = path.join(HERE, "..", "mic-check", "run.mjs");
// --step webcodecs-check が呼ぶツール。
// mic-check と同じ形 (別の Electron を短く起動し、結果 JSON を読んで判定する)。
const WEBCODECS_CHECK_RUN_MJS = path.join(HERE, "..", "webcodecs-check", "run.mjs");
// 録画ファイルの作成の待ちで「増えなくなった」と判断するまでのポーリングの回数 (既定の 2 秒間隔で
// 10 秒ぶん)。
const FINALIZE_STABLE_POLLS = 5;
// OBS 側の録画の fps (seed-config.py の basic.ini の FPSCommon=60)。対の判定でフレーム番号の
// 範囲を作るのに使う。
const OBS_RECORD_FPS = 60;
// 対の判定の範囲の前後に置く余裕 (ms)。ボタンを押してから枠の画像が変わるまで、
// テスト用の送信側が終わってから枠が黒くなるまでの間を、どちらの側にも入れない。
const OBS_BLACK_JUDGE_MARGIN_MS = 3000;
// harness (テスト用の送信側) の product モードが結果 JSON を書く場所 (test/tools/harness/main.js の
// PRODUCT_OUT_DIR と同じ場所。_work/out)。
const PRODUCT_OUT_DIR = path.join(REPO_ROOT, "_work", "out");

const USAGE =
  "使い方: node run-window.mjs --obs <OBS> --out <出力> --plugin <cordmux install> --step place|through|integration|guard|second-obs|discord-first|dock-view|mic-check|call-gate-a|call-gate-b|call-gate-c|call-gate-d|call-1|record|webcodecs-check|hold|call-hold " +
  "[--call-window] [--real-profile] [--allow-state-json-write] [--decoder software|hardware] [--call-minutes 20] " +
  "[--broadcaster-id <メインアカウントの ID> (call-1 だけ)] [--rehearsal (call-1 と call-hold だけ。--real-profile / --call-window と両立しない)] " +
  "[--call-accounts <アカウントの名前の JSON ファイル> (call-1 だけ。ドライランとリハーサル以外では必須)] " +
  "[--display-buffer-ms <0-1000> (製品の既定 100。合成のソースがあるときに適用される)] " +
  "[--n-screen 4] [--n-voice 8] [--seconds 180] [--warmup 0|1] [--hw prefer-hardware|prefer-software] " +
  "[--integration-config default|one-screen|one-screen-one-voice] " +
  "[--dock-scenario none|connected|no-scene|degraded|contact-missing|idle-10min|select-four|dummy|export-bundle] " +
  "[--dock-max-long-edge 640] " +
  "[--dock-install elsewhere|missing|ours|absent] " +
  "[--assign-mode manual|auto] " +
"[--hold-minutes 30 (hold と call-hold)] [--no-seed] [--screen-sizes <幅>x<高さ>,... (--n-screen と)] [--keep-recordings] [--replay-buffer] [--self-mic (音声入力キャプチャを作り、自分の枠の音声ソースにして始める)] (この 5 つは hold だけ) " +
  "[--screen-names <表示名>,... (--n-screen と)] [--screen-images <画像>,... (--n-screen と)] [--voice-users <userId>,... (--n-voice と)] [--label-em-ratio-pct 1.0..8.5] [--wire-composite] (この 5 つも hold だけ。README の「実際の OBS の画面を撮る」) " +
  "[--stream-sink (hold と call-hold。同梱の ffmpeg で 127.0.0.1:1935 の配信を受ける)] " +
  "[--obs-language ja-JP|en-US (hold だけ)] [--obs-theme <OBS のテーマの id>] [--obs-font-scale 8..12] [--obs-density 1|-2|-3|-4|-5] [--obs-hdr] [--obs-full-height-docks on|off] (見た目は hold と dock-view) " +
  "[--start-with-add] [--vesktop-share] [--skip-continuous] [--skip-av2] [--skip-speaker-mute] [--extra-checks] [--skip-export-bundle] [--latency-person <名前>] [--voice-channel <ボイスチャンネルの名前>] (この 9 つは call-1 だけ。README の「call-1 の旗」) " +
  "[--stream] [--deadline-min 120 (call-1 の既定は実行計画からの見積もりと 180 の大きいほう)] [--tag <名前>] [--allow-unknown-encoder] [--dry-run] " +
  "[--allow-outside-work (--obs を repo の _work の外、--out を _work と build_x64 の外でも受ける。往復でも使える)]\n" +
  "   or: node run-window.mjs --out <out> --step install-roundtrip --tag <tag> --zip <cordmux zip> --obs-zip <OBS zip> " +
  "[--scenario roundtrip|move|move-foreign|force-update-missing|force-update-tamper|update|update-while-running|other-vencord|other-vencord-cancel|motw|dll-clash|old-obs|newer-libobs] " +
  "[--layout plugins|programdata --vm] [--old-zip <zip>] [--test-dll <dll>] [--keep-work] [--dry-run]";

// ---- 小さなツール ----------------------------------------------------------

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function ps(cmd) {
  try {
    return execFileSync("powershell", ["-NoProfile", "-Command", cmd + "; exit 0"], { encoding: "utf8" }).trim();
  } catch (e) {
    return (e.stdout || "").toString().trim();
  }
}

// JSON のテキストを読む (先頭の BOM を外す)。読めなければ undefined (null は JSON の null と区別する)。
function parseJsonTextOrUndefined(text) {
  try {
    const s = String(text);
    return JSON.parse(s.charCodeAt(0) === 0xfeff ? s.slice(1) : s);
  } catch {
    return undefined;
  }
}

function toAbs(p) {
  return path.resolve(REPO_ROOT, p);
}

// LOCALAPPDATA と APPDATA と VENCORD_USER_DATA_DIR を差し替えた環境変数のセットを作る。子プロセス (OBS と cleanup.ps1) の
// 両方にこれを渡すことで、cleanup.ps1 の既定の $env:LOCALAPPDATA / $env:APPDATA / $env:VENCORD_USER_DATA_DIR 参照が
// そのまま差し替え後の場所を指す (cleanup.ps1 自体は書き換えない)。製品 (cordmux.dll) は環境変数の APPDATA を読まず、
// Vesktop の設定の場所を VENCORD_USER_DATA_DIR (Vesktop 自身の上書きの手段。platform_win.cpp の vesktop_data_dir_w) か
// Windows の既知のフォルダーで決める。そこで VENCORD_USER_DATA_DIR をここの APPDATA の下の Vesktop に向けると、
// cordmux.dll は <差し替えた APPDATA>\Vesktop\state.json の実体としてここの配下だけを読む。
// 実際の %APPDATA%\Vesktop には触れない。VENCORD_USER_DATA_DIR が作業のフォルダーの下を指していなければ、OBS を
// 起動する前に止める (vesktopDirSwapProblem)。
// **差し替える 4 つは、同じ名前 (大文字小文字を無視して) のキーを先に全部落としてから
// 入れ直す**: 元の環境に AppData のような別綴りのキーがあると、素朴にコピーして足すだけでは
// 子プロセスへ両方渡り、どちらが動作するかは保証されない (= 実際の %APPDATA% を読みに行く
// 余地が残る)。落としてから入れれば、読む人がいてもいなくても構造で閉じる
// (--dry-run の envDuplicateCheck は、その結果を目でも確認できるようにしているだけ)。
function localAppDataEnv() {
  return overriddenEnv(LOCAL_APPDATA_OVERRIDE_REL, APPDATA_OVERRIDE_REL, HANDSHAKE_DIR_REL);
}

// 導入の往復は差し替え先も rt-<tag> の下に置く (スナップショットを取るルートを往復ごとに分け、前の実行の
// _work/appdata の残りを混ぜない)。CORDMUX_HANDSHAKE_DIR はテスト用の送信側だけが読む (製品は読まない)。
function roundtripEnv(args) {
  const p = roundtripPaths(args.tag);
  return overriddenEnv(p.localAppDataRel, p.appDataRel, p.handshakeRel);
}

// create=false は --dry-run の報告用 (往復の作業場所を dry-run で作らない = 残骸を残さない)。
function overriddenEnv(localAppDataRel, appDataRel, handshakeRel, create = true) {
  const localAppDataDir = toAbs(localAppDataRel);
  const appDataDir = toAbs(appDataRel);
  if (create) {
    fs.mkdirSync(localAppDataDir, { recursive: true });
    fs.mkdirSync(appDataDir, { recursive: true });
  }
  const overrides = {
    LOCALAPPDATA: localAppDataDir,
    APPDATA: appDataDir,
    // 製品が Vesktop の設定の場所を決める環境変数 (製品は APPDATA を読まない。platform_win.cpp の vesktop_data_dir_w)。
    // APPDATA の差し替え先の下の Vesktop に向けるので、state.json は <差し替えた APPDATA>\Vesktop\state.json のまま。
    VENCORD_USER_DATA_DIR: path.join(appDataDir, "Vesktop"),
    CORDMUX_HANDSHAKE_DIR: toAbs(handshakeRel),
  };
  // 判定の Python のスクリプト (latency.py など) は、numpy を利用者の site-packages から読む。Python は Windows では
  // %APPDATA%\Python を利用者の場所にするので、APPDATA を差し替えると numpy が見えなくなり、リハーサルの latency.py が
  // ModuleNotFoundError で止まる。元の場所を PYTHONUSERBASE で渡す。前から PYTHONUSERBASE があれば、それをそのままコピーする。
  if (process.env.APPDATA && !process.env.PYTHONUSERBASE) overrides.PYTHONUSERBASE = path.join(process.env.APPDATA, "Python");
  const overridden = new Set(Object.keys(overrides).map((k) => k.toLowerCase()));
  const env = {};
  for (const [key, value] of Object.entries(process.env)) {
    if (!overridden.has(key.toLowerCase())) env[key] = value;
  }
  Object.assign(env, overrides);
  // 取り違えたときに安全に失敗する: VENCORD_USER_DATA_DIR が作業のフォルダーの下を指していなければ、OBS を起動する前に
  // 止める (製品が利用者の実際の Vesktop の設定を書き換えないため)。dry-run でも同じ確認が通る。
  const problem = vesktopDirSwapProblem(env, toAbs("_work"));
  if (problem) throw new Error("refusing to run: " + problem);
  return env;
}

// 子プロセスへ渡す環境変数。
// **--real-profile のときは 1 つも差し替えない**: 実機の Vesktop の送信側は実際の
// %LOCALAPPDATA%\cordmux\session.json を読むので、OBS 側だけ差し替えるとトークンが合わず、
// 20 分たっても接続できない状態になる。差し替えないので _work/localappdata と
// _work/appdata も作らない (mkdir すらしない)。
function childEnv(args) {
  if (args.realProfile) return { ...process.env };
  if (args.step === INSTALL_ROUNDTRIP_STEP) return roundtripEnv(args);
  return localAppDataEnv();
}

// env のキーを大文字小文字を無視して集め、name に一致するものを列挙する (上の
// localAppDataEnv が落とし切れていれば必ず 1 つだけになる。--dry-run の出力
// (envDuplicateCheck) はその結果を見せる表示であって、保証そのものではない)。
function envKeysMatchingCaseInsensitive(env, name) {
  const lower = name.toLowerCase();
  return Object.keys(env).filter((k) => k.toLowerCase() === lower);
}

// ---- 先頭のガード: プロセス一覧と GPU ----------------------------------------

// tasklist で名前だけを素早く見る。利用者が守るプロセス (CORDMUX_PROTECTED_PROCESSES) の名前は
// extraNames で渡す。obs64.exe だけ、動いていれば
// PowerShell の Get-Process で path も引く (Program Files 配下かどうかで判定を分けるため。
// tasklist にはパスの列がない)。毎回 PowerShell で名前とパスを一緒に取ると重いので、軽い一覧 (tasklist) と、
// path が要るときだけの PowerShell とに分けている。
function firstCsvField(line) {
  // 正規表現リテラルの中に " を書くと、この文字列を単なる文字の並びだと知らない簡易な
  // 字句解析のツール (test/lint/identifiers_english.py の tokenize_c_like) が
  // それを文字列の開始と誤認して以降の行がずれる。二重引用符を含む正規表現を書かず、
  // 文字列の操作だけで CSV の先頭列を取り出す。
  if (!line.startsWith('"')) return null;
  const end = line.indexOf('"', 1);
  return end === -1 ? null : line.slice(1, end);
}

function gatherProcessList(extraNames) {
  const watched = new Set([...DEFAULT_GUARDED_PROCESS_NAMES, ...(extraNames || [])].map((n) => String(n).toLowerCase()));
  const csv = execFileSync("tasklist", ["/fo", "csv", "/nh"], { encoding: "utf8" });
  const names = new Set();
  for (const line of csv.split(/\r?\n/)) {
    const name = firstCsvField(line);
    if (name) names.add(name);
  }
  const list = [];
  for (const name of names) {
    const lower = name.toLowerCase();
    if (lower !== "obs64.exe" && watched.has(lower)) {
      list.push({ name, path: null });
    }
  }
  if (names.has("obs64.exe")) {
    const raw = ps("Get-Process obs64 -ErrorAction SilentlyContinue | ForEach-Object { $_.Path }");
    const paths = raw ? raw.split(/\r?\n/).map((s) => s.trim()).filter(Boolean) : [];
    if (paths.length === 0) list.push({ name: "obs64.exe", path: null });
    else for (const p of paths) list.push({ name: "obs64.exe", path: p });
  }
  return list;
}

function queryEncoderText() {
  return execFileSync("nvidia-smi", ["--query-gpu=utilization.encoder", "--format=csv,noheader"], { encoding: "utf8" });
}

// エンコーダを使っているプロセス (nvidia-smi encodersessions の Process Id) を、tasklist の名前とセットにして返す。
// encodersessions は一覧を繰り返し出し続けるので、4 秒で打ち切って最初の一覧だけを読む。
function queryEncoderSessions() {
  const r = spawnSync("nvidia-smi", ["encodersessions"], { encoding: "utf8", timeout: 4000, windowsHide: true });
  const pids = parseEncoderSessionPids(r.stdout || "");
  const csv = execFileSync("tasklist", ["/fo", "csv", "/nh"], { encoding: "utf8" });
  const names = new Map();
  for (const line of csv.split(/\r?\n/)) {
    const name = firstCsvField(line);
    if (!name) continue;
    // 2 列目が PID ("名前","PID",...)。正規表現に二重引用符を書かない (firstCsvField のコメント)。
    const rest = line.slice(name.length + 2);
    const pidText = rest.startsWith(",") ? firstCsvField(rest.slice(1)) : null;
    const pid = Number(pidText);
    if (Number.isInteger(pid)) names.set(pid, name);
  }
  return pids.map((pid) => ({ pid, name: names.get(pid) ?? null }));
}

function evaluateGuard(allowUnknownEncoder, callWindow) {
  // 利用者が環境変数 CORDMUX_PROTECTED_PROCESSES のファイルで指定したプロセスも守る。
  // ファイルが読めないときは、守るはずのものを守らずに始めないよう、止める理由に入れる。
  const protectedList = loadProtectedProcesses(process.env[PROTECTED_PROCESSES_ENV], (f) => fs.readFileSync(f, "utf8"));
  const processList = gatherProcessList(protectedList.names);
  // --call-window のときだけ vesktop.exe をガードの対象から外す (起動中の OBS と、
  // 利用者が指定したプロセスは引き続き確認する)。
  const blockers = [
    ...blockersFromProcessList(processList, { callWindow: Boolean(callWindow), protectedNames: protectedList.names }),
    ...protectedList.blockers,
  ];
  let encoder = { busy: false, values: [] };
  let encoderError = null;
  try {
    encoder = encoderBusy(queryEncoderText());
  } catch (e) {
    encoderError = e.message;
  }
  // --call-window のときは、エンコーダを使っているプロセスが全部通話のアプリ (CALL_WINDOW_ENCODER_APPS) なら通す。
  let encoderSessions = null;
  let encoderSessionsError = null;
  let encoderOnlyCallApps = false;
  if (callWindow && encoder.busy) {
    try {
      encoderSessions = queryEncoderSessions();
      encoderOnlyCallApps = encoderUsedOnlyByCallApps(encoderSessions);
    } catch (e) {
      encoderSessionsError = e.message;
    }
  }
  return {
    processList,
    blockers,
    encoderBusy: encoder.busy,
    encoderValues: encoder.values,
    encoderError,
    encoderSessions,
    encoderSessionsError,
    encoderOnlyCallApps,
    blocked: guardBlocked({
      blockers,
      encoderBusy: encoder.busy,
      encoderValues: encoder.values,
      encoderError,
      allowUnknownEncoder,
      encoderOnlyCallApps,
    }),
  };
}

// ---- 配置の一覧を実際のファイルに当てて存在を見る (dry-run と place の両方で使う) ----------

function manifestWithExistence(pluginDir) {
  return filesToPlace(pluginDir).map((entry) => {
    const srcAbs = toAbs(entry.src);
    if (entry.kind === "copy-file") return { ...entry, srcAbs, exists: fs.existsSync(srcAbs) };
    if (entry.kind === "copy-dir" || entry.kind === "copy-dir-contents") {
      const exists = fs.existsSync(srcAbs) && fs.statSync(srcAbs).isDirectory();
      return { ...entry, srcAbs, exists };
    }
    return { ...entry, srcAbs, exists: fs.existsSync(srcAbs) };
  });
}

// ---- OBS の起動と閉じ方 ------------------------

// 引数は OBS の設定の保存先 (ポータブルなら <obs>/config/obs-studio。--layout programdata では
// ポータブルでない OBS の保存先 = %APPDATA%\obs-studio)。
function listLogFiles(configDir) {
  const logDir = path.join(configDir, "logs");
  if (!fs.existsSync(logDir)) return new Set();
  return new Set(fs.readdirSync(logDir));
}

function newestLogFile(configDir, beforeSet) {
  const logDir = path.join(configDir, "logs");
  if (!fs.existsSync(logDir)) return null;
  const all = fs.readdirSync(logDir);
  const fresh = all.filter((f) => !beforeSet.has(f));
  const candidates = (fresh.length > 0 ? fresh : all).map((f) => path.join(logDir, f));
  if (candidates.length === 0) return null;
  return candidates.sort((a, b) => fs.statSync(b).mtimeMs - fs.statSync(a).mtimeMs)[0];
}

function processAlive(pid) {
  const out = ps(`Get-Process -Id ${pid} -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id`);
  return out.trim() === String(pid);
}

// まず穏やかに (CloseMainWindow を 6 回 x 8 秒待つ)、閉じなければ強制終了する。
async function closeObsGracefully(pid) {
  if (!pid) return true;
  let exited = !processAlive(pid);
  for (let attempt = 0; attempt < 6 && !exited; attempt += 1) {
    ps(`$p = Get-Process -Id ${pid} -ErrorAction SilentlyContinue; if ($p) { $null = $p.CloseMainWindow() }`);
    for (let i = 0; i < 8 && !exited; i += 1) {
      await sleep(1000);
      exited = !processAlive(pid);
    }
  }
  if (!exited) {
    try {
      process.kill(pid);
    } catch {
      /* 既に居なければ何もしない */
    }
    await sleep(1000);
    exited = !processAlive(pid);
  }
  return exited;
}

function launchObs(obsDir, env, launchArgs) {
  const binDir = path.join(obsDir, "bin", "64bit");
  const child = spawn(path.join(binDir, "obs64.exe"), launchArgs || OBS_LAUNCH_ARGS, { cwd: binDir, env, stdio: "ignore", detached: true });
  child.unref();
  return child;
}

// ---- 配置 (filesToPlace の一覧を実際にコピーする) -------------------------------

function placeFiles(pluginDir, obsDir) {
  const report = [];
  for (const entry of manifestWithExistence(pluginDir)) {
    const destAbs = path.join(obsDir, ...entry.destRel.split("/"));
    if (entry.kind === "copy-file") {
      if (!entry.exists) {
        report.push({ id: entry.id, ok: false, required: entry.required, error: "ない: " + entry.srcAbs });
        continue;
      }
      fs.mkdirSync(path.dirname(destAbs), { recursive: true });
      fs.copyFileSync(entry.srcAbs, destAbs);
      report.push({ id: entry.id, ok: true, copied: 1 });
    } else if (entry.kind === "copy-dir-contents") {
      if (!entry.exists) {
        report.push({ id: entry.id, ok: !entry.required, required: entry.required, error: "ない: " + entry.srcAbs });
        continue;
      }
      fs.mkdirSync(destAbs, { recursive: true });
      const exclude = new Set(entry.excludeNames || []);
      let copied = 0;
      // 運ぶのは直下のファイルだけ。OBS 33 の置き方では、cordmux.dll と同じフォルダーに data フォルダーもある
      // (data は copy-dir の手順が運ぶ)。
      for (const f of fs.readdirSync(entry.srcAbs)) {
        if (exclude.has(f) || !fs.statSync(path.join(entry.srcAbs, f)).isFile()) continue;
        fs.copyFileSync(path.join(entry.srcAbs, f), path.join(destAbs, f));
        copied += 1;
      }
      report.push({ id: entry.id, ok: true, copied });
    } else if (entry.kind === "copy-dir") {
      if (!entry.exists) {
        report.push({ id: entry.id, ok: false, required: entry.required, error: "ない: " + entry.srcAbs });
        continue;
      }
      fs.cpSync(entry.srcAbs, destAbs, { recursive: true });
      report.push({ id: entry.id, ok: true });
    }
  }
  return report;
}

// ---- 導入の往復 (--step install-roundtrip) の実行側 ----------------------------
// 手順は run-window-plan.mjs の buildInstallRoundtripSteps が決める。ここは場所を解決し、ファイルを
// 展開・配置・消去し、install_snapshot.py / dialog-click.ps1 を呼ぶだけ。

const INSTALL_SNAPSHOT_PY = path.join(HERE, "install_snapshot.py");
const DIALOG_CLICK_PS1 = path.join(HERE, "dialog-click.ps1");
const ROUNDTRIP_WORK_ROOT = toAbs("_work");

// runReal の switch が扱う手順の種類の全部。**switch に case を足したらここにも足す** (--dry-run はこの一覧に
// ない種類を含む実行計画を失敗にする: 知らない種類は switch の default で「unknown-step」と記録されるだけで、
// メッセージもログも出さずに手順が飛ぶため。run-window-plan.test.ts がこの一覧と switch の case を文字列で突き合わせる)。
const HANDLED_KINDS = [
  "seed-config", "copy-file", "copy-dir-contents", "copy-dir", "launch-obs", "obsws-connect", "obsws-call",
  "check-file-exists", "check-guide-update-hint", "icacls", "launch-obs-second", "read-obs-log", "close-obs", "harness", "harness-start", "harness-wait",
  "wait", "copy-state-json", "read-state-json", "judge-vencord-dir", "move-file", "restore-file", "obs-stats",
  "judge-output-skipped", "stream-stop", "count-operations", "screenshot-window",
  "dock-click", "dialog-sendkeys", "screenshot-source", "write-call-window-artifacts", "wait-connection", "obsws-request", "judge-composite-placement", "judge-ac31",
  "judge-dummy-tile", "check-bundle-zip", "mic-check", "judge-backpressure", "judge-connection-held", "stream-sink-start",
  "record-start", "record-stop", "stream-sink-stop", "collect-recording", "judge", "wait-finalize", "collect-record-session",
  "judge-record", "judge-record-second", "judge-record-off", "webcodecs-check", "rt-prepare", "rt-extract", "rt-place",
  "rt-remove-placed", "rt-rename-obs", "rt-replace-dll", "snapshot", "snapshot-compare", "dialog-click", "capture-file",
  "judge-rt", "edit-state-json", "delete-bundle-files", "tamper-file", "hash-bundle", "hash-files", "mark-motw",
  "run-placed-ffmpeg", "list-modules", "copy-obs-logs", "collect-plugin-log", "ws-probe", "write-result", "summarize",
  "cleanup",
  // 人の操作待ち・テスト用にインストールした Vencord のビルド成果のコピー・わざと異常にした状態の戻し・アンインストールの判定・A/V の差・
  // テスト用の送信側の停止。
  "human", "place-vencord-bundle", "untamper-file", "judge-vencord-dir-restored", "judge-av-drift", "harness-stop",
  "capture-backup", "harness-alive",
  // A/V の録画の間の音の切り分けと戻し、映像の数、飛ばした手順、OBS の録画を残す手順。
  "measure-video-rate", "skip", "keep-obs-recordings",
  // 実機でのセルの領域のツール。
  "hold-info", "mark-time", "measure-plate",
  // call-hold: 開き直す前の hello の数を保存する。
  "count-connections",
  // 実際の通話の --extra-checks: 接続が切れていないか、cordmux の録画の参加者ごとの A/V、使う声の音、
  // OBS のログのタイムスタンプのずれ・フォールバック・アイコン・接続の数。
  "judge-connections-kept", "judge-record-sync", "judge-voice-source", "judge-call-log",
  // 遅延、合否のしきい値、Vencord の設定のコピーと比較。
  "place-window-capture", "check-window-capture", "switch-to-monitor-capture", "remove-window-capture", "judge-latency",
  "judge-av-sync", "judge-ac32", "judge-lines",
  "copy-vencord-settings", "compare-vencord-settings",
  // 合成のソース: 合成のソースを置く、配置の行を読む、配置・表示の遅延の測定・音声バッファーの判定。
  "wire-composite", "composite-layout-snapshot", "judge-composite-layout", "judge-display-delay", "judge-audio-buffering",
  // 声の音量、実際のアバターの並び、hold の --self-mic のログの抜き出し。
  "judge-voice-volume", "count-roster-labels", "judge-roster-labels", "collect-self-speaking-log",
  // --recheck: 送信側が自分のアカウントを届けたかを OBS のログの Facts の行で待つ。
  "wait-self-account",
  // --recheck の追加の確認: 他のプロセスのウィンドウの大きさを変える・入力の音量メーターを読む・ログの行を数える。
  "resize-window", "measure-input-level", "judge-log-lines",
];
// stopOnFail で止めた後も通す手順 (結果を残して後始末する)。
const FINISH_KINDS = ["copy-obs-logs", "write-result", "summarize", "cleanup"];

// seed-config.py の引数 (手順の実行と --dry-run の seedPreview で共有する)。where は { obsAbs, windowOutDir, obsConfigAbs }。
function seedConfigArgv(s, args, where) {
  const argv = [path.join(HERE, "seed-config.py"), where.obsAbs, where.windowOutDir, "--step", s.step || args.step];
  if (s.shape === "user") {
    argv.push("--shape", "user", "--appdata", toAbs(s.appDataRel));
    if (s.vencordState === "custom") argv.push("--vencord-state", "custom", "--custom-vencord-dir", toAbs(s.customVencordRel));
    if (s.vencordCheckDisconnectedS) argv.push("--vencord-check-disconnected-s", String(s.vencordCheckDisconnectedS));
    if (s.vencordCheckConnectedS) argv.push("--vencord-check-connected-s", String(s.vencordCheckConnectedS));
    // --layout programdata (VM だけ): ポータブルでない OBS の保存先へ書く (seed-config.py は --vm が無ければ断る)。
    if (s.installedObs) argv.push("--config-dir", where.obsConfigAbs, "--no-portable-mode-file", ...(s.vm ? ["--vm"] : []));
  } else {
    // リハーサル (call-1 + 差し替えた APPDATA): seed-config.py はこのオプションがないと call-* を --appdata で断る。
    // 照合の間隔の 2 つのキーはリハーサルだけが渡す (連続運転の待ち時間を縮めるため)。
    if (s.rehearsal) argv.push("--rehearsal");
    if (s.vencordCheckDisconnectedS) argv.push("--vencord-check-disconnected-s", String(s.vencordCheckDisconnectedS));
    if (s.vencordCheckConnectedS) argv.push("--vencord-check-connected-s", String(s.vencordCheckConnectedS));
    argv.push(
      "--shape", "test",
      "--dock-install", s.dockInstall || DEFAULT_DOCK_INSTALL,
      "--vencord-dist", toAbs(VENCORD_DIST_REL),
      "--assign-mode", s.assignMode || DEFAULT_DOCK_ASSIGN_MODE,
      "--decoder", s.decoder || DEFAULT_DECODER,
    );
    // 表示のバッファー (製品の display_buffer_ms)。実行計画が持つときだけ渡す (渡さなければ製品の既定のまま)。
    if (s.displayBufferMs != null) argv.push("--display-buffer-ms", String(s.displayBufferMs));
    // 名前ラベルの背景を出して始める。
    if (s.labelPlate) argv.push("--label-plate");
    // --appdata か --real-profile のどちらかを必ず渡す。
    if (s.useAppData !== false) argv.push("--appdata", toAbs(APPDATA_OVERRIDE_REL));
    else argv.push("--real-profile");
    if (s.allowStateJsonWrite) argv.push("--allow-state-json-write");
    if (s.recordSpaceCheckIntervalS) argv.push("--record-space-check-interval-s", String(s.recordSpaceCheckIntervalS));
    if (s.recordSpaceWarnMinutes) argv.push("--record-space-warn-minutes", String(s.recordSpaceWarnMinutes));
    // テスト用の OBS のプロファイルにリプレイバッファを書く (hold の --replay-buffer。test の形だけ)。
    if (s.replayBuffer) argv.push("--replay-buffer");
    // hold の --self-mic: 自分の枠の音声ソースの名前 (しきい値は書かない)。
    if (s.selfSpeakingSourceName) argv.push("--self-speaking-source-name", s.selfSpeakingSourceName);
    // hold の --label-em-ratio-pct: 名前の大きさ (0.1 刻みの数を 1 桁の小数で渡す)。
    if (typeof s.labelEmRatioPct === "number") argv.push("--label-em-ratio-pct", s.labelEmRatioPct.toFixed(1));
  }
  // cordmux の録画の保存先を必ず作業用のフォルダの下へ差し替える。
  // 実行計画が recordDirRel を必ず付ける (付け忘れても seed-config.py の側が止める)。
  if (s.recordDirRel) argv.push("--record-dir", path.join(where.windowOutDir, s.recordDirRel));
  // OBS の見た目と言語。値を確認するのは seed-config.py (その OBS の themes と locale を見る)。
  // 実行スクリプトが作業用のフォルダの外の保存先を受けたときは、seed-config.py にも同じオプションを渡す (渡さないと seed-config.py が断る)。
  if (args.allowOutsideWork) argv.push(ALLOW_OUTSIDE_WORK_FLAG);
  const look = s.obsLook || null;
  if (look) {
    if (look.language) argv.push("--language", look.language);
    if (look.theme) argv.push("--theme", look.theme);
    if (look.fontScale !== null && look.fontScale !== undefined) argv.push("--font-scale", String(look.fontScale));
    if (look.density !== null && look.density !== undefined) argv.push("--density", String(look.density));
    if (look.hdr) argv.push("--hdr");
    if (typeof look.fullHeightDocks === "boolean") argv.push("--full-height-docks", look.fullHeightDocks ? "on" : "off");
  }
  return argv;
}

// 製品の ja-JP.ini (key="value") を読む。押せるコンポーネントの文言は ini の id から引く (同じ文言を 2 か所で決めない)。file を渡すと別の ini (OBS 自身の ja-JP.ini) を読む。
function readLocaleTexts(file) {
  const texts = {};
  try {
    for (const raw of fs.readFileSync(file || toAbs(JA_LOCALE_INI_REL), "utf8").replace(/^\uFEFF/, "").split(/\r?\n/)) {
      const line = raw.trim();
      if (!line || line.startsWith("#") || line.indexOf("=") < 0) continue;
      const eq = line.indexOf("=");
      let value = line.slice(eq + 1).trim();
      if (value.length >= 2 && value.startsWith('"') && value.endsWith('"')) value = value.slice(1, -1);
      texts[line.slice(0, eq).trim()] = value;
    }
  } catch {
    /* 読めなければ空 (引けない id は dry-run が失敗にする) */
  }
  return texts;
}

// OBS 自身の ini の場所 (obs_frontend_get_locale_string が引く [OK] [キャンセル] [適用] のテキスト。製品の
// ウィンドウのボタンはこのテキストを使う)。obsDir は OBS の保存先 (bin/64bit/obs64.exe の 3 つ上)。locale の既定は ja-JP。
function obsLocaleIniPath(obsDir, locale) {
  return obsDir ? path.join(obsDir, "data", "obs-studio", "locale", (locale || "ja-JP") + ".ini") : null;
}

// 画面のテキストの id を引く (run-window-plan.mjs の textRef と、ソースを指定する欄)。"obs:" で始まる id は OBS 自身の ini (ja-JP に
// 無ければ en-US。OBS_LOCALE_FALLBACK_ORDER)、ほかは製品の ja-JP.ini。obsDir は現在指定している OBS (導入の往復では
// 展開した OBS) を返す関数。引けなければ null。返すテキストは Qt のアクセシブル名と同じく & を外した形 (stripQtMnemonic)。
function makeTextLookup(obsDir) {
  let productTexts = null;
  const obsTexts = new Map();
  const obsIni = (file) => {
    if (!obsTexts.has(file)) obsTexts.set(file, fs.existsSync(file) ? readLocaleTexts(file) : {});
    return obsTexts.get(file);
  };
  return (id) => {
    const key = String(id || "");
    if (key.startsWith(OBS_TEXT_PREFIX)) {
      const dir = obsDir();
      if (!dir || !fs.existsSync(obsLocaleIniPath(dir))) return null;
      const inOrder = OBS_LOCALE_FALLBACK_ORDER.map((locale) => obsIni(obsLocaleIniPath(dir, locale)));
      return stripQtMnemonic(lookupObsText(inOrder, key.slice(OBS_TEXT_PREFIX.length)));
    }
    if (!productTexts) productTexts = readLocaleTexts();
    return stripQtMnemonic(productTexts[key] ?? null);
  };
}

// cordmux のソースの一覧を obs-websocket で読む (種類の id で絞り、枠の番号は設定の slot から。名前は人が載ると変わり、
// 言語でも変わるので使わない)。人が載った枠の名前の形 (x08・x09) は ini から引いて、誰が載って
// いるかを読む。
async function readCordmuxInputs(ws, textOf) {
  const inputs = ((await ws.call("GetInputList", {})) || {}).inputs || [];
  const kinds = Object.values(CORDMUX_SOURCE_KINDS);
  const settings = {};
  for (const input of inputs) {
    if (!input || !kinds.includes(input.inputKind)) continue;
    try {
      const data = await ws.call("GetInputSettings", { inputName: input.inputName });
      settings[input.inputName] = (data && data.inputSettings) || {};
    } catch {
      settings[input.inputName] = {};
    }
  }
  return { all: inputs, cordmux: classifyCordmuxInputs({ inputs, settings, personForms: {} }) };
}

// 手順が指定するソース ({ kind, slot }) の現在の名前。見つからなければ例外 (呼び出し元が ok:false で記録する)。
async function resolveSourceName(ws, textOf, ref) {
  // 入力の名前で指定する手順 (利用者が付けた名前の入力は、種類では選べない)。
  if (ref && typeof ref.inputName === "string" && ref.inputName !== "") return ref.inputName;
  const { cordmux } = await readCordmuxInputs(ws, textOf);
  const hit = findCordmuxInput(cordmux, ref);
  if (!hit) throw new Error("no cordmux source for " + sourceRefLabel(ref) + " (found: " + cordmux.map((x) => x.kind + ":" + x.slot).join(", ") + ")");
  return hit.inputName;
}

// rt-<tag> の下 (_work/rt-*) でなければ消さない (作業場所の外を消さないためのガード)。
function assertRoundtripWorkPath(absPath) {
  const rel = path.relative(ROUNDTRIP_WORK_ROOT, absPath);
  if (!rel || rel.startsWith("..") || path.isAbsolute(rel) || !rel.split(/[\\/]/)[0].startsWith("rt-")) {
    throw new Error("refusing to touch a path outside _work/rt-*: " + absPath);
  }
}

function removeRoundtripDir(absPath) {
  assertRoundtripWorkPath(absPath);
  fs.rmSync(absPath, { recursive: true, force: true });
}

function sha256Of(file) {
  return createHash("sha256").update(fs.readFileSync(file)).digest("hex");
}

function hashDir(dir) {
  const out = {};
  if (!fs.existsSync(dir)) return out;
  for (const name of fs.readdirSync(dir)) {
    const full = path.join(dir, name);
    if (fs.statSync(full).isFile()) out[name] = sha256Of(full);
  }
  return out;
}

function listFilesRecursive(target) {
  if (!fs.existsSync(target)) return [];
  if (fs.statSync(target).isFile()) return [target];
  const out = [];
  for (const entry of fs.readdirSync(target, { withFileTypes: true })) {
    const full = path.join(target, entry.name);
    if (entry.isDirectory()) out.push(...listFilesRecursive(full));
    else out.push(full);
  }
  return out;
}

// 展開した zip の中の cordmux フォルダ (最上位は cordmux-<バージョン>/、その下に cordmux/)。
function findZipPluginDir(zipDirAbs) {
  const candidates = [];
  if (fs.existsSync(path.join(zipDirAbs, "cordmux", "cordmux.dll"))) candidates.push(path.join(zipDirAbs, "cordmux"));
  if (fs.existsSync(zipDirAbs)) {
    for (const entry of fs.readdirSync(zipDirAbs, { withFileTypes: true })) {
      if (!entry.isDirectory() || !entry.name.startsWith("cordmux-")) continue;
      const dir = path.join(zipDirAbs, entry.name, "cordmux");
      if (fs.existsSync(path.join(dir, "cordmux.dll"))) candidates.push(dir);
    }
  }
  return candidates.length === 1 ? candidates[0] : null;
}

// 導入の往復の保存先 (--layout programdata は %ProgramData%\obs-studio\plugins\cordmux。VM だけ)。
function programDataPluginsDir() {
  return path.join(process.env.ProgramData || "C:\\ProgramData", "obs-studio", "plugins");
}

// zip の cordmux フォルダを置く。置いたものは { root, rel } で返す (install_snapshot.py の ModuleFolder の見方と
// rt-remove-placed が読む)。tolerant ならコピーできなかったファイルを failed に積んで先へ進む。
function placeFromZip(pluginDir, obsDir, layout, tolerant) {
  const placed = [];
  const failed = [];
  let copied = 0;
  const copyOne = (from, to) => {
    try {
      fs.mkdirSync(path.dirname(to), { recursive: true });
      fs.copyFileSync(from, to);
      copied += 1;
    } catch (e) {
      if (!tolerant) throw e;
      failed.push({ dest: to, error: e.message });
    }
  };
  if (layout === "programdata") {
    const dest = path.join(programDataPluginsDir(), "cordmux");
    for (const file of listFilesRecursive(pluginDir)) copyOne(file, path.join(dest, path.relative(pluginDir, file)));
    placed.push({ root: "programdata_plugins", rel: "cordmux", abs: dest });
    return { ok: failed.length === 0, copied, placed, failed };
  }
  // plugins: ポータブルの OBS 33 以上。zip の cordmux フォルダーをそのまま OBS のフォルダーの plugins の中へ (手順書の手順 2)。
  // plugins が無ければ作り、作ったことを記録する (削除の後に、空なら消す)。
  const pluginsAbs = path.join(obsDir, "plugins");
  if (!fs.existsSync(pluginsAbs)) placed.push({ root: "obs_program", rel: "plugins", abs: pluginsAbs, createdDir: true });
  const dest = path.join(pluginsAbs, "cordmux");
  for (const file of listFilesRecursive(pluginDir)) copyOne(file, path.join(dest, path.relative(pluginDir, file)));
  placed.push({ root: "obs_program", rel: "plugins/cordmux", abs: dest });
  return { ok: failed.length === 0, copied, placed, failed };
}

function runInstallSnapshot(cliArgs) {
  try {
    const out = execFileSync("python", [INSTALL_SNAPSHOT_PY, ...cliArgs], { encoding: "utf8", env: { ...process.env, PYTHONIOENCODING: "utf-8" } });
    return { exitCode: 0, parsed: lastPrefixedJson(out, "CORDMUX_SNAPSHOT_JSON:") };
  } catch (e) {
    return { exitCode: typeof e.status === "number" ? e.status : 2, parsed: lastPrefixedJson((e.stdout || "").toString(), "CORDMUX_SNAPSHOT_JSON:"), error: e.message };
  }
}

function lastPrefixedJson(text, prefix) {
  const line = String(text || "").split(/\r?\n/).reverse().find((l) => l.startsWith(prefix));
  if (!line) return null;
  try {
    return JSON.parse(line.slice(prefix.length));
  } catch {
    return null;
  }
}

// ---- 押したコンポーネントの名前のコピー --------------------------------------------------
// dock-click.ps1 は結果の JSON に押したコンポーネントの文言を "name" として入れる。
// record(name, data) は data を後から広げるので、そのまま渡すと **手順の名前 (dock-click) が
// コンポーネントの文言で上書きされ**、結果 JSON から「どの種類の手順だったか」が読めなくなる
// (操作の数を数える judgeOperationCount がまさにそれを読む)。文言は elementName へ
// 移し替えて残す。
function withElementName(parsed) {
  if (!parsed || typeof parsed !== "object") return parsed;
  if (!Object.prototype.hasOwnProperty.call(parsed, "name")) return parsed;
  const { name, ...rest } = parsed;
  return { ...rest, elementName: name };
}


// ---- 判定スクリプト (frames.py / sync.py / smooth.py) と icacls -------------------

// ---- 録画のツール ---------------------------------------------------
// 録画ログとフォルダの形は src-obs/record/README.md を見よ。

// 同梱の FFmpeg (obs-deps) の保存先。判定スクリプトは --ffmpeg で場所を受けるので、ここに
// 無ければ何も渡さず PATH の ffmpeg に任せる (CI の ubuntu は apt の ffmpeg で通る)。
// obs-studio と obs-deps のバージョンの文字列は src-obs/buildspec.json の 1 か所から導く
// (読めなければ既定の場所にフォールバックする。フォールバックしたことは起動のログに出ない。判定スクリプトの ffmpeg が見つからない
// ことで気付く)。
const OBS_DEPS_BIN = (() => {
  try {
    const rel = obsDepsBinRelFromBuildspec(JSON.parse(fs.readFileSync(path.join(REPO_ROOT, "src-obs", "buildspec.json"), "utf8")));
    if (rel) return path.join(REPO_ROOT, ...rel.split("/"));
  } catch {
    /* 読めなければ下の既定 */
  }
  return path.join(REPO_ROOT, "src-obs", ".deps", "obs-studio-31.1.1", ".deps", "obs-deps-2025-07-11-x64", "bin");
})();
const BUNDLED_FFMPEG = path.join(OBS_DEPS_BIN, "ffmpeg.exe");
const LEDGER_CHECK_PY = path.join(HERE, "ledger_check.py");
const BLACK_SPAN_PY = path.join(HERE, "black_span.py");
const AU_HASH_PY = path.join(HERE, "au_hash.py");
// src-obs/record/record_types.h の kLedgerFileName。
const LEDGER_FILE_NAME = "ledger.jsonl";

function ffmpegArgsForJudge() {
  return fs.existsSync(BUNDLED_FFMPEG) ? ["--ffmpeg", BUNDLED_FFMPEG] : [];
}

// <保存先>/<セッションのフォルダー>/ のうち、いちばん新しいものを返す (無ければ null)。保存先 (settings.json の record_dir) の
// 直下にセッションのフォルダーを作る (src-obs/record/record_location.h)。置く場所の決め方は recordSessionsRoot にある。
// **名前で並べる** (「年-月-日 時-分-秒」の形も「年月日-時分秒」の形も日時の綴りなので辞書順 = 時刻順。
// 同じ秒の 2 つ目の「 (2)」は元の名前の後ろに並ぶ。ファイルの時刻に頼らない)。
function newestRecordSessionDir(recordDirAbs) {
  const root = recordSessionsRoot(recordDirAbs);
  if (!fs.existsSync(root)) return null;
  const dirs = fs
    .readdirSync(root, { withFileTypes: true })
    .filter((e) => e.isDirectory() && isRecordSessionDirName(e.name))
    .map((e) => e.name)
    .sort();
  return dirs.length > 0 ? path.join(root, dirs[dirs.length - 1]) : null;
}

// セッションのフォルダーの中間ファイルのフォルダー (録画ログ・セグメント・黒埋め用のフレームの保存先)。
// <セッション>/data があればそれ、なければセッションのフォルダーそのもの
// (ledger_check.py の materials_dir_of と同じ読み分け。src-obs/record/README.md)。
function materialsDirOf(sessionDir) {
  const candidate = path.join(sessionDir, RECORD_MATERIALS_DIR_NAME);
  return fs.existsSync(candidate) && fs.statSync(candidate).isDirectory() ? candidate : sessionDir;
}

// 録画ログの行を種類ごとに分ける。**途中で切れた末尾の 1 行は読み飛ばす**。
// 行の種類の見分け方は ledger_check.py と同じ (type のキーがあればそれ、無ければほかの行にないキー)。
// **同じものを指す行は後の行が優先される** (製品の parse_ledger = record_ledger.h と ledger_check.py と
// 同じ読み方。キーは person が user_id、segment と black が file、finalize が output_file。並びは最初に出た位置を保つ)。
// 録画側はセグメントを開いた瞬間にも segment の行を書くので、全部を並べると segmentRows が
// セグメントの数の 2 倍になる。session の行は全部を残す (最後の行で録画ファイルの作成の状態を見る)。
const LEDGER_ROW_KEYS = { person: "user_id", segment: "file", black: "file", finalize: "output_file" };

function pushLedgerRow(out, kind, row) {
  const key = LEDGER_ROW_KEYS[kind];
  if (key) {
    const i = out[kind].findIndex((existing) => existing[key] === row[key]);
    if (i >= 0) {
      out[kind][i] = row;
      return;
    }
  }
  out[kind].push(row);
}

function readLedger(sessionDir) {
  const file = path.join(materialsDirOf(sessionDir), LEDGER_FILE_NAME);
  if (!fs.existsSync(file)) return null;
  const out = { session: [], person: [], segment: [], black: [], finalize: [], skipped: 0 };
  for (const line of fs.readFileSync(file, "utf8").split(/\r?\n/)) {
    const s = line.trim();
    if (!s) continue;
    let row = null;
    try {
      row = JSON.parse(s);
    } catch {
      out.skipped += 1;
      continue;
    }
    if (!row || typeof row !== "object") continue;
    // 行の種類は `type` で決まる (製品は必ず `type` を書く)。
    // 下の「ほかの行にないキーの有無」は **`type` を書く前に作った録画ログのための保険**で、現在の
    // 製品が書く録画ログはこの枝を 1 行も通らない (ledger_check.py も同じ)。
    const named = typeof row.type === "string" ? row.type : null;
    if (named && Array.isArray(out[named])) pushLedgerRow(out, named, row);
    else if ("schema" in row) out.session.push(row);
    else if ("kinds" in row) pushLedgerRow(out, "person", row);
    else if ("close_reason" in row) pushLedgerRow(out, "segment", row);
    else if ("origin" in row) pushLedgerRow(out, "black", row);
    else if ("output_file" in row) pushLedgerRow(out, "finalize", row);
  }
  return out;
}

// 録画ファイルの作成が終わったと言える形かどうか。**2 つのどちらかで決める**:
// (1) 録画ログの最後の session の行が終わりの状態を示した、
// (2) 録画ファイルの作成の行が 1 つ以上あり、しばらく増えず、作業用のフォルダの下の ffmpeg.exe も動いていない。
const FINALIZE_DONE_STATES = ["done", "failed", "pending", "finished", "complete"];

function finalizeStateIsTerminal(rows) {
  if (!rows || rows.session.length === 0) return false;
  const last = rows.session[rows.session.length - 1];
  const v = String(last.finalize_state || "").toLowerCase();
  return FINALIZE_DONE_STATES.includes(v);
}

// 作業用のフォルダの下で動いている ffmpeg.exe の数 (cleanup.ps1 の判定と同じ「パスで見る」形)。
function ffmpegRunningUnder(dirFull) {
  try {
    const out = execFileSync(
      "powershell",
      [
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-Command",
        "@(Get-Process -Name ffmpeg -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith('" +
          // PowerShell の単引用符の中では、単引用符を 2 つ重ねて 1 つとして書く。
          // (正規表現の中に引用符を書かない: test/lint の字句解析が正規表現リテラルを
          //  読まないため。split/join なら同じことを文字列だけで書ける)
          dirFull.split("'").join("''") +
          "', [System.StringComparison]::OrdinalIgnoreCase) }).Count",
      ],
      { encoding: "utf8" },
    );
    const n = Number(String(out).trim());
    return Number.isFinite(n) ? n : 0;
  } catch {
    return 0; // 数えられなければ 0 として扱う (待ちの条件を厳しくしない)。
  }
}

function runRecordJudge(scriptPath, judgeArgs) {
  const argv = [scriptPath, ...judgeArgs];
  try {
    const out = execFileSync("python", argv, { encoding: "utf8", env: { ...process.env, PYTHONIOENCODING: "utf-8" } });
    return { ok: true, data: JSON.parse(out) };
  } catch (e) {
    // 判定スクリプトは判定が失敗でも JSON を出して終了コード 1 で終わる。標準出力が読めれば結果として扱う。
    const stdout = (e.stdout || "").toString();
    try {
      const data = JSON.parse(stdout);
      return { ok: false, data };
    } catch {
      return { ok: false, error: (e.message || "").slice(0, 400), stdout: stdout.slice(0, 400) };
    }
  }
}

// 子の Python が Windows の既定のコードページで書くと、結果 JSON で sync.py の note が文字化けするので、
// runRecordJudge と同じく UTF-8 で書かせる。
function runJudge(tool, recordingPath, extraArgs) {
  const scriptPath = path.join(HERE, tool);
  const out = execFileSync("python", [scriptPath, recordingPath, ...(extraArgs || [])], { encoding: "utf8", env: { ...process.env, PYTHONIOENCODING: "utf-8" } });
  return JSON.parse(out);
}

function runIcacls(targetPath) {
  try {
    return execFileSync("icacls", [targetPath], { encoding: "utf8" }).trim();
  } catch (e) {
    return "icacls 失敗: " + e.message;
  }
}

// ---- cleanup.ps1 -----------------------------------------------------------

// opts.realProfile: 実際の保存先のまま実行した後の後片付け。
//   - `-RealProfile` を渡す: cleanup.ps1 は %APPDATA%\cordmux-harness と %LOCALAPPDATA%\electron を
//     削除しなくなる (差し替えていない = この実行が作ったものではなく利用者のものだから)。
//     %LOCALAPPDATA%\cordmux\session.json は共通で削除する。
//   - `-ObsDir` を渡す: 製品がバックアップ (vencord_dir_backup.json) を書く場所は
//     obs_module_config_path 配下 = ポータブル OBS では <obs>/config/obs-studio/plugin_config/
//     cordmux/ なので、そこを cleanup.ps1 に教える (元に戻す -CallWindow が読む先)。
function runCleanup(dryRun, env, opts) {
  // オプションの組み立ては cleanupFlags (run-window-plan.mjs。テストが検証する) の 1 か所。
  // --step record の録画ファイルは、あとで別の確認に使うため録画のフォルダを残す
  // (残したものを削除するのは、確認が終わった後に cleanup.ps1 をオプションなしで 1 回実行する = README.md の手順)。
  // ProgramData のファイルの残りの監視は、VM の往復 (--layout programdata) のときだけ入れる。
  // opts.callWindow は実行スクリプトの finally だけが立てる (finallyVencordRestorePlan が call-window のとき)。
  const flags = cleanupFlags({ dryRun, ...(opts || {}) });
  try {
    return execFileSync("powershell", ["-NoProfile", "-ExecutionPolicy", "Bypass", "-File", CLEANUP_PS1, ...flags], { encoding: "utf8", env }).trim();
  } catch (e) {
    return (e.stdout || e.message || "").toString().trim();
  }
}

// check_bundle_zip.py の結果に、zip がこの実行の書き出しの後に書かれたかの判定を足す。古い zip なら ok を false にする。
function withBundleZipFreshness(result, zipPath, startedAtMs) {
  let mtimeMs = null;
  try {
    mtimeMs = fs.statSync(zipPath).mtimeMs;
  } catch {
    /* zip がないときは mtimeMs を null のままにする (judgeBundleZipFresh が古い扱いにする) */
  }
  const fresh = judgeBundleZipFresh({ mtimeMs, notBeforeMs: startedAtMs });
  return { ...result, fresh: fresh.fresh, freshReason: fresh.reason, ok: result.ok === true && fresh.fresh };
}

// ---- harness (テスト用の送信側) の起動 ------------------------------------------
// product モードは test/tools/harness (sender.ts) 側で実装している。
// ここでは HARNESS_MODE=product と、この実行で渡したいパラメータを
// HARNESS_* の環境変数として渡すだけにする (既存の sustain/send モードの命名に揃えた名前。
// README.md 参照)。
function launchHarness(step, env) {
  const dist = path.join(HARNESS_DIR, "dist", "sender.js");
  if (!fs.existsSync(dist)) {
    return { ok: false, error: "harness の dist/sender.js がない (esbuild のバンドルがまだ済んでいない。test/tools/harness で npm run build を先に通す)" };
  }
  // node_modules/.bin/electron.cmd は shell:true 経由でないと起動できないが、shell:true だと
  // 直接の子が cmd.exe になり、cmd.exe だけを kill しても electron.exe (と、その GPU/renderer の
  // 子プロセス) が生き残って残骸になる (実測で確認済み)。electron パッケージが直接
  // 収める electron.exe を shell なしで起動し、殺すときは taskkill /T /F で木ごと落とす
  // (killHarnessTree)。
  const electronBin = path.join(HARNESS_DIR, "node_modules", "electron", "dist", "electron.exe");
  if (!fs.existsSync(electronBin)) {
    return { ok: false, error: "harness の electron.exe がない (" + electronBin + "。test/tools/harness で npm install を先に通す)" };
  }
  const harnessEnv = {
    ...env,
    HARNESS_MODE: "product",
    HARNESS_NSCREEN: String(step.nScreen ?? step.n ?? 1),
    HARNESS_NVOICE: String(step.nVoice ?? 0),
    HARNESS_SECONDS: String(step.seconds ?? 10),
    HARNESS_HW: step.hw || "prefer-hardware",
    HARNESS_WARMUP: step.warmup ? "1" : "0",
  };
  // テスト用の送信側が同期用のマーカーの音を乗せる 1 つのストリームの指定。
  // ここは実行計画 (run-window-plan.mjs) が明示した値をそのまま渡すだけで、
  // 実行計画に無ければ何も渡さない (harness 側の既定に委ねる)。
  if (step.syncTone) harnessEnv.HARNESS_SYNC_TONE = step.syncTone;
  if (step.extra) {
    if (step.extra.oversend) harnessEnv.HARNESS_OVERSEND = "1";
    // failStage は個数 (数値)。falsy の 0 は「強制しない」の既定と同じ意味なので送らなくてよい。
    if (typeof step.extra.failStage === "number" && step.extra.failStage > 0) harnessEnv.HARNESS_FAIL_STAGE = String(step.extra.failStage);
    if (step.extra.maxLongEdge) harnessEnv.HARNESS_MAX_LONG_EDGE = String(step.extra.maxLongEdge);
    if (step.extra.fpsDivisor) harnessEnv.HARNESS_FPS_DIVISOR = String(step.extra.fpsDivisor);
    // --dock-scenario contact-missing: Discord の内部への依存箇所 1 つを Missing として facts に
    // 載せる (test/tools/harness/preload.js 参照)。
    if (typeof step.extra.failContact === "string" && step.extra.failContact) harnessEnv.HARNESS_FAIL_CONTACT = step.extra.failContact;
    // 実行中の解像度の変更のスケジュール。形式は
    // "<起動からの秒数>:<幅>x<高さ>" の並び ("35:1280x720,60:640x360")。形式を決めるのは
    // 実行スクリプトの側 (run-window-plan.mjs の resolutionScheduleArg) で、読むのはテスト用の送信側。
    if (typeof step.extra.resolutionSchedule === "string" && step.extra.resolutionSchedule) {
      harnessEnv.HARNESS_RESOLUTION_SCHEDULE = step.extra.resolutionSchedule;
    }
    // 画面共有のストリームごとの最初の解像度 ("1280x720,3840x2160")。読むのはテスト用の送信側
    // (test/tools/harness/preload.js の HARNESS_SCREEN_SIZES と sender.ts の parseScreenSizes)。
    if (typeof step.extra.screenSizes === "string" && step.extra.screenSizes) harnessEnv.HARNESS_SCREEN_SIZES = step.extra.screenSizes;
    // フレームだけを止める通知ファイルの場所 (絶対パス)。読むのはテスト用の送信側。
    if (typeof step.extra.framesPauseFile === "string" && step.extra.framesPauseFile) harnessEnv[HARNESS_FRAMES_PAUSE_ENV] = step.extra.framesPauseFile;
    // 声に音を乗せる通知ファイルの場所 (絶対パス。hold の --voice-users のときだけ。読むのはテスト用の送信側)。
    if (typeof step.extra.voiceToneFile === "string" && step.extra.voiceToneFile) harnessEnv[HARNESS_VOICE_TONE_ENV] = step.extra.voiceToneFile;
    // hold の --screen-names / --screen-images / --voice-users (読むのは test/tools/harness/sender_pure.ts)。画像の場所は、送信側が
    // test/tools/harness で動くので、repo のルートから決めた絶対の場所にして渡す (OBS を起動する前の確認と同じ toAbs)。
    if (typeof step.extra.screenNames === "string" && step.extra.screenNames) harnessEnv.HARNESS_SCREEN_NAMES = step.extra.screenNames;
    if (typeof step.extra.screenImages === "string" && step.extra.screenImages) {
      harnessEnv.HARNESS_SCREEN_IMAGES = screenImageFiles(step.extra.screenImages, toAbs, () => true).files.join(",");
    }
    if (typeof step.extra.voiceUsers === "string" && step.extra.voiceUsers) harnessEnv.HARNESS_VOICE_USERS = step.extra.voiceUsers;
  }
  // userData は _work/appdata の下に向ける (--real-profile のときは APPDATA を差し替えないので、環境変数では届かない)。
  const userDataDir = path.join(toAbs(APPDATA_OVERRIDE_REL), HARNESS_USER_DATA_DIR_NAME);
  const child = spawn(electronBin, harnessElectronArgs(userDataDir), { cwd: HARNESS_DIR, env: harnessEnv, stdio: ["ignore", "pipe", "pipe"] });
  return { ok: true, child };
}

// electron.exe (と、その GPU/renderer の子プロセス) をツリーごと確実に止める。
// taskkill /T /F はプロセスツリー全体に適用されるので、shell を経由しない直接起動でも安全。
async function killHarnessTree(pid) {
  if (!pid) return true;
  try {
    execFileSync("taskkill", ["/PID", String(pid), "/T", "/F"], { stdio: "ignore" });
  } catch {
    /* 既に居なければ何もしない (taskkill は対象がないとエラー終了する) */
  }
  for (let i = 0; i < 6; i += 1) {
    if (!processAlive(pid)) return true;
    await sleep(500);
  }
  return !processAlive(pid);
}

// ---- トークンなし / 違うトークンの接続 (Node 22+ の組み込み WebSocket) -----------------
// session.json の port を読み、hello を送らない・違うトークンの hello を送る、の 2 通りを試し、
// hello_ok が返らない (拒否される) ことを記録する。ws/ が実際に何を返すか (close か hello_ng か)
// は決め打ちにせず、そのまま記録する。
async function wsProbe(variant, session) {
  const url = `ws://127.0.0.1:${session.port}`;
  return new Promise((resolve) => {
    const events = [];
    const socket = new WebSocket(url);
    const done = (result) => {
      try {
        socket.close();
      } catch {
        /* 既に閉じていれば何もしない */
      }
      resolve({ variant, events, ...result });
    };
    const timer = setTimeout(() => done({ outcome: "timeout" }), 5000);
    socket.onopen = () => {
      const hello = variant === "no-secret" ? {} : { type: "hello", version: "0.0.0-window3b", secret: "0".repeat(64), wire: 1 };
      if (variant !== "no-secret") socket.send(JSON.stringify(hello));
      events.push({ at: Date.now(), note: variant === "no-secret" ? "hello を送らない" : "違うトークンの hello を送った" });
    };
    socket.onmessage = (ev) => {
      events.push({ at: Date.now(), data: String(ev.data).slice(0, 200) });
      let parsed = null;
      try {
        parsed = JSON.parse(String(ev.data));
      } catch {
        /* JSON でなければ無視 */
      }
      if (parsed && parsed.type === "hello_ok") {
        clearTimeout(timer);
        done({ outcome: "hello_ok を受けた (拒否できていない = 問題)" });
      }
    };
    socket.onclose = (ev) => {
      clearTimeout(timer);
      done({ outcome: "closed", code: ev.code, reason: ev.reason });
    };
    socket.onerror = () => {
      events.push({ at: Date.now(), note: "error" });
    };
  });
}

// ---- 手順の実行 --------------------------------------------------------------

async function runReal(args, steps, outDir) {
  fs.mkdirSync(outDir, { recursive: true });
  const windowOutDir = path.join(outDir, "window3b");
  fs.mkdirSync(windowOutDir, { recursive: true });
  const tag = args.tag || `${args.step}-${new Date().toISOString().replace(/[:.]/g, "-")}`;
  const logPath = path.join(windowOutDir, `run-${tag}.log`);
  // 実際の ID の形のメインアカウントの ID は、ログの行 (run-<tag>.log と標準出力)
  // のテキストの全部から伏せる (redactBroadcasterId。テスト用の短い ID では何もしない)。
  const log = (...a) => {
    const line = redactBroadcasterId(`[${new Date().toISOString()}] ${a.join(" ")}`, args.broadcasterId);
    console.log(line);
    fs.appendFileSync(logPath, line + "\n");
  };

  const startMs = Date.now();
  const env = childEnv(args);
  // --step install-roundtrip の move は OBS のフォルダを途中で改名するので、場所は変えられる形に持つ。
  let obsAbs = toAbs(args.obs);
  const pluginAbs = args.plugin ? toAbs(args.plugin) : "";
  const roundtrip = args.step === INSTALL_ROUNDTRIP_STEP;
  const rtPaths = roundtrip ? roundtripPaths(args.tag) : null;
  // OBS の設定の保存先。ポータブルは <obs>/config/obs-studio。--layout programdata (VM だけ) はポータブルで
  // ない OBS が読む %APPDATA%\obs-studio (子の APPDATA の差し替えは cordmux の state.json の場所にだけ適用され、
  // OBS 自身の保存先は利用者のフォルダから決まる。VM の中でだけ組める = parseArgs の --vm)。
  const installedObs = roundtrip && args.layout === "programdata";
  const obsConfigFor = (dir) => (installedObs ? path.join(process.env.APPDATA || "", "obs-studio") : path.join(dir, "config", "obs-studio"));
  let obsConfigAbs = obsConfigFor(obsAbs);

  const state = {
    firstPid: null,
    secondPid: null,
    harnessChild: null,
    firstLogFile: null,
    results: [],
    // 実行計画の gate:true が ok:true 以外で終わったもの (終了コード 3 の根拠)。
    gateFailures: [],
    // 退かしたファイル (call-gate-d の package.json)。**finally でも必ず戻す**。
    movedFiles: [],
    // わざと 1 バイト足したファイル (call-1 の連続運転) のコピー。untamper-file が戻し、**finally でも必ず戻す**。
    tamperedFiles: [],
    // label 付きの録画 (call-1 は 2 つの録画を録る)。
    recordings: {},
    // GetStats の 2 回ぶん。
    obsStats: {},
    // 対の判定が見る時間の範囲 (録画を ON にした時刻 / OFF にした時刻 /
    // 最初の送信側が終わった時刻) と、cordmux の録画のフォルダ。
    recordMarks: { recordOnMs: null, recordOffMs: null, firstHarnessEndMs: null },
    recordSessionDir: null,
    // 導入の往復: 読んだファイル (label -> 中身)、ハッシュ、ダイアログとドックのウィンドウで読んだもの、
    // 置いたもの (rt-remove-placed と install_snapshot.py が読む)、止めた手順 (stopOnFail)。
    captures: {},
    hashes: {},
    dialogs: {},
    placeReports: {},
    placed: [],
    placedRemoved: false,
    stoppedAt: null,
    obsLogsCopied: false,
    //   cellRegionCaptures: 録画ごと (label、無ければ "_last") に、開始時と終了時に読んだ枠の区画 (screenCellRegions)
    //   openRecordingKey: 現在録っている録画のキー (record-start が置き、対の record-stop が使って消す)
    //   lastRecordingKey: 最後に止めた録画のキー (label のない judge が見る録画)
    //   videoRates: measure-video-rate が読んだ映像の数 (label -> parseVideoRateSamples)
    //   obsRecordingsKept: keep-obs-recordings が済んだか (finally で二重に移さない)
    cellRegionCaptures: {},
    openRecordingKey: null,
    lastRecordingKey: null,
    videoRates: {},
    obsRecordingsKept: false,
    // 配信の受信側 (stream-sink-start が作る stream-sink.mjs の createStreamSink の戻り値。hold と call-hold は正常に
    // 終わるたびに開き直す。run-window-plan.mjs の STREAM_SINK_URL を見よ)。
    streamSinkInfo: null,
  };
  // 結果 JSON の steps に積む記録も、手順を問わずテキストの全部からメインアカウントの ID を伏せる
  // (redactBroadcasterId。実際の ID の形のときだけ。dialog-click.ps1 の Read-Edits はウィンドウの Edit の値を全部読んで edits と
  // texts に入れるので、手順の種類ごとに伏せる所を足すと漏れる)。
  const record = (name, data) => {
    const safe = redactBroadcasterId(data, args.broadcasterId);
    state.results.push({ name, ...safe });
    log(name, JSON.stringify(safe).slice(0, 2000));
  };

  // 遅延の基準のキャプチャ (ウィンドウ・画面のどちらも) が画像を捉えたか。ソースの大きさが 0 なら捉えていない (window-capture.c は
  // ウィンドウが見つかるまで描かない)。大きければ PNG を撮り、dummy_tile.py で黒でないかも見る。PNG には Vesktop のウィンドウが写るので、
  // 結果のコピーに含めない (ID_BEARING_FILE_PATTERNS の latency-reference)。check-window-capture と switch-to-monitor-capture が同じ確認をする。
  const checkLatencyCapture = async (s, ref) => {
    let size = null;
    let shot = null;
    let tile = null;
    let error = null;
    try {
      const t = await ws.call("GetSceneItemTransform", { sceneName: ref.sceneName, sceneItemId: ref.sceneItemId });
      const tr = (t && t.sceneItemTransform) || {};
      size = { sourceWidth: Number(tr.sourceWidth) || 0, sourceHeight: Number(tr.sourceHeight) || 0 };
      if (size.sourceWidth > 0 && size.sourceHeight > 0) {
        const data = await ws.call("GetSourceScreenshot", { sourceName: ref.inputName, imageFormat: "png" });
        const raw = data.imageData || "";
        const comma = raw.indexOf(",");
        shot = path.join(windowOutDir, "shot-" + tag + "-" + s.id + "-" + (s.shotLabel || "latency-reference") + ".png");
        fs.writeFileSync(shot, Buffer.from(comma >= 0 ? raw.slice(comma + 1) : raw, "base64"));
        try {
          tile = JSON.parse(execFileSync("python", [DUMMY_TILE_PY, shot], { encoding: "utf8", env: { ...env, PYTHONIOENCODING: "utf-8" } }));
        } catch (e) {
          tile = { error: (e.stdout || "").toString().slice(0, 400) || e.message };
        }
      }
    } catch (e) {
      error = e.message;
    }
    const hooked = Boolean(size && size.sourceWidth > 0 && size.sourceHeight > 0);
    const nonBlack = tile && typeof tile.notBlack === "boolean" ? tile.notBlack : null;
    return { ok: hooked && nonBlack !== false, hooked, nonBlack, size, shot, tile, error };
  };

  // ---- 配信の受信側 (stream-sink-start / stream-sink-stop / finally) ----
  // 起動・開き直し・停止は stream-sink.mjs が行う。ffmpeg は同梱の obs-deps のもの (BUNDLED_FFMPEG)、
  // 無ければ PATH のもの (判定スクリプトと同じく同梱のものを先に使い、どちらを使ったかをログに書く)。
  // 接続先は STREAM_SINK_URL (127.0.0.1) だけ。
  function stopStreamSink() {
    if (state.streamSinkInfo) state.streamSinkInfo.stop();
  }
  const streamSinkSummary = () => {
    const sink = state.streamSinkInfo;
    if (!sink) return null;
    return { ...sink.summary(), bundled: sink.info.exe === BUNDLED_FFMPEG };
  };

  // 合成のソースが描く画面共有の区画を読む。OBS のログの最後の配置の行から区画を取る (枠の番号と人の対応はログの
  // 「Screen slot N carries ...」の行から。録画の前の最後の行 = 読んだ時点の最後の行)。読めなければ ok:false を返すだけで先へ進む
  // (判定スクリプトは固定の区画にフォールバックし、そのことを regionSource に書く)。
  async function captureCellRegions(when) {
    try {
      const videoSettings = await ws.call("GetVideoSettings");
      const { cordmux } = await readCordmuxInputs(ws, textOf);
      const logLines = readFirstObsLogLines();
      const persons = harnessPersonsFromSlotStreams(screenSlotStreamsFromLines(logLines));
      return { when, ...compositeCellRegions({ layout: lastCompositeLayout(logLines), videoSettings, inputs: cordmux, persons }) };
    } catch (e) {
      return { when, ok: false, regionSource: "composite-layout", error: e.message, cells: [], skipped: [] };
    }
  }
  // 枠の名前 (sourceName) も書く。遅延を読む人の名前が枠に見つからなかったとき、テストを実行する人が
  // record-start (label latency) のログの枠の名前で区画を選べるようにするため。
  const regionSummary = (capture) => ({
    ok: capture.ok,
    error: capture.error || null,
    cells: (capture.cells || []).map((c) => ({ cell: c.cell, sourceName: c.sourceName ?? null, x: c.x, y: c.y, w: c.w, h: c.h, bitCell: c.bitCell, sourceWidth: c.sourceWidth, sourceHeight: c.sourceHeight })),
    skipped: capture.skipped || [],
    // 合成のソースがあるとき: 区画の出どころと、自分の枠の矩形 (画面共有の区画には入れない)。
    ...(capture.regionSource ? { regionSource: capture.regionSource, self: capture.self ?? null } : {}),
  });

  // call-1 の OBS の録画を、cleanup.ps1 -KeepRecordings が残す場所へ移す (keep-obs-recordings と finally で共有する)。
  // 書き込むプロセス (OBS と配信の受信側) がファイルを放すまで少し待つことがあるので、数回やり直す。
  async function keepObsRecordings() {
    if (state.obsRecordingsKept) return null;
    state.obsRecordingsKept = true;
    // call-hold: 実行スクリプトが始めていない録画 (テストを実行する人が obs-websocket で録ったもの) も label "extra-<n>" で足す。
    // OBS の録画の保存先は seed-config.py の RecFilePath = windowOutDir の直下。
    if (STEPS_KEEPING_UNLABELED_OBS_RECORDINGS.includes(args.step)) {
      let entries = [];
      try {
        entries = fs.readdirSync(windowOutDir, { withFileTypes: true }).map((d) => {
          const full = path.join(windowOutDir, d.name);
          let mtimeMs = 0;
          try {
            mtimeMs = fs.statSync(full).mtimeMs;
          } catch {
            /* 読めなければ 0 (実行の開始より前とみなして選ばない) */
          }
          return { name: d.name, path: full, isFile: d.isFile(), mtimeMs };
        });
      } catch {
        entries = [];
      }
      for (const extra of unlabeledObsRecordings({ entries, known: Object.values(state.recordings), sinceMs: startMs })) {
        state.recordings[extra.label] = extra.from;
      }
    }
    const plan = obsRecordingMoves({ recordings: state.recordings, windowOutDir, tag });
    const moved = [];
    const failed = [];
    // 移すものがないとき (録画の前に抜けた) は空のフォルダを作らない (置いたものを残さない)。
    if (plan.moves.length > 0) fs.mkdirSync(path.normalize(plan.dir), { recursive: true });
    for (const mv of plan.moves) {
      const from = path.normalize(mv.from);
      const to = path.normalize(mv.to);
      if (!fs.existsSync(from)) {
        failed.push({ label: mv.label, from, error: "not found" });
        continue;
      }
      let error = null;
      for (let attempt = 0; attempt < 5; attempt += 1) {
        try {
          fs.renameSync(from, to);
          error = null;
          break;
        } catch (e) {
          error = e.message;
          await sleep(1000);
        }
      }
      if (error) failed.push({ label: mv.label, from, error });
      else {
        moved.push({ label: mv.label, from, to });
        state.recordings[mv.label] = to;
      }
    }
    return { ok: failed.length === 0, dir: path.normalize(plan.dir), moved, failed };
  }

  // <出力>/window3b/stop-run.txt が置かれたら、期限になったときと同じ流れで手順を抜けて
  // finally (OBS を閉じる・後片付け・vencordDir を元に戻す) へ進む。待ちはどれも 1 秒間隔でこの関数を見るので、遅くとも約 2 秒で抜ける。
  const stopFile = path.join(windowOutDir, STOP_FILE_NAME);
  function stopRequested() {
    return fs.existsSync(stopFile);
  }
  function deadlineHit() {
    return stopRequested() || deadlineExceeded(startMs, Date.now(), args.deadlineMin);
  }
  // 前の実行の通知ファイルが残っていたら、始める前に消す (残ったまま始めると最初の手順で抜ける)。
  if (fs.existsSync(stopFile)) {
    try {
      fs.unlinkSync(stopFile);
      log("前の実行の停止の通知ファイルを消した:", stopFile);
    } catch (e) {
      log("前の実行の停止の通知ファイルを消せなかった (このまま始めると最初の手順で抜ける):", stopFile, e.message);
    }
  }
  log("停止の通知ファイル (置くと次の手順の境目で手順を抜け、後片付けと vencordDir の復元へ進む):", stopFile);

  const ws = new ObsWs(OBSWS_CONTROL_PORT);
  let firstLogsBefore = new Set();
  let recordingPath = null;

  // 1 つ目の OBS のログのパスを一度だけ確定させる (obsws-connect が確立した時点で確実に
  // 1 つしか存在しないため)。second-obs は途中で 2 つ目の OBS も起動するので、newestLogFile を毎回
  // mtime だけで呼び直すと、開いたまま書き込み中の 1 つ目より、閉じ終わった 2 つ目が新しく
  // 見えることがある。read-obs-log(first) / judge-backpressure /
  // judge-connection-held / wait-connection / collect-plugin-log / summarize はすべて
  // これ経由にする。
  function firstObsLogFile() {
    return state.firstLogFile || newestLogFile(obsConfigAbs, firstLogsBefore);
  }
  // 1 つ目の OBS のログの行 (読めなければ空)。合成のソースがあるときは、配置の行と判定に使う行をここから読む。
  function readFirstObsLogLines() {
    const latest = firstObsLogFile();
    if (!latest || !fs.existsSync(latest)) return [];
    return fs.readFileSync(latest, "utf8").split(/\r?\n/);
  }

  // ---- 導入の往復の場所 (実行計画の名前 -> 現在の絶対パス) ----
  // --layout plugins (ポータブルの OBS 33 以上) は OBS のフォルダーの plugins\cordmux、programdata は
  // ProgramData の plugins\cordmux。cordmux.dll と同梱のファイルは cordmux のフォルダーの直下、データは data の中。
  // OBS のフォルダーは move の場面で改名されるので、使うたびに求める。
  const moduleFolder = () => (installedObs ? path.join(programDataPluginsDir(), "cordmux") : path.join(obsAbs, "plugins", "cordmux"));
  const pluginDataDir = () => path.join(moduleFolder(), "data");
  const pluginBinDir = () => moduleFolder();
  const rtPlaces = {
    state_json: () => path.join(toAbs(rtPaths.appDataRel), "Vesktop", "state.json"),
    backup: () => path.join(obsConfigAbs, "plugin_config", "cordmux", "vencord_dir_backup.json"),
    cordmux_settings: () => path.join(obsConfigAbs, "plugin_config", "cordmux", "settings.json"),
    bundle_src: () => path.join(pluginDataDir(), "vencord-src"),
    bundle_shown: () => path.join(pluginDataDir(), "vencord"),
    mark_src: () => path.join(pluginDataDir(), "vencord-src", BUILD_MARK_FILE_NAME),
    mark_shown: () => path.join(pluginDataDir(), "vencord", BUILD_MARK_FILE_NAME),
    zip_plugin: () => findZipPluginDir(toAbs(rtPaths.zipDirRel)),
    zip_mark: () => {
      const dir = findZipPluginDir(toAbs(rtPaths.zipDirRel));
      return dir ? path.join(dir, "data", "vencord-src", BUILD_MARK_FILE_NAME) : null;
    },
    placed_dll: () => path.join(pluginBinDir(), "cordmux.dll"),
    zip_dll: () => {
      const dir = findZipPluginDir(toAbs(rtPaths.zipDirRel));
      return dir ? path.join(dir, "cordmux.dll") : null;
    },
    placed_ffmpeg: () => path.join(pluginBinDir(), "ffmpeg.exe"),
    // 利用者が自分でビルドした Vencord の場所 (seed-config.py が書いたのと同じ文字列 = 区切りは Windows の \)。
    custom_vencord: () => path.normalize(toAbs(rtPaths.customVencordRel)),
  };
  const snapshotRoots = {
    obs_config: () => obsConfigAbs,
    obs_program: () => obsAbs,
    appdata: () => toAbs(rtPaths.appDataRel),
    localappdata: () => toAbs(rtPaths.localAppDataRel),
    custom_vencord: () => path.dirname(toAbs(rtPaths.customVencordRel)),
    programdata_plugins: () => programDataPluginsDir(),
  };
  // 画面のテキストの id を引く (製品の ja-JP.ini と、"obs:" の id は現在の OBS の ja-JP.ini)。
  const textOf = makeTextLookup(() => obsAbs);
  // Vesktop の state.json の場所。**子プロセスへ渡す env の APPDATA から組む**: --real-profile なら実際の
  // %APPDATA%、それ以外 (--rehearsal) なら差し替えた _work/appdata。process.env から組むと、リハーサルが
  // 実際の state.json を読みに行ってしまう。
  const stateJsonPathFor = (rel) => path.join(env.APPDATA || process.env.APPDATA || "", ...String(rel).split("/"));
  const readTextOrNull = (file) => {
    try {
      return fs.readFileSync(file, "utf8");
    } catch {
      return null;
    }
  };
  // Vencord の設定ファイルの現在のものを、元のコピーの隣へコピーし、キーの上で比較する
  // (compare-vencord-settings と、比較する前に実行が終わったときの finally)。値は返さない (settingsKeyDiff)。元に戻さない。
  function compareVencordSettings(s) {
    const srcPath = stateJsonPathFor(s.appDataRel);
    const beforePath = path.join(windowOutDir, s.beforeName);
    const afterPath = path.join(windowOutDir, s.afterName);
    const beforeExists = fs.existsSync(beforePath);
    const nowExists = fs.existsSync(srcPath);
    if (state.vencordSettings) state.vencordSettings.compared = true;
    try {
      if (nowExists) fs.copyFileSync(srcPath, afterPath);
      const before = beforeExists ? parseJsonTextOrUndefined(fs.readFileSync(beforePath, "utf8")) : null;
      const after = nowExists ? parseJsonTextOrUndefined(fs.readFileSync(afterPath, "utf8")) : null;
      if (before === undefined || after === undefined) return { ok: false, error: "a settings file is not valid JSON", beforeExists, nowExists };
      const diff = settingsKeyDiff(before ?? {}, after ?? {});
      // mustMatch (アンインストールの後の比較) は、違いがあれば失敗にする。実行の開始時のコピーを取れていないときも失敗。
      const verdict = s.mustMatch
        ? judgeVencordSettingsRestored({ copied: Boolean(state.vencordSettings), compared: { ok: true, changed: diff.changes.length } })
        : { ok: true, reason: null };
      return {
        ok: verdict.ok,
        ...(verdict.reason ? { reason: verdict.reason } : {}),
        beforeExists,
        nowExists,
        changed: diff.changes.length,
        changes: diff.changes,
        truncated: diff.truncated,
        before: beforeExists ? beforePath : null,
        after: nowExists ? afterPath : null,
        note: "not restored; restore only with the user's consent, from the copy taken before the run",
      };
    } catch (e) {
      return { ok: false, error: e.message, beforeExists, nowExists };
    }
  }
  const placedListPath = () => path.join(windowOutDir, "rt-placed-" + tag + ".json");
  const snapshotPath = (label) => path.join(windowOutDir, "snapshot-" + tag + "-" + label + ".json");
  const comparePath = (label) => path.join(windowOutDir, "compare-" + tag + "-" + label + ".json");

  function copyObsLogsOnce() {
    if (state.obsLogsCopied) return null;
    const from = path.join(obsConfigAbs, "logs");
    const to = path.join(windowOutDir, "obs-logs-" + tag);
    if (!fs.existsSync(from)) return { copied: 0, from, to };
    fs.mkdirSync(to, { recursive: true });
    let copied = 0;
    for (const name of fs.readdirSync(from)) {
      fs.copyFileSync(path.join(from, name), path.join(to, name));
      copied += 1;
    }
    state.obsLogsCopied = true;
    return { copied, from, to };
  }

  function removePlaced() {
    const removed = [];
    const failed = [];
    for (const entry of state.placed) {
      if (entry.createdDir) continue; // 置くときに作ったフォルダーは、下で空なら消す
      try {
        // 置いた先は OBS のフォルダの下か ProgramData の plugins\cordmux だけ (それ以外は消さない)。
        const relToObs = path.relative(obsAbs, entry.abs);
        const underObs = relToObs !== "" && !relToObs.startsWith("..") && !path.isAbsolute(relToObs);
        const underProgramData = entry.root === "programdata_plugins" && path.basename(entry.abs) === "cordmux";
        if (!underObs && !underProgramData) throw new Error("placed path outside the OBS folder: " + entry.abs);
        fs.rmSync(entry.abs, { recursive: true, force: true });
        removed.push(entry.abs);
      } catch (e) {
        failed.push({ path: entry.abs, error: e.message });
      }
    }
    // 置くときに作ったフォルダー (OBS 33 のポータブルの plugins など) は、空になったら消す (アンインストールと同じ)。深い順に。
    const created = state.placed.filter((entry) => entry.createdDir).sort((a, b) => b.rel.split("/").length - a.rel.split("/").length);
    for (const entry of created) {
      try {
        const relToObs = path.relative(obsAbs, entry.abs);
        if (relToObs === "" || relToObs.startsWith("..") || path.isAbsolute(relToObs)) throw new Error("created folder outside the OBS folder: " + entry.abs);
        if (fs.existsSync(entry.abs) && fs.readdirSync(entry.abs).length === 0) {
          fs.rmdirSync(entry.abs);
          removed.push(entry.abs);
        }
      } catch (e) {
        failed.push({ path: entry.abs, error: e.message });
      }
    }
    state.placedRemoved = failed.length === 0;
    return { ok: failed.length === 0 && removed.length > 0, removed, failed };
  }

  // 期限で抜けた・予期しない例外で抜けた、のどちらの経路でも結果 JSON を書いてから finally に
  // 入る (write-result 手順まで届かないことがあるため)。二重に書かないよう一度
  // 書いたら以後は何もしない。
  // 手順の列を最後まで進めずに抜けたか (期限で打ち切った / 予期しない例外)。ゲートの判定に
  // 届かないまま終わったのに終了コード 0 を返すと、**失敗が成功に見える**
  // ので、main() がこれを見て終了コードを決める。
  let aborted = null;
  // finally が書く vencordDir の最後の確認と、比較する前に終わった実行での Vencord の設定の比較 (main() が読む)。
  let vencordFinal = null;
  let finallyVencordSettings = null;

  let resultWritten = false;
  // 結果 JSON の args にはメインアカウントの ID を書かない (argsForResult が REDACTED_BROADCASTER_ID に置き換える)。
  const resultArgs = argsForResult(args);
  function writeResultNow() {
    if (resultWritten) return;
    resultWritten = true;
    const file = path.join(windowOutDir, `result-${args.step}-${tag}.json`);
    fs.writeFileSync(file, JSON.stringify({ tag, args: resultArgs, startedAt: new Date(startMs).toISOString(), gateFailures: state.gateFailures, steps: state.results }, null, 2));
    state.resultFilePath = file;
    record("write-result", { file });
  }

  // ---- harness (テスト用の送信側) の起動と完了待ち ---------------------------
  // --step second-obs / discord-first は、harness を起動したまま obs-websocket の
  // 呼び出し (シーンコレクションの切り替え、OBS 自体の遅延起動) を挟む必要があるため、
  // 起動 (startHarnessProcess) と完了待ち (waitHarnessProcess) を分けている。
  // "harness" 手順 (through/integration/guard) は
  // 両方を続けて呼ぶ。
  function startHarnessProcess(hsIn) {
    state.harnessProductOutBefore = fs.existsSync(PRODUCT_OUT_DIR) ? fs.readdirSync(PRODUCT_OUT_DIR) : [];
    let hs = hsIn;
    // 実行計画が framesPause を立てたテスト用の送信側には、フレームだけを止める通知ファイルの場所を渡す。前の実行の通知ファイルが
    // 残っていると起動した直後からフレームが止まるので、渡す前に消す (消せなければ渡さずにログへ残す)。
    if (hsIn && hsIn.framesPause) {
      const file = path.join(windowOutDir, FRAMES_PAUSE_FILE_PREFIX + tag + FRAMES_PAUSE_FILE_SUFFIX);
      try {
        if (fs.existsSync(file)) fs.unlinkSync(file);
        hs = { ...hsIn, extra: { ...(hsIn.extra || {}), framesPauseFile: file } };
        state.framesPauseFile = file;
      } catch (e) {
        log("フレームを止める通知ファイルの前の残りを消せなかったので、テスト用の送信側に渡さない:", file, e.message);
      }
    }
    // hold の --voice-users: 声に音を乗せる通知ファイルの場所を渡す (置き方と消し方はフレームを止める通知ファイルと同じ)。
    if (hsIn && hsIn.voiceTone) {
      const file = path.join(windowOutDir, VOICE_TONE_FILE_PREFIX + tag + VOICE_TONE_FILE_SUFFIX);
      try {
        if (fs.existsSync(file)) fs.unlinkSync(file);
        hs = { ...hs, extra: { ...(hs.extra || {}), voiceToneFile: file } };
        state.voiceToneFile = file;
      } catch (e) {
        log("声に音を乗せる通知ファイルの前の残りを消せなかったので、テスト用の送信側に渡さない:", file, e.message);
      }
    }
    const r = launchHarness(hs, env);
    if (!r.ok) return r;
    state.harnessChild = r.child;
    state.harnessOut = "";
    state.harnessExited = false;
    state.harnessExitCode = null;
    state.harnessSpawnError = null;
    r.child.stdout.on("data", (d) => (state.harnessOut += d.toString()));
    r.child.stderr.on("data", (d) => (state.harnessOut += d.toString()));
    r.child.on("exit", (c) => {
      state.harnessExitCode = c;
      state.harnessExited = true;
    });
    // spawn 自体が非同期に失敗すると (electron.exe が居ない等) "error" が来る。"exit" は
    // 発火しないので、listener が無ければ未捕捉例外でプロセスごと落ち、finally (OBS を
    // 閉じる・killHarnessTree・cleanup.ps1) を通らずに起動済みの OBS が残る。
    r.child.on("error", (e) => {
      state.harnessSpawnError = e.message;
      state.harnessExited = true;
    });
    return r;
  }

  async function waitHarnessProcess(hs) {
    const child = state.harnessChild;
    if (!child) return { ok: false, error: "harness が起動していない (harness-start を先に呼ぶ)" };
    // 長い待ちを 1 秒間隔にして、実行の期限 (deadlineHit) も毎回見る。
    const hardDeadlineMs = Date.now() + ((hs.seconds || 30) + 60) * 1000;
    while (!state.harnessExited && Date.now() < hardDeadlineMs && !deadlineHit()) {
      await sleep(1000);
    }
    const timedOut = !state.harnessExited;
    if (timedOut) await killHarnessTree(child.pid);
    state.harnessChild = null;

    if (state.harnessSpawnError) {
      return { ok: false, error: "electron の起動に失敗した: " + state.harnessSpawnError, timedOut: false, productResultFile: null, productResult: null, tail: [] };
    }

    const productOutAfter = fs.existsSync(PRODUCT_OUT_DIR) ? fs.readdirSync(PRODUCT_OUT_DIR) : [];
    const newFiles = pickNewResultFiles(state.harnessProductOutBefore || [], productOutAfter, "product-send-");
    let productResultFile = null;
    let productResult = null;
    if (newFiles.length > 0) {
      const chosen = newFiles
        .map((f) => ({ f, mtimeMs: fs.statSync(path.join(PRODUCT_OUT_DIR, f)).mtimeMs }))
        .sort((a, b) => b.mtimeMs - a.mtimeMs)[0].f;
      productResultFile = `${hs.id}-${chosen}`;
      fs.copyFileSync(path.join(PRODUCT_OUT_DIR, chosen), path.join(windowOutDir, productResultFile));
      try {
        productResult = JSON.parse(fs.readFileSync(path.join(PRODUCT_OUT_DIR, chosen), "utf8"));
      } catch {
        /* 壊れていれば null のまま (tail のログで診断する) */
      }
    }
    state.lastHarnessData = productResult ? productResult.data : null;
    return { ok: true, code: state.harnessExitCode, timedOut, productResultFile, productResult, tail: (state.harnessOut || "").split(/\r?\n/).slice(-10) };
  }

  try {
    for (const rawStep of steps) {
      // 手順の中の textRef (人への文、obs-websocket のリクエストの中のシーン名など) を、ini のテキストに置き換えてから実行する。
      // 引けなかった id はそのまま残り、ログに text-refs-missing が出る (gate の手順は下の各 case が失敗にする)。
      const expanded = expandTextRefsDeep(rawStep, textOf);
      const s = expanded.value;
      if (expanded.missing.length > 0) record("text-refs-missing", { step: s.id, ids: expanded.missing });
      if (deadlineHit()) {
        // 停止の通知ファイルで抜けたのか期限で抜けたのかを分けて書く (どちらも残りの判定は付かない = 終了コード 3)。
        if (stopRequested()) {
          record("stop-requested", { atStep: s.id, stopFile });
          aborted = "stopped";
        } else {
          record("deadline-exceeded", { atStep: s.id });
          aborted = "deadline";
        }
        writeResultNow();
        break;
      }
      // stopOnFail の手順 (測定の基準に利用者のソースが入った) が失敗したら、後片付けの 4 つ
      // (OBS のログをコピーする・結果・要約・後片付け) だけを通し、残りは飛ばす (ばらつきで利用者の変化を隠したまま進めない)。
      if (state.stoppedAt && !FINISH_KINDS.includes(s.kind)) {
        record("skipped", { step: s.id, kind: s.kind, reason: "stopped at " + state.stoppedAt });
        continue;
      }
      log("手順:", s.id, s.label);
      const resultsBefore = state.results.length;
      switch (s.kind) {
        case "seed-config": {
          // --dock-install (既定 elsewhere) と、APPDATA の差し替え先・vencord-dist の
          // 場所を渡す。seed-config.py は末尾に
          // CORDMUX_SEED_CONFIG_JSON: で始まる 1 行を書くので、それを拾って結果 JSON に残す
          // (--dock-install ours で vencord-dist がないときの「vencord-dist がない」記録を
          // 結果 JSON まで届かせるため。stdio:"inherit" のままだと標準出力は画面に出るだけで
          // state.results に残らない)。
          const dockInstall = s.dockInstall || DEFAULT_DOCK_INSTALL;
          // 誰を枠に入れるか。cordmux の settings.json に書くのは
          // seed-config.py の役目で、ここは実行計画が決めた値を渡すだけである。
          const assignMode = s.assignMode || DEFAULT_DOCK_ASSIGN_MODE;
          // settings.json へ書く鍵は assign_mode / allow_state_json_write /
          // decoder の 3 つだけ。**--appdata は実行計画が useAppData:false と言った呼び出し
          // (call-*) では渡さない** = 偽の state.json を作らない (本物を書くのは製品の
          // [Vesktop に追加] だけ)。
          const decoder = s.decoder || DEFAULT_DECODER;
          // 引数は seedConfigArgv の 1 か所で組む (--dry-run の seedPreview も同じ引数で呼ぶ)。
          const seedArgs = seedConfigArgv(s, args, { obsAbs, windowOutDir, obsConfigAbs });
          let seedOut = "";
          try {
            seedOut = execFileSync("python", seedArgs, { encoding: "utf8", env: { ...process.env, PYTHONIOENCODING: "utf-8" } });
          } catch (e) {
            // seed-config.py の安全確認 (終了コード 4 / 3) に掛かった場合。何も
            // 書かれていないので、ここで手順を止める (設定のない OBS を起こさない)。
            const refused = lastPrefixedJson((e.stdout || "").toString(), "CORDMUX_SEED_CONFIG_JSON:");
            record("seed-config", { ok: false, exitCode: typeof e.status === "number" ? e.status : null, seed: refused, argv: seedArgs.slice(1) });
            throw new Error("seed-config.py refused (exit " + e.status + "): " + ((refused && refused.reason) || e.message));
          }
          log(seedOut.trimEnd());
          const seedResult = lastPrefixedJson(seedOut, "CORDMUX_SEED_CONFIG_JSON:");
          record("seed-config", {
            ok: true,
            shape: s.shape || "test",
            dockInstall,
            assignMode,
            decoder,
            allowStateJsonWrite: !!s.allowStateJsonWrite,
            useAppData: s.useAppData !== false,
            // seed-config.py が実際に書いた値 (settings.json を書かない場合は null)。
            allowStateJsonWriteWritten: seedResult ? seedResult.allowStateJsonWrite : null,
            seed: seedResult,
          });
          break;
        }
        case "copy-file":
        case "copy-dir-contents":
        case "copy-dir": {
          // 3 つとも 1 回の place で済ませる (最初の copy-* の手順が来たときにまとめて実行し、
          // 2 個目以降は前の結果を使い回す)。
          if (!state.placeReport) state.placeReport = placeFiles(pluginAbs, obsAbs);
          record("place", { report: state.placeReport });
          break;
        }
        case "launch-obs": {
          // move の場合: 実行計画が別の場所の OBS (改名した後) を名指しすることがある。
          if (roundtrip && s.obs) {
            obsAbs = toAbs(s.obs);
            obsConfigAbs = obsConfigFor(obsAbs);
          }
          firstLogsBefore = listLogFiles(obsConfigAbs);
          state.firstLogFile = null;
          // 実行のたびに「初回」としてドックを浮いたウィンドウで出す (この印は実際の運用のためのもので、
          // 残っていると 2 回目以降の scenario でドックが撮れない)。
          const shownOnce = path.join(obsConfigAbs, "plugin_config", "cordmux", "dock_shown_once");
          if (fs.existsSync(shownOnce)) fs.unlinkSync(shownOnce);
          const child = launchObs(obsAbs, env, s.launchArgs);
          state.firstPid = child.pid;
          record("launch-obs", { pid: child.pid, obs: obsAbs, launchArgs: s.launchArgs || OBS_LAUNCH_ARGS });
          break;
        }
        case "obsws-connect": {
          // old-obs / newer-libobs の scenario では OBS が落ちる・止まることも答えなので、tolerant の手順だけ
          // 繋がらなくても例外にせず記録して先へ進む (記録と画面を撮るため)。
          let ver = null;
          try {
            await ws.connect(90000);
            // 繋がった直後は OBS がまだ読み込み中で、最初の要求に 207 (NotReady) を返すことがある
            // (起動から数秒で繋がり GetVersion が 207 になる)。1 秒おきに最長 60 秒まで待つ。
            // 207 以外の誤りはそのまま投げる。
            const notReadyDeadline = Date.now() + 60000;
            for (;;) {
              try {
                ver = await ws.call("GetVersion");
                break;
              } catch (e) {
                if (!isObsNotReadyError(e.message) || Date.now() > notReadyDeadline) throw e;
                await new Promise((r) => setTimeout(r, 1000));
              }
            }
          } catch (e) {
            if (!s.tolerant) throw e;
            state.firstLogFile = newestLogFile(obsConfigAbs, firstLogsBefore);
            record("obsws-connect", { ok: false, tolerant: true, error: e.message, firstLogFile: state.firstLogFile });
            break;
          }
          // obs-websocket が答えた時点で OBS のログファイルは確実に存在し、2 つ目の OBS は
          // まだ居ない。この後 launch-obs-second が起きても迷わないよう、ここで一度だけ
          // 確定させる。
          state.firstLogFile = newestLogFile(obsConfigAbs, firstLogsBefore);
          record("obsws-connect", { ok: true, obsVersion: ver.obsVersion, wsVersion: ver.obsWebSocketVersion, firstLogFile: state.firstLogFile });
          break;
        }
        case "obsws-call": {
          // 枠のソースは型 id と slot で見る (expectSources。名前は人が載ると変わる)。シーンは ini の名前 (expect)。
          if (s.expectSources) {
            const { cordmux } = await readCordmuxInputs(ws, textOf);
            const missing = s.expectSources.filter((ref) => !findCordmuxInput(cordmux, ref)).map(sourceRefLabel);
            record("obsws-call", { request: s.request, sources: cordmux.map((x) => ({ name: x.inputName, kind: x.kind, slot: x.slot })), expectSources: s.expectSources, missing });
            break;
          }
          const data = await ws.call(s.request);
          const names = s.request === "GetSceneList" ? (data.scenes || []).map((x) => x.sceneName) : (data.inputs || []).map((x) => x.inputName);
          const missing = (s.expect || []).filter((n) => !names.includes(n));
          record("obsws-call", { request: s.request, names, expect: s.expect, missing });
          break;
        }
        case "wire-composite": {
          // 合成のソース (cordmux_composite) をシーン (s.sceneName。既定は録画するシーン Main) に 1 つ置く。キャンバス全体 (位置 0,0、
          // 拡大なし)。製品はシーンもソースも作らないので、OBS の [+] から足す操作の代わりにここで作る。同じ型のソースが既にあれば
          // 作らずにシーンの項目だけ足す。
          const sceneName = s.sceneName || WIRE_SCENE_NAME;
          try {
            const { all, cordmux } = await readCordmuxInputs(ws, textOf);
            const existing = findCordmuxInput(cordmux, { kind: "composite" });
            let inputName;
            let sceneItemId;
            if (existing) {
              inputName = existing.inputName;
              const item = await ws.call("CreateSceneItem", { sceneName, sourceName: inputName });
              sceneItemId = item.sceneItemId;
            } else {
              inputName = s.inputName || COMPOSITE_INPUT_NAME;
              if (all.some((x) => x && x.inputName === inputName)) throw new Error("an input named " + inputName + " already exists and is not a composite source");
              const made = await ws.call("CreateInput", {
                sceneName,
                inputName,
                inputKind: CORDMUX_SOURCE_KINDS.composite,
                inputSettings: {},
                sceneItemEnabled: true,
              });
              sceneItemId = made.sceneItemId;
            }
            await ws.call("SetSceneItemTransform", {
              sceneName,
              sceneItemId,
              sceneItemTransform: { positionX: 0, positionY: 0, scaleX: 1, scaleY: 1, rotation: 0 },
            });
            record("wire-composite", { ok: true, sceneName, inputName, inputKind: CORDMUX_SOURCE_KINDS.composite, sceneItemId, reusedInput: !!existing });
          } catch (e) {
            // 合成のソースがないまま録画の判定に進むと全部が「区画が取れない」になるので、投げて回を止める。
            throw new Error("wire-composite: " + e.message);
          }
          break;
        }
        case "composite-layout-snapshot": {
          // OBS の記録の最後の配置の行と、枠の番号 -> 人の対応 (合成の回は記録の Screen slot の行から) を読む。判定は
          // judge-composite-layout (label で引く)。配置の行が無ければ layout は null で、判定が「読めていない」で赤にする。
          try {
            // 合成の回は枠の公開のソースがないので、枠の番号と人の対応は記録の「Screen slot N carries ...」の最後の行から引く
            // (ストリームの番号 -> harness-screen-<番号>。行がない枠は人が不明)。
            const logLines = readFirstObsLogLines();
            const layout = lastCompositeLayout(logLines);
            const slotStreams = screenSlotStreamsFromLines(logLines);
            // 実際の通話 (s.namesUnknown) はストリームの番号が人と結び付かないので、人は不明のまま番号だけを残す。
            const persons = s.namesUnknown ? {} : harnessPersonsFromSlotStreams(slotStreams);
            record("composite-layout-snapshot", { ok: layout !== null, label: s.label, layout, persons, slotStreams, personsFrom: s.namesUnknown ? "unknown" : "screen-slot-lines", ...(layout ? {} : { error: "no Composite layout line in the OBS log" }) });
          } catch (e) {
            record("composite-layout-snapshot", { ok: false, label: s.label, layout: null, persons: {}, error: e.message });
          }
          break;
        }
        case "judge-composite-layout": {
          // 前後 3 つの配置の読み (composite-layout-snapshot の label) に「外した人のタイルが消えて後ろが詰まり、戻すと元に戻る」を当てる。
          const labels = s.labels || AC30_ITEM_LABELS;
          const snap = (label) => {
            const r = [...state.results].reverse().find((x) => x && x.name === "composite-layout-snapshot" && x.label === label) || null;
            return r ? { layout: r.layout || null, persons: r.persons || {}, slotStreams: r.slotStreams || null } : null;
          };
          record("judge-composite-layout", judgeCompositeLayout({ before: snap(labels.before), afterRemove: snap(labels.afterRemove), afterRestore: snap(labels.afterRestore), person: s.person }));
          break;
        }
        case "judge-display-delay": {
          // 表示の遅れの測り: OBS の記録 (OBS を閉じた後なので書き終わっている) の video_latency の shown_n が 0 でない。
          const lines = readFirstObsLogLines();
          if (lines.length === 0) {
            record("judge-display-delay", { ok: false, reason: "the OBS log is missing" });
            break;
          }
          record("judge-display-delay", judgeDisplayDelay(videoLatencyShownFromLog(lines)));
          break;
        }
        case "judge-audio-buffering": {
          // OBS の全体の音の待ち時間の増え: 「adding N milliseconds of audio buffering」の行を時刻とソースの名前ごとに記録する (線はまだない)。
          const lines = readFirstObsLogLines();
          if (lines.length === 0) {
            record("judge-audio-buffering", { ok: false, reason: "the OBS log is missing" });
            break;
          }
          record("judge-audio-buffering", judgeAudioBuffering(audioBufferingFromLog(lines)));
          break;
        }
        case "check-file-exists": {
          const p = toAbs(s.path);
          record("check-file-exists", { path: p, exists: fs.existsSync(p) });
          break;
        }
        case "check-guide-update-hint": {
          // old-obs: 手順書 (日本語と英語) の「ドックが表示されない」の項に、OBS を 33 以上へ更新する案内があるか。
          const guides = (s.hints || []).map((h) => {
            const file = toAbs(h.rel);
            return { ...h, text: fs.existsSync(file) ? fs.readFileSync(file, "utf8") : "" };
          });
          const judged = judgeGuideUpdateHint(guides);
          record("check-guide-update-hint", { ok: judged.ok, reason: judged.reason, items: judged.items });
          break;
        }
        case "icacls": {
          const p = toAbs(s.path);
          record("icacls", { path: p, output: runIcacls(p) });
          break;
        }
        case "launch-obs-second": {
          state.secondLogsBefore = listLogFiles(obsConfigAbs);
          const child = launchObs(obsAbs, env);
          state.secondPid = child.pid;
          record("launch-obs-second", { pid: child.pid });
          await sleep(5000); // ログに BlockedByOtherWriter が出るまでの猶予
          break;
        }
        case "read-obs-log": {
          const isSecond = s.target === "second";
          const latest = isSecond ? newestLogFile(obsConfigAbs, state.secondLogsBefore || new Set()) : firstObsLogFile();
          let hit = false;
          let sampleLines = [];
          if (latest) {
            const text = fs.readFileSync(latest, "utf8");
            const lines = text.split(/\r?\n/);
            if (s.pattern === "module-loaded") {
              // obs-module.c の「Loaded Modules:」節の中だけを見る (節の外にある読み込み失敗の
              // 行を誤って hit=true と数えないため)。
              const judged = judgeModuleLoaded(lines, (s.moduleName || "") + ".dll");
              hit = judged.hit;
              sampleLines = judged.blockLines.length > 0 ? judged.blockLines : lines.filter((l) => /cordmux|Loaded Modules|Failed to load module|not loaded/i.test(l));
            } else if (s.pattern === "module-not-loaded") {
              // old-obs: OBS が最後まで起動し、「Loaded Modules:」の節にも「Loading module:」の行にも cordmux がない。
              const judged = judgeModuleNotLoaded(lines, (s.moduleName || "") + ".dll");
              hit = judged.hit;
              sampleLines = judged.loadingLines.length > 0 ? judged.loadingLines : judged.blockLines;
            } else if (s.pattern === "blocked-by-other-writer") {
              sampleLines = lines.filter((l) => /BlockedByOtherWriter|cordmux/i.test(l));
              // 製品の記録の目印 (plugin-main.cpp)。現在は英語の文で、列挙子の名前は書かない。古い記録の列挙子の名前も読む。
              hit = lines.some((l) => /BlockedByOtherWriter|Not waiting for Vesktop: another OBS is already running cordmux/.test(l));
            } else if (s.pattern === "ffmpeg-versions") {
              // motw / dll-clash の確認: src-obs/plugin-main.cpp の起動時の 1 行 (同梱 FFmpeg の版)。
              sampleLines = lines.filter((l) => /\[cordmux\].*(?:ffmpeg|FFmpeg) avcodec=/.test(l));
              hit = sampleLines.length > 0;
            } else if (s.pattern === "cordmux-lines") {
              // old-obs / newer-libobs の確認: cordmux に触れた行を全部 (読み込めなかった理由を探す)。
              sampleLines = lines.filter((l) => /cordmux|Failed to load module|not loaded|MODULE_|incompatible/i.test(l));
              hit = sampleLines.length > 0;
            }
          }
          // ok は hit と同じ (実行計画が gate を付けたときに終了コードへ届かせるため)。
          record("read-obs-log", { ok: hit, pattern: s.pattern, target: s.target || "first", logFile: latest, hit, sampleLines: sampleLines.slice(0, s.pattern === "cordmux-lines" ? 40 : 20) });
          break;
        }
        case "close-obs": {
          if (s.target === "second") {
            const exited = await closeObsGracefully(state.secondPid);
            record("close-obs", { target: "second", exited });
          } else {
            const exited = await closeObsGracefully(state.firstPid);
            record("close-obs", { target: "first", exited });
          }
          break;
        }
        case "harness": {
          const r = startHarnessProcess(s);
          if (!r.ok) {
            record("harness", { ok: false, error: r.error });
            break;
          }
          const result = await waitHarnessProcess(s);
          record("harness", result);
          break;
        }
        case "harness-start": {
          // second-obs / discord-first 用: 起動だけして次の手順 (obsws の操作や
          // OBS の起動) へ進む。完了は後続の "harness-wait" が待つ。
          const r = startHarnessProcess(s);
          const pauseNote = {
            ...(s.framesPause ? { framesPauseFile: state.framesPauseFile || null } : {}),
            ...(s.voiceTone ? { voiceToneFile: state.voiceToneFile || null } : {}),
          };
          record("harness-start", r.ok ? { ok: true, pid: r.child.pid, ...pauseNote } : { ok: false, error: r.error, ...pauseNote });
          break;
        }
        case "harness-wait": {
          const result = await waitHarnessProcess(s);
          // 録画の ON / OFF の対の判定は「録画を ON にした後、最初の送信側が終わる前」の
          // 帯だけを見る (共有の終わりの黒は正当に出うるので、そこまで見ると偽の失敗になる)。
          // その帯の終わりがここである。**最初の 1 回だけ**覚える。
          if (state.recordMarks && state.recordMarks.firstHarnessEndMs === null) state.recordMarks.firstHarnessEndMs = Date.now();
          record("harness-wait", result);
          // --dock-scenario degraded の確認用: harness-wait が
          // copyEncodeFacts を持つときだけ、直近の configure の寸法と facts.stages を
          // 結果 JSON の 1 段目 (steps 配列) へコピーする (5 段掘らずに人が読める形にするため。
          // encodeFactsFromHarnessData は run-window-plan.mjs の純関数)。
          if (s.copyEncodeFacts) {
            record("encode-facts", encodeFactsFromHarnessData(state.lastHarnessData));
          }
          break;
        }
        case "wait": {
          // 1 秒刻みで待ち、毎回 期限 (deadlineHit) も見る。call-1 の定常状態は 20 分の
          // 1 回の待ちになるので、丸ごと眠ると期限で止まれない (実行を途中で止めたい
          // ときに打ち切りが動作しない)。
          const ms = s.ms || 0;
          let waited = 0;
          while (waited < ms && !deadlineHit()) {
            const chunk = Math.min(1000, ms - waited);
            await sleep(chunk);
            waited += chunk;
          }
          record("wait", { ms, waited });
          break;
        }
        case "copy-state-json": {
          // 実際の state.json のコピーを結果フォルダへ取る (証拠)。
          // **読むだけ** (書くのは製品の [Vesktop に追加] を押した結果だけ)。場所は
          // 実際の %APPDATA% 配下で、--real-profile のときだけこの手順が実行計画に現れる。
          const srcPath = stateJsonPathFor(s.appDataRel);
          const destPath = path.join(windowOutDir, s.destName);
          try {
            const text = fs.readFileSync(srcPath, "utf8");
            fs.writeFileSync(destPath, text);
            state.stateJsonBeforeText = text;
            record("copy-state-json", { ok: true, source: srcPath, dest: destPath, bytes: Buffer.byteLength(text) });
          } catch (e) {
            record("copy-state-json", { ok: false, source: srcPath, dest: destPath, error: e.message });
          }
          break;
        }
        case "read-state-json": {
          // 実際の state.json を読み直す (読むだけ)。読んだ全文は
          // 次の judge-vencord-dir が使う。結果 JSON には vencordDir の値と鍵の一覧だけを
          // 残す (state.json の全文には利用者の設定が入るので、丸ごとは残さない)。
          const srcPath = stateJsonPathFor(s.appDataRel);
          try {
            const text = fs.readFileSync(srcPath, "utf8");
            state.stateJsonAfterText = text;
            let keys = null;
            let vencordDir = null;
            try {
              const parsed = JSON.parse(text);
              if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) {
                keys = Object.keys(parsed);
                vencordDir = typeof parsed.vencordDir === "string" ? parsed.vencordDir : null;
              }
            } catch {
              /* 壊れていれば keys は null のまま (judge-vencord-dir が理由を出す) */
            }
            record("read-state-json", { ok: true, label: s.label || null, source: srcPath, keys, vencordDir, bytes: Buffer.byteLength(text) });
          } catch (e) {
            state.stateJsonAfterText = null;
            record("read-state-json", { ok: false, label: s.label || null, source: srcPath, error: e.message });
          }
          break;
        }
        case "judge-vencord-dir": {
          // vencordDir が書き換わったかの判定 (純関数 judgeVencordDirWritten)。「書き換える前の
          // コピー」は同じ --out に置いた STATE_JSON_BEFORE_NAME を読む (call-gate-b は別の
          // プロセスなので、state ではなくファイルから拾える形にしてある)。
          const beforePath = path.join(windowOutDir, STATE_JSON_BEFORE_NAME);
          let beforeText = state.stateJsonBeforeText || null;
          if (!beforeText && fs.existsSync(beforePath)) {
            try {
              beforeText = fs.readFileSync(beforePath, "utf8");
            } catch {
              beforeText = null;
            }
          }
          const judged = judgeVencordDirWritten({
            beforeText,
            afterText: state.stateJsonAfterText,
            expectedDir: toAbs(s.expectedDir),
            compareOtherKeys: s.compareOtherKeys !== false,
          });
          record("judge-vencord-dir", { ...judged, expectedDir: toAbs(s.expectedDir), beforeFile: beforePath, compareOtherKeys: s.compareOtherKeys !== false });
          break;
        }
        case "move-file": {
          // 配置物の package.json を一時的に退かす。戻すのは
          // restore-file と finally の両方 (実行の途中で抜けても置いたものを残さない)。
          const from = toAbs(s.path);
          const to = path.join(windowOutDir, s.destName);
          try {
            fs.renameSync(from, to);
            state.movedFiles.push({ id: s.restoreId, from, to });
            record("move-file", { ok: true, restoreId: s.restoreId, from, to });
          } catch (e) {
            record("move-file", { ok: false, restoreId: s.restoreId, from, to, error: e.message });
          }
          break;
        }
        case "restore-file": {
          const entry = state.movedFiles.find((m) => m.id === s.restoreId);
          if (!entry) {
            record("restore-file", { ok: false, restoreId: s.restoreId, error: "退かした記録がない (move-file が失敗した可能性)" });
            break;
          }
          try {
            fs.renameSync(entry.to, entry.from);
            state.movedFiles = state.movedFiles.filter((m) => m !== entry);
            record("restore-file", { ok: true, restoreId: s.restoreId, from: entry.to, to: entry.from });
          } catch (e) {
            record("restore-file", { ok: false, restoreId: s.restoreId, from: entry.to, to: entry.from, error: e.message });
          }
          break;
        }
        case "obs-stats": {
          // GetStats を record-start の 15 秒後と record-stop の直前の 2 回。
          // 結果 JSON には 4 つの値と、録画を始めてからの経過を残す (2 点の差分で数えるため、
          // どの区間の値かが後から分かる必要がある)。
          try {
            const data = await ws.call("GetStats");
            const sample = {
              outputSkippedFrames: data.outputSkippedFrames,
              outputTotalFrames: data.outputTotalFrames,
              renderSkippedFrames: data.renderSkippedFrames,
              renderTotalFrames: data.renderTotalFrames,
              cpuUsage: data.cpuUsage,
            };
            state.obsStats[s.label] = sample;
            record("obs-stats", { ok: true, label: s.label, atMsSinceRecordStart: state.recordStartMs ? Date.now() - state.recordStartMs : null, ...sample });
          } catch (e) {
            record("obs-stats", { ok: false, label: s.label, error: e.message });
          }
          break;
        }
        case "judge-output-skipped": {
          record("judge-output-skipped", judgeOutputSkipped(state.obsStats.first, state.obsStats.last));
          break;
        }
        case "stream-stop": {
          // call-1: 録画とは別に配信だけを止める (操作の数を数える「配信中」の区間の終わり)。
          // hold と call-hold (ifActive): テストを実行する人が配信を始めていたときだけ止める (OBS を配信中のまま閉じない、
          // call-hold は締めのアンインストールの前に止める)。配信していなければ何もしない。
          if (s.ifActive) {
            try {
              const st = await ws.call("GetStreamStatus");
              if (!st || !st.outputActive) {
                record("stream-stop", { ok: true, ifActive: true, wasActive: false });
                break;
              }
              await ws.call("StopStream");
              record("stream-stop", { ok: true, ifActive: true, wasActive: true });
            } catch (e) {
              record("stream-stop", { ok: false, ifActive: true, error: e.message });
            }
            break;
          }
          try {
            await ws.call("StopStream");
            record("stream-stop", { ok: true });
          } catch (e) {
            record("stream-stop", { ok: false, error: e.message });
          }
          break;
        }
        case "count-operations": {
          // 配信中に触った操作の数。判定は純関数 judgeOperationCount。
          record("count-operations", judgeOperationCount(state.results));
          break;
        }
        case "screenshot-window": {
          // dock-view の下準備: OBS が見えているトップレベルウィンドウを全部
          // screenshot.ps1 に撮らせる (ドックが浮いた別ウィンドウになっていても撮れるように、
          // メインウィンドウ 1 枚だけに決め打ちしない)。s.target は second-obs 等が持つ
          // close-obs/read-obs-log と同じ考え方 ("first"/"second"。dock-view は "first" だけ使う)。
          const pid = s.target === "second" ? state.secondPid : state.firstPid;
          if (!pid) {
            record("screenshot-window", { ok: false, error: "対象の OBS の pid がない (起動できていない可能性)" });
            break;
          }
          // tag を名前に入れる (scenario ごとに --out が同じとき、前の回の PNG を上書きしない。ドックのウィンドウ 1 回目で実測)。
          // s.shotLabel があれば PNG の名前にも -<shotLabel> を足す (ファイル名だけで何を撮ったか分かるようにする)。
          const prefix = "shot-" + tag + "-" + s.id + (s.shotLabel ? "-" + s.shotLabel : "");
          try {
            const out = execFileSync(
              "powershell",
              ["-NoProfile", "-ExecutionPolicy", "Bypass", "-File", SCREENSHOT_PS1, "-Pid", String(pid), "-OutDir", windowOutDir, "-Prefix", prefix],
              { encoding: "utf8" },
            );
            record("screenshot-window", Object.assign({ shotLabel: s.shotLabel || null }, JSON.parse(out)));
          } catch (e) {
            record("screenshot-window", { ok: false, shotLabel: s.shotLabel || null, error: (e.stdout || "").toString() || e.message });
          }
          break;
        }
        case "dock-click": {
          // 浮いたドックのウィンドウのボタン (またはチェックボックス) を UI Automation で押す (dock-click.ps1)。
          // 参加者の行のチェックは人の見えている名前 (s.name)、ほかは ini の id (s.textId) で名指す。
          // ウィンドウが見つからない・要素が見つからない、のどちらも dock-click.ps1 が ok:false を JSON で返すので、
          // そのまま記録して次の手順へ進む (失敗しても止めずに先へ進む)。
          const pid = s.target === "second" ? state.secondPid : state.firstPid;
          // 実行計画が ini の id (textId) で名指ししたコンポーネントは、製品の ja-JP.ini から文言を引く。
          const clickName = s.name || (s.textId && textOf(s.textId) ? fillTextArgs(textOf(s.textId), s.textArgs) : null);
          if (!clickName) {
            record("dock-click", { ok: false, textId: s.textId || null, error: "no text for id " + String(s.textId) + " in " + JA_LOCALE_INI_REL });
            break;
          }
          if (!pid) {
            record("dock-click", { ok: false, elementName: clickName, error: "対象の OBS の pid がない (起動できていない可能性)" });
            break;
          }
          const clickArgs = ["-NoProfile", "-ExecutionPolicy", "Bypass", "-File", DOCK_CLICK_PS1, "-Pid", String(pid), "-Name", clickName, "-Index", String(s.index ?? 0)];
          // 録画の ON / OFF。押せるコンポーネントが入り切りのつまみ (CheckBox)
          // だったとき、dock-click.ps1 の既定は「On でなければ Toggle する」なので OFF へは
          // 戻せない。行き先を明示する呼び出しだけ -Desired を渡す。
          if (s.desired) clickArgs.push("-Desired", s.desired);
          // テキストの後ろに値が付くボタン (録画中の [録画を終了] は経過時間が付く) は、前方一致で探す。
          if (s.namePrefix) clickArgs.push("-NamePrefix");
          // 録画のボタンを押す手順だけが印を打つ (行のチェックも desired を持つ)。
          const mark = recordMarkOf(s);
          if (mark === "on") state.recordMarks.recordOnMs = Date.now();
          if (mark === "off") state.recordMarks.recordOffMs = Date.now();
          try {
            // モーダルのウィンドウを開くボタン (リハーサルの [Vesktop に追加] など) は、Invoke がそのウィンドウの
            // モーダルループの間ブロックする可能性があるため、s.invokeTimeoutMs がある手順だけ
            // execFileSync の timeout を切る (ROUNDTRIP_INVOKE_TIMEOUT_MS)。時間切れになった場合は
            // ok:false + timedOut:true として記録し、次の手順へ進む (ウィンドウ自体は Invoke が返る前に
            // 開いている見込みのため)。メニューから開く診断情報のエクスポートは dialog-click の
            // noWait で押すので、ここを通らない。
            const execOpts = { encoding: "utf8" };
            if (s.invokeTimeoutMs) execOpts.timeout = s.invokeTimeoutMs;
            const out = execFileSync("powershell", clickArgs, execOpts);
            record("dock-click", { ...withElementName(JSON.parse(out)), textId: s.textId || null, slotOp: !!s.slotOp, muteOp: !!s.muteOp });
          } catch (e) {
            const timedOut = !!e.signal || e.code === "ETIMEDOUT";
            const stdout = (e.stdout || "").toString();
            let parsed = null;
            try {
              parsed = JSON.parse(stdout);
            } catch {
              /* JSON でなければ e.message を使う */
            }
            record("dock-click", { ...(withElementName(parsed) || { ok: false, elementName: clickName, timedOut, error: timedOut ? "invoke did not return within " + s.invokeTimeoutMs + " ms (dialog likely opened)" : stdout || e.message }), textId: s.textId || null, slotOp: !!s.slotOp, muteOp: !!s.muteOp });
          }
          break;
        }
        case "dialog-sendkeys": {
          // export-bundle: 診断情報のエクスポートが開く保存のウィンドウ (ドックとは別のトップレベルウィンドウ。
          // dialog-sendkeys.ps1 の README.md を見よ) へフルパスを打ち込んで確定する。
          // 保存先は --out の下 (windowOutDir) に固定する (課題の文面どおり)。
          const pid = s.target === "second" ? state.secondPid : state.firstPid;
          const fullPath = path.join(windowOutDir, s.fileNameRel);
          const sendTitle = s.title || (s.titleTextId ? textOf(s.titleTextId) : null);
          state.exportBundleZipPath = fullPath;
          state.exportBundleStartedAtMs = null;
          // 前の回の同じ名前の zip を先に消す。残っていると保存のウィンドウが上書きの確認で止まり、check-bundle-zip が古い zip を読む。
          try {
            if (fs.existsSync(fullPath)) fs.unlinkSync(fullPath);
          } catch (e) {
            record("dialog-sendkeys", { ok: false, title: sendTitle, path: fullPath, error: "前の回の zip を消せなかったので、保存のウィンドウへは打たない: " + e.message });
            break;
          }
          if (!pid || !sendTitle) {
            record("dialog-sendkeys", { ok: false, title: sendTitle, path: fullPath, error: !pid ? "対象の OBS の pid がない (起動できていない可能性)" : "no text for id " + String(s.titleTextId) });
            break;
          }
          state.exportBundleStartedAtMs = Date.now();
          try {
            const out = execFileSync(
              "powershell",
              ["-NoProfile", "-ExecutionPolicy", "Bypass", "-File", DIALOG_SENDKEYS_PS1, "-Pid", String(pid), "-Title", sendTitle, "-Text", fullPath, "-Timeout", String(s.timeoutMs ?? 10000)],
              { encoding: "utf8" },
            );
            record("dialog-sendkeys", Object.assign({ path: fullPath }, JSON.parse(out)));
          } catch (e) {
            const stdout = (e.stdout || "").toString();
            let parsed = null;
            try {
              parsed = JSON.parse(stdout);
            } catch {
              /* JSON でなければ e.message を使う */
            }
            record("dialog-sendkeys", Object.assign({ path: fullPath }, parsed || { ok: false, title: sendTitle, error: stdout || e.message }));
          }
          break;
        }
        case "screenshot-source": {
          // dock-view: obs-websocket の GetSourceScreenshot (imageFormat png、imageWidth は
          // 省略 = ソースの元の大きさのまま) で指定のソースを PNG に落とす。obsws.mjs に
          // 薄いラッパーは足さず、既に汎用の ObsWs#call をそのまま使う (--step second-obs の
          // obsws-request と同じ判断)。
          // 枠のソースは型 id と slot で、撮る直前に毎回探す (送信側が繋ぐと名前が人の名前に変わる。harness が撮影の
          // 直前に繋がったばかりの可能性があるため、他の手順の結果をキャッシュしない)。
          try {
            const resolvedName = await resolveSourceName(ws, textOf, s.source);
            const data = await ws.call("GetSourceScreenshot", { sourceName: resolvedName, imageFormat: "png" });
            const raw = data.imageData || "";
            const comma = raw.indexOf(",");
            const buf = Buffer.from(comma >= 0 ? raw.slice(comma + 1) : raw, "base64");
            const outPath = path.join(windowOutDir, "shot-" + tag + "-" + s.id + "-1.png");
            fs.writeFileSync(outPath, buf);
            // dummy scenario の judge-dummy-tile が「直前に撮った画面共有の枠の PNG」を
            // 拾えるよう、最後に書いた画像のパスを残しておく (色タイル判定の材料)。
            state.lastScreenshotSourcePath = outPath;
            // label 付きの撮影は名前で覚える (judge-ac31 が設定を変える前と後を読む)。
            if (s.label) state.screenshotSourcePaths = { ...(state.screenshotSourcePaths || {}), [s.label]: outPath };
            record("screenshot-source", { ok: true, label: s.label || null, source: s.source, resolvedName, shots: [{ path: outPath, sourceName: resolvedName, bytes: buf.length }] });
          } catch (e) {
            record("screenshot-source", { ok: false, label: s.label || null, source: s.source, error: e.message });
          }
          break;
        }
        case "write-call-window-artifacts": {
          // 実行スクリプトだけが知る値を 1 ファイルに書き、cleanup.ps1 -CallWindow が
          // それも読んで消す。**既にある中身へ足すだけ**にする
          // (前の実行や別のツールが書いた場所を落とすと、落とした側が PC に残る)。
          const artifactsPath = toAbs(s.path);
          try {
            let existing = null;
            if (fs.existsSync(artifactsPath)) existing = JSON.parse(fs.readFileSync(artifactsPath, "utf8"));
            const merged = mergeCallWindowArtifacts(existing, s.artifacts);
            fs.mkdirSync(path.dirname(artifactsPath), { recursive: true });
            fs.writeFileSync(artifactsPath, JSON.stringify(merged, null, 2) + "\n", "utf8");
            record("write-call-window-artifacts", { ok: true, path: artifactsPath, ...merged });
          } catch (e) {
            record("write-call-window-artifacts", { ok: false, path: artifactsPath, error: e.message });
          }
          break;
        }
        case "wait-connection": {
          // second-obs 用: harness-start の直後にそのまま切り替えると、送信側が
          // まだ接続を確立する前に切り替えが終わってしまう (エンコーダーの直列のラダー確保 +
          // ウォームアップの 60 フレームが socket を開くより前に走るため。sender.ts 784-813)。hello を
          // 実際に見つけるまで待ち、その時点の記録を judge-connection-held の基準
          // (state.secondObsBeforeSwitchLines) として残す。
          // s.baseline (call-hold): count-connections が保存した数より増えたときだけ通す (開き直す前の接続や
          // 前の記録の hello で通らないため)。バックアップが無ければ 0 (前と同じ「1 つでもあれば」)。
          const timeoutMs = s.timeoutMs || 0;
          const pollDeadlineAt = Date.now() + timeoutMs;
          const baselineCount = s.baseline ? Number((state.connectionBaselines || {})[s.baseline] ?? 0) : 0;
          let lines = [];
          let found = false;
          while (Date.now() < pollDeadlineAt && !deadlineHit()) {
            const latest = firstObsLogFile();
            lines = latest && fs.existsSync(latest) ? fs.readFileSync(latest, "utf8").split(/\r?\n/) : [];
            if (countConnectionEvents(lines).helloEstablishedCount >= baselineCount + 1) {
              found = true;
              break;
            }
            await sleep(500);
          }
          state.secondObsBeforeSwitchLines = found ? lines : null;
          record("wait-connection", {
            ok: found,
            timeoutMs,
            helloEstablishedCount: countConnectionEvents(lines).helloEstablishedCount,
            ...(s.baseline ? { baseline: s.baseline, baselineCount } : {}),
          });
          break;
        }
        case "wait-self-account": {
          // --recheck: 接続の後、Facts の行に self=yes が出るまで待つ (最長 s.timeoutMs)。s.baseline は count-connections が保存した hello の数の名前
          // (無ければ 0)。それより後の接続の行から後の Facts だけを見る。出なければ赤 (gate)。回は止めない。
          const baselineCount = s.baseline ? Number((state.connectionBaselines || {})[s.baseline] ?? 0) : 0;
          const startedAt = Date.now();
          const pollDeadlineAt = startedAt + (s.timeoutMs || 0);
          let judged = judgeSelfAccount({ lines: readFirstObsLogLines(), afterHello: baselineCount });
          while (!judged.ok && Date.now() < pollDeadlineAt && !deadlineHit()) {
            await sleep(s.pollMs || 1000);
            judged = judgeSelfAccount({ lines: readFirstObsLogLines(), afterHello: baselineCount });
          }
          record("wait-self-account", { ...judged, timeoutMs: s.timeoutMs || 0, waitedMs: Date.now() - startedAt, ...(s.baseline ? { baseline: s.baseline } : {}) });
          break;
        }
        case "resize-window": {
          // --recheck の解像度の変化: 他のプロセス (Chrome) の見えている最大のウィンドウの大きさを window-size.ps1 で変える (動かさない・前面に出さない)。
          // mode set は s.width x s.height へ縮め、縮める前の四角と最大化の有無を s.label で覚える。mode restore は覚えた大きさへ戻す
          // (最大化していたウィンドウは最大化へ戻す)。ウィンドウが見つからないときは ok:false で記録して先へ進む。
          const saved = (state.windowResizes || {})[s.label] || null;
          const cli = ["-NoProfile", "-ExecutionPolicy", "Bypass", "-File", WINDOW_SIZE_PS1, "-ProcessName", s.processName, "-NoRestore", "-SettleMs", String(s.settleMs ?? 500), "-Timeout", "5000"];
          if (s.mode === "restore") {
            if (!saved) {
              record("resize-window", { ok: false, mode: s.mode, label: s.label || null, processName: s.processName, error: "no saved size to restore (the shrink step did not succeed)" });
              break;
            }
            if (saved.zoomed) cli.push("-Maximize");
            else cli.push("-Width", String(saved.before[2]), "-Height", String(saved.before[3]));
          } else {
            cli.push("-Width", String(s.width), "-Height", String(s.height));
          }
          let parsed = null;
          try {
            parsed = JSON.parse(execFileSync("powershell", cli, { encoding: "utf8", timeout: 60000 }));
          } catch (e) {
            try {
              parsed = JSON.parse(String(e.stdout || ""));
            } catch {
              parsed = { ok: false, toolFailed: true, error: String(e.message || e) };
            }
          }
          if (s.mode !== "restore" && parsed.ok === true && Array.isArray(parsed.before)) {
            state.windowResizes = { ...(state.windowResizes || {}), [s.label]: { before: parsed.before, zoomed: parsed.zoomed === true } };
          }
          record("resize-window", { ...parsed, mode: s.mode, label: s.label || null, processName: s.processName });
          break;
        }
        case "measure-input-level": {
          // --recheck の画面共有の音: 入力の音量メーター (obs-websocket の InputVolumeMeters) を s.seconds 秒読み、合成のソースの入力の
          // peak の最大 (dBFS) が線 (s.thresholdDbfs) より大きいかを当てる。高頻度の伝えなので、読む間だけ別の接続で購読する。
          // 記録するだけ (gate にしない)。入力の名前は、合成のソースがあればそのソースの現在の名前。
          let inputName = s.inputName;
          try {
            const { cordmux } = await readCordmuxInputs(ws, textOf);
            const hit = findCordmuxInput(cordmux, { kind: "composite" });
            if (hit) inputName = hit.inputName;
          } catch {
            /* 読めなければ実行計画の名前のまま */
          }
          const meter = new ObsWs(OBSWS_CONTROL_PORT, { eventSubscriptions: OBS_EVENT_SUBSCRIPTION_INPUT_VOLUME_METERS });
          try {
            await meter.connect(15000);
            const until = Date.now() + (Number(s.seconds) || 10) * 1000;
            while (Date.now() < until && !deadlineHit()) await sleep(250);
            record("measure-input-level", { ...judgeInputLevel({ events: meter.events, inputName, thresholdDbfs: s.thresholdDbfs }), label: s.label || null, seconds: s.seconds });
          } catch (e) {
            record("measure-input-level", { ok: false, label: s.label || null, inputName: inputName ?? null, error: e.message });
          } finally {
            try {
              meter.close();
            } catch {
              /* 閉じ損ねても先へ進む */
            }
          }
          break;
        }
        case "judge-log-lines": {
          // 記録の行を正規表現で数える (judgeLogLines)。s.since があれば、その label の手が保存した行数より後の行だけを数える
          // (バックアップが無ければ全体では数えず、理由つきの赤)。checks が空の手は、行数を保存するだけ (ok)。s.checks の delta は、前の手の結果と比べる。
          const lines = readFirstObsLogLines();
          const previous = {};
          for (const r of state.results) if (r && r.name === "judge-log-lines" && r.label) previous[r.label] = r;
          let scoped = lines;
          if (s.since) {
            const from = previous[s.since];
            if (!from || !Number.isInteger(from.totalLines)) {
              record("judge-log-lines", { ok: false, label: s.label || null, since: s.since, totalLines: lines.length, reason: "the line count of '" + s.since + "' was not taken", checks: [] });
              break;
            }
            scoped = lines.slice(from.totalLines);
          }
          if (lines.length === 0 && (s.checks || []).length > 0) {
            record("judge-log-lines", { ok: false, label: s.label || null, since: s.since || null, totalLines: 0, reason: "the OBS log is missing", checks: [] });
            break;
          }
          record("judge-log-lines", { label: s.label || null, since: s.since || null, totalLines: lines.length, ...judgeLogLines({ lines: scoped, checks: s.checks, previous }) });
          break;
        }
        case "count-connections": {
          // call-hold 用: 現在の OBS の記録の hello の数を保存する (次の wait-connection の baseline)。
          const latest = firstObsLogFile();
          const lines = latest && fs.existsSync(latest) ? fs.readFileSync(latest, "utf8").split(/\r?\n/) : [];
          const counted = countConnectionEvents(lines);
          const count = counted.helloEstablishedCount;
          state.connectionBaselines = { ...(state.connectionBaselines || {}), [s.label]: count };
          // --extra-checks 用: 切断の数も保存する (judge-connections-kept が現在の数と比べる)。
          state.connectionCounts = { ...(state.connectionCounts || {}), [s.label]: { hello: count, close: counted.closeCount } };
          record("count-connections", { ok: true, label: s.label, helloEstablishedCount: count, closeCount: counted.closeCount, logFile: latest || null });
          break;
        }
        case "judge-connections-kept": {
          // --extra-checks 用: count-connections が保存した数 (s.baseline) から、送信部の接続が切れていないか。
          // gate にしない (Vesktop が共有で落ちたら、それが答え。実行は続ける)。
          const latest = firstObsLogFile();
          const lines = latest && fs.existsSync(latest) ? fs.readFileSync(latest, "utf8").split(/\r?\n/) : null;
          const counted = lines ? countConnectionEvents(lines) : null;
          const now = counted ? { hello: counted.helloEstablishedCount, close: counted.closeCount } : null;
          record("judge-connections-kept", { label: s.label || null, baseline: s.baseline || null, ...judgeConnectionsKept({ before: (state.connectionCounts || {})[s.baseline] || null, now }) });
          break;
        }
        case "obsws-request": {
          // second-obs 用の汎用呼び出し (SetCurrentSceneCollection /
          // GetSceneCollectionList / GetSceneItemList)。obsws.mjs 自体に薄いラッパーは足さず、
          // 既に汎用の ObsWs#call をそのまま使う (必要以上にインターフェースを増やさない)。
          const requestData = s.requestData || {};
          try {
            const data = await ws.call(s.request, requestData);
            // s.label は判定が「どの時点の一覧か」を
            // 結果 JSON だけで見分けるために付ける。無ければ既定どおり出さない。
            record("obsws-request", { request: s.request, requestData, label: s.label || null, ok: true, data });
            // s.track (second-obs のシーンコレクション切り替え) が付いている手順だけ、成功の可否や
            // 戻り値を state.secondObsSwitch に残す (judge-connection-held が読む。obsws-request は
            // 例外を握って ok:false を返すだけで先へ進むので、「呼んだこと」と「切り替わった
            // こと」は別に確認する必要がある)。
            if (s.track === "scene-collection-switch") {
              state.secondObsSwitch = state.secondObsSwitch || {};
              if (s.trackField === "duringSwitchCollectionName") state.secondObsSwitch.duringSwitchCollectionName = data && data.currentSceneCollectionName;
              else if (s.trackField) state.secondObsSwitch[s.trackField] = true;
            }
          } catch (e) {
            record("obsws-request", { request: s.request, requestData, label: s.label || null, ok: false, error: e.message });
            if (s.track === "scene-collection-switch" && s.trackField && s.trackField !== "duringSwitchCollectionName") {
              state.secondObsSwitch = state.secondObsSwitch || {};
              state.secondObsSwitch[s.trackField] = false;
            }
          }
          break;
        }
        case "judge-composite-placement": {
          // select-four: composite-layout-snapshot (label) が読んだ配置の行が、4 等分の決まった位置 (画面共有が s.count 個) か。
          const r = [...state.results].reverse().find((x) => x && x.name === "composite-layout-snapshot" && x.label === s.label) || null;
          record("judge-composite-placement", judgeCompositeCount({ layout: r ? r.layout || null : null, persons: r ? r.persons || {} : {}, count: s.count }));
          break;
        }
        case "judge-dummy-tile": {
          // dummy: 直前の screenshot-source が撮った画面共有の枠 1 の PNG を dummy_tile.py に
          // かけ、色タイル (黒でない) が出ているかを見る。
          const p = state.lastScreenshotSourcePath;
          if (!p) {
            record("judge-dummy-tile", { ok: false, error: "画面共有の枠 1 の PNG がない (screenshot-source が失敗した可能性)" });
            break;
          }
          try {
            const out = execFileSync("python", [DUMMY_TILE_PY, p], { encoding: "utf8", env: { ...env, PYTHONIOENCODING: "utf-8" } });
            record("judge-dummy-tile", JSON.parse(out));
          } catch (e) {
            record("judge-dummy-tile", { ok: false, error: (e.stdout || "").toString() || e.message });
          }
          break;
        }
        case "check-bundle-zip": {
          // export-bundle: dialog-sendkeys が確定した先 (state.exportBundleZipPath) に
          // zip があり、4 つの中身が揃っていてトークンがないかを check_bundle_zip.py で見る。
          const p = state.exportBundleZipPath;
          if (!p) {
            record("check-bundle-zip", { ok: false, error: "zip の保存先が分からない (dialog-sendkeys の前に呼ばれた可能性)" });
            break;
          }
          try {
            // 製品が書いた session.json (差し替えた、または実際の LOCALAPPDATA の下) を渡し、その secret そのものを
            // 探させる。
            const sessionJsonPath = path.join(env.LOCALAPPDATA || process.env.LOCALAPPDATA || "", "cordmux", "session.json");
            const out = execFileSync("python", [CHECK_BUNDLE_ZIP_PY, p, "--session-json", sessionJsonPath], { encoding: "utf8", env: { ...env, PYTHONIOENCODING: "utf-8" } });
            record("check-bundle-zip", withBundleZipFreshness(JSON.parse(out), p, state.exportBundleStartedAtMs));
          } catch (e) {
            // check_bundle_zip.py は判定結果を JSON で出しつつ ok:false のときは終了コード 1 に
            // なる (execFileSync が例外を投げる) ので、まず stdout の JSON を試す。
            const stdout = (e.stdout || "").toString();
            let parsed = null;
            try {
              parsed = JSON.parse(stdout);
            } catch {
              /* JSON でなければ e.message を使う */
            }
            record("check-bundle-zip", withBundleZipFreshness(parsed || { ok: false, path: p, error: stdout || e.message }, p, state.exportBundleStartedAtMs));
          }
          break;
        }
        case "mic-check": {
          // OBS は起動しない (この --step だけ commonSetupSteps
          // を使わない)。test/tools/mic-check/run.mjs が harness と同じ Electron をフェイクデバイスの
          // 起動時の指定つきで起動し、結果 JSON を stdout に JSON で出す (exit code は ok なら 0)。
          const micCheckOutDir = path.join(windowOutDir, "mic-check");
          try {
            const out = execFileSync("node", [MIC_CHECK_RUN_MJS, "--out", micCheckOutDir], { encoding: "utf8" });
            let parsed = null;
            try {
              parsed = JSON.parse(out);
            } catch {
              /* JSON でなければ e.message と同じ扱いにする (下の catch と揃える) */
            }
            record("mic-check", parsed || { ok: false, error: "run.mjs の出力が JSON でなかった: " + out.slice(0, 2000) });
          } catch (e) {
            const stdout = (e.stdout || "").toString();
            let parsed = null;
            try {
              parsed = JSON.parse(stdout);
            } catch {
              /* JSON でなければ e.message を使う (check-bundle-zip と同じ形) */
            }
            record("mic-check", parsed || { ok: false, error: stdout || e.message });
          }
          break;
        }
        case "judge-backpressure": {
          const latest = firstObsLogFile();
          const lines = latest && fs.existsSync(latest) ? fs.readFileSync(latest, "utf8").split(/\r?\n/) : [];
          record("judge-backpressure", judgeBackpressure(state.lastHarnessData, lines));
          break;
        }
        case "judge-connection-held": {
          const latest = firstObsLogFile();
          const lines = latest && fs.existsSync(latest) ? fs.readFileSync(latest, "utf8").split(/\r?\n/) : [];
          const sw = state.secondObsSwitch || {};
          const switchApplied = !!(sw.switchAwayOk && sw.switchBackOk && sw.duringSwitchCollectionName === SECOND_OBS_EMPTY_SCENE_COLLECTION);
          record("judge-connection-held", judgeConnectionHeld({ beforeLines: state.secondObsBeforeSwitchLines || null, afterLines: lines, switchApplied }));
          break;
        }
        case "stream-sink-start": {
          // 親のいない受信プロセス (同梱の ffmpeg・-listen と同じ URL・親の pid に
          // 誰も居ない) を探して記録に書くだけにする。止めない (-Stop を渡さない)。実行スクリプトを
          // 外から打ち切っても受信プロセスは node のジョブごと終わるので、前の実行の受信プロセスは残らない。親のいない受信プロセスは
          // 手で起動した物だけで、止めるかはテストを実行する人が記録 (staleCheck.stale) を読んで stream-sink-stale.ps1 -Stop を手で打って決める。
          // ここで -Stop しない理由: 止める相手がないのに、動いている別の実行の受信プロセスや使い回された pid を誤って止める
          // 道だけが残るため。判定は stream-sink-stale.ps1 の 1 か所 (cleanup.ps1 と同じ)。1935 番を持つ受信プロセスがあれば、下の先客の
          // 確認が断る。
          const staleRun = spawnSync(
            "powershell",
            ["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", STREAM_SINK_STALE_PS1, "-StreamSinkExe", BUNDLED_FFMPEG, "-StreamSinkUrl", STREAM_SINK_URL],
            { encoding: "utf8", windowsHide: true, timeout: 60000 },
          );
          const staleCheck = summarizeStaleCheck(staleRun.stdout, staleRun.status);
          if (staleCheck.stale.length > 0 || staleCheck.undecided.length > 0)
            log("stream-sink-start: a receiver without a parent (not stopped; stop it by hand with stream-sink-stale.ps1 -Stop):", JSON.stringify(staleCheck));
          // 同じポートに先客の受信プロセスがあると、Windows は 2 つ目の -listen も断らずに待たせ、どちらが受けるか分からない
          // (同梱の ffmpeg で 2 つ並べて確認した)。先客がいれば起こさずに理由を残す。
          const owners = ps(
            "@(Get-NetTCPConnection -State Listen -LocalPort " + STREAM_SINK_PORT + " -ErrorAction SilentlyContinue | ForEach-Object { $_.OwningProcess }) -join ','",
          );
          if (owners) {
            record("stream-sink-start", { ok: false, url: STREAM_SINK_URL, staleCheck, error: "port " + STREAM_SINK_PORT + " already has a listener (pid " + owners + ")" });
            break;
          }
          const sink = createStreamSink({
            exe: fs.existsSync(BUNDLED_FFMPEG) ? BUNDLED_FFMPEG : "ffmpeg",
            url: STREAM_SINK_URL,
            dir: windowOutDir,
            tag,
            rearm: !!s.rearm,
            maxRearms: STREAM_SINK_MAX_REARMS,
            onEvent: (ev) => log("stream-sink " + ev.type + ":", JSON.stringify(ev.entry).slice(0, 600)),
          });
          state.streamSinkInfo = sink;
          const entry = sink.start();
          await sleep(1000); // ffmpeg が -listen を始めるまでの猶予。
          const alive = sink.alive();
          record("stream-sink-start", {
            ok: alive,
            flvPath: entry.path,
            pid: entry.pid,
            url: sink.info.url,
            exe: sink.info.exe,
            bundled: sink.info.exe === BUNDLED_FFMPEG,
            rearm: sink.info.rearm,
            staleCheck,
            ...(alive ? {} : { error: entry.error || "the listener ended right after the start", stderrTail: entry.stderrTail }),
          });
          break;
        }
        case "record-start": {
          await ws.call("StartRecord");
          if (s.stream) await ws.call("StartStream");
          // 録画の区間と obs-stats の経過を「録画を始めてから」で書けるようにする。
          state.recordStartMs = Date.now();
          // 判定ツールが使う領域の材料: 録画の始めの枠の位置と大きさ。終わりにも読む (record-stop)。
          // 鍵は対の record-stop へ持ち回る: label を足していない手順の s.label は手順の説明の文 (step() の第 2 引数) で、
          // 始めと終わりで文字列が違うため、record-stop の s.label で引くと始めの分を見失う。
          const startRegions = await captureCellRegions("start");
          state.openRecordingKey = s.label || "_last";
          state.cellRegionCaptures[state.openRecordingKey] = [startRegions];
          record("record-start", { stream: !!s.stream, label: s.label || null, cellRegions: regionSummary(startRegions) });
          break;
        }
        case "record-stop": {
          // through/integration は送信側 (harness 手順) の終了を既に待ち終えてからここに
          // 来る。second-obs/discord-first は録画がシーン切り替えや繋ぎ直しの
          // 直後だけを見ればよいので、送信側がまだ動いている途中でここに来る (どちらも
          // tailSeconds は 0 で、送信側の残りは待たない。送信側の終了待ちは後続の
          // harness-wait が行う)。ここでは録画の末尾の余裕 (tailSeconds) だけ、期限を
          // 見ながら 1 秒刻みで待つ。
          const tailMs = (s.tailSeconds || 0) * 1000;
          let waited = 0;
          while (waited < tailMs && !deadlineHit()) {
            const chunk = Math.min(1000, tailMs - waited);
            await sleep(chunk);
            waited += chunk;
          }
          // 録画の終わりの枠の位置と大きさ (止める前に読む。始めと重ねて判定ツールが使う領域にする)。鍵は対の record-start が
          // 置いたもの (OBS の録画は 1 つずつなので、止めるのは直前に始めた録画)。
          const regionKey = state.openRecordingKey || s.label || "_last";
          state.openRecordingKey = null;
          const endRegions = await captureCellRegions("end");
          state.cellRegionCaptures[regionKey] = [...(state.cellRegionCaptures[regionKey] || []), endRegions];
          state.lastRecordingKey = regionKey;
          if (s.stream) await ws.call("StopStream");
          const rec = await ws.call("StopRecord");
          recordingPath = rec.outputPath;
          // call-1 はメインの手順と追加の確認用の 2 つの録画を録る。label 付きで覚えておき、judge / collect-recording が
          // 「どの録画に対して」を選べるようにする (label がない呼び出しは既定どおり最後の 1 本)。
          if (s.label) state.recordings[s.label] = recordingPath;
          await sleep(1500);
          record("record-stop", { tailSeconds: s.tailSeconds || 0, label: s.label || null, outputPath: recordingPath, cellRegions: regionSummary(endRegions) });
          break;
        }
        case "stream-sink-stop": {
          // 開き直しを先に止める (止めた後に起き直さない)。Windows の kill は信号の名前によらず打ち切りなので、配信の後の
          // flv を書き終える猶予として 2 秒待ってから止める (前と同じ)。hold と call-hold は OBS を閉じた後なので待つ物はない。
          if (state.streamSinkInfo) state.streamSinkInfo.info.rearm = false;
          if (state.streamSinkInfo && state.streamSinkInfo.alive()) await sleep(2000);
          stopStreamSink();
          await sleep(300);
          record("stream-sink-stop", { sink: streamSinkSummary() });
          break;
        }
        case "collect-recording": {
          const collected = s.label ? state.recordings[s.label] || null : recordingPath;
          record("collect-recording", { label: s.label || null, outputPath: collected });
          break;
        }
        case "judge": {
          // s.recording があれば label 付きで覚えた録画を見る (call-1 のメインの手順。後の録画で
          // 上書きされた後でも、メインの手順の録画を判定できるようにするため)。
          const judgeTarget = s.recording ? state.recordings[s.recording] || null : recordingPath;
          // s.label は run-window-plan.mjs が意味を書き添えたいときだけ持つ
          // (「声の音 - 画面の映像」等)。無ければ結果には出さない (既存の judge 呼び出しの
          // 形を変えない)。どの録画を読んだか (s.recording) も残す (judge-av-sync が label のない
          // メインの手順の sync.py を recording で引く)。
          const labelField = { ...(s.label ? { label: s.label } : {}), ...(s.recording ? { recording: s.recording } : {}) };
          if (!judgeTarget) {
            record("judge", { tool: s.tool, ok: false, error: "録画がない" + (s.recording ? " (" + s.recording + ")" : ""), ...labelField });
            break;
          }
          try {
            // 区画は録画の始めと終わりに読んだ枠の位置と大きさ (cellRegionsForJudge)。960x540 の象限に固定すると、
            // 640x360 の枠の白のマーカーを 1 つも拾えない。読めなかった区画は固定の区画に切り替え、
            // そのことを regions の regionSource に残す。升の一辺は 16 px x 枠の倍率 (0.5 倍なら 8。テスト用の送信側の前の帯)。
            // frames.py は区画の中で実際の共有の時計つきの帯を探し、見つからなければ barcode_found: false を返す (bit_strip.py)。
            const cells = Number(s.tool === "smooth.py" ? s.ncells || s.cells || 1 : s.cells || 1);
            const regionKey = s.recording || state.lastRecordingKey || "_last";
            const regions = cellRegionsForJudge({ captures: state.cellRegionCaptures[regionKey] || [], cells });
            const argList = judgeCellArgs(s.tool, regions);
            if (s.tool === "smooth.py") {
              const data = runJudge(s.tool, judgeTarget, argList[0]);
              record("judge", { tool: s.tool, ok: true, data, regions, ...labelField });
            } else if (cells <= 1) {
              const data = runJudge(s.tool, judgeTarget, argList[0]);
              record("judge", { tool: s.tool, ok: true, data, regions, ...labelField });
            } else {
              const perCell = [];
              for (let i = 0; i < cells; i++) {
                perCell.push({ cell: i, region: regions[i], data: runJudge(s.tool, judgeTarget, argList[i]) });
              }
              record("judge", { tool: s.tool, ok: true, cells, perCell, ...labelField });
            }
          } catch (e) {
            record("judge", { tool: s.tool, ok: false, error: e.message, ...labelField });
          }
          break;
        }
        case "wait-finalize": {
          // **cleanup.ps1 が _work 配下の ffmpeg.exe を止める前に**
          // 録画ファイルの作成を終わらせる。終わったと言える形は 2 つ (finalizeStateIsTerminal のコメント)。
          const recordDirAbs = path.join(windowOutDir, s.recordDirRel || "recordings");
          const timeoutMs = s.timeoutMs || 15 * 60 * 1000;
          // 録画ファイルの作成の子プロセスは**同梱の ffmpeg.exe** (配置先 = --plugin の下) で、作業用の
          // フォルダの下にも居る。どちらを prefix にしても取りこぼさないよう 2 か所を見る
          // (cleanup.ps1 が止めるのは作業用のフォルダの下だけなので、そちらが本命)。
          const countFinalizeFfmpeg = () => ffmpegRunningUnder(toAbs("_work")) + (pluginAbs.toLowerCase().startsWith(toAbs("_work").toLowerCase()) ? 0 : ffmpegRunningUnder(pluginAbs));
          const pollMs = s.pollMs || 2000;
          const startedAt = Date.now();
          let rows = null;
          let sessionDir = null;
          let stable = 0;
          let lastCount = -1;
          let done = false;
          let reason = "timeout";
          while (Date.now() - startedAt < timeoutMs && !deadlineHit()) {
            sessionDir = newestRecordSessionDir(recordDirAbs);
            rows = sessionDir ? readLedger(sessionDir) : null;
            if (rows) {
              if (finalizeStateIsTerminal(rows)) {
                done = true;
                reason = "session-row";
                break;
              }
              const count = rows.finalize.length;
              stable = count === lastCount ? stable + 1 : 0;
              lastCount = count;
              // 録画ファイルの作成の行が 1 つ以上あり、5 周期 (既定 10 秒) 増えず、
              // 作業用のフォルダの下の ffmpeg.exe も居ないなら、終わったとみなす。
              if (count > 0 && stable >= FINALIZE_STABLE_POLLS && countFinalizeFfmpeg() === 0) {
                done = true;
                reason = "stable";
                break;
              }
            }
            await sleep(pollMs);
          }
          if (sessionDir) state.recordSessionDir = sessionDir;
          record("wait-finalize", {
            ok: done,
            reason,
            sessionDir,
            waitedMs: Date.now() - startedAt,
            finalizeRows: rows ? rows.finalize.length : 0,
            segmentRows: rows ? rows.segment.length : 0,
            blackRows: rows ? rows.black.length : 0,
            skippedLines: rows ? rows.skipped : 0,
            ffmpegStillRunning: countFinalizeFfmpeg(),
          });
          break;
        }
        case "collect-record-session": {
          const recordDirAbs = path.join(windowOutDir, s.recordDirRel || "recordings");
          const sessionDir = state.recordSessionDir || newestRecordSessionDir(recordDirAbs);
          if (!sessionDir) {
            record("collect-record-session", { ok: false, error: "cordmux の録画のフォルダがない: " + recordDirAbs });
            break;
          }
          state.recordSessionDir = sessionDir;
          const materialsDir = materialsDirOf(sessionDir);
          const listDir = (sub) => {
            const d = path.join(materialsDir, sub);
            if (!fs.existsSync(d)) return [];
            return fs.readdirSync(d).map((n) => ({ name: n, bytes: fs.statSync(path.join(d, n)).size }));
          };
          const outputs = fs
            .readdirSync(sessionDir, { withFileTypes: true })
            .filter((e) => e.isFile() && e.name.toLowerCase().endsWith(".mov"))
            .map((e) => ({ name: e.name, bytes: fs.statSync(path.join(sessionDir, e.name)).size }));
          const rows = readLedger(sessionDir);
          record("collect-record-session", {
            ok: true,
            sessionDir,
            materialsDir,
            outputs,
            segments: listDir("segments"),
            blacks: listDir("black"),
            ledger: rows
              ? { session: rows.session.length, person: rows.person.length, segment: rows.segment.length, black: rows.black.length, finalize: rows.finalize.length, skipped: rows.skipped }
              : null,
          });
          break;
        }
        case "judge-record": {
          // 3 つの判定ツールを使う。**frames.py は使わない** (黒の区間は升が
          // 全部 0 に読めて「逆行」に化ける)。
          const recordDirAbs = path.join(windowOutDir, s.recordDirRel || "recordings");
          const sessionDir = state.recordSessionDir || newestRecordSessionDir(recordDirAbs);
          if (!sessionDir && s.target !== "obs") {
            record("judge-record", { tool: s.tool, ok: false, error: "cordmux の録画のフォルダがない" });
            break;
          }
          if (sessionDir) state.recordSessionDir = sessionDir;
          if (s.tool === "ledger_check.py") {
            const r = runRecordJudge(LEDGER_CHECK_PY, [sessionDir, ...ffmpegArgsForJudge()]);
            record("judge-record", { tool: s.tool, ...r });
            break;
          }
          if (s.tool === "au_hash.py") {
            const r = runRecordJudge(AU_HASH_PY, ["--session-dir", sessionDir, ...ffmpegArgsForJudge()]);
            record("judge-record", { tool: s.tool, ...r });
            break;
          }
          if (s.tool !== "black_span.py") {
            record("judge-record", { tool: s.tool, ok: false, error: "知らない判定ツール" });
            break;
          }
          if (s.target === "obs") {
            // 対の判定のもう一方。**帯を絞る**: 録画を ON にしてから落ち着くまでの猶予を置いた
            // 時点から、最初の送信側が終わる少し前まで。OBS 側の録画はテスト用の送信側が繋がる
            // 前から動いており (空のシーン = 黒)、送信側が終われば枠も黒くなるので、
            // 全体を見ると必ず黒が見つかって偽の失敗になる (見るのは
            // 最初の実フレームより前に黒が出ていないことに絞るのと同じ理由)。
            if (!recordingPath) {
              record("judge-record", { tool: s.tool, target: "obs", ok: false, error: "OBS の録画がない" });
              break;
            }
            const marks = state.recordMarks;
            if (!state.recordStartMs || !marks.recordOnMs || !marks.firstHarnessEndMs) {
              record("judge-record", { tool: s.tool, target: "obs", ok: false, error: "帯を決める時刻が揃っていない", marks });
              break;
            }
            const fromMs = marks.recordOnMs - state.recordStartMs + OBS_BLACK_JUDGE_MARGIN_MS;
            const toMs = marks.firstHarnessEndMs - state.recordStartMs - OBS_BLACK_JUDGE_MARGIN_MS;
            const fromFrame = Math.max(0, Math.round((fromMs * OBS_RECORD_FPS) / 1000));
            const toFrame = Math.round((toMs * OBS_RECORD_FPS) / 1000);
            if (toFrame <= fromFrame) {
              record("judge-record", { tool: s.tool, target: "obs", ok: false, error: "帯が空になった", fromFrame, toFrame });
              break;
            }
            const cells = [];
            let allOk = true;
            for (let i = 0; i < 4; i += 1) {
              const cell = Math.floor(i / 2) + "," + (i % 2);
              const r = runRecordJudge(BLACK_SPAN_PY, [
                recordingPath,
                "--mode",
                "no-black",
                "--grid",
                s.grid || "2x2",
                "--cell",
                cell,
                "--from-frame",
                String(fromFrame),
                "--to-frame",
                String(toFrame),
                ...ffmpegArgsForJudge(),
              ]);
              allOk = allOk && r.ok;
              cells.push({ cell, ...r });
            }
            record("judge-record", { tool: s.tool, target: "obs", ok: allOk, fromFrame, toFrame, cells });
            break;
          }
          // cordmux の完成。**最初の実フレームより前に黒がないこと** (no-leading-black) と、
          // **黒の区間が録画ログの欠落と 1 対 1 で合うこと** (match-gaps) の 2 通り。
          const rows = readLedger(sessionDir);
          const screenRows = (rows ? rows.finalize : []).filter((f) => !String(f.kind || "").toLowerCase().startsWith("voice"));
          const targets =
            screenRows.length > 0
              ? screenRows.map((f) => ({ file: String(f.output_file || ""), row: f }))
              : screenOutputFileNames(fs.readdirSync(sessionDir), textOf(TEXT_ID_FILE_NAME_FORMAT), textOf(TEXT_ID_FILE_SCREEN_SHARE)).map((n) => ({ file: n, row: null }));
          if (targets.length === 0) {
            record("judge-record", { tool: s.tool, target: "cordmux", ok: false, error: "画面の完成が 1 本もない" });
            break;
          }
          const mode = s.mode === "match-gaps" ? "match-gaps" : "no-leading-black";
          const files = [];
          let ok = true;
          for (const target of targets) {
            const abs = path.join(sessionDir, target.file.replace(/\//g, path.sep));
            if (mode !== "match-gaps") {
              // 録画ログに先頭の欠落があれば、その長さまでの先頭の黒は正当 (遅れて始まった人の先頭は
              // 黒で埋まる)。黒が配信に漏れていないことは OBS 側の no-black が見る。
              const allowLeading = leadingGapFrames(rows, target.row);
              const r = runRecordJudge(BLACK_SPAN_PY, [
                abs,
                "--mode",
                "no-leading-black",
                "--allow-leading",
                String(allowLeading),
                ...ffmpegArgsForJudge(),
              ]);
              ok = ok && r.ok;
              files.push({ file: target.file, allowLeading, ...r });
              continue;
            }
            // 録画ログの finalize の行の gaps (欠落の開始と長さ。マイクロ秒) を、その人の
            // 映像の区間の行が名乗る fps でフレーム番号に変換して black_span.py へ渡す。
            // **fps を決められなければ合格にしない** (見ていないものを通さない)。
            const gaps = buildGapFrames(rows, target.row);
            if (gaps.error) {
              ok = false;
              files.push({ file: target.file, ok: false, error: gaps.error });
              continue;
            }
            const gapsPath = path.join(windowOutDir, "gaps-" + path.basename(target.file).replace(/[^0-9A-Za-z._-]/g, "_") + ".json");
            fs.writeFileSync(gapsPath, JSON.stringify(gaps.frames, null, 2) + "\n");
            // **黒とみなす最小の連続フレーム数を 1 に下げる** (既定は 2)。録画ログの欠落は 1 フレームだけの
            // こともあり (record_black.h の plan_black_frames は総フレーム数が 1 でも行を作る)、
            // 既定のままだとその欠落が黒の区間として拾えず「欠落の数が合わない」で偽の失敗になる。
            // 既定の 2 は「場面の切り替わりの 1 フレームの黒」を除けるための値だが、この判定は
            // cordmux の完成だけを対象にし、そこに映る実映像の背景は真っ黒ではない
            // (sender.ts の背景) ので、除く対象がない。
            const r = runRecordJudge(BLACK_SPAN_PY, [abs, "--mode", "match-gaps", "--gaps", gapsPath, "--min-frames", "1", ...ffmpegArgsForJudge()]);
            ok = ok && r.ok;
            files.push({ file: target.file, fps: gaps.fps, gapsFile: gapsPath, gaps: gaps.frames, ...r });
          }
          record("judge-record", { tool: s.tool, target: "cordmux", mode, ok, files });
          break;
        }
        case "judge-record-second": {
          // 同じ OBS の起動の中での 2 回目の録画。見るのは、セッションのフォルダーが 2 つ以上あり、いちばん新しい
          // フォルダー (直前の wait-finalize が読んだもの) の録画ログに、人の行と区間の行があること。前のセッションの
          // 人の行の表を残していると、2 回目に区間を開いたときに OBS が落ちて、人の行が書かれない。
          const recordDirAbs = path.join(windowOutDir, s.recordDirRel || "recordings");
          const root = recordSessionsRoot(recordDirAbs);
          const sessions = fs.existsSync(root)
            ? fs.readdirSync(root, { withFileTypes: true }).filter((e) => e.isDirectory() && isRecordSessionDirName(e.name)).map((e) => e.name).sort()
            : [];
          const newest = sessions.length > 0 ? path.join(root, sessions[sessions.length - 1]) : null;
          const rows = newest ? readLedger(newest) : null;
          const people = rows ? rows.person.length : 0;
          const segments = rows ? rows.segment.length : 0;
          record("judge-record-second", {
            ok: sessions.length >= 2 && people > 0 && segments > 0,
            sessions: sessions.length,
            sessionDir: newest,
            personRows: people,
            segmentRows: segments,
          });
          break;
        }
        case "judge-record-off": {
          // 起動し直すと録画は OFF に戻っている (録画の ON/OFF は覚えない)。
          // 見るのは 2 つ: (1) settings.json に録画の ON/OFF のキーが増えていないこと
          // (録画の ON/OFF はメモリだけに持つ)、(2) 起動し直した後の
          // 保存先に新しいセッションのフォルダが増えていないこと。
          // **(2) は送信側が繋がっている状態で見る** (実行計画がこの直前にもう 1 ストリームつないで
          // いる)。区間は最初のキーフレームが来るまで開かないので、繋がずに
          // 見ると録画が ON でもフォルダは増えず、判定が必ず合格になって意味を持たない。
          const recordDirAbs = path.join(windowOutDir, s.recordDirRel || "recordings");
          const settingsPath = path.join(obsAbs, "config", "obs-studio", "plugin_config", "cordmux", "settings.json");
          let keys = [];
          try {
            keys = Object.keys(JSON.parse(fs.readFileSync(settingsPath, "utf8")));
          } catch (e) {
            keys = [];
          }
          const recordingKey = keys.filter((k) => k === "recording" || k === "record_enabled" || k === "recording_on");
          const before = state.recordSessionDir;
          const now = newestRecordSessionDir(recordDirAbs);
          const grewNewSession = Boolean(now && before && path.resolve(now) !== path.resolve(before));
          // **見ていないものを合格にしない**: 起動し直す前のフォルダを 1 つも取得できていなければ
          // 「増えていない」を言う根拠がない (判定に届かないまま合格にしない)。
          const sawBefore = Boolean(before);
          record("judge-record-off", {
            ok: sawBefore && recordingKey.length === 0 && !grewNewSession,
            sawBefore,
            reason: sawBefore ? null : "起動し直す前の録画のフォルダを 1 つも見ていない (録画ファイルの作成の待ちが時間切れだった可能性)",
            settingsKeys: keys,
            recordingKeyInSettings: recordingKey,
            sessionDirBefore: before,
            sessionDirNow: now,
            grewNewSession,
          });
          break;
        }
        case "webcodecs-check": {
          // WebCodecs のエンコーダの確認。mic-check と同じ形 (別の Electron を短く起動し、
          // 結果 JSON を読んで判定する)。**OBS は起こさない。**
          const outDir = path.join(windowOutDir, "webcodecs-check");
          try {
            const out = execFileSync("node", [WEBCODECS_CHECK_RUN_MJS, "--out", outDir], { encoding: "utf8", env });
            const data = JSON.parse(out);
            record("webcodecs-check", { ok: !!data.ok, outDir, data });
          } catch (e) {
            const stdout = (e.stdout || "").toString();
            let data = null;
            try {
              data = JSON.parse(stdout);
            } catch {
              /* JSON でなければ e.message を使う */
            }
            record("webcodecs-check", { ok: false, outDir, data, error: data ? null : (stdout || e.message).slice(0, 2000) });
          }
          break;
        }
        // ---- インストールの往復 (--step install-roundtrip) ----
        case "rt-prepare": {
          // 毎回展開し直す (現在の置き方は足すだけで消さない = placeFiles)。作業場所の外は消さない。
          const workAbs = toAbs(s.workRel);
          try {
            removeRoundtripDir(workAbs);
            fs.mkdirSync(workAbs, { recursive: true });
            // 差し替え先の LOCALAPPDATA / APPDATA のフォルダも作る。製品は <LOCALAPPDATA>/cordmux/session.json を
            // 書くときに親フォルダを作らないので、ないままだと「session ファイルの書き込みに失敗した」になり、テスト用の送信側が
            // 繋げない (force-update-tamper の wait-connection が失敗する)。
            // APPDATA の側は seed-config.py が偽の state.json を書くときに作るが、同じ理由でここでも作っておく。
            const swapped = [];
            for (const rel of [rtPaths.localAppDataRel, rtPaths.appDataRel]) {
              const abs = toAbs(rel);
              fs.mkdirSync(abs, { recursive: true });
              swapped.push(abs);
            }
            record("rt-prepare", { ok: true, workDir: workAbs, swapped });
          } catch (e) {
            record("rt-prepare", { ok: false, workDir: workAbs, error: e.message });
          }
          break;
        }
        case "rt-extract": {
          const destAbs = toAbs(s.destRel);
          const zipAbs = s.zip ? toAbs(s.zip) : null;
          if (!zipAbs || !fs.existsSync(zipAbs)) {
            record("rt-extract", { ok: false, what: s.what, zip: zipAbs, error: "zip not found" });
            break;
          }
          assertRoundtripWorkPath(destAbs);
          const r = runInstallSnapshot(["extract", "--zip", zipAbs, "--dest", destAbs, ...(s.what === "obs" ? ["--strip-single-top"] : [])]);
          record("rt-extract", { ok: r.exitCode === 0, what: s.what, zip: zipAbs, dest: destAbs, ...(r.parsed || { error: r.error }) });
          break;
        }
        case "rt-place": {
          const fromAbs = toAbs(s.fromRel);
          const pluginDir = findZipPluginDir(fromAbs);
          if (!pluginDir) {
            record("rt-place", { ok: false, label: s.label, from: fromAbs, error: "no single cordmux-<version>/cordmux folder with cordmux.dll in the extracted zip" });
            break;
          }
          if (s.layout === "programdata") {
            // VM だけ (parseArgs が --vm を求める)。まっさらな VM には cordmux がないはずで、
            // 既にあれば (VM でない PC なら利用者が実際に使っているインストール) 置かずに止める。
            const target = path.join(programDataPluginsDir(), "cordmux");
            if (!args.vm || fs.existsSync(target)) {
              record("rt-place", { ok: false, label: s.label, error: args.vm ? "refusing: " + target + " already exists" : "--layout programdata needs --vm" });
              break;
            }
          }
          let report;
          try {
            report = placeFromZip(pluginDir, obsAbs, s.layout, Boolean(s.tolerant));
          } catch (e) {
            record("rt-place", { ok: false, label: s.label, from: pluginDir, error: e.message });
            break;
          }
          for (const entry of report.placed) {
            if (!state.placed.some((p) => p.abs === entry.abs)) state.placed.push(entry);
          }
          state.placedRemoved = false;
          state.placeReports[s.label || "new"] = report;
          fs.writeFileSync(placedListPath(), JSON.stringify({ placed: state.placed.map(({ root, rel }) => ({ root, rel })) }, null, 2));
          record("rt-place", { ok: report.ok, label: s.label, from: pluginDir, layout: s.layout, copied: report.copied, failed: report.failed, placed: report.placed.map((p) => p.root + ":" + p.rel) });
          break;
        }
        case "rt-remove-placed": {
          record("rt-remove-placed", removePlaced());
          break;
        }
        case "rt-rename-obs": {
          const fromAbs = toAbs(s.fromRel);
          const toAbsPath = toAbs(s.toRel);
          try {
            assertRoundtripWorkPath(fromAbs);
            assertRoundtripWorkPath(toAbsPath);
            fs.renameSync(fromAbs, toAbsPath);
            obsAbs = toAbsPath;
            obsConfigAbs = obsConfigFor(obsAbs);
            record("rt-rename-obs", { ok: true, from: fromAbs, to: toAbsPath });
          } catch (e) {
            record("rt-rename-obs", { ok: false, from: fromAbs, to: toAbsPath, error: e.message });
          }
          break;
        }
        case "rt-replace-dll": {
          const target = rtPlaces.placed_dll();
          try {
            fs.copyFileSync(toAbs(s.testDll), target);
            record("rt-replace-dll", { ok: true, from: toAbs(s.testDll), to: target, sha256: sha256Of(target) });
          } catch (e) {
            record("rt-replace-dll", { ok: false, from: s.testDll, to: target, error: e.message });
          }
          break;
        }
        case "snapshot": {
          const cli = ["snapshot", "--out", snapshotPath(s.label)];
          for (const name of s.roots || []) cli.push("--root", name + "=" + snapshotRoots[name]());
          // ポータブルの OBS のフォルダの config は obs_config の根として別に撮る。
          if (!installedObs) cli.push("--exclude", "obs_program=config");
          const r = runInstallSnapshot(cli);
          record("snapshot", { ok: r.exitCode === 0, label: s.label, file: snapshotPath(s.label), ...(r.parsed ? { entryCount: r.parsed.entryCount, missingRoots: r.parsed.missingRoots, reason: r.parsed.reason } : { error: r.error }) });
          break;
        }
        case "snapshot-compare": {
          const cli = ["compare", "--mode", s.mode, "--base", snapshotPath(s.base), "--other", snapshotPath(s.other), "--out", comparePath(s.label),
            "--table", toAbs(REMOVAL_TABLE_REL), "--scene-names", toAbs(SCENE_NAMES_HEADER_REL)];
          if (s.wobble) cli.push("--wobble", comparePath(s.wobble));
          if (s.installed) cli.push("--installed", snapshotPath(s.installed));
          if (fs.existsSync(placedListPath())) cli.push("--placed", placedListPath());
          const r = runInstallSnapshot(cli);
          const p = r.parsed || {};
          record("snapshot-compare", {
            ok: r.exitCode === 0 && p.ok === true,
            mode: s.mode,
            label: s.label,
            file: comparePath(s.label),
            reason: p.reason || p.error || r.error || null,
            diffCount: p.diffCount,
            wobbleCount: p.wobbleCount,
            wobbledCount: p.wobbledCount,
            violationCount: p.violationCount,
            contaminatedCount: p.contaminatedCount,
            rows: (p.rows || []).map((row) => ({ item: row.item, remover: row.remover, count: row.count })),
            violations: (p.violations || []).slice(0, 20),
          });
          break;
        }
        case "dialog-click": {
          // ウィンドウのコンポーネントを押す・読む (dialog-click.ps1)。ウィンドウはタイトル (ini の id) か、メニューのウィンドウ (popup)。コンポーネントは ini の id で
          // 名指し、種類 (Button / CheckBox / MenuItem / ListItem) と行き先 (desired) を渡す。モーダルのウィンドウを開くコンポーネントは
          // noWait (Invoke が閉じるまで戻らない)。
          const pidD = s.target === "second" ? state.secondPid : state.firstPid;
          const title = s.popup ? "" : s.titleText || (s.titleTextId ? textOf(s.titleTextId) : null);
          // 人の見えている名前 (メニューの項目など) は ini の id ではなく文字列そのもの (buttonText)。
          let button = s.buttonText ? s.buttonText : s.buttonTextId ? textOf(s.buttonTextId) : "";
          // 同じ名前のコンポーネントがまとまりごとにあるときは、製品が付ける見出しを添えたアクセシブル名 ("%1 (%2)") で名指す。
          if (button && s.buttonGroupTextId) {
            const format = textOf(TEXT_ID_LOOK_FIELD_IN_GROUP);
            const group = textOf(s.buttonGroupTextId);
            button = format && group ? format.replace("%1", button).replace("%2", group) : "";
          }
          // ComboBox の項目 (-Select)。ini の id か文字列そのまま。
          const select = s.selectTextId ? textOf(s.selectTextId) : s.selectText || "";
          if (!pidD || (!s.popup && !title) || (s.buttonTextId && !button) || (s.selectTextId && !select)) {
            record("dialog-click", { ok: false, label: s.label || null, error: !pidD ? "no OBS pid" : "no text for the title, button or item id (" + [s.titleTextId, s.buttonTextId, s.selectTextId].filter(Boolean).join(", ") + ")" });
            break;
          }
          const cli = ["-NoProfile", "-ExecutionPolicy", "Bypass", "-File", DIALOG_CLICK_PS1, "-Pid", String(pidD), "-Timeout", String(s.timeoutMs ?? 10000)];
          if (s.popup) cli.push("-Popup");
          else cli.push("-Title", title);
          if (button) cli.push("-Button", button);
          if (s.controlType && s.controlType !== "Button") cli.push("-ControlType", s.controlType);
          if (s.namePrefix) cli.push("-NamePrefix");
          if (s.desired) cli.push("-Desired", s.desired);
          if (select) cli.push("-Select", select);
          if (s.noWait) cli.push("-NoWait");
          if (s.listOnly) cli.push("-ListOnly");
          let parsed = null;
          try {
            parsed = JSON.parse(execFileSync("powershell", cli, { encoding: "utf8", timeout: (s.timeoutMs ?? 10000) + 30000 }));
          } catch (e) {
            try {
              parsed = JSON.parse(String(e.stdout || ""));
            } catch {
              // ツールそのものが動かなかった (JSON がない)。「ウィンドウが出なかった」と取り違えない。
              parsed = { ok: false, found: null, toolFailed: true, error: String(e.message || e) };
            }
          }
          state.dialogs[s.label || s.id] = parsed;
          // expect: absent = ウィンドウが出ないこと、present = ウィンドウがあって (押す場合は) 押せたこと、button = そのボタンが出ていること、
          // no-button = ウィンドウはあるがそのボタンが出ていないこと (案内が消えたことの判定)。
          const ok =
            s.expect === "absent"
              ? parsed.found === false && !parsed.toolFailed
              : s.expect === "button"
                ? parsed.ok === true && parsed.buttonFound === true
                : s.expect === "no-button"
                  ? parsed.ok === true && parsed.buttonFound === false
                  : parsed.ok === true;
          // readOnlyOp は読むだけの押下の印 (judgeOperationCount が「その他の操作」に数えない)。
          record("dialog-click", { ...parsed, ok, rawOk: parsed.ok, expect: s.expect || "present", label: s.label || null, ...(s.readOnlyOp === true ? { readOnlyOp: true } : {}) });
          break;
        }
        case "capture-file": {
          const file = rtPlaces[s.from] ? rtPlaces[s.from]() : null;
          if (!file || !fs.existsSync(file)) {
            state.captures[s.label] = null;
            record("capture-file", { ok: true, from: s.from, label: s.label, path: file, exists: false });
            break;
          }
          const text = fs.readFileSync(file, "utf8");
          state.captures[s.label] = text;
          const copy = path.join(windowOutDir, "capture-" + tag + "-" + s.label + ".txt");
          fs.writeFileSync(copy, text);
          record("capture-file", { ok: true, from: s.from, label: s.label, path: file, exists: true, bytes: Buffer.byteLength(text), sha256: createHash("sha256").update(text).digest("hex"), copy });
          break;
        }
        case "judge-rt": {
          const inputs = {};
          for (const [key, source] of Object.entries(s.inputs || {})) {
            if (source && Object.prototype.hasOwnProperty.call(source, "capture")) inputs[key] = state.captures[source.capture] ?? null;
            else if (source && source.dir) inputs[key] = rtPlaces[source.dir] ? rtPlaces[source.dir]() : null;
            else if (source && source.hashes) inputs[key] = state.hashes[source.hashes] || null;
            else if (source && source.dialog) inputs[key] = ((state.dialogs[source.dialog] || {}).texts) || [];
            else if (source && source.placeReport) inputs[key] = state.placeReports[source.placeReport] || null;
            else if (source && Object.prototype.hasOwnProperty.call(source, "value")) inputs[key] = source.value;
            // measure-video-rate が読んだ映像の数。
            else if (source && source.videoRate) inputs[key] = state.videoRates[source.videoRate] || null;
            // 案内の文言 (ini の値) の %1 より前の文字列を名前として渡す (案内が出た / 消えたをドックの文で判定する)。
            // ini から引けなければ空の並び (named-in-texts は「名前がない」で緑になりうるので、引けないことを別に残す)。
            else if (source && source.textPrefix) {
              // 1 つの id か並び。並びの id は全部引けることを求める (引けない id を黙って捨てない)。
              const ids = Array.isArray(source.textPrefix) ? source.textPrefix : [source.textPrefix];
              const prefixes = [];
              const missingIds = [];
              for (const id of ids) {
                const text = textOf(id);
                const prefix = text ? text.split("%1")[0].trim() : "";
                if (prefix) prefixes.push(prefix);
                else missingIds.push(id);
              }
              inputs[key] = prefixes;
              if (missingIds.length > 0) inputs[key + "Missing"] = missingIds.join(", ");
            }
          }
          if (s.anyName) inputs.anyName = true;
          const judged = judgeRoundtrip(s.judge, inputs);
          // 文言が ini から引けなかった (names が空) なら、名前がないことを理由に合格にしない。
          const unresolved = Object.keys(inputs).filter((k) => k.endsWith("Missing"));
          if (unresolved.length > 0) record("judge-rt", { judge: s.judge, ...judged, ok: false, reason: "no text for id " + unresolved.map((k) => inputs[k]).join(", ") + " in " + JA_LOCALE_INI_REL });
          else record("judge-rt", { judge: s.judge, ...judged, names: inputs.names ?? null });
          break;
        }
        case "edit-state-json": {
          // move-foreign の反例: 利用者が vencordDir を自分の値に戻した (偽の state.json だけを書く)。
          const file = rtPlaces.state_json();
          try {
            const obj = JSON.parse(fs.readFileSync(file, "utf8"));
            obj.vencordDir = rtPlaces[s.vencordDir] ? rtPlaces[s.vencordDir]() : s.vencordDir;
            fs.writeFileSync(file, JSON.stringify(obj, null, 2));
            record("edit-state-json", { ok: true, path: file, vencordDir: obj.vencordDir });
          } catch (e) {
            record("edit-state-json", { ok: false, path: file, error: e.message });
          }
          break;
        }
        case "delete-bundle-files": {
          const dir = rtPlaces.bundle_shown();
          const deleted = [];
          const missing = [];
          for (const name of s.names || []) {
            const file = path.join(dir, name);
            if (fs.existsSync(file)) {
              fs.unlinkSync(file);
              deleted.push(name);
            } else missing.push(name);
          }
          record("delete-bundle-files", { ok: missing.length === 0 && deleted.length > 0, dir, deleted, missing });
          break;
        }
        case "tamper-file": {
          // 往復 (install-roundtrip) はテスト用にインストールしたファイルの見せる場所 (rtPlaces.bundle_shown) を、call-1 は実行計画が名指しした
          // ファイルの場所 (s.obs + s.dirRel) を使う。s.keepCopy なら足す前の中身を結果フォルダへコピーし、
          // untamper-file (と途中で抜けた経路の finally) がそこから戻す。
          const dir = s.dirRel ? path.join(toAbs(s.obs), ...String(s.dirRel).split("/")) : rtPlaces.bundle_shown();
          const file = path.join(dir, s.name);
          try {
            const before = sha256Of(file);
            let copy = null;
            if (s.keepCopy) {
              copy = path.join(windowOutDir, "tamper-copy-" + tag + "-" + s.name);
              fs.copyFileSync(file, copy);
              state.tamperedFiles.push({ id: s.restoreId || s.name, path: file, copy, before });
            }
            fs.appendFileSync(file, Buffer.from([0x20]));
            record("tamper-file", { ok: true, path: file, before, after: sha256Of(file), copy, restoreId: s.restoreId || null });
          } catch (e) {
            record("tamper-file", { ok: false, path: file, error: e.message });
          }
          break;
        }
        case "untamper-file": {
          // 足す前のコピーから戻す。戻したら sha256 が足す前と同じであることまで見る (見ていないものを合格にしない)。
          const entry = state.tamperedFiles.find((t) => t.id === s.restoreId);
          if (!entry) {
            record("untamper-file", { ok: false, restoreId: s.restoreId, error: "足した記録がない (tamper-file が失敗した可能性)" });
            break;
          }
          try {
            fs.copyFileSync(entry.copy, entry.path);
            const after = sha256Of(entry.path);
            state.tamperedFiles = state.tamperedFiles.filter((t) => t !== entry);
            record("untamper-file", { ok: after === entry.before, restoreId: s.restoreId, path: entry.path, copy: entry.copy, before: entry.before, after });
          } catch (e) {
            record("untamper-file", { ok: false, restoreId: s.restoreId, path: entry.path, error: e.message });
          }
          break;
        }
        case "place-vencord-bundle": {
          // リハーサル: absent の初期設定は data/vencord を空にするので、_work/vencord-dist の 4 ファイル (と、あれば
          // package.json 等) を見せる場所と原本の両方へコピーする (seed-config.py の ours と同じ中身)。
          const distDir = toAbs(s.distRel);
          const dataDir = path.join(toAbs(s.obs), "plugins", "cordmux", "data");
          const targets = [path.join(dataDir, "vencord"), path.join(dataDir, "vencord-src")];
          const missing = (s.names || []).filter((n) => !fs.existsSync(path.join(distDir, n)));
          if (missing.length > 0) {
            record("place-vencord-bundle", { ok: false, distDir, missing, error: "vencord-dist にないファイルがある" });
            break;
          }
          try {
            let copied = 0;
            for (const target of targets) {
              fs.mkdirSync(target, { recursive: true });
              for (const name of fs.readdirSync(distDir)) {
                const from = path.join(distDir, name);
                if (!fs.statSync(from).isFile()) continue;
                fs.copyFileSync(from, path.join(target, name));
                copied += 1;
              }
            }
            record("place-vencord-bundle", { ok: true, distDir, targets, copied });
          } catch (e) {
            record("place-vencord-bundle", { ok: false, distDir, targets, error: e.message });
          }
          break;
        }
        case "capture-backup": {
          // アンインストールはバックアップを消すので、押す (依頼する) 前に中身を読んでおく。judge-vencord-dir-restored と human (until
          // state-json-restored) の比べる相手。無ければ gate が失敗になる (バックアップがないのにアンインストールの判定へ進まない)。
          const backupPath = toAbs(s.backupPath);
          const text = readTextOrNull(backupPath);
          if (text !== null) state.lastBackupText = text;
          let previous = null;
          try {
            previous = JSON.parse(text).previous ?? null;
          } catch {
            previous = null;
          }
          record("capture-backup", { ok: text !== null, backupPath, bytes: text === null ? 0 : Buffer.byteLength(text), previous });
          break;
        }
        case "judge-vencord-dir-restored": {
          // アンインストールの後: 直前の read-state-json が読んだ state.json と、製品のバックアップ (vencord_dir_backup.json) を比べる。
          // バックアップはアンインストールが消すので、capture-backup (と human の待ちの中) で読めた時点の中身を使い、
          // 無ければ現在のファイルを読む。
          const backupPath = toAbs(s.backupPath);
          const backupText = state.lastBackupText || readTextOrNull(backupPath);
          const judged = judgeVencordDirRestored({ stateText: state.stateJsonAfterText, backupText });
          record("judge-vencord-dir-restored", { ...judged, backupPath, backupReadFrom: state.lastBackupText ? "human-wait" : "file" });
          break;
        }
        case "judge-record-sync": {
          // --extra-checks の追加の確認: 途中で始めた cordmux の録画のうち、s.person の画面の完成に sync.py を実行する (区画なし =
          // その人の共有の全面)。人は録画ログの person の行の display_name で引き、その user_id の finalize の行のファイル (pickPersonScreenOutput)。
          // 結果に user_id は書かない (ファイル名は表示名から作られる)。gate にしない。
          const recordDirAbs = path.join(windowOutDir, s.recordDirRel || "recordings");
          const sessionDir = state.recordSessionDir || newestRecordSessionDir(recordDirAbs);
          if (!sessionDir) {
            record("judge-record-sync", { ok: false, person: s.person || null, error: "cordmux の録画のフォルダがない" });
            break;
          }
          const picked = pickPersonScreenOutput({ rows: readLedger(sessionDir), person: s.person });
          if (!picked.file) {
            record("judge-record-sync", { ok: false, person: s.person || null, sessionDir, error: picked.reason });
            break;
          }
          const file = path.join(sessionDir, picked.file.replace(/\//g, path.sep));
          try {
            const data = runJudge("sync.py", file, []);
            // メインの手順と同じ基準線で判定する (judgeAvSyncLines。組み合わせが無ければ失敗 = 測れていない)。記録するだけ。
            const lines = judgeAvSyncLines(data);
            record("judge-record-sync", { ok: lines.ok === true, reason: lines.ok === true ? null : lines.verdict === "no-pairs" ? "no paired events in the recording" : "outside the line (" + lines.verdict + ", trend " + lines.trend.verdict + ")", person: s.person || null, file: picked.file, lines, data });
          } catch (e) {
            record("judge-record-sync", { ok: false, person: s.person || null, file: picked.file, error: (e.stdout || "").toString().slice(0, 400) || e.message });
          }
          break;
        }
        case "judge-voice-source": {
          // --extra-checks の追加の確認: 使う声の確認の録画ごとに tone_scan.py (s.hz、既定 1 kHz) を実行し、
          // judgeVoiceSourceSwitch で判定する。gate にしない。
          const ffmpeg = fs.existsSync(BUNDLED_FFMPEG) ? BUNDLED_FFMPEG : "ffmpeg";
          const checks = [];
          for (const c of s.checks || []) {
            const recording = state.recordings[c.label] || null;
            const entry = { label: c.label, expectTone: c.expectTone, targetFound: null, target: null, recording };
            if (!recording) {
              checks.push({ ...entry, error: "録画がない (record-stop の失敗)" });
              continue;
            }
            try {
              // tone_share は --windows のときの windows[] にだけある (全体の値にはない。全体の rms は
              // Hann 窓をかけた後の値)。8 秒の録画の先頭と末尾を削った 1 区間で読み、取り出しは純関数 toneWindowValues (テストが tone_scan.py の
              // 実際の出力の形で確認する)。
              const scanWindow = voiceSourceScanWindow({ recordSeconds: s.recordSeconds, marginSeconds: s.marginSeconds });
              const out = JSON.parse(
                execFileSync("python", [TONE_SCAN_PY, "--ffmpeg", ffmpeg, "--hz", String(s.hz || 1000), "--windows", scanWindow, recording], {
                  encoding: "utf8",
                  env: { ...env, PYTHONIOENCODING: "utf-8" },
                }),
              );
              const values = toneWindowValues(out);
              checks.push({ ...entry, scanWindow, rms: values.rms, toneShare: values.toneShare, windowSeconds: values.windowSeconds, error: values.error });
            } catch (e) {
              checks.push({ ...entry, error: (e.stdout || "").toString().slice(0, 400) || e.message });
            }
          }
          record("judge-voice-source", { ...judgeVoiceSourceSwitch({ checks }), hz: s.hz || 1000 });
          break;
        }
        case "judge-call-log": {
          // --extra-checks の追加の確認: 今回の実行の OBS のログを読み、整列のずれの傾き・送信側のフォールバックの段・アイコンを送らなかった行・
          // 接続と切断の数を出す (judgeCallLog)。OBS を閉じた後なのでログは書き終わっている。gate にしない。
          const latest = firstObsLogFile();
          if (!latest || !fs.existsSync(latest)) {
            record("judge-call-log", { ok: false, error: "OBS のログがない" });
            break;
          }
          const lines = fs.readFileSync(latest, "utf8").split(/\r?\n/);
          record("judge-call-log", { logFile: latest, ...judgeCallLog(lines, { maxDriftPercent: s.maxDriftPercent, minSpanSeconds: s.minSpanSeconds }) });
          break;
        }
        case "copy-vencord-settings": {
          // 利用者の Vesktop の中の Vencord の設定ファイルを結果フォルダ (_work の下) へ
          // そのままコピーする (読むだけ・コピーするだけ)。結果には大きさと最上位のキーの数だけを書き、中身は書かない (プラグインの設定に人の ID が
          // 入りうる)。ファイルが無ければ「ない」と書いて先へ進む (比べる側も「ない」と比べる)。
          const srcPath = stateJsonPathFor(s.appDataRel);
          const destPath = path.join(windowOutDir, s.destName);
          try {
            if (!fs.existsSync(srcPath)) {
              state.vencordSettings = { copied: false, exists: false, compared: false };
              record("copy-vencord-settings", { ok: true, exists: false, source: srcPath, note: "the file does not exist; the comparison compares with no file" });
              break;
            }
            fs.copyFileSync(srcPath, destPath);
            const parsed = parseJsonTextOrUndefined(fs.readFileSync(destPath, "utf8"));
            state.vencordSettings = { copied: true, exists: true, compared: false };
            record("copy-vencord-settings", {
              ok: true,
              exists: true,
              source: srcPath,
              dest: destPath,
              bytes: fs.statSync(destPath).size,
              topLevelKeys: parsed && typeof parsed === "object" && !Array.isArray(parsed) ? Object.keys(parsed).length : null,
            });
          } catch (e) {
            record("copy-vencord-settings", { ok: false, source: srcPath, dest: destPath, error: e.message });
          }
          break;
        }
        case "compare-vencord-settings": {
          // Vesktop を終了してもらった後の設定ファイルを、コピーと最上位のキーの上で比べる (戻さない)。
          record("compare-vencord-settings", compareVencordSettings(s));
          break;
        }
        case "place-window-capture": {
          // Main の空いた区画に Vesktop のウィンドウのウィンドウキャプチャを置く。区画の四角
          // (OBS の画面の上と録画の上) と fps を覚えて、judge-latency が latency.py の --region-b と偏りに使う。
          try {
            const videoSettings = await ws.call("GetVideoSettings");
            const rect = latencyReferenceRect({ cell: s.cell, videoSettings });
            const fps = Number(videoSettings.fpsNumerator) > 0 && Number(videoSettings.fpsDenominator) > 0 ? Number(videoSettings.fpsNumerator) / Number(videoSettings.fpsDenominator) : null;
            const created = await ws.call("CreateInput", {
              sceneName: s.sceneName,
              inputName: s.inputName,
              inputKind: s.inputKind,
              inputSettings: s.inputSettings,
              sceneItemEnabled: true,
            });
            await ws.call("SetSceneItemTransform", { sceneName: s.sceneName, sceneItemId: created.sceneItemId, sceneItemTransform: latencyReferenceTransform(rect) });
            state.latencyReference = { inputName: s.inputName, sceneName: s.sceneName, sceneItemId: created.sceneItemId, rect, fps, method: "window", hooked: null };
            record("place-window-capture", { ok: true, inputName: s.inputName, sceneItemId: created.sceneItemId, cell: s.cell, rect, fps, window: (s.inputSettings || {}).window || null });
          } catch (e) {
            record("place-window-capture", { ok: false, inputName: s.inputName, cell: s.cell, error: e.message });
          }
          break;
        }
        case "check-window-capture": {
          // ウィンドウキャプチャが Vesktop のウィンドウを捉えたか (checkLatencyCapture)。捉えたかどうかは、続く
          // switch-to-monitor-capture が ref.hooked で見る。
          const ref = state.latencyReference;
          if (!ref) {
            record("check-window-capture", { ok: false, error: "the window capture was not placed" });
            break;
          }
          const checked = await checkLatencyCapture(s, ref);
          ref.hooked = checked.hooked;
          record("check-window-capture", {
            ...checked,
            method: "window",
            note: checked.hooked ? null : "the window capture found no Vesktop window (not running, minimized, or the window string did not match)",
          });
          break;
        }
        case "switch-to-monitor-capture": {
          // ウィンドウキャプチャが捉えていなければ (check-window-capture が hooked:false)、Vesktop のウィンドウが最大化されて画面の全体を覆うときだけ、
          // その画面の画面キャプチャに 1 度だけ切り替える (windowFillsMonitor。覆っていなければ利用者の別のウィンドウが写るので切り替えない)。
          // 同じ名前・同じ区画・同じ変形に置き、画面は monitor_id の一覧の位置で選ぶ。分からないときは何も変えず、
          // 結果の switched:false と reason に理由を書く。捉えていれば何もしない。
          const ref = state.latencyReference;
          if (!ref) {
            record("switch-to-monitor-capture", { ok: false, switched: false, method: null, error: "the window capture was not placed" });
            break;
          }
          if (ref.hooked !== false) {
            record("switch-to-monitor-capture", {
              ok: true,
              switched: false,
              method: ref.method || "window",
              reason: ref.hooked === true ? "the window capture hooked" : "the window capture was not checked",
            });
            break;
          }
          let info = null;
          try {
            info = JSON.parse(execFileSync("powershell", ["-NoProfile", "-ExecutionPolicy", "Bypass", "-File", WINDOW_RECT_PS1], { encoding: "utf8" }));
          } catch (e) {
            record("switch-to-monitor-capture", {
              ok: true,
              switched: false,
              method: "window",
              reason: "the window rectangle could not be read: " + ((e.stdout || "").toString().slice(0, 200) || e.message),
            });
            break;
          }
          const verdict = windowFillsMonitor(info);
          const seen = { window: info.rect || null, zoomed: info.zoomed ?? null, monitor: info.monitor || null, windowCount: info.count ?? null };
          if (!verdict.fills) {
            record("switch-to-monitor-capture", { ok: true, switched: false, method: "window", reason: verdict.reason, ...seen });
            break;
          }
          // 別の名前で置いて画面を選び、選べてからウィンドウキャプチャを外して名前を付け替える (選べなければ何も変わらない)。
          try {
            const created = await ws.call("CreateInput", {
              sceneName: ref.sceneName,
              inputName: s.trialName,
              inputKind: s.inputKind,
              inputSettings: s.inputSettings,
              sceneItemEnabled: false,
            });
            const listed = await ws.call("GetInputPropertiesListPropertyItems", { inputName: s.trialName, propertyName: "monitor_id" });
            const picked = pickMonitorItem(listed && listed.propertyItems, verdict.monitor);
            if (!picked.item) {
              await ws.call("RemoveInput", { inputName: s.trialName });
              record("switch-to-monitor-capture", { ok: true, switched: false, method: "window", reason: picked.reason, ...seen });
              break;
            }
            await ws.call("SetInputSettings", { inputName: s.trialName, inputSettings: { ...s.inputSettings, monitor_id: picked.item.itemValue }, overlay: true });
            await ws.call("SetSceneItemTransform", { sceneName: ref.sceneName, sceneItemId: created.sceneItemId, sceneItemTransform: latencyReferenceTransform(ref.rect) });
            await ws.call("RemoveInput", { inputName: ref.inputName });
            await ws.call("SetInputName", { inputName: s.trialName, newInputName: s.inputName });
            await ws.call("SetSceneItemEnabled", { sceneName: ref.sceneName, sceneItemId: created.sceneItemId, sceneItemEnabled: true });
            state.latencyReference = { ...ref, sceneItemId: created.sceneItemId, method: "monitor", hooked: null };
            await sleep(s.settleMs || 0);
            const checked = await checkLatencyCapture(s, state.latencyReference);
            state.latencyReference.hooked = checked.hooked;
            record("switch-to-monitor-capture", { ...checked, switched: true, method: "monitor", monitorName: picked.item.itemName, ...seen });
          } catch (e) {
            // 置きかけの画面キャプチャが残っていれば外す (ウィンドウキャプチャが残っていれば、外すのは remove-window-capture)。
            try {
              await ws.call("RemoveInput", { inputName: s.trialName });
            } catch {
              // 置く前に失敗した (または付け替えまで済んでいる) ときは、外す物がない。
            }
            record("switch-to-monitor-capture", { ok: false, switched: false, method: "window", error: e.message, ...seen });
          }
          break;
        }
        case "remove-window-capture": {
          // 録り終えたらすぐ外す (この後の録画と PNG に利用者の Vesktop のウィンドウを入れない)。
          try {
            await ws.call("RemoveInput", { inputName: s.inputName });
            record("remove-window-capture", { ok: true, inputName: s.inputName });
          } catch (e) {
            record("remove-window-capture", { ok: false, inputName: s.inputName, error: e.message });
          }
          break;
        }
        case "judge-latency": {
          // 遅れの録画に latency.py を実行する。a = 遅れを読む人の cordmux の枠、b = Vesktop のウィンドウを置いた区画 (latencyToolArgs)。
          // ツールが無ければ「ツールがない」と記録して先へ進む。gate にしない。
          const recording = state.recordings[s.recording] || null;
          const toolPath = path.join(HERE, s.tool || "latency.py");
          const ref = state.latencyReference || null;
          const regions = latencyRegions({
            captures: state.cellRegionCaptures[s.recording] || [],
            cells: s.cells,
            person: s.person,
            reference: ref ? ref.rect.output : null,
          });
          // 遅れの録画の始めと終わりに読んだ枠ごとの名前と四角もコピーする (latencyCellsSeen)。人が見つからなかったときに、
          // テストを実行する人が枠の名前で領域 A を選んで手で読み直せるようにするため。
          const cellsSeen = latencyCellsSeen(state.cellRegionCaptures[s.recording] || []);
          const base = { recording, person: s.person || null, regions, cellsSeen, fps: ref ? ref.fps : null };
          if (!recording) {
            record("judge-latency", { ok: false, reason: "録画がない (" + s.recording + ")", ...base });
            break;
          }
          if (!fs.existsSync(toolPath)) {
            record("judge-latency", { ok: false, toolMissing: true, reason: "ツールがない (" + path.basename(toolPath) + ")", ...base });
            break;
          }
          if (!regions.regionA && regions.regionB && s.namesUnknown) {
            // 実際の通話は、枠に載っている人がログから分からない。失敗にせず、手で読む。
            record("judge-latency", { ok: null, handRead: true, reason: "枠に載っている人が記録から分からないので、PNG で枠を読み、latency.py を手で実行する", ...base });
            break;
          }
          if (!regions.regionA || !regions.regionB) {
            record("judge-latency", { ok: false, reason: regions.reason, ...base });
            break;
          }
          // latency.py のバージョンによって --json のオプションの有無が違う (latencyToolArgs) ので、--help にあるときだけ渡す。
          let jsonFlag = false;
          try {
            jsonFlag = /--json\b/.test(execFileSync("python", [toolPath, "--help"], { encoding: "utf8", env: { ...env, PYTHONIOENCODING: "utf-8" } }));
          } catch {
            jsonFlag = false;
          }
          const argv = latencyToolArgs({ recording, regionA: regions.regionA, regionB: regions.regionB, jsonFlag });
          try {
            const data = JSON.parse(execFileSync("python", [toolPath, ...argv], { encoding: "utf8", env: { ...env, PYTHONIOENCODING: "utf-8" } }));
            record("judge-latency", { ...judgeLatency({ data, fps: base.fps }), ...base, data });
          } catch (e) {
            // 判定ツールは結果を JSON で出して終了コード 1 で終わることがある (runRecordJudge と同じ扱い)。
            const stdout = (e.stdout || "").toString();
            const data = parseJsonTextOrUndefined(stdout);
            if (data && typeof data === "object") record("judge-latency", { ...judgeLatency({ data, fps: base.fps }), ...base, data, exitCode: typeof e.status === "number" ? e.status : null });
            else record("judge-latency", { ok: false, reason: "latency.py failed: " + (stdout.slice(0, 400) || e.message), ...base });
          }
          break;
        }
        case "judge-av-sync": {
          // sync.py の判定結果 (recording の label で引く。av1 / av2 は label、メインの手順は recording) に基準値を適用する。記録するだけ。
          const judged = [...state.results].reverse().find((r) => r && r.name === "judge" && r.tool === "sync.py" && (r.recording === s.recording || r.label === s.recording)) || null;
          record("judge-av-sync", { recording: s.recording, ...judgeAvSync(judged) });
          break;
        }
        case "judge-ac31": {
          // 設定を変える前と、名前の [表示] を外した後の合成のソースの画像 (screenshot-source の label) の名前ラベルの板を
          // plate_measure.py で測り、後の画像で板が狭まるか消えたかを judgeAc31 で判定する。記録するだけ (gate にしない)。
          const paths = state.screenshotSourcePaths || {};
          const measure = (label) => {
            const file = paths[label];
            if (!file) return { found: null, error: "no picture for " + label };
            try {
              const out = JSON.parse(execFileSync("python", [PLATE_MEASURE_PY, file, "--tile-bands", "2x2"], { encoding: "utf8", env: { ...env, PYTHONIOENCODING: "utf-8" } }));
              return { found: !!out.found, plate: out.plate || null, plates: out.plates || [] };
            } catch (e) {
              return { found: null, error: (e.stdout || "").toString().slice(0, 300) || e.message };
            }
          };
          record("judge-ac31", judgeAc31({ before: measure(s.before), after: measure(s.after) }));
          break;
        }
        case "judge-voice-volume": {
          // 声の音量: 100% の録画 (s.before) と上げた後の録画 (s.after) を同じ区間で tone_scan.py --windows に渡し、
          // judgeVoiceVolume でゲインを判定する。gate にしない。
          const ffmpeg = fs.existsSync(BUNDLED_FFMPEG) ? BUNDLED_FFMPEG : "ffmpeg";
          const scanWindow = voiceSourceScanWindow({ recordSeconds: s.recordSeconds, marginSeconds: s.marginSeconds });
          const scan = (label) => {
            const recording = state.recordings[label] || null;
            if (!recording) return { rms: null, toneShare: null, error: "録画がない (" + label + ")" };
            try {
              const out = JSON.parse(
                execFileSync("python", [TONE_SCAN_PY, "--ffmpeg", ffmpeg, "--hz", String(s.hz || 1000), "--windows", scanWindow, recording], {
                  encoding: "utf8",
                  env: { ...env, PYTHONIOENCODING: "utf-8" },
                }),
              );
              return toneWindowValues(out);
            } catch (e) {
              return { rms: null, toneShare: null, error: (e.stdout || "").toString().slice(0, 400) || e.message };
            }
          };
          const before = scan(s.before);
          const after = scan(s.after);
          record("judge-voice-volume", {
            ...judgeVoiceVolume({ before, after, pct: s.pct, toleranceDb: s.toleranceDb }),
            beforeLabel: s.before,
            afterLabel: s.after,
            scanWindow,
            ...(before.error ? { beforeError: before.error } : {}),
            ...(after.error ? { afterError: after.error } : {}),
          });
          break;
        }
        case "count-roster-labels": {
          // 実際のアバターの並び: 共有を止める前の、並びの名前ラベルのログの行 (Composite roster label ready) の数を保存する。
          const lines = readFirstObsLogLines();
          const count = rosterLabelReadyFromLog(lines).length;
          state.rosterBaselines = { ...(state.rosterBaselines || {}), [s.label]: count };
          record("count-roster-labels", { ok: true, label: s.label, count, logLines: lines.length });
          break;
        }
        case "judge-roster-labels": {
          // 保存した数より後に出た行で、並びの名前ラベルが描かれたかを判定する (記録するだけ)。
          const lines = readFirstObsLogLines();
          if (lines.length === 0) {
            record("judge-roster-labels", { ok: false, reason: "the OBS log is missing" });
            break;
          }
          const baselineCount = (state.rosterBaselines || {})[s.baseline];
          record("judge-roster-labels", { baseline: s.baseline, ...judgeRosterLabels({ lines, baselineCount: baselineCount ?? 0, expectedPeople: s.expectedPeople }), baselineKnown: baselineCount !== undefined });
          break;
        }
        case "collect-self-speaking-log": {
          // hold の --self-mic: OBS を閉じた後のログから、自分の枠の音声ソースの測定値 (peak の分布と話している割合の行、入力を
          // 見つけた・見つからない・失った行) を抜き出して結果にコピーする。コピーするのは音の大きさ (peak の dBFS) だけで、音声は残らない。
          const lines = readFirstObsLogLines();
          if (lines.length === 0) {
            record("collect-self-speaking-log", { ok: false, reason: "the OBS log is missing" });
            break;
          }
          record("collect-self-speaking-log", judgeSelfSpeakingLog({ rows: selfSpeakingStatsFromLog(lines), events: selfSourceEventsFromLog(lines) }));
          break;
        }
        case "judge-ac32": {
          // 録画の区間ごとの音量を tone_scan.py --windows で読み、judgeAc32 で判定する。区間は mark-time の印から作る。
          const recording = state.recordings[s.recording] || null;
          const built = ac32Windows({ marks: state.timeMarks || {}, voices: s.voices, marginS: s.marginSeconds });
          if (!recording || built.windows.length === 0) {
            record("judge-ac32", { ok: false, reason: !recording ? "録画がない (" + s.recording + ")" : "区間を組めない (印が足りない)", recording, problems: built.problems });
            break;
          }
          try {
            const ffmpeg = fs.existsSync(BUNDLED_FFMPEG) ? BUNDLED_FFMPEG : "ffmpeg";
            const out = JSON.parse(
              execFileSync("python", [TONE_SCAN_PY, "--ffmpeg", ffmpeg, "--hz", String(s.hz || 1000), "--windows", toneScanWindowsArg(built.windows), recording], {
                encoding: "utf8",
                env: { ...env, PYTHONIOENCODING: "utf-8" },
              }),
            );
            const f = out && out.files && out.files[0] ? out.files[0] : {};
            if (f.error) record("judge-ac32", { ok: false, reason: "tone_scan.py: " + f.error, recording, windows: built.windows, problems: built.problems });
            else record("judge-ac32", { ...judgeAc32({ windows: built.windows, scanned: f.windows || [] }), recording, windows: built.windows, problems: built.problems });
          } catch (e) {
            record("judge-ac32", { ok: false, reason: (e.stdout || "").toString().slice(0, 400) || e.message, recording, windows: built.windows });
          }
          break;
        }
        case "judge-lines": {
          // 基準値を適用した判定を 1 つの表にまとめる (JUDGE_LINES。実行中に止めるものと記録するだけのものを分けて書く)。gate にしない。
          record("judge-lines", judgeLinesSummary(state.results, { items: s.items }));
          break;
        }
        case "judge-av-drift": {
          // av1 / av2 の sync.py の結果 (judge の結果そのもの) から区画ごとの差を出す。判定はしない (傾きは人が出す)。
          // s.deltaLimitMs (--extra-checks のとき) があれば区画ごとの |差| に基準値を適用する (記録するだけ)。
          const find = (label) => state.results.find((r) => r && r.name === "judge" && r.tool === "sync.py" && r.label === label) || null;
          record("judge-av-drift", { first: s.first, second: s.second, ...judgeAvDrift({ first: find(s.first), second: find(s.second), deltaLimitMs: s.deltaLimitMs }) });
          break;
        }
        case "measure-video-rate": {
          // プラグインのログの stats video の直近の行から、映像が届いているかを読む。
          const latest = firstObsLogFile();
          const lines = latest && fs.existsSync(latest) ? fs.readFileSync(latest, "utf8").split(/\r?\n/) : [];
          const measured = parseVideoRateSamples(lines, s.windowLines || VIDEO_RATE_WINDOW_LINES);
          state.videoRates[s.label] = measured;
          record("measure-video-rate", { ok: measured.framesStopped !== null, label: s.label, logFile: latest, ...measured });
          break;
        }
        case "skip": {
          // 実行計画が「現在は実行できない」とした手順 (理由と要件を結果に残す。利用者には依頼しない)。
          record("skip", { ok: null, skipped: true, requirement: s.requirement || null, reason: s.reason || null, ...(s.item ? { item: s.item } : {}), what: s.label });
          break;
        }
        case "keep-obs-recordings": {
          const kept = await keepObsRecordings();
          record("keep-obs-recordings", kept || { ok: true, note: "already kept" });
          break;
        }
        case "harness-stop": {
          // リハーサル: テスト用の送信側を手順の中で止める (finally に任せると「残っているプロセス」のログに残る)。
          const child = state.harnessChild;
          if (!child) {
            record("harness-stop", { ok: true, note: "既に居ない" });
            break;
          }
          const stopped = await killHarnessTree(child.pid);
          state.harnessChild = null;
          record("harness-stop", { ok: stopped, pid: child.pid });
          break;
        }
        case "harness-alive": {
          // リハーサル: テスト用の送信側が寿命 (harness-start の seconds) で先に終了していないか。終了していると、この後の
          // av2 の sync.py とわざと異常にする判定が「実際の実行にはない理由」で失敗するので、ここで理由つきの失敗にして見分ける。
          const child = state.harnessChild;
          const alive = child !== null && child !== undefined && !state.harnessExited;
          record("harness-alive", {
            ok: alive,
            pid: child ? child.pid : null,
            exitCode: state.harnessExitCode ?? null,
            reason: alive ? null : "テスト用の送信側が既に終了している (寿命 = 実行計画の見込み x 倍率 + 余裕 が足りない)",
          });
          break;
        }
        case "human": {
          // 人の操作待ち。ログに HUMAN: の行を書き、HUMAN_POLL_MS ごとに条件を見る。通知ファイル
          // (<出力>/window3b/human-go.txt) はどの until でも通す。手順の始めに残っていた古い通知ファイルは消してから待つ
          // (前の手順の通知ファイルで進んでしまわないため)。タイムアウトで ok:false (gate なら失敗)。
          const goFile = path.join(windowOutDir, HUMAN_GO_FILE_NAME);
          let staleGoFileRemoved = false;
          if (fs.existsSync(goFile)) {
            try {
              fs.unlinkSync(goFile);
              staleGoFileRemoved = true;
            } catch {
              /* 消せなければそのまま (次の確認で拾って進むことになるが、結果に残す) */
            }
          }
          // 通知ファイルの絶対パスを添える (実行スクリプトを動かす側がコピーして打てるように。--out の下の window3b なので、
          // 相対パスで書いてコピーすると out が二重になる誤りが起きやすい)。
          log("HUMAN: " + s.message + " (通知ファイル: " + goFile + ")");
          const startedAt = Date.now();
          const timeoutMs = s.timeoutMs || HUMAN_TIMEOUT_MS;
          const stateJsonPath = s.appDataRel ? stateJsonPathFor(s.appDataRel) : null;
          const check = () => {
            if (fs.existsSync(goFile)) {
              try {
                fs.unlinkSync(goFile);
              } catch {
                /* 消せなくても進む (次の human の始めで消す) */
              }
              return "go-file";
            }
            if (s.until === "seconds") return Date.now() - startedAt >= (Number(s.waitSeconds) || 0) * 1000 ? "seconds" : null;
            if (s.until === "state-json-points-to-placed") {
              const text = readTextOrNull(stateJsonPath);
              if (text === null) return null;
              const judged = judgeVencordDirWritten({ beforeText: null, afterText: text, expectedDir: toAbs(s.expectedDir), compareOtherKeys: false });
              return judged.ok ? "state-json" : null;
            }
            if (s.until === "state-json-restored") {
              const stateText = readTextOrNull(stateJsonPath);
              const backupText = readTextOrNull(toAbs(s.backupPath));
              if (backupText !== null) state.lastBackupText = backupText;
              if (stateText === null) return null;
              const judged = judgeVencordDirRestored({ stateText, backupText: state.lastBackupText || null });
              return judged.ok ? "state-json" : null;
            }
            return null;
          };
          let satisfiedBy = null;
          while (!satisfiedBy && Date.now() - startedAt < timeoutMs && !deadlineHit()) {
            satisfiedBy = check();
            if (!satisfiedBy) await sleep(HUMAN_POLL_MS);
          }
          record("human", {
            ok: satisfiedBy !== null,
            message: s.message,
            until: s.until,
            satisfiedBy,
            waitedMs: Date.now() - startedAt,
            timeoutMs,
            timedOut: satisfiedBy === null && !deadlineHit(),
            goFile,
            staleGoFileRemoved,
          });
          break;
        }
        case "measure-plate": {
          // 名前ラベルの板を plate_measure.py で読む。s.recording があれば録画のフレーム (s.atS 秒)、
          // 無ければ直前の screenshot-source の PNG。数値を記録するだけで、ok は「読めた (板が見つかった)」。
          const recording = s.recording ? state.recordings[s.recording] || null : null;
          const input = s.recording ? recording : state.lastScreenshotSourcePath;
          if (!input) {
            record("measure-plate", { ok: false, error: s.recording ? "録画がない (" + s.recording + ")" : "screenshot-source の PNG がない" });
            break;
          }
          const plateArgs = [PLATE_MEASURE_PY];
          if (s.recording) {
            plateArgs.push("--video", input, "--at-s", String(s.atS ?? 1));
            // 同梱の ffmpeg と ffprobe があればそれ、無ければ PATH にあるもの (plate_measure.py の既定)。
            const bundledProbe = path.join(path.dirname(BUNDLED_FFMPEG), "ffprobe.exe");
            if (fs.existsSync(BUNDLED_FFMPEG)) plateArgs.push("--ffmpeg", BUNDLED_FFMPEG);
            if (fs.existsSync(bundledProbe)) plateArgs.push("--ffprobe", bundledProbe);
          } else {
            plateArgs.push(input);
          }
          if (s.corner) plateArgs.push("--corner", s.corner);
          if (s.bg) plateArgs.push("--bg", s.bg);
          if (s.transfer) plateArgs.push("--transfer", s.transfer);
          try {
            const out = JSON.parse(execFileSync("python", plateArgs, { encoding: "utf8", env: { ...env, PYTHONIOENCODING: "utf-8" } }));
            const found = s.recording ? (out.frames || []).some((f) => f.found) : !!out.found;
            record("measure-plate", { ok: found, input, ...out });
          } catch (e) {
            record("measure-plate", { ok: false, input, error: (e.stdout || "").toString() || e.message });
          }
          break;
        }
        case "mark-time": {
          // 録画の始めからの時刻 (ミリ秒) に名前を付けて覚える (音量を測る区間の端)。録画の前なら null。
          const atMs = state.recordStartMs ? Date.now() - state.recordStartMs : null;
          state.timeMarks = { ...(state.timeMarks || {}), [s.label]: atMs };
          record("mark-time", { ok: atMs !== null, label: s.label, atMsSinceRecordStart: atMs });
          break;
        }
        case "hold-info": {
          // --step hold: 開いている OBS の pid・obs-websocket のポート・通知ファイルを 1 つの JSON に書く。テストを実行する人は
          // ここから -Pid を読んで dialog-click.ps1 ほかを実行し、終えるときは goFile を置く。
          const info = {
            tag,
            // 実行スクリプト (実際の node) の pid。Volta を使う環境では `node` が shim で、shim が実際の node を子プロセスとして起動する。
            // shim の pid だけを終了すると、実行スクリプトと配信の受け口は動き続ける。終了するときはこの pid か、プロセスの木ごと (taskkill /T /F) にする。
            driverPid: process.pid,
            obsPid: state.firstPid,
            obsDir: obsAbs,
            obsConfigDir: obsConfigAbs,
            wsPort: OBSWS_CONTROL_PORT,
            goFile: path.join(windowOutDir, HUMAN_GO_FILE_NAME),
            harnessPid: state.harnessChild ? state.harnessChild.pid : null,
            // 置くとフレームだけが止まり、消すと戻る通知ファイル (テスト用の送信側があるときだけ)。
            framesPauseFile: state.harnessChild && state.framesPauseFile ? state.framesPauseFile : null,
            // 置くと声に音が乗り、その声の持ち主が話している扱いになる通知ファイル (hold の --voice-users のときだけ)。
            voiceToneFile: state.harnessChild && state.voiceToneFile ? state.voiceToneFile : null,
            // --stream-sink: 配信の受け口。flv は <out>/window3b/stream-<tag>.flv、配信が止まるたびに stream-<tag>-<n>.flv で
            // 開き直すので pid は替わる (現在のものを書く)。後始末 (cleanup.ps1) は *.flv を消す。
            streamSink: state.streamSinkInfo
              ? {
                  url: state.streamSinkInfo.info.url,
                  exe: state.streamSinkInfo.info.exe,
                  dir: windowOutDir,
                  firstFile: (state.streamSinkInfo.info.files[0] || {}).path || null,
                  pid: state.streamSinkInfo.current() ? state.streamSinkInfo.current().pid : null,
                  rearm: state.streamSinkInfo.info.rearm,
                }
              : null,
            // call-hold か (実際の保存先のまま = 利用者の Vesktop が送信側)。OBS の録画の保存先 (テストを実行する人が
            // obs-websocket の StartRecord で録画するファイルの保存先。call-hold は実行の終わりに recordings/obs-<tag> へ移す)。
            step: args.step,
            realProfile: !!args.realProfile,
            rehearsal: !!args.rehearsal,
            obsRecordDir: windowOutDir,
            holdMinutes: args.holdMinutes,
            obsLook: args.obsLook,
            noSeed: !!args.noSeed,
            // hold の --self-mic: 作った音声入力の名前 (ミキサーの行の名前。テストを実行する人が利用者に伝える)。
            selfMic: args.selfMic ? { inputName: SELF_MIC_INPUT_NAME, inputKind: SELF_MIC_INPUT_KIND } : null,
            // 後始末で cordmux の録画のフォルダ (recordings の下) を残すか (hold の --keep-recordings。keepsRecordings)。
            keepRecordings: keepsRecordings(args),
            writtenAt: new Date().toISOString(),
          };
          const file = path.join(windowOutDir, HOLD_INFO_FILE_PREFIX + tag + ".json");
          try {
            fs.writeFileSync(file, JSON.stringify(info, null, 2));
            record("hold-info", { ok: !!state.firstPid, file, ...info, ...(state.firstPid ? {} : { error: "OBS の pid がない (起動できていない可能性)" }) });
          } catch (e) {
            record("hold-info", { ok: false, file, error: e.message });
          }
          break;
        }
        case "hash-bundle": {
          const hashes = { src: hashDir(rtPlaces.bundle_src()), shown: hashDir(rtPlaces.bundle_shown()) };
          state.hashes[s.label] = hashes;
          record("hash-bundle", { ok: true, label: s.label, ...hashes });
          break;
        }
        case "hash-files": {
          const hashes = {};
          for (const name of s.files || []) {
            const file = rtPlaces[name] ? rtPlaces[name]() : null;
            hashes[name] = file && fs.existsSync(file) ? sha256Of(file) : null;
          }
          state.hashes[s.label] = hashes;
          record("hash-files", { ok: true, label: s.label, hashes });
          break;
        }
        case "mark-motw": {
          // Explorer が展開したときと同じ印 (Zone.Identifier の ZoneId=3) を、置いたものの全ファイルに付ける。
          const body = "[ZoneTransfer]\r\nZoneId=" + String(s.zoneId ?? 3) + "\r\n";
          let marked = 0;
          const failed = [];
          for (const entry of state.placed) {
            for (const file of listFilesRecursive(entry.abs)) {
              try {
                fs.writeFileSync(file + ":Zone.Identifier", body);
                if (fs.readFileSync(file + ":Zone.Identifier", "utf8") !== body) throw new Error("read back differs");
                marked += 1;
              } catch (e) {
                failed.push({ path: file, error: e.message });
              }
            }
          }
          record("mark-motw", { ok: marked > 0 && failed.length === 0, marked, failed: failed.slice(0, 20) });
          break;
        }
        case "run-placed-ffmpeg": {
          const ff = rtPlaces.placed_ffmpeg();
          const marked = fs.existsSync(ff + ":Zone.Identifier");
          try {
            const out = execFileSync(ff, ["-hide_banner", "-version"], { encoding: "utf8", timeout: 20000 });
            record("run-placed-ffmpeg", { ok: /ffmpeg version/i.test(out), path: ff, zoneIdentifier: marked, firstLine: out.split(/\r?\n/)[0] });
          } catch (e) {
            record("run-placed-ffmpeg", { ok: false, path: ff, zoneIdentifier: marked, exitCode: e.status ?? null, error: e.message });
          }
          break;
        }
        case "list-modules": {
          const pidM = s.target === "second" ? state.secondPid : state.firstPid;
          const raw = pidM ? ps(`(Get-Process -Id ${pidM} -ErrorAction SilentlyContinue).Modules | ForEach-Object { $_.FileName }`) : "";
          const modules = raw
            .split(/\r?\n/)
            .map((l) => l.trim())
            .filter((l) => /[\\/](av|sw|postproc)[^\\/]*\.dll$/i.test(l));
          record("list-modules", { ...judgeFfmpegModules({ modules, obsDir: obsAbs }), pid: pidM, obsDir: obsAbs });
          break;
        }
        case "copy-obs-logs": {
          const copied = copyObsLogsOnce();
          record("copy-obs-logs", { ok: true, ...(copied || { note: "already copied" }) });
          break;
        }
        case "collect-plugin-log": {
          const latest = firstObsLogFile();
          let lines = [];
          if (latest && fs.existsSync(latest)) {
            const text = fs.readFileSync(latest, "utf8");
            lines = text.split(/\r?\n/).filter((l) => /\[cordmux(\/js)?\]/.test(l));
          }
          record("collect-plugin-log", { logFile: latest, lineCount: lines.length, sample: lines.slice(0, 30) });
          break;
        }
        case "ws-probe": {
          const sessionPath = toAbs(SESSION_JSON_REL);
          if (!fs.existsSync(sessionPath)) {
            record("ws-probe", { variant: s.variant, ok: false, error: "session.json がない" });
            break;
          }
          const session = JSON.parse(fs.readFileSync(sessionPath, "utf8"));
          const result = await wsProbe(s.variant, session);
          record("ws-probe", result);
          break;
        }
        case "write-result": {
          writeResultNow();
          break;
        }
        case "summarize": {
          // summarize-window.py を結果 JSON と OBS のログに対して実行し、出力を結果 JSON の
          // summary に入れる (run-window-plan.mjs の finishSteps 参照)。write-result が
          // 既に呼ばれている前提だが、念のためここでも呼ぶ (writeResultNow は 2 回目以降は
          // 何もしない)。
          writeResultNow();
          const latest = firstObsLogFile();
          const scriptArgs = [path.join(HERE, "summarize-window.py"), state.resultFilePath];
          if (latest) scriptArgs.push(latest);
          // summarize-window.py は OBS のログの日本語の行をそのまま print する
          // (summarize_log の NOTE: / stream K の行)。Windows の既定コードページの子プロセスに
          // 出力させると文字化けや UnicodeEncodeError で失敗するので、selfcheck.py と同じく
          // 明示的に UTF-8 にする。
          let summaryText = "";
          try {
            summaryText = execFileSync("python", scriptArgs, { encoding: "utf8", env: { ...env, PYTHONIOENCODING: "utf-8" } });
          } catch (e) {
            summaryText = "summarize-window.py 失敗: " + ((e.stdout || "").toString() || e.message);
          }
          // record を書き戻しの前に呼ぶ (state.results に summarize 自身の結果を積んでから
          // 結果 JSON の steps を丸ごと書き直す)。summary 欄だけを後から足すと、
          // summarize 自身の成否が結果 JSON に残らない。cleanup はこの後の
          // 手順なので、cleanup の結果は結果 JSON には残らない (run-*.log には残る)。
          record("summarize", { file: state.resultFilePath, summaryLength: summaryText.length });
          try {
            // summarize-window.py の出力 (OBS のログの行を含む) からも、実際の ID の形の配信者の ID を伏せる。
            fs.writeFileSync(
              state.resultFilePath,
              JSON.stringify({ tag, args: resultArgs, startedAt: new Date(startMs).toISOString(), gateFailures: state.gateFailures, steps: state.results, summary: redactBroadcasterId(summaryText, args.broadcasterId) }, null, 2),
            );
          } catch (e) {
            log("summarize: 結果 JSON への書き戻しに失敗:", e.message);
          }
          break;
        }
        case "cleanup": {
          const out = runCleanup(false, env, { realProfile: args.realProfile, obsDir: obsAbs, artifactsPath: toAbs(CALL_WINDOW_ARTIFACTS_REL), keepRecordings: !!s.keepRecordings, watchProgramData: !!s.watchProgramData });
          // 往復の作業場所 (rt-<tag>) は、撮影したものと結果を --out に残した後で消す
          // (--keep-work なら残す)。OBS は cleanup.ps1 が止めた後である。
          let rtWork = null;
          if (s.rtWorkRel) {
            rtWork = { path: toAbs(s.rtWorkRel), kept: !!s.keepWork };
            if (!s.keepWork) {
              try {
                removeRoundtripDir(toAbs(s.rtWorkRel));
                rtWork.removed = true;
              } catch (e) {
                rtWork.removed = false;
                rtWork.error = e.message;
              }
            }
          }
          record("cleanup", { callWindow: false, keepRecordings: !!s.keepRecordings, watchProgramData: !!s.watchProgramData, rtWork, output: out });
          break;
        }
        default:
          record("unknown-step", { kind: s.kind });
      }
      // ゲートは自分の終了コードで失敗を返す。実行計画が gate:true を付けた
      // 手順が ok:true 以外で終わったら、その場では止めずに (後始末と結果の記録まで通すため) 印だけ
      // 残し、最後に終了コード 3 にする。
      if (s.gate) {
        const written = state.results.slice(resultsBefore);
        const last = written.length > 0 ? written[written.length - 1] : null;
        if (!last || last.ok !== true) {
          state.gateFailures.push({
            step: s.id,
            label: s.label,
            ok: last ? last.ok ?? null : null,
            reason: (last && (last.reason || last.error)) || null,
          });
          log("ゲートが失敗:", s.id, JSON.stringify(state.gateFailures[state.gateFailures.length - 1]).slice(0, 500));
          if (s.stopOnFail) state.stoppedAt = s.id;
        }
      }
    }
  } catch (e) {
    record("error", { message: e.message, stack: e.stack });
    aborted = "error";
    writeResultNow(); // 予期しない例外で抜けても結果 JSON を残す。
  } finally {
    ws.close();
    // 退避したファイル (call-gate-d の package.json) は、手順の途中で抜けた経路でも必ず戻す
    // (置いたものを残さないため。restore-file を通ったものは
    // state.movedFiles から外れているので、ここに残っているのは戻せていないものだけ)。
    for (const moved of [...state.movedFiles]) {
      try {
        fs.renameSync(moved.to, moved.from);
        state.movedFiles = state.movedFiles.filter((m) => m !== moved);
        log("退避したファイルを finally で戻した:", moved.from);
      } catch (e) {
        log("退避したファイルを戻せなかった:", moved.from, e.message);
      }
    }
    // わざと 1 バイト足したファイル (call-1 の連続実行) も、途中で抜けた経路ではコピーから戻す
    // (untamper-file を通ったものは state.tamperedFiles から外れている)。
    for (const tampered of [...state.tamperedFiles]) {
      try {
        fs.copyFileSync(tampered.copy, tampered.path);
        state.tamperedFiles = state.tamperedFiles.filter((t) => t !== tampered);
        log("足したファイルを finally でコピーから戻した:", tampered.path);
      } catch (e) {
        log("足したファイルを戻せなかった:", tampered.path, e.message);
      }
    }
    // 手順の途中で抜けた場合でも、開いた OBS (1 つ目・2 つ目とも) は必ず閉じる。
    if (state.secondPid && processAlive(state.secondPid)) await closeObsGracefully(state.secondPid);
    if (state.firstPid && processAlive(state.firstPid)) await closeObsGracefully(state.firstPid);
    if (state.harnessChild && !state.harnessChild.killed) {
      await killHarnessTree(state.harnessChild.pid);
    }
    // フレームを止める通知ファイルを残さない (テストを実行する人が置いたまま実行が終わっても、次の実行が止まったまま始まらないようにする)。
    if (state.framesPauseFile && fs.existsSync(state.framesPauseFile)) {
      try {
        fs.unlinkSync(state.framesPauseFile);
        log("フレームを止める通知ファイルを finally で消した:", state.framesPauseFile);
      } catch (e) {
        log("フレームを止める通知ファイルを消せなかった:", state.framesPauseFile, e.message);
      }
    }
    // 声に音を乗せる通知ファイルも残さない (フレームを止める通知ファイルと同じ理由)。
    if (state.voiceToneFile && fs.existsSync(state.voiceToneFile)) {
      try {
        fs.unlinkSync(state.voiceToneFile);
        log("声に音を乗せる通知ファイルを finally で消した:", state.voiceToneFile);
      } catch (e) {
        log("声に音を乗せる通知ファイルを消せなかった:", state.voiceToneFile, e.message);
      }
    }
    // 配信の受け口は開き直しを止めてから閉じる (止めた後に再び起動しないため)。途中で抜けた場合にも flv の大きさをログに残す。
    stopStreamSink();
    if (state.streamSinkInfo) log("stream-sink (finally):", JSON.stringify(streamSinkSummary()).slice(0, 2000));
    // cleanup.ps1 は最後に必ず呼ぶ (手順の中で既に呼んでいても、途中で抜けた経路のために
    // もう一度呼んでおく。冪等な後始末なので二重に呼んでも問題ない)。**-CallWindow は、締めで戻す実行が
    // 戻ったと確認する前に終わったときだけ付ける** (下の finallyVencordRestorePlan。手順の cleanup は
    // 付けない。run-window-plan.mjs の finishSteps 参照)。
    // **record では録画のフォルダを残す**。手順の cleanup が -KeepRecordings 付きで残しても、
    // この finally が付けずに呼び直すと録画ファイルが毎回消えてしまう。残す判断は実行計画の
    // keepsRecordings (一覧 STEPS_KEEPING_RECORDINGS と hold のオプション) の 1 か所に置く。
    // call-1 は OBS の録画も残す (期限で手順の keep-obs-recordings に届かないと、この cleanup が消してしまうため)。
    // OBS と配信の受け口を止めた後なので、ここで recordings の下へ移せば -KeepRecordings が残す。
    // 残すかは実行計画の cleanup と同じ keepsRecordings の 1 か所で決める (hold の --keep-recordings もここで動作する)。
    if (STEPS_KEEPING_OBS_RECORDINGS.includes(args.step) && !state.obsRecordingsKept) {
      try {
        log("OBS の録画を finally で残す場所へ移した:", JSON.stringify(await keepObsRecordings()));
      } catch (e) {
        log("OBS の録画を移せなかった:", e.message);
      }
    }
    // 期限・止める通知ファイル・例外で締めのアンインストールの前に終わった場合でも、利用者の Vesktop の
    // vencordDir をバックアップの値へ戻す (cleanup.ps1 -CallWindow -RealProfile)。アンインストールで戻ったことを確認した場合と、バックアップがない場合は
    // 呼ばない (バックアップがないと -CallWindow は終了コード 2 になるだけ)。
    let restorePlan = { action: "skip", reason: "not decided" };
    try {
      restorePlan = finallyVencordRestorePlan({
        realProfile: Boolean(args.realProfile),
        planHasRestoreJudge: steps.some((x) => x && x.kind === "judge-vencord-dir-restored"),
        restoredOk: state.results.some((r) => r && r.name === "judge-vencord-dir-restored" && r.ok === true),
        backupExists: fs.existsSync(path.join(obsConfigAbs, "plugin_config", "cordmux", "vencord_dir_backup.json")),
      });
    } catch (e) {
      restorePlan = { action: "report-only", reason: "could not decide: " + e.message };
    }
    log("vencordDir の戻し (finally):", JSON.stringify(restorePlan));
    const out = runCleanup(false, env, {
      realProfile: args.realProfile,
      obsDir: obsAbs,
      artifactsPath: toAbs(CALL_WINDOW_ARTIFACTS_REL),
      keepRecordings: keepsRecordings(args),
      watchProgramData: installedObs,
      callWindow: restorePlan.action === "call-window",
    });
    log("cleanup.ps1 (finally):", out);
    // 戻す実行 (実行計画が締めで戻す場合) は、最後に実際の state.json の vencordDir が実行前のコピーと同じかを確認し、
    // <出力>/window3b/vencord-dir-final-<tag>.json に書く (「元に戻りました」と報告する前にこれを読む)。
    if (args.realProfile && steps.some((x) => x && x.kind === "judge-vencord-dir-restored")) {
      try {
        const cleanupJson = lastPrefixedJson(out, "CORDMUX_CLEANUP_JSON:");
        const check = vencordDirFinalCheck({
          stateText: readTextOrNull(stateJsonPathFor(VESKTOP_STATE_JSON_REAL_REL)),
          beforeText: readTextOrNull(path.join(windowOutDir, STATE_JSON_BEFORE_NAME)),
        });
        const vesktopRunning = /\S/.test(ps("Get-Process vesktop -ErrorAction SilentlyContinue | ForEach-Object { $_.Id }"));
        vencordFinal = {
          checkedAt: new Date().toISOString(),
          aborted,
          restorePlan,
          cleanup: cleanupJson ? { exitCode: cleanupJson.exitCode ?? null, callWindow: cleanupJson.callWindow ?? null, restoreBranch: cleanupJson.restoreBranch ?? null, problems: cleanupJson.problems || [] } : null,
          ...check,
          vesktopRunning,
          note: vesktopRunning && restorePlan.action === "call-window" ? "restored while Vesktop was running; check state.json again after the user quits Vesktop with [Quit]" : null,
        };
        fs.writeFileSync(path.join(windowOutDir, VENCORD_DIR_FINAL_PREFIX + tag + ".json"), JSON.stringify(vencordFinal, null, 2));
        log("vencordDir の最後の確認 (finally):", JSON.stringify(vencordFinal));
      } catch (e) {
        log("vencordDir の最後の確認に失敗:", e.message);
      }
    }
    // Vencord の設定をコピーした実行で、比較する前に終わったときは、ここで比較する (戻さない)。
    if (state.vencordSettings && state.vencordSettings.copied && !state.vencordSettings.compared) {
      try {
        finallyVencordSettings = compareVencordSettings({ appDataRel: VENCORD_SETTINGS_REAL_REL, beforeName: VENCORD_SETTINGS_BEFORE_NAME, afterName: VENCORD_SETTINGS_AFTER_NAME });
        log("Vencord の設定の比較 (finally):", JSON.stringify(finallyVencordSettings).slice(0, 2000));
      } catch (e) {
        log("Vencord の設定を比較できなかった:", e.message);
      }
    }
    // 結果 JSON に finally の結果を足す (結果 JSON は手順の write-result か、途中で抜けた経路の writeResultNow が先に書いている)。
    if ((vencordFinal || finallyVencordSettings || aborted === "stopped") && state.resultFilePath && fs.existsSync(state.resultFilePath)) {
      try {
        const written = JSON.parse(fs.readFileSync(state.resultFilePath, "utf8"));
        written.finally = { vencordRestore: vencordFinal, vencordSettings: finallyVencordSettings, aborted };
        fs.writeFileSync(state.resultFilePath, JSON.stringify(written, null, 2));
      } catch (e) {
        log("結果 JSON に finally の結果を足せなかった:", e.message);
      }
    }
    // インストールの往復: 途中で抜けた経路でも、OBS のログを --out へコピーし、置いたもの (ProgramData の
    // plugins\cordmux を含む) と作業場所を残さない (--keep-work なら作業場所だけ残す)。
    if (roundtrip) {
      try {
        const copied = copyObsLogsOnce();
        if (copied) log("copied the OBS logs (finally):", JSON.stringify(copied));
      } catch (e) {
        log("could not copy the OBS logs (finally):", e.message);
      }
      if (state.placed.length > 0 && !state.placedRemoved && (installedObs || !args.keepWork)) {
        log("removed the placed files (finally):", JSON.stringify(removePlaced()));
      }
      if (!args.keepWork && fs.existsSync(toAbs(rtPaths.workRel))) {
        try {
          removeRoundtripDir(toAbs(rtPaths.workRel));
          log("removed the work folder (finally):", rtPaths.workRel);
        } catch (e) {
          log("could not remove the work folder:", rtPaths.workRel, e.message);
        }
      }
    }
    const leftover = ps("Get-Process obs64,electron,ffmpeg -ErrorAction SilentlyContinue | ForEach-Object { $_.ProcessName + ' ' + $_.Id }");
    log("残っているプロセス:", leftover || "なし");
  }

  return { gateFailures: state.gateFailures, aborted, vencordFinal };
}

// ---- main ------------------------------------------------------------------

async function main() {
  const parsed = parseArgs(process.argv.slice(2));
  if (!parsed.ok) {
    for (const e of parsed.errors) console.error("引数エラー:", e);
    console.error(USAGE);
    process.exitCode = 1;
    return;
  }
  const args = parsed.args;
  // 実際の通話の実行に使うアカウントの名前 (--call-accounts のファイル)。実行計画を組む前に読む。
  if (args.callAccountsPath) {
    let text = null;
    try {
      text = fs.readFileSync(toAbs(args.callAccountsPath), "utf8");
    } catch (e) {
      console.error("引数エラー: --call-accounts のファイルが読めない (" + e.message + ")");
      process.exitCode = 1;
      return;
    }
    const accounts = parseCallAccounts(text);
    if (!accounts.ok) {
      for (const e of accounts.errors) console.error("引数エラー:", e);
      process.exitCode = 1;
      return;
    }
    args.callAccounts = accounts.accounts;
  }
  const accountsProblem = callAccountsProblem(args);
  if (accountsProblem) {
    console.error("引数エラー:", accountsProblem);
    console.error(USAGE);
    process.exitCode = 1;
    return;
  }
  // --obs と --out の保存先の検査 (workPathProblems)。実行計画を組む前・dry-run の前・プロセスの確認 (tasklist) の前に行う。
  // 往復の --obs は parseArgs が入れた _work/rt-<tag>/obs なので同じ検査を通る。
  if (!args.allowOutsideWork) {
    const pathProblems = workPathProblems({
      obsAbs: args.obs ? toAbs(args.obs) : null,
      outAbs: args.out ? toAbs(args.out) : null,
      repoRoot: REPO_ROOT,
      caseInsensitive: process.platform === "win32",
    });
    if (pathProblems.length > 0) {
      for (const e of pathProblems) console.error("引数エラー:", e);
      console.error(USAGE);
      process.exitCode = 1;
      return;
    }
  }
  // hold の --screen-images: 画像がそろっていなければ、OBS を起動する前 (dry-run を含む) に止める。送信側は読めない画像を動く図形に
  // 戻して続けるので、ここで止めないと、撮ってから気付いて OBS を起動し直すことになる。
  if (args.screenImages) {
    const images = screenImageFiles(args.screenImages, toAbs, (file) => fs.existsSync(file));
    if (images.problems.length > 0) {
      for (const e of images.problems) console.error("引数エラー:", e);
      process.exitCode = 1;
      return;
    }
  }
  const planResult = planSteps(args);
  if (!planResult.ok) {
    console.error(planResult.error);
    process.exitCode = 1;
    return;
  }
  // 実行の期限: call-1 の実際の実行は実行計画から見積もる (deadlineMinFor。--deadline-min を渡せばそれを使う)。
  // 結果 JSON の args にも、実際に使った値が残る。
  args.deadlineMin = deadlineMinFor(args, planResult.steps);

  const guard = evaluateGuard(args.allowUnknownEncoder, args.callWindow);
  const roundtrip = args.step === INSTALL_ROUNDTRIP_STEP;
  // インストールの往復は --plugin を取らない。代わりに入力の zip と DLL があるかを報告する。
  const manifest = roundtrip
    ? Object.fromEntries(
        [["zip", args.zip], ["obsZip", args.obsZip], ["oldZip", args.oldZip], ["testDll", args.testDll]]
          .filter(([, v]) => v)
          .map(([k, v]) => [k, { path: toAbs(v), exists: fs.existsSync(toAbs(v)) }]),
      )
    : manifestWithExistence(toAbs(args.plugin));

  if (args.dryRun) {
    // APPDATA の差し替えが「利用者の実際の %APPDATA%\Vesktop を
    // 読みにも行かない」ことを、実際に子プロセスへ渡す env から確認できるようにする
    // (envDuplicateCheck は、大文字小文字違いの重複キーが残っていない = ここで上書きした
    // 値だけが子プロセスに渡ることを目でも見られるようにするもの。重複キーを取り除く処理そのものは
    // localAppDataEnv がしている)。
    // --real-profile は差し替えそのものを行わない。事前確認の
    // dry-run でも _work/localappdata や _work/appdata を作らないよう、
    // localAppDataEnv (mkdir を行う) を呼ばずに「差し替えなし」をそのまま報告する。
    const envOverride = args.realProfile
      ? {
          realProfile: true,
          note: "APPDATA / LOCALAPPDATA / CORDMUX_HANDSHAKE_DIR を差し替えない (実機の送信部と同じ保存先を使う)",
          LOCALAPPDATA: null,
          APPDATA: null,
          VENCORD_USER_DATA_DIR: null,
          CORDMUX_HANDSHAKE_DIR: null,
        }
      : (() => {
          const dryRunEnv = roundtrip
            ? overriddenEnv(roundtripPaths(args.tag).localAppDataRel, roundtripPaths(args.tag).appDataRel, roundtripPaths(args.tag).handshakeRel, false)
            : // 差し替えの値だけを組み、保存先は作らない (上のコメントのとおり。localAppDataEnv を使うと mkdir するため、
              // dry-run のたびに空の _work/appdata が残る)。
              overriddenEnv(LOCAL_APPDATA_OVERRIDE_REL, APPDATA_OVERRIDE_REL, HANDSHAKE_DIR_REL, false);
          return {
            realProfile: false,
            LOCALAPPDATA: dryRunEnv.LOCALAPPDATA,
            APPDATA: dryRunEnv.APPDATA,
            VENCORD_USER_DATA_DIR: dryRunEnv.VENCORD_USER_DATA_DIR,
            // 製品が読み書きする state.json (VENCORD_USER_DATA_DIR の下)。作業のフォルダーの下であることの確認結果も出す。
            productStateJson: path.join(dryRunEnv.VENCORD_USER_DATA_DIR, "state.json"),
            vesktopDirUnderWork: vesktopDirSwapProblem(dryRunEnv, toAbs("_work")) === null,
            CORDMUX_HANDSHAKE_DIR: dryRunEnv.CORDMUX_HANDSHAKE_DIR,
            envDuplicateCheck: {
              LOCALAPPDATA: envKeysMatchingCaseInsensitive(dryRunEnv, "LOCALAPPDATA"),
              APPDATA: envKeysMatchingCaseInsensitive(dryRunEnv, "APPDATA"),
              VENCORD_USER_DATA_DIR: envKeysMatchingCaseInsensitive(dryRunEnv, "VENCORD_USER_DATA_DIR"),
            },
          };
        })();
    // 実行計画が指定する種類が全部 switch にあること (ない種類は実行で黙って飛ばされる)。
    const unhandledKinds = [...new Set(planResult.steps.map((s) => s.kind))].filter((k) => !HANDLED_KINDS.includes(k));
    // ini の id で指定したコンポーネントとウィンドウのタイトル、人への文と要求の中の textRef が、製品の ja-JP.ini (と OBS 自身の
    // ja-JP.ini) から引けること。OBS 自身のテキストは --obs の OBS があるときだけ確認し、無ければ確認していない id として出す。
    const dryTextOf = makeTextLookup(() => (args.obs ? toAbs(args.obs) : null));
    const textIds = [...new Set(planResult.steps.flatMap((s) => stepTextIds(s)))];
    const obsIniReadable = Boolean(args.obs && fs.existsSync(obsLocaleIniPath(toAbs(args.obs))));
    const obsTextIdsUnchecked = obsIniReadable ? [] : textIds.filter((id) => id.startsWith(OBS_TEXT_PREFIX));
    const resolvedTexts = Object.fromEntries(textIds.map((id) => [id, dryTextOf(id)]));
    const missingTextIds = textIds.filter((id) => !obsTextIdsUnchecked.includes(id) && !resolvedTexts[id]);
    // seed-config.py が書く値 (allow_state_json_write を含む) を、同じ引数に --dry-run を
    // 足して問い合わせる (値を決めるのは seed-config.py の 1 か所。何も書かない)。VM の programdata の場合は OBS の
    // 実際の保存先を見に行くので問い合わせない。
    const dryWindowOutDir = path.join(toAbs(args.out), "window3b");
    const seedPreview = planResult.steps
      .filter((s) => s.kind === "seed-config")
      .map((s) => {
        if (s.installedObs) return { step: s.id, skipped: "programdata layout (asks only inside the VM)" };
        const argv = [...seedConfigArgv(s, args, { obsAbs: toAbs(s.obs || args.obs), windowOutDir: dryWindowOutDir, obsConfigAbs: null }), "--dry-run"];
        try {
          const out = execFileSync("python", argv, { encoding: "utf8", env: { ...process.env, PYTHONIOENCODING: "utf-8" } });
          const p = lastPrefixedJson(out, "CORDMUX_SEED_CONFIG_JSON:") || {};
          return { step: s.id, exitCode: 0, allowStateJsonWrite: p.allowStateJsonWrite ?? null, allowStateJsonWriteRule: p.allowStateJsonWriteRule ?? null, settings: p.settings ?? null, replayBuffer: p.replayBuffer ?? null, fileCount: (p.files || []).length };
        } catch (e) {
          const p = lastPrefixedJson((e.stdout || "").toString(), "CORDMUX_SEED_CONFIG_JSON:") || {};
          return { step: s.id, exitCode: typeof e.status === "number" ? e.status : null, refused: p.reason || e.message };
        }
      });
    const seedRefused = seedPreview.filter((p) => p.exitCode !== undefined && p.exitCode !== 0);
    // 実行計画が見込む待ちの合計 (分) と、人の操作 (HUMAN の文と進む条件) の一覧。
    const humanSteps = planResult.steps.filter((s) => s.kind === "human").map((s) => ({ id: s.id, message: expandTextRefsDeep(s.message, dryTextOf).value, until: s.until, timeoutMs: s.timeoutMs, waitSeconds: s.waitSeconds, gate: !!s.gate }));
    const report = {
      // 配信者の ID は args に書かない (argsForResult)。
      args: argsForResult(args),
      guard: {
        blockers: guard.blockers,
        encoderBusy: guard.encoderBusy,
        encoderValues: guard.encoderValues,
        encoderError: guard.encoderError,
        encoderSessions: guard.encoderSessions,
        encoderSessionsError: guard.encoderSessionsError,
        encoderOnlyCallApps: guard.encoderOnlyCallApps,
      },
      blocked: guard.blocked,
      stepCount: planResult.steps.length,
      plannedMinutes: plannedMinutes(planResult.steps),
      // 見込みが期限 (--deadline-min) に足りない分。0 でなければ期限で打ち切られる実行計画なので、呼び出し行を修正する。
      deadlineMin: args.deadlineMin,
      deadlineShortfallMinutes: deadlineShortfallMinutes(planResult.steps, args.deadlineMin),
      // 期限の見積もりの内訳 (deadlineEstimate。call-1 の実際の実行はこれと 180 の大きいほうを使う)。
      deadlineEstimate: deadlineEstimate(planResult.steps, deadlineEstimateOptsFor(args)),
      humanSteps,
      // 実行計画の中でキーを送る手順 (KEY_SENDING_KINDS)。再確認の実際の実行では 0 であること。
      keySendingSteps: keySendingSteps(planResult.steps),
      // 後始末 (手順の cleanup と finally の cleanup の両方) が cordmux の録画のフォルダ (recordings の下) を残すか。
      keepRecordings: keepsRecordings(args),
      steps: planResult.steps,
      filesManifest: manifest,
      env: envOverride,
      unhandledKinds,
      resolvedTexts,
      missingTextIds,
      obsTextIdsUnchecked,
      seedPreview,
    };
    // 実際の ID の形 (17-20 桁) を渡した dry-run は、出力の文字列の全部から ID を伏せる (端末とログに ID を出さない)。
    const reportText = JSON.stringify(report, null, 2);
    const realId = args.broadcasterId && /^[0-9]{17,20}$/.test(args.broadcasterId) ? args.broadcasterId : null;
    console.log(realId ? reportText.split(realId).join(REDACTED_BROADCASTER_ID) : reportText);
    if (report.deadlineShortfallMinutes > 0) {
      console.error("dry-run: 実行計画の見込み " + report.plannedMinutes + " 分が期限 --deadline-min " + args.deadlineMin + " に足りない (" + report.deadlineShortfallMinutes + " 分)。期限に達すると残りの手順は打ち切られる");
    }
    if (unhandledKinds.length > 0 || missingTextIds.length > 0 || seedRefused.length > 0) {
      console.error("dry-run: the plan cannot run as is:", JSON.stringify({ unhandledKinds, missingTextIds, seedRefused }));
      process.exitCode = 1;
      return;
    }
    // cleanup.ps1 は -DryRun でも実際の %APPDATA%\Vesktop\state.json と
    // %LOCALAPPDATA%\cordmux\session.json の存在を見に行くので、--real-profile の事前確認
    // では呼ばない (利用者の実際の保存先には読みにも行かない)。
    // cleanup の dry-run にも差し替えの値だけを渡し、保存先は作らない (上の envOverride と同じ)。
    const cleanupOut = args.realProfile
      ? "skipped (real profile)"
      : runCleanup(true, overriddenEnv(LOCAL_APPDATA_OVERRIDE_REL, APPDATA_OVERRIDE_REL, HANDSHAKE_DIR_REL, false), { keepRecordings: keepsRecordings(args) });
    console.log("cleanup.ps1 -DryRun:", cleanupOut);
    process.exitCode = guard.blocked ? 2 : 0;
    return;
  }

  if (guard.blocked) {
    console.error(
      "守りに引っかかったので始めない:",
      JSON.stringify(
        {
          blockers: guard.blockers,
          encoderBusy: guard.encoderBusy,
          encoderValues: guard.encoderValues,
          encoderError: guard.encoderError,
          encoderSessions: guard.encoderSessions,
          encoderSessionsError: guard.encoderSessionsError,
          encoderOnlyCallApps: guard.encoderOnlyCallApps,
        },
        null,
        2,
      ),
    );
    process.exitCode = 2;
    return;
  }

  const outcome = await runReal(args, planResult.steps, toAbs(args.out));
  // ゲートは自分の終了コードで失敗を返す (人はこの終了コードを見てから
  // 次の step を起動する。1 つの step の中で分岐しない)。
  // **手順を最後まで進めずに抜けたときも成功にしない**: 期限で打ち切った場合・止める通知ファイルで抜けた場合は判定が
  // 付いていない (失敗と同じ扱い = 3)、予期しない例外はツール側の失敗 (1) とする。
  if (outcome && outcome.aborted === "error") {
    console.error("予期しない例外で手順を最後まで進められなかった (結果 JSON の error を見よ)");
    process.exitCode = 1;
  } else if (outcome && (outcome.gateFailures.length > 0 || outcome.aborted === "deadline" || outcome.aborted === "stopped")) {
    if (outcome.gateFailures.length > 0) console.error("失敗 (判定が通らなかった手順):", JSON.stringify(outcome.gateFailures, null, 2));
    if (outcome.aborted === "deadline") console.error("期限で打ち切ったので、残りの判定は付いていない");
    if (outcome.aborted === "stopped") console.error("止める通知ファイル (" + STOP_FILE_NAME + ") で抜けたので、残りの判定は付いていない");
    process.exitCode = 3;
  }
  // 利用者の Vesktop の vencordDir が実行前の値に戻っていなければ、ほかが成功でも失敗にする (「元に戻りました」と報告させない)。
  if (outcome && outcome.vencordFinal && outcome.vencordFinal.same !== true) {
    console.error(
      "利用者の Vesktop の vencordDir が実行前の値と同じだと確認できない (" + VENCORD_DIR_FINAL_PREFIX + "<tag>.json を見よ):",
      JSON.stringify({ same: outcome.vencordFinal.same, reason: outcome.vencordFinal.reason, restorePlan: outcome.vencordFinal.restorePlan }),
    );
    if (!process.exitCode) process.exitCode = 3;
  }
}

main().catch((e) => {
  console.error("予期しない失敗:", e);
  process.exitCode = 1;
});
