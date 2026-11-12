// SPDX-License-Identifier: Apache-2.0

// cordmux の設定 (settings.json) の値と、JSON の文字列との変換。settings.json は OBS のモジュールの設定フォルダー
// (obs_module_config_path) に置く。ファイルやキーがなければ既定値を使い、未知のキーは無視する。形が違う値が
// 1 つでもあれば、すべてを既定値に戻して理由を返す (一部の値だけが動作した、読み取りにくい状態を作らないため)。
// ログに書くのは呼び出し元 (plugin-main.cpp)。libobs にも FFmpeg にも依存しない。値の使い方 (デコーダーの種類を
// AVHWDeviceType に変換する、A/V の補正をかける、など) は pipeline/ などの使う側が決め、ここは値を読むだけで
// ある。録画の ON と OFF はここに持たない (保存せず、起動するたびに OFF から始める)。

#pragma once

// nlohmann/json はそのまま使うだけなので、ヘッダーが出す警告は抑える (ws/control_messages.h と同じ)。
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <nlohmann/json.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

#include "config/allocation.h"
#include "config/label_placement.h"

namespace cordmux::config {

// デコーダーの種類。settings.json の値は "software" と "hardware" (hardware は D3D11VA) で、既定値は software。
// 値は利用者の語にし、API の名前を設定に書かない。値を "d3d11va" にしないもう 1 つの理由は、
// test/lint/windows_api_confinement.py が platform/ の外で禁じる d3d11 (test/lint/windows_names.txt) を、
// 文字列の中に含んでしまうためである。
enum class DecoderChoice { Software, D3D11VA };

// 送信側 (Vesktop) が映像をエンコードするときの、エンコーダの好み。既定値は Hardware で、ハードウェアを先に試し、使えなければ
// ソフトウェアに落ちる (送信側 encode/ladder.ts のはしご)。Software は、はしごのどの段階もソフトウェアで試す。値の
// 書き方は "hardware" と "software" で、制御用の JSON の limits の encoder_preference と同じである。
enum class EncoderPreference { Hardware, Software };

// 送信側が、エンコードで速さと画質のどちらを優先するか。既定値は Speed (WebCodecs の latencyMode "realtime")。Quality は
// latencyMode "quality" で、遅延が増える代わりに画質が上がる。値の書き方は "speed" と "quality"。制御用の JSON
// の limits では latency_mode の "realtime" と "quality" になる (pipeline/pipeline.h の limits_from_settings)。
enum class EncodePriority { Speed, Quality };

// 誰を枠に載せるかの決め方 (載せる人の選び方)。Manual はチェックを付けた人だけを載せ、Auto は画面共有を始めた
// 人を、枠の数まで載せる。既定値は Manual。値の書き方は "manual" と "auto" で、画面に表示する語 (「手動」
// 「自動」) は ini が持つ。
//
// 動作するのは画面共有の枠だけである。声の枠は、誰を載せるかの対象にしない。声の枠を映像の枠に結び付けると、
// 載せていない人の声が配信から消えるためである。決め方を適用するのは pipeline/stream_router.h。
enum class AssignMode { Manual, Auto };

// Vesktop がどの人の映像を受信するか (設定の receive_scope)。既定値は Slots で、値の書き方は "everyone" と "slots"。
// Slots は、枠に載せた人の画面共有だけを視聴する。cordmux の画面共有の録画も枠に載せた人の分だけになる。Everyone は
// 負荷が高い代わりに、枠に載せる前から映像が届いていて、載せるとすぐ映る。この得失は利用者が選ぶ。
enum class ReceiveScope { Everyone, Slots };

// ドックで開いているタブ (設定の dock_tab)。次の起動でも同じタブを開く。値の書き方は "people"、"audio"、"display"、
// "status"。
enum class DockTab { People, Audio, Display, Status };

// 録画ファイルの作成で書き込む PCM の形式。
//
// セグメントファイルの PCM は、ワイヤー形式のまま f32 で書く (変換せずに書くので、音質が落ちない)。切り替え
// られるのは録画ファイルの作成の側だけで、この値はそこにだけ影響する。Float32 (同梱の FFmpeg の pcm_f32le、MOV の 4 文字
// コードは fl32) と Int24 (同じく in24) から選べる。既定値の Float32 は、届いた音声を変換せずに書く。
//
// 値の書き方を fl32 と in24 にするのは、MOV の 4 文字コードのままにするためで、利用者の語ではなく、設定ファイルの
// 値である (画面に表示する語は ini が持つ。decoder の software と hardware が利用者の語なのは、あちらが利用者の
// 選択そのものだからである)。
enum class RecordPcmFormat { Float32, Int24 };

// 2 つの A/V の補正の範囲。上限の 2000 ms は、測った値を収めるための目安の範囲で、測って決めたものではない。
//
// 補正で修正するずれは、この範囲よりずっと小さい。ずれの原因 (ソースごとに、映像をバッファーに持つフレーム数が
// 違う) を測ると 0 から 3 フレーム (16.7、33、50 ms) で、A/V のずれも +129 ms までだった。2000 ms はその 40 倍以上
// である。範囲の端に近い値を設定したときに、libobs が音声のタイムスタンプをどう扱うか (MAX_TS_VAR などの境界) は
// 確認していない。
constexpr int kScreenAudioOffsetMinMs = -2000;
constexpr int kScreenAudioOffsetMaxMs = 2000;
constexpr int kVoiceAudioDelayMinMs = 0;
constexpr int kVoiceAudioDelayMaxMs = 2000;

// 送信側から届くログの行 (制御用の JSON の log。docs/wire_format.md の「メッセージの種類」) を、受信側で保つ
// 行数。上限のあるリングバッファーに保ち、上限の行数は目安なので設定で変えられる。0 なら 1 行も持たない。範囲の
// 上限の 10000 は、送信側のリングバッファー (500 行) の 20 倍まで持てる大きさで、測って決めたものではない。
constexpr int kJsLogRingMinLines = 0;
constexpr int kJsLogRingMaxLines = 10000;

// ドックが画面を更新する周期 (ms)。ドックはイベントを受け取らず、QTimer で状態を読みに行く。下限の 50 ms と
// 上限の 60 秒は目安の範囲で、測って決めたものではない。0 を許さないのは、0 にすると状態を読みに行かなくなり、
// 画面が更新されなくなるためである。
constexpr int kDockPollMinMs = 50;
constexpr int kDockPollMaxMs = 60000;

// 映像が届かない時間がこの長さを超えると、映像が止まったと判断する (ms)。既定値の 2 秒は目安の値で、
// ledger::kDefaultVideoStallUs と同じ値を ms で持つ。利用者のトレードオフに関わる値なので、固定しない。
constexpr int kVideoStallMinMs = 100;
constexpr int kVideoStallMaxMs = 60000;

// 「これまでに見た人の一覧」に持つ人数の上限 (捨て方は config/seen_people.h)。1 人未満にできないのは、0 にすると
// 一覧が働かなくなるためである (一覧を使わないための設定ではない)。範囲の上限の 10000 は目安で、測って決めた
// ものではない。
constexpr int kSeenPeopleMaxMin = 1;
constexpr int kSeenPeopleMaxLimit = 10000;

// cordmux.dll 自身のログのリングバッファーの行数。上限と下限は、送信側のログの行数 (js_log_ring_lines) と同じ
// 範囲で、測って決めたものではない。
constexpr int kPluginLogRingMinLines = 0;
constexpr int kPluginLogRingMaxLines = 10000;

// 出来事の行 (状態が変わったときの行。診断情報の plugin.log と discord.log) を、受信側と送信側のそれぞれで保つ行数
// (plugin_event_log_lines と js_event_log_lines)。統計の行 (plugin_log_ring_lines と js_log_ring_lines。数秒ごとの行) とは
// 別に持つ。統計の行と同じ保持に入れると、出来事の行が数分で押し出され、映像が止まった時刻の前後を後から追えない。
// 既定の 5000 行は、通話の全体 (4 時間の通話を想定する) を残せる大きさとして決めた。実機テストの OBS のログ (22 分、
// 画面共有 1 人) では、受信側の出来事の行は 94 行 (1 時間あたり約 260 行) だった。4 人の通話では、人の出入り、
// 共有の開始と終了、メッセージの表示と解消、送信側の接続とトラックの行が増えるので、その 4 倍 (1 時間あたり約 1000 行)
// まで見込み、4 時間で 4000 行に余裕を足した。1 行はおよそ 100 から 200 バイトなので、1 つの保持で 1 MB 前後である。
// 上限の 50000 行は、長い配信で増やすときの範囲で、1 行が長くても数十 MB に収まる。
constexpr int kEventLogDefaultLines = 5000;
constexpr int kEventLogMinLines = 0;
constexpr int kEventLogMaxLines = 50000;

// 画面共有の映像が届かなくなってから、Vesktop に受信のやり直しを頼むまでの時間 (ms。最後のフレームが届いた時刻から
// 数える)。既定の 5 秒は、映像が止まったとみなす 2 秒 (video_stall_ms) の後、さらに 3 秒待つ値である。止まった直後の
// 数秒は、回線の揺れなどで自然に戻ることが多い (実際の 4 人の通話で、届かない状態の多くは 2 秒から 3 秒で戻った)
// ので、すぐには頼まない。届くまでの揺れは環境で変わるので、settings.json で変えられるようにする。0 なら頼まない。
// 上限の 10 分は目安の範囲である。
constexpr int kReceiveRestartWaitDefaultMs = 5000;
constexpr int kReceiveRestartWaitMinMs = 0;
constexpr int kReceiveRestartWaitMaxMs = 600000;

// ---- 録画の値 (どれも目安の値なので、固定せずに設定で変えられるようにする) ----

// 空き容量を確認する周期 (秒)。下限を 1 秒にしてあるのは、空き容量がなくなったときに録画が止まるかを、実機の
// テストで短い時間で確認するためである (測って決めた値ではない)。0 を許さないのは、0 にすると確認が止まり、
// 空き容量がなくなる前に伝えることも、なくなったときに録画を止めることもできなくなるためである。
constexpr int kRecordSpaceCheckIntervalMinS = 1;
constexpr int kRecordSpaceCheckIntervalMaxS = 86400;

// 空き容量が少ないとメッセージ始める、録画できる残り時間 (分)。ledger::kDefaultSpaceWarnSeconds と同じ値を分で持つ。
constexpr int kRecordSpaceWarnMinMinutes = 1;
constexpr int kRecordSpaceWarnMaxMinutes = 1440;

// 音声の置く位置を時刻に追従させる 3 つの値 (align/audio_drift.h の AudioDriftConfig)。配信 (pipeline/ の AudioMixer) と
// 録画が同じ値を使う。既定の値の理由は AudioDriftConfig のコメントにある。範囲は、桁を間違えた値で追従が働かなく
// ならないための目安で、測って決めたものではない。速さの補正の上限の 50000 ppm は 5% (85 セント) で、それより大きい
// 補正は音の高さの変化がはっきり聞こえる。
constexpr int kAudioResyncMinMs = 1;
constexpr int kAudioResyncMaxMs = 2000;
// 配信の声の置き直しのしきい値 (ms)。既定の理由は pipeline/audio_mixer.h の AudioMixerConfig::voice_resync_us にある。
// 範囲は audio_resync_ms と同じ。
constexpr int kVoiceAudioResyncMinMs = 1;
constexpr int kVoiceAudioResyncMaxMs = 2000;
constexpr int kAudioDriftMaxPpmMin = 1;
constexpr int kAudioDriftMaxPpmMax = 50000;
constexpr int kAudioDriftTimeConstantMinMs = 100;
constexpr int kAudioDriftTimeConstantMaxMs = 600000;

// OBS を終了するときに、録画ファイルの作成を待つ時間の上限 (秒)。既定値の 30 秒は目安の値で、測って決めたものではない。0 なら
// 待たずに、次に起動したときに録画ファイルを作成する (いつも次の起動で録画ファイルを作成する動作を、テストで確認するための値)。範囲の上限の
// 1 時間は目安である。
constexpr int kRecordFinalizeWaitMinS = 0;
constexpr int kRecordFinalizeWaitMaxS = 3600;

// 録画の待ち行列に積めるメッセージの数。既定値の 1024 の理由は、下の Settings::record_queue_limit のコメントに
// ある。
constexpr int kRecordQueueLimitMin = 16;
constexpr int kRecordQueueLimitMax = 65536;

// タイムスタンプの後戻りを 3 段階に分ける、2 つのしきい値。
//
// 1 段階目と 2 段階目の境は 1 フレームの間隔なので、ms ではなくフレームの間隔の数で持つ。60 fps と 30 fps では
// ms の値が 2 倍違うので、ms で固定すると、どちらかで必ず段階を取り違える。既定値の 1 は
// align/timestamp_aligner.h の min_output_step_us と同じ考え方である。
constexpr int kRecordClockPushMaxFramesMin = 1;
constexpr int kRecordClockPushMaxFramesMax = 60;

// 2 段階目と 3 段階目の境は 1 秒。これを超える後戻りや、前への大きな不連続があれば、セグメントを閉じて、次の
// キーフレームから新しいセグメントを始める (CloseReason::ClockJump)。
constexpr int kRecordClockReanchorMinMs = 1;
constexpr int kRecordClockReanchorMaxMs = 60000;

// ---- 画質の上限と、同梱のファイルを確認する周期 (どれも利用者のトレードオフか目安の値) ----

// 送信側に送ってもらう映像の長い辺の上限 (ピクセル。limits メッセージの max_long_edge)。0 は制限なしで、そのとき
// は limits からキーごと省く (ワイヤー形式では、省いたら制限なしで、0 は範囲の外として捨てられる。
// ws/control_messages.h の LimitsMessage)。画面の選択肢は、制限なし、1920、1280、960、640 だが、設定
// ファイルは選択肢の外の値も受け付ける (実機のテストで別の値を試せるように)。1 から 15 を受け付けないのは、
// 文字がつぶれる大きさを作らないためである。上限は 8K (7680x4320) の長い辺。どちらも目安の値で、測って決めたものでは
// ない。
constexpr int kLimitMaxLongEdgeNone = 0;
constexpr int kLimitMaxLongEdgeMin = 16;
constexpr int kLimitMaxLongEdgeMax = 7680;

// 送信側に送ってもらうフレームレートの割る数 (limits メッセージの fps_divisor)。1 ならそのまま、2 なら半分。
// 1 と 2 だけにする (送信側のエンコードが受け付けるのがこの 2 つ。src-vencord/encode/session.ts の
// validateParams)。
constexpr int kLimitFpsDivisorMin = 1;
constexpr int kLimitFpsDivisorMax = 2;

// 同梱の 4 つのファイルを、元のファイルと比べる周期 (秒。既定値は、Vesktop と接続していないとき 30 秒、
// 接続しているとき 5 分)。platform/os_probe.h の OsProbeConfig::disconnected_interval_ms と connected_interval_ms
// に、ms に修正してコピーする (コピーするのは呼び出し元)。画面には表示しない。下限の 1 秒と上限の 1 日は、空き容量を
// 確認する周期 (kRecordSpaceCheckInterval*) と同じ範囲で、目安の値である (下限は、テストで周期を短くして、
// Vencord の更新でファイルが上書きされたことを検知できるかを確認するため)。
constexpr int kVencordCheckIntervalMinS = 1;
constexpr int kVencordCheckIntervalMaxS = 86400;

// ---- メッセージ、名前ラベル、話している人の光、ダミー、録画ファイルの作成の値 (どれも利用者のトレードオフか目安の値) ----
//
// 範囲はどれも目安で、測って決めたものではない。既定値の理由は Settings のフィールドのそばに書く。

// メッセージを表示するまでと、消すまでの待ち時間 (ms)。0 なら待たない。上限の 1 分は目安の範囲である。
constexpr int kNoticeDelayMinMs = 0;
constexpr int kNoticeDelayMaxMs = 60000;

// 画面共有をしていると分かってから、映像が届き始めないと伝えるまでの待ち時間 (ms)。下限を 1 秒にするのは、
// 画面共有を始めた直後の最初のフレームは、届くまでに必ず時間がかかり、それより短いと、何も壊れていないのに
// メッセージを表示してしまうためである。上限の 2 分は目安の範囲である。
constexpr int kFirstFrameWaitMinMs = 1000;
constexpr int kFirstFrameWaitMaxMs = 120000;

// Vesktop との接続が切れてから、枠を空けるまで最後のフレームを表示し続ける時間 (ms)。0 ならすぐ空ける。
constexpr int kLinkLostHoldMinMs = 0;
constexpr int kLinkLostHoldMaxMs = 60000;

// Vesktop と接続し直してから、映像が戻らない枠の配置を保つ時間 (ms)。0 なら接続し直した時点で並べ直す。
constexpr int kReconnectLayoutHoldMinMs = 0;
constexpr int kReconnectLayoutHoldMaxMs = 60000;

// [移動] と [退出] を押してから、結果が出ないとき (通話が変わらないとき) にメッセージを出すまでの待ち時間 (ms)。送信側は
// 失敗しても何も返さないことがあるので、受信側が通話の変化を待って決める。下限は、Discord の画面が通話を切り替える時間より
// 短くしない値で、上限の 1 分は目安の範囲である。どちらも測って決めたものではない。
constexpr int kCallActionTimeoutMinMs = 1000;
constexpr int kCallActionTimeoutMaxMs = 60000;

// 画面共有の映像の表示の緩衝 D (ms)。合成のソースがあるときだけ動作し、cordmux が送信側のタイムスタンプ + D の後の
// OBS の描く刻みに OBS へ渡す (OBS の緩衝は使わない。0 でも同じ)。合成のソースがないときは D を見ずに、受け取った
// フレームをすぐ OBS に渡し、表示の時刻は OBS の緩衝に任せる。上限の 1 秒は、滑らかさのための遅れとして許せる長さの
// 目安で、測って決めた値ではない。画面の音は映像の表示に合わせて置くが、音の位置のずれが 2 秒を超えると OBS が
// タイミングを取り直すので、2 秒より十分小さくする。
constexpr int kDisplayBufferMinMs = 0;
constexpr int kDisplayBufferMaxMs = 1000;

// 枠の間のすき間 (配信の画面のピクセル)。上限の 64 は 1080p の高さの 6% で、4 分割の枠を目に見えて
// 小さくしない範囲の目安である。
constexpr int kTileGapMinPx = 0;
constexpr int kTileGapMaxPx = 64;

// 名前ラベルの文字の大きさ (配信の画面の高さに対する %。0.1 刻み)。1440p では下限の 1.0 が 14 px、上限の 8.5 が
// 122 px になる。既定値の 2.5 は、1080p で 27 px、1440p で 36 px になる。sources/decor_params.h の
// kDefaultLabelEmRatio は、この既定値と同じ割合である。
constexpr double kLabelEmRatioMinPct = 1.0;
constexpr double kLabelEmRatioMaxPct = 8.5;
constexpr double kDefaultLabelEmRatioPct = 2.5;

// 合成のソースの名前ラベルの値。割合は、断りのないものは名前の大きさ (em) に対する倍率。
// 枠の隅からの余白 (配信の画面の高さに対する %)。名前の大きさには連動させない (枠との距離を決める値なので)。
constexpr double kLabelMarginMinPct = 0.0;
constexpr double kLabelMarginMaxPct = 13.9;
constexpr double kDefaultLabelMarginPct = 0.5;
// 文字の縁取りの太さ。線は文字の輪郭を中心に引くので、見える縁はこの半分になる。
constexpr double kLabelOutlineRatioMin = 0.0;
constexpr double kLabelOutlineRatioMax = 0.4;
constexpr double kDefaultLabelOutlineRatio = 0.1;
// 名前ラベル全体の不透明度 (%)。
constexpr int kLabelOpacityMinPct = 0;
constexpr int kLabelOpacityMaxPct = 100;
constexpr int kDefaultLabelOpacityPct = 100;
// アバターの直径 (名前の大きさに対する倍率)。
constexpr double kLabelAvatarRatioMin = 0.4;
constexpr double kLabelAvatarRatioMax = 3.0;
constexpr double kDefaultLabelAvatarRatio = 1.5;
// 発言中のグローのぼかしと広がり。ぼかしの標準偏差はぼかしの半分。
constexpr double kLabelGlowBlurRatioMin = 0.0;
constexpr double kLabelGlowBlurRatioMax = 3.0;
constexpr double kDefaultLabelGlowBlurRatio = 0.10;
constexpr double kLabelGlowSpreadRatioMin = 0.0;
constexpr double kLabelGlowSpreadRatioMax = 1.0;
constexpr double kDefaultLabelGlowSpreadRatio = 0.10;
// 話していない人の名前ラベルを暗くする度合い (%。5 刻み)。0 は暗くしない。
constexpr int kLabelDimMinPct = 0;
constexpr int kLabelDimMaxPct = 90;
constexpr int kLabelDimStepPct = 5;
constexpr int kDefaultLabelDimPct = 30;
// 自分の枠の人が話しているかを音声ソースの音で決めるときの、話し始めのしきい値 (dBFS。1 刻み)。範囲は OBS のノイズゲートの
// しきい値と同じ。既定は、フィルタを付けていないマイク 1 本を静かな部屋で測った値から決めた: 黙っている間の peak の最大は
// -28 から -27 dB で、OBS のノイズゲートの開放しきい値 (-26 dB) では余裕が 1 から 2 dB しかない。-24 dB なら 3 から 4 dB の
// 余裕があり、ふつうの声の peak の上位 5% (-19 から -16 dB) はなお 5 dB 以上上回る。マイクと部屋で変わるので、利用者が
// 設定で合わせる。
constexpr int kSelfSpeakingThresholdMinDb = -96;
constexpr int kSelfSpeakingThresholdMaxDb = 0;
constexpr int kDefaultSelfSpeakingThresholdDb = -24;
// 画面共有していない参加者の間隔 (名前の大きさに対する倍率。0.05 刻み)。人と人の間と、行 (列) と行 (列) の間の両方。
constexpr double kRosterGapRatioMin = 0.0;
constexpr double kRosterGapRatioMax = 3.0;
constexpr double kDefaultRosterGapRatio = 0.25;
// 画面共有していない参加者の名前の最大幅 (名前の大きさに対する倍率。0.1 刻み)。これより長い名前は末尾を省略する。
// 既定の 12 は、全角で 12 文字ほど (半角の英字で 24 文字ほど) で、長すぎる名前がキャンバスの幅を取らない目安の値。
constexpr double kRosterNameMaxEmMin = 3.0;
constexpr double kRosterNameMaxEmMax = 40.0;
constexpr double kDefaultRosterNameMaxEm = 12.0;
// 画面共有していない参加者の並びの位置を、配置と余白で決まる位置からずらす量 (キャンバスのピクセル。符号付きで、右と下が正。
// OBS の座標と同じ向き)。配信の画面の時計などの装飾と重ならないように、利用者がピクセルで合わせる。キャンバスの解像度を変えても
// 値はピクセルのまま変えない (配信の画面の配置はピクセルで作る人が多いため)。範囲は固定にする。設定を読むときはキャンバスの
// 大きさが分からず、キャンバスの大きさで範囲を変えると、解像度を変えただけで保存した値が変わるためである。上限の 7680 は 8K
// (7680x4320) のキャンバスの幅で、並びをキャンバスの端から端まで動かせる。範囲の中なら、キャンバスの外へはみ出す値も
// そのまま使い、自動で戻したりほかの物を避けたりはしない (利用者が決める配置を動かさない)。
constexpr int kRosterOffsetMinPx = -7680;
constexpr int kRosterOffsetMaxPx = 7680;

// 名前ラベルの背景の不透明度 (%)。
constexpr int kLabelPlateOpacityMinPct = 0;
constexpr int kLabelPlateOpacityMaxPct = 100;

// 枠の背景 (映像を描かない所の色) の不透明度 (%)。
constexpr int kSlotBackgroundOpacityMinPct = 0;
constexpr int kSlotBackgroundOpacityMaxPct = 100;

// 話している人の光を表示し始める、保つ、消える時間 (ms)。
constexpr int kLabelBorderTimingMinMs = 0;
constexpr int kLabelBorderTimingMaxMs = 5000;

// 名前ラベルの文字の太さ (OpenType と Qt 6 の QFont の太さの尺度で 1 から 1000。既定値は太字の 700)。100 刻みに
// しない。フォントを選ぶダイアログが返す太さはフォントが持つ値のまま (Semilight の 350 など) で、ダイアログが
// 書いた値を次の起動で読めないと、すべてのキーが既定値に戻るためである (parse_settings は、1 つでも形が違えば
// すべてを既定値に戻す)。
constexpr int kLabelFontWeightMin = 1;
constexpr int kLabelFontWeightMax = 1000;
constexpr int kDefaultLabelFontWeight = 700;

// ドックの一覧の並べ替えを保留している間、ポインターが止まってから反映するまでの時間の範囲 (ms)。0 は待たない。
constexpr int kLayoutHoldIdleMinMs = 0;
constexpr int kLayoutHoldIdleMaxMs = 60000;

// ダミーの動く線の 1 往復の時間 (ms)。下限を 2 秒にするのは、同じ場所が明暗で何度も入れ替わる点滅を作らない
// ためである (sources/dummy_tiles.h の kDefaultDummyCycleNs)。
constexpr int kDummyCycleMinMs = 2000;
constexpr int kDummyCycleMaxMs = 60000;

// 録画ファイルの作成の進み具合が止まったとみなすまでの時間 (秒)。
constexpr int kRecordFinalizeStallMinS = 10;
constexpr int kRecordFinalizeStallMaxS = 3600;

// 色の既定値 (`#RRGGBB`)。話している人の光は Discord の発言中の色と同じ色相の明るい緑、背景は黒、文字は白。
inline constexpr const char *kDefaultLabelBorderColor = "#57F287";
inline constexpr const char *kDefaultLabelPlateColor = "#000000";
inline constexpr const char *kDefaultLabelTextColor = "#FFFFFF";
inline constexpr const char *kDefaultLabelOutlineColor = "#000000";
inline constexpr const char *kDefaultSlotBackgroundColor = "#000000";

struct Settings {
	DecoderChoice decoder = DecoderChoice::Software;

	// 画面共有の「音声 - 映像」の補正 (ms。符号付き)。正なら画面共有の音声のタイムスタンプにその分を足して
	// 遅らせ、負なら映像の出力のタイムスタンプに |値| を足す。
	int screen_audio_offset_ms = 0;

	// 声を遅らせる量 (ms。0 以上)。既定は遅らせない。画面には出さず、設定のファイルでだけ変えられる。
	int voice_audio_delay_ms = 0;

	// 送信側のログの統計の行を、受信側で保つ行数 (診断情報の discord-stats.log)。
	int js_log_ring_lines = 200;

	// 送信側のログの出来事の行を、受信側で保つ行数 (診断情報の discord.log)。
	int js_event_log_lines = kEventLogDefaultLines;

	// ドックが画面を更新する周期 (ms)。設定ファイルでだけ変える。既定の 200 ms は、更新 1 回の時間を測って決めた
	// (docs/architecture.md の「ドック」)。
	int dock_poll_ms = 200;

	// 映像が届かない時間がこの長さを超えると、映像が止まったと判断する (ms)。
	int video_stall_ms = 2000;

	// 誰を枠に載せるかの決め方。既定値は Manual。ledger::Choices の同じ名前のフィールドにコピーする。
	AssignMode assign_mode = AssignMode::Manual;

	// 枠に載っている人を配信のどの位置に置くかの決め方 (config/allocation.h の SlotOrder)。既定値は表示名の順。
	SlotOrder slot_order = SlotOrder::Name;

	// 配信の開始で cordmux の録画を開始し、配信の終了で録画を終了するか。録画そのものの ON と OFF は保存しない
	// (起動するたびに OFF から始める) が、この設定は保存する。
	bool record_with_stream = false;

	// 配信中に録画を終了するときと、配信中か録画中に cordmux を無効にするときに確認するか。
	bool confirm_while_streaming = true;

	// Vesktop がどの人の映像を受信するか。
	ReceiveScope receive_scope = ReceiveScope::Slots;

	// ドックの一覧の並べ替えを保留している間、ポインターが止まってから反映するまでの時間 (ms)。
	int layout_hold_idle_ms = 2000;

	// ドックで開いているタブ。
	DockTab dock_tab = DockTab::People;

	// 「これまでに見た人の一覧」に持つ人数。
	int seen_people_max = 200;

	// cordmux.dll 自身のログの統計の行を保つ行数 (診断情報の plugin-stats.log に入る)。
	int plugin_log_ring_lines = 200;

	// cordmux.dll 自身のログの出来事の行を保つ行数 (診断情報の plugin.log に入る)。
	int plugin_event_log_lines = kEventLogDefaultLines;

	// メインアカウントのユーザー ID (設定の cordmux.settings.main_account_id.label の入力欄)。空なら、メイン
	// アカウントだけの構成で、メインアカウントが通話に参加していないことのメッセージ
	// (cordmux.dock.main_account_not_in_call) は意味を持たない。利用者が入れた userId をそのまま持つ (Discord の値
	// なので、翻訳しない)。ledger::Choices の同じ名前のフィールドにコピーする。
	std::string broadcaster_user_id;

	// 自分の枠に映す OBS のソース (設定の cordmux.look.self_frame_source.label の入力欄)。self_frame_source_uuid が空で
	// self_frame_source_name も空なら指定なしで、自分の枠には何も映さない。UUID で探し、見つからなければ
	// self_frame_source_name で探す (名前を変えても UUID は変わらないが、シーンコレクションを切り替えたときは UUID が
	// 変わる)。cordmux が見つけたソースに合わせて、この 2 つを書き直すことがある。
	std::string self_frame_source_uuid;
	std::string self_frame_source_name;

	// 自分の枠のチェックの状態 (ほかの人の枠が残るのと同じ)。既定は false で、自分の枠は、ドックで付けたとき、または
	// ほかの人と同じ規則 (載せる人の選び方が自動、[自動で載せる]) で自分の Discord の共有を始めたときに付く。付いたり
	// 外れたりするたびに書く。映像を OBS から選んでいない間は、値だけが残って何も出ない。
	bool self_frame_on = false;

	// 自分の枠に映す映像 (自分の枠と、「Discordの画面共有」のときのメインアカウントの画面共有) を、並び順によらず先頭 (左上) に
	// 置くか ([表示] タブの [先頭に配置])。偽なら、ほかの参加者と同じ並び順で置く (ledger/slot_cells.h の SlotCellSelf)。既定は
	// true で、キーがない settings.json は今までと同じ配置になる。参加者の一覧でメインアカウントの行が先頭に固定されるのとも、
	// 順序がそろう。
	bool self_slot_first = true;

	// ---- 録画 ----

	// 保存先 (UTF-8 の絶対パス)。空なら既定の保存先を使う (OBS の録画の保存先の下。読めなければ、cordmux 自身の
	// 設定フォルダーの下)。既定の保存先を作るのは platform/ の中で、ここは利用者が入れた値を持つだけである (既定の
	// 場所を表す値は、platform/ の外に文字列として書けない。test/lint/windows_api_confinement.py)。
	std::string record_dir;

	// 録画ファイルの作成で書き込む PCM の形式 (RecordPcmFormat のコメント)。
	RecordPcmFormat record_output_pcm = RecordPcmFormat::Float32;

	// 空き容量を確認する周期 (秒。既定値は 5 分)。
	int record_space_check_interval_s = 300;

	// 空き容量が少ないとメッセージ始める、録画できる残り時間 (分。既定値は 30 分)。
	int record_space_warn_minutes = 30;

	// 音声の置く位置を時刻に追従させる値 (上の定数のコメント)。置き直しのしきい値 (ms)、速さの補正の上限 (ppm)、
	// 追従の時定数 (ms)。配信と録画の両方に使い、起動のときに読む。
	int audio_resync_ms = 45;
	int audio_drift_max_ppm = 1000;
	int audio_drift_time_constant_ms = 8000;
	// 配信の声の組だけに使う置き直しのしきい値 (ms)。声は届いた時刻から置くので、画面の音より広くする。録画の声は
	// タイムスタンプで置くので audio_resync_ms を使う。
	int voice_audio_resync_ms = 70;

	// OBS を終了するときに、録画ファイルの作成を待つ時間の上限 (秒。既定値は 30 秒)。
	int record_finalize_wait_on_exit_s = 30;

	// 録画の待ち行列に積めるメッセージの数。
	//
	// 既定値の 1024 は目安の値で、測って決めたものではない。待ち行列があふれる原因として考えられる一時的な詰まり
	// (ウイルス対策ソフトの走査など) は秒の単位で続くので、枠がすべて埋まっているときの 1 秒分のメッセージの数
	// (画面共有の 4 つのストリームで 60 フレームずつと、声の 8 つのストリームで音声のチャンク 100 個ずつ。合わせて
	// 千の桁) を、吸収できる大きさにした。録画は枠に関係なく通話の全員の声を受け取るので、30 人が
	// 同時に話すと音声だけで 1 秒に 3000 個になり、そのような場面では 1 秒の詰まりを吸収できない。値は変えていない。
	// 表示の側の上限 (1 つのストリームにつき 8 アクセスユニット) より桁で大きいのは、表示は遅れたら捨てるのが
	// 正しいのに対し、録画は捨てた分がそのまま欠落になるためである。
	int record_queue_limit = 1024;

	// タイムスタンプの後戻りの 3 段階の境。1 段階目と 2 段階目の境はフレームの間隔の数、2 段階目と 3 段階目の境は
	// ms (上の 2 つの定数のコメント)。
	int record_clock_push_max_frames = 1;
	int record_clock_reanchor_max_ms = 1000;

	// Vesktop の state.json への書き込みを許すか。既定値は true (false にすると、[Vesktopに追加]
	// (cordmux.dock.add_to_vesktop.button) を押しても何もしない)。キーを残すのは、実際のプロファイルを使う実機
	// テストで書き込ませないためである。test/tools/obs-run/seed-config.py は、実際のプロファイルを使うテストの
	// うち、通話のテスト以外で false を書く。
	bool allow_state_json_write = true;

	// ---- 機能の ON と OFF、画質の上限、同梱のファイルを確認する周期 ----

	// 機能の ON と OFF (選んだ値を保存する)。既定値は true。OFF の間も接続の待ち受けは開いたままで、送信側には
	// media を false にして送る。ledger::Choices::feature にコピーする。
	bool feature_enabled = true;

	// メインアカウントの声 (Discord を通った自分の声) をミュートにするか。既定値は true。配信に載せる自分の声を OBS の
	// マイクにするか、Discord を通った声にするかの選択で、起動のたびに戻ると困るので保存する。メインアカウントの
	// ユーザー ID (broadcaster_user_id) を変えても、新しいメインアカウントの声に同じ値を当てる。ほかの人のミュートは
	// 保存しない (pipeline/mixer_roster.h の MixerLevels)。
	bool self_voice_muted = true;

	// 送信側に送ってもらう画質の上限。0 なら制限なし (上の定数のコメント)。
	int limit_max_long_edge = kLimitMaxLongEdgeNone;
	// 1 ならそのまま、2 なら半分。
	int limit_fps_divisor = 1;
	// 送信側のエンコードの設定 (上の 2 つの上限と同じく、OBS の実行中に送信側へ送り直す)。画面共有が始まるときに使う
	// ので、すでに続いている画面共有には適用されない。
	EncoderPreference encoder_preference = EncoderPreference::Hardware;
	EncodePriority encode_priority = EncodePriority::Speed;

	// 同梱の 4 つのファイルを確認する周期 (秒。目安の値)。
	int vencord_check_disconnected_s = 30;
	int vencord_check_connected_s = 300;

	// ---- メッセージ、名前ラベル、話している人の光、ダミー、録画ファイルの作成、ウィンドウ ----
	// (設定ウィンドウのどこに表示するかと、再起動が必要かは、dock/settings_fields.cpp にある)

	// メッセージを表示するまでと、消すまでの待ち時間 (映像が 2 秒届かなければ表示し、1 秒届き続けたら消す)。映像が
	// 止まったと判断するしきい値 (video_stall_ms) とは別の値である (判定のしきい値と、表示の待ち時間は役目が違う)。
	int notice_show_delay_ms = 2000;
	int notice_clear_delay_ms = 1000;

	// 映像が届き始めないと伝えるまでの待ち時間。既定値の 5 秒は目安の値で、測って決めたものではない。
	// ledger::kDefaultFirstFrameWaitUs と同じ値を ms で持つ。
	int first_frame_wait_ms = 5000;

	// 枠に載せている人の映像が届かなくなってから、Vesktop に受信のやり直しを頼むまでの時間 (最後のフレームから数える)。
	// 0 なら頼まない (kReceiveRestartWaitDefaultMs)。
	int receive_restart_wait_ms = kReceiveRestartWaitDefaultMs;

	// 接続が切れてから、枠を空けるまで最後のフレームを表示し続ける時間 (既定値は 2 秒)。
	int link_lost_hold_ms = 2000;
	// 接続し直してから、映像が戻らない枠の配置を保つ時間 (既定値は 10 秒。画面共有の音を同じ共有の続きとして扱う長さ
	// (pipeline/mixer_roster.h の kShareResumeWindowUs) にそろえた目安の値)。切れたときに映っていた枠は、切れている間と、
	// 接続し直してからこの時間は並べ直さない (pipeline/pipeline.h の LinkLostLayoutHold)。
	int reconnect_layout_hold_ms = 10000;

	// [移動] と [退出] を押してから、通話が変わらないときにメッセージを出すまでの待ち時間 (既定値は 5 秒。目安の値)。
	int call_action_timeout_ms = 5000;

	// 画面共有の映像の表示の緩衝 D (ms)。合成のソースがあるときだけ動作する。既定値の 100 ms は、テスト用の送信側で映像の欠落が
	// 0 になる値で、実際の通話の映像が届くまでの遅れ (平均約 17 ms、95 パーセンタイルで約 40 ms) を吸える大きさである。
	// 利用者の配信の滑らかさと遅れのトレードオフに関わる値なので固定せず、設定で変えられるようにする。
	int display_buffer_ms = 100;

	// 枠の間のすき間 (既定値は 0。すき間を空けると、映像が小さくなる)。
	int tile_gap_px = 0;

	// 枠の背景。合成のソースの、映像を描かない所 (空いた枠、枠の間隔、映像の縦横の比が枠と違うときの余り、誰も載っていない
	// ときのキャンバス全体) に、キャンバス全体の 1 枚の矩形を、ほかの何よりも下に敷く。slot_background_show が false なら
	// 描かない。既定は描く。多くの使い方は 4 分割をそのまま配信する形で、空いた枠や枠の間が背景の色で埋まるほうが自然なため
	// である。cordmux のソースの下に別の背景を置く構成では、false にして透明にする (誰も枠に載せず背景を出さなければ、参加者の
	// 並びだけを出せる)。色は slot_background_color、不透明度は slot_background_opacity_pct。
	bool slot_background_show = true;
	std::string slot_background_color = kDefaultSlotBackgroundColor;
	int slot_background_opacity_pct = 100;

	// 名前ラベルの文字の大きさ (配信の画面の高さに対する %)。
	double label_em_ratio_pct = kDefaultLabelEmRatioPct;

	// 名前ラベルに表示するもの。話している人の光を表示しないときは、設定ウィンドウで、光の色の入力欄を操作できなくする。
	bool label_show_name = true;
	bool label_show_avatar = true;
	bool label_show_speaking = true;

	// 色 (`#RRGGBB`)。利用者が選んだ話している人の光の色には、相対輝度 0.40 の下限を適用しない。文字の色は、
	// 設定ウィンドウに表示しない。
	std::string label_border_color = kDefaultLabelBorderColor;
	std::string label_plate_color = kDefaultLabelPlateColor;
	int label_plate_opacity_pct = 50; // 背景の不透明度 (%)
	std::string label_text_color = kDefaultLabelTextColor;

	// 名前ラベルのフォントと太さ。ドックの [表示] タブで、利用者が変えられる。フォントの名前が空なら、
	// sources/decor_paint.h の kDefaultLabelFontFamilies を使う (設定ファイルに OS ごとのフォントの名前を書かない)。
	// sources::DecorLabelParams の、同じ意味の 2 つのフィールドにコピーする。
	// 斜体のキー label_font_italic は持たない (斜体は利用者に選ばせない)。前のバージョンか、手で書いた
	// settings.json にそのキーがあっても、知らないキーとして読み飛ばす (値の形も確認しない)。設定ファイルだけで
	// 変えられるキーとして残すと、手で true を書いた人の名前ラベルだけが、設定ウィンドウで修正する方法がないまま
	// 斜体になる。読む側だけで false に固定すると、読まないキーを書き出し続け、動作しない値がファイルに残る。
	std::string label_font_family;
	int label_font_weight = kDefaultLabelFontWeight;

	// 合成のソースの名前ラベル。位置は枠の隅から label_margin_pct (配信の画面の高さに対する %) 離す。縁取りの
	// 太さとアバターの直径は、名前の大きさに対する倍率で持つ。縁取りは label_show_outline が false なら描かない
	// (色と太さは label_outline_color と label_outline_ratio)。背景は label_show_plate が false なら描かない
	// (色と不透明度は label_plate_color と label_plate_opacity_pct)。label_hide_when_single は、合成が全面に
	// 1 枚だけ置いているときに、名前ラベルを隠す。label_show_in_slot は、画面共有している参加者 (と自分の枠) の名前ラベルを
	// 枠に描くか ([表示] タブの「画面共有している参加者」の [表示])。false なら枠には描かず、その人を [1人のときは非表示] で
	// 隠した人と同じく、画面共有していない参加者の並びに出す (並びも出さない設定なら、どこにも出ない)。枠の光は、枠が残るので
	// この値によらず描く。既定は true で、キーがない settings.json は今までと同じになる。
	double label_margin_pct = kDefaultLabelMarginPct;
	std::string label_outline_color = kDefaultLabelOutlineColor;
	double label_outline_ratio = kDefaultLabelOutlineRatio;
	bool label_show_outline = true;
	int label_opacity_pct = kDefaultLabelOpacityPct;
	bool label_show_plate = true;
	double label_avatar_ratio = kDefaultLabelAvatarRatio;
	bool label_hide_when_single = true;
	bool label_show_in_slot = true;

	// 合成のソースで話している人を示す光。枠・アバター・名前のうち光らせるものを label_glow_* で選ぶ。色は
	// label_border_color を使う。ぼかしと広がりは名前の大きさに対する倍率。
	bool label_glow_frame = false;
	bool label_glow_avatar = true;
	bool label_glow_name = true;
	double label_glow_blur_ratio = kDefaultLabelGlowBlurRatio;
	double label_glow_spread_ratio = kDefaultLabelGlowSpreadRatio;

	// 画面共有している参加者の名前ラベルを置く枠の隅 (全員一律) と、その名前ラベルの中のアバターの位置。話していない人の
	// 名前ラベルを暗くする度合い (%。0 なら暗くしない。話している人を光らせる設定とは別に動作する) は、画面共有していない
	// 参加者の名前ラベルにも反映される。
	LabelCorner label_corner = LabelCorner::BottomLeft;
	AvatarSide label_avatar_side = AvatarSide::Left;
	// アバターの形 (画面共有している参加者と、していない参加者の並びの両方)。
	AvatarShape label_avatar_shape = AvatarShape::Circle;
	int label_dim_pct = kDefaultLabelDimPct;

	// 自分の枠の人が話しているかを決める OBS の音声ソース。self_speaking_source_uuid が空なら指定なしで、Discord から受け取った
	// 自分の声で決める。UUID で探し、見つからなければ self_speaking_source_name で探す (名前を変えても UUID は変わらないが、
	// OBS の「マイク音声」を無効にして戻したときとシーンコレクションを切り替えたときは UUID が変わる)。cordmux が見つけた
	// ソースに合わせて、この 2 つを書き直すことがある。self_speaking_threshold_db は話し始めのしきい値 (dBFS)。
	std::string self_speaking_source_uuid;
	std::string self_speaking_source_name;
	int self_speaking_threshold_db = kDefaultSelfSpeakingThresholdDb;

	// 画面共有していない参加者 (通話に参加しているが映像の枠を持たない人) の名前ラベルの並び。roster_show で表示を切り替え、
	// roster_anchor の位置に寄せて、roster_direction の向きに並べる。位置と向きの合わない組は、読むときに修正する
	// (roster_direction_for)。roster_avatar_side は並びの名前ラベルの中のアバターの位置で、縦並びの列の中の揃えもこれに
	// 従う。roster_gap_ratio は名前の大きさに対する間隔の倍率。roster_name_max_em は名前の幅の上限 (名前の大きさに対する
	// 倍率)。roster_show_bots は、Bot のアカウント (profiles の bot)
	// も並びに出すか (既定は出さない。通話に参加できた Bot が配信に並ばないようにする)。roster_offset_x_px と
	// roster_offset_y_px は、配置と余白で決まる並びの位置に足すずれ (キャンバスのピクセル。右と下が正。kRosterOffsetMinPx の
	// 前のコメント)。既定の 0 は、ずらさない。
	bool roster_show = true;
	RosterAnchor roster_anchor = RosterAnchor::BottomRight;
	RosterDirection roster_direction = RosterDirection::Column;
	AvatarSide roster_avatar_side = AvatarSide::Right;
	double roster_gap_ratio = kDefaultRosterGapRatio;
	double roster_name_max_em = kDefaultRosterNameMaxEm;
	bool roster_show_bots = false;
	int roster_offset_x_px = 0;
	int roster_offset_y_px = 0;

	// 発言中の光のタイミング。
	int label_border_rise_ms = 150;
	int label_border_hold_ms = 200;
	int label_border_fade_ms = 150;

	// ダミーの動く線の 1 往復の時間 (2 秒以上の中から選んだ 4 秒)。ダミー映像の大きさとフレームレートのキーは持たない
	// (キャンバスの大きさと 60 fps で作る。sources/dummy_feed.h)。dummy_width、dummy_height、dummy_fps が前のバージョンの
	// settings.json にあっても、知らないキーとして読み飛ばす。
	int dummy_cycle_ms = 4000;

	// 録画ファイルの作成の進み具合が止まったとみなすまでの時間 (秒)。既定値の 120 は目安の値で、測って決めたものではない。
	// record::kDefaultFinalizeStallS と同じ値。
	int record_finalize_stall_s = 120;

	// 設定ウィンドウと、参加者の設定ウィンドウの大きさと位置 (QWidget::saveGeometry の base64。空なら、初めて
	// 開くときの大きさで開く)。ウィンドウを閉じたときに書く。アンインストールでは、settings.json と一緒に削除
	// される。
	std::string settings_window_geometry;
	std::string participants_window_geometry;
};

// 自分の枠に映す映像として、OBS のソースかシーンを選んでいるか。どちらも空なら「Discord の画面共有」(メインアカウントの
// ユーザーIDが空なら「(なし)」) を選んでいる。
inline bool self_frame_video_selected(const Settings &settings)
{
	return !settings.self_frame_source_uuid.empty() || !settings.self_frame_source_name.empty();
}

namespace detail {

// 整数のキーを 1 つ読む。キーがなければ *out を変えずに true を返す (なければ既定値)。整数でない (小数、文字列、
// 真偽値、null) か、lo から hi の外なら false を返し、理由を *error に入れる。hi は 0 以上であること (下の符号なしの
// 比較が、それを前提にしている)。
inline bool read_bounded_int(const nlohmann::json &j, const char *key, int lo, int hi, int *out, std::string *error)
{
	// 範囲の確認は下の 1 か所 (value < lo || value > hi) で行う。符号なしの分岐は、int64 に変換できる値に絞る
	// だけで、lo の側の判定は下に任せる。
	const auto it = j.find(key);
	if (it == j.end())
		return true;

	if (!it->is_number_integer()) {
		if (error)
			*error = std::string(key) + " の値が整数でない";
		return false;
	}

	const auto out_of_range = [&]() {
		if (error)
			*error = std::string(key) + " の値が範囲 (" + std::to_string(lo) + " から " +
				 std::to_string(hi) + ") の外";
		return false;
	};

	// 符号なしとして持っている値 (JSON の大きな正の数) は、int64 に変換する前に上限と比べて拒む。先に変換すると、
	// int64 の範囲を超える値が負の値になり、範囲の中に見えてしまう。
	std::int64_t value = 0;
	if (it->is_number_unsigned()) {
		const std::uint64_t raw = it->get<std::uint64_t>();
		if (raw > static_cast<std::uint64_t>(hi))
			return out_of_range();
		value = static_cast<std::int64_t>(raw);
	} else {
		value = it->get<std::int64_t>();
	}

	if (value < static_cast<std::int64_t>(lo) || value > static_cast<std::int64_t>(hi))
		return out_of_range();
	*out = static_cast<int>(value);
	return true;
}

// 決まった文字列のどれかを持つキーを読む表。read_choice と choice_name が使う。
template <typename T>
struct ChoiceName {
	const char *name;
	T value;
};

inline constexpr ChoiceName<SlotOrder> kSlotOrderNames[] = {
	{"name", SlotOrder::Name},
	{"added", SlotOrder::Added},
	// 「位置を固定」を外す前の設定の値。読むときだけ使い、名前順として扱う。書くときは choice_name が先頭の "name" を返す。
	{"fixed", SlotOrder::Name},
};
inline constexpr ChoiceName<ReceiveScope> kReceiveScopeNames[] = {
	{"everyone", ReceiveScope::Everyone},
	{"slots", ReceiveScope::Slots},
};
inline constexpr ChoiceName<DockTab> kDockTabNames[] = {
	{"people", DockTab::People},
	{"audio", DockTab::Audio},
	{"display", DockTab::Display},
	{"status", DockTab::Status},
};

// キーがなければ *out を変えない。文字列でないか、表にない文字列なら false を返し、理由を *error に入れる。
template <typename T, std::size_t N>
inline bool read_choice(const nlohmann::json &j, const char *key, const ChoiceName<T> (&choices)[N], T *out,
			std::string *error)
{
	const auto it = j.find(key);
	if (it == j.end())
		return true;
	if (it->is_string()) {
		const std::string value = it->get<std::string>();
		for (const ChoiceName<T> &choice : choices) {
			if (value == choice.name) {
				*out = choice.value;
				return true;
			}
		}
	}
	if (error)
		*error = std::string(key) + " の値が未知";
	return false;
}

// 値の書き方を返す。表の先頭を既定にする (列挙の値はどれも表にある)。
template <typename T, std::size_t N>
inline const char *choice_name(const ChoiceName<T> (&choices)[N], T value)
{
	for (const ChoiceName<T> &choice : choices) {
		if (choice.value == value)
			return choice.name;
	}
	return choices[0].name;
}

// 真偽値のキーを 1 つ読む。キーがなければ *out を変えずに true を返す (なければ既定値)。真偽値でなければ (整数の
// 0 と 1 や、文字列の "true" も含めて) false を返し、理由を *error に入れる。0 と 1 や文字列を真偽値として受け
// 入れないのは、read_bounded_int と同じく、形が違う設定は既定値に戻すためである。
inline bool read_bool(const nlohmann::json &j, const char *key, bool *out, std::string *error)
{
	const auto it = j.find(key);
	if (it == j.end())
		return true;

	if (!it->is_boolean()) {
		if (error)
			*error = std::string(key) + " の値が真偽値でない";
		return false;
	}
	*out = it->get<bool>();
	return true;
}

// 小数のキーを 1 つ読む。キーがなければ *out を変えずに true を返す。数でない (文字列、真偽値、null) か、lo から
// hi の外なら false を返す。整数で書かれた値も数として受け取る (`5` と `5.0` は同じ値。人が手で書くと整数に
// なりやすい)。
inline bool read_bounded_double(const nlohmann::json &j, const char *key, double lo, double hi, double *out,
				std::string *error)
{
	const auto it = j.find(key);
	if (it == j.end())
		return true;

	if (!it->is_number()) {
		if (error)
			*error = std::string(key) + " の値が数でない";
		return false;
	}
	const double value = it->get<double>();
	if (!(value >= lo && value <= hi)) {
		if (error)
			*error = std::string(key) + " の値が範囲 (" + std::to_string(lo) + " から " + std::to_string(hi) +
				 ") の外";
		return false;
	}
	*out = value;
	return true;
}

// 0.1 刻みの小数のキーを 1 つ読む。範囲の外の値と、刻みに合わない値なら false を返す。0.1 は 2 進数で正確に
// 表せないので、10 倍した値が整数から 1e-6 以内なら刻みに合うとみなす (2.9 を 10 倍すると 29.000000000000004 に
// なり、floor との比較では判定できない)。値は丸めずに、読んだとおり持つ。
inline bool read_bounded_tenth_step(const nlohmann::json &j, const char *key, double lo, double hi, double *out,
				    std::string *error)
{
	double value = *out;
	if (!read_bounded_double(j, key, lo, hi, &value, error))
		return false;
	const double scaled = value * 10.0;
	if (std::fabs(scaled - std::round(scaled)) > 1e-6) {
		if (error)
			*error = std::string(key) + " の値が 0.1 刻みでない";
		return false;
	}
	*out = value;
	return true;
}

// step 刻みの整数のキーを 1 つ読む。範囲の外の値と、lo から step ごとの刻みに合わない値なら false を返す (読むときは
// 丸めない)。
inline bool read_bounded_step_int(const nlohmann::json &j, const char *key, int lo, int hi, int step, int *out,
				  std::string *error)
{
	int value = *out;
	if (!read_bounded_int(j, key, lo, hi, &value, error))
		return false;
	if ((value - lo) % step != 0) {
		if (error)
			*error = std::string(key) + " の値が " + std::to_string(step) + " 刻みでない";
		return false;
	}
	*out = value;
	return true;
}

// 0.05 刻みの小数のキーを 1 つ読む。範囲の外の値と、刻みに合わない値なら false を返す。0.05 は 2 進数で正確に表せないので、
// 20 倍した値が整数から 1e-6 以内なら刻みに合うとみなす。値は丸めずに、読んだとおり持つ。
inline bool read_bounded_twentieth_step(const nlohmann::json &j, const char *key, double lo, double hi, double *out,
					std::string *error)
{
	double value = *out;
	if (!read_bounded_double(j, key, lo, hi, &value, error))
		return false;
	const double scaled = value * 20.0;
	if (std::fabs(scaled - std::round(scaled)) > 1e-6) {
		if (error)
			*error = std::string(key) + " の値が 0.05 刻みでない";
		return false;
	}
	*out = value;
	return true;
}

// 文字列で書く選択肢のキーを 1 つ読む。キーがなければ *out を変えずに true を返す。文字列でないか、choices のどれとも
// 同じでなければ false を返す。
template <typename E>
inline bool read_choice(const nlohmann::json &j, const char *key, std::initializer_list<std::pair<const char *, E>> choices,
			E *out, std::string *error)
{
	const auto it = j.find(key);
	if (it == j.end())
		return true;
	if (!it->is_string()) {
		if (error)
			*error = std::string(key) + " の値が文字列でない";
		return false;
	}
	const std::string value = it->get<std::string>();
	for (const auto &choice : choices) {
		if (value == choice.first) {
			*out = choice.second;
			return true;
		}
	}
	if (error) {
		std::string names;
		for (const auto &choice : choices)
			names += (names.empty() ? "" : " / ") + std::string(choice.first);
		*error = std::string(key) + " の値が未知 (" + names + " のどれでもない)";
	}
	return false;
}

// `#RRGGBB` の形か (7 文字。16 進数の大文字と小文字のどちらも受け付ける)。
inline bool is_hex_color(const std::string &text)
{
	if (text.size() != 7 || text[0] != '#')
		return false;
	for (std::size_t i = 1; i < text.size(); ++i) {
		const char c = text[i];
		const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
		if (!hex)
			return false;
	}
	return true;
}

// 色のキーを 1 つ読む。キーがなければ *out を変えずに true を返す。文字列でないか、`#RRGGBB` の形でなければ false
// を返す。書かれた文字 (大文字か小文字か) はそのまま持つ (利用者が書いた値を、メッセージもログも出さずに変えない)。
inline bool read_color(const nlohmann::json &j, const char *key, std::string *out, std::string *error)
{
	const auto it = j.find(key);
	if (it == j.end())
		return true;
	if (!it->is_string()) {
		if (error)
			*error = std::string(key) + " の値が文字列でない";
		return false;
	}
	const std::string value = it->get<std::string>();
	if (!is_hex_color(value)) {
		if (error)
			*error = std::string(key) + " の値が #RRGGBB の形でない";
		return false;
	}
	*out = value;
	return true;
}

// 形を決めない文字列のキーを 1 つ読む (ウィンドウの大きさと位置の base64 など)。キーがなければ *out を変えずに
// true を返す。文字列でなければ false を返す。
inline bool read_string(const nlohmann::json &j, const char *key, std::string *out, std::string *error)
{
	const auto it = j.find(key);
	if (it == j.end())
		return true;
	if (!it->is_string()) {
		if (error)
			*error = std::string(key) + " の値が文字列でない";
		return false;
	}
	*out = it->get<std::string>();
	return true;
}

} // namespace detail

// json_text を解析して *out に書く。成功したら (キーがないか、どの値も正しい形なら) true を返す。形が違えば
// (JSON として読めない、オブジェクトでない、どれかのキーの値の型か中身が正しくない) false を返し、*out を既定値
// (Settings{}) に戻す。out と error はどちらも nullptr でよい。error は人が読む理由で、トークンやポート番号を扱う
// 設定ではないので、ログに書く前に隠す処理は必要ない。
inline bool parse_settings(std::string_view json_text, Settings *out, std::string *error)
{
	if (out)
		*out = Settings{};

	const nlohmann::json j = nlohmann::json::parse(json_text, nullptr, /*allow_exceptions=*/false);
	if (j.is_discarded()) {
		if (error)
			*error = "壊れた JSON";
		return false;
	}
	if (!j.is_object()) {
		if (error)
			*error = "JSON がオブジェクトでない";
		return false;
	}

	// すべてのキーを読み終えるまで *out には書かない (途中で形の違う値が見つかったときに、手前のキーだけが動作した
	// 設定を残さないため。このファイルの先頭のコメント)。
	Settings parsed{};

	const auto decoder_it = j.find("decoder");
	if (decoder_it != j.end()) {
		// 未知のキーは、ここまでで無視している (find で探さなかっただけ)。
		if (!decoder_it->is_string()) {
			if (error)
				*error = "decoder の値が文字列でない";
			return false;
		}
		const std::string v = decoder_it->get<std::string>();
		if (v == "software") {
			parsed.decoder = DecoderChoice::Software;
		} else if (v == "hardware") {
			parsed.decoder = DecoderChoice::D3D11VA;
		} else {
			if (error)
				*error = "decoder の値が未知 (software / hardware のどちらでもない)";
			return false;
		}
	}

	const auto encoder_preference_it = j.find("encoder_preference");
	if (encoder_preference_it != j.end()) {
		if (!encoder_preference_it->is_string()) {
			if (error)
				*error = "encoder_preference の値が文字列でない";
			return false;
		}
		const std::string v = encoder_preference_it->get<std::string>();
		if (v == "hardware") {
			parsed.encoder_preference = EncoderPreference::Hardware;
		} else if (v == "software") {
			parsed.encoder_preference = EncoderPreference::Software;
		} else {
			if (error)
				*error = "encoder_preference の値が未知 (hardware / software のどちらでもない)";
			return false;
		}
	}

	const auto encode_priority_it = j.find("encode_priority");
	if (encode_priority_it != j.end()) {
		if (!encode_priority_it->is_string()) {
			if (error)
				*error = "encode_priority の値が文字列でない";
			return false;
		}
		const std::string v = encode_priority_it->get<std::string>();
		if (v == "speed") {
			parsed.encode_priority = EncodePriority::Speed;
		} else if (v == "quality") {
			parsed.encode_priority = EncodePriority::Quality;
		} else {
			if (error)
				*error = "encode_priority の値が未知 (speed / quality のどちらでもない)";
			return false;
		}
	}

	const auto assign_mode_it = j.find("assign_mode");
	if (assign_mode_it != j.end()) {
		if (!assign_mode_it->is_string()) {
			if (error)
				*error = "assign_mode の値が文字列でない";
			return false;
		}
		const std::string v = assign_mode_it->get<std::string>();
		if (v == "manual") {
			parsed.assign_mode = AssignMode::Manual;
		} else if (v == "auto") {
			parsed.assign_mode = AssignMode::Auto;
		} else {
			if (error)
				*error = "assign_mode の値が未知 (manual / auto のどちらでもない)";
			return false;
		}
	}

	if (!detail::read_choice(j, "slot_order", detail::kSlotOrderNames, &parsed.slot_order, error))
		return false;
	if (!detail::read_bool(j, "record_with_stream", &parsed.record_with_stream, error))
		return false;
	if (!detail::read_bool(j, "confirm_while_streaming", &parsed.confirm_while_streaming, error))
		return false;
	if (!detail::read_choice(j, "receive_scope", detail::kReceiveScopeNames, &parsed.receive_scope, error))
		return false;
	if (!detail::read_bounded_int(j, "layout_hold_idle_ms", kLayoutHoldIdleMinMs, kLayoutHoldIdleMaxMs,
				      &parsed.layout_hold_idle_ms, error))
		return false;
	if (!detail::read_choice(j, "dock_tab", detail::kDockTabNames, &parsed.dock_tab, error))
		return false;

	const auto record_pcm_it = j.find("record_output_pcm");
	if (record_pcm_it != j.end()) {
		if (!record_pcm_it->is_string()) {
			if (error)
				*error = "record_output_pcm の値が文字列でない";
			return false;
		}
		const std::string v = record_pcm_it->get<std::string>();
		if (v == "fl32") {
			parsed.record_output_pcm = RecordPcmFormat::Float32;
		} else if (v == "in24") {
			parsed.record_output_pcm = RecordPcmFormat::Int24;
		} else {
			if (error)
				*error = "record_output_pcm の値が未知 (fl32 / in24 のどちらでもない)";
			return false;
		}
	}

	if (!detail::read_bounded_int(j, "screen_audio_offset_ms", kScreenAudioOffsetMinMs, kScreenAudioOffsetMaxMs,
				      &parsed.screen_audio_offset_ms, error))
		return false;
	if (!detail::read_bounded_int(j, "voice_audio_delay_ms", kVoiceAudioDelayMinMs, kVoiceAudioDelayMaxMs,
				      &parsed.voice_audio_delay_ms, error))
		return false;
	if (!detail::read_bounded_int(j, "js_log_ring_lines", kJsLogRingMinLines, kJsLogRingMaxLines,
				      &parsed.js_log_ring_lines, error))
		return false;
	if (!detail::read_bounded_int(j, "js_event_log_lines", kEventLogMinLines, kEventLogMaxLines,
				      &parsed.js_event_log_lines, error))
		return false;
	if (!detail::read_bounded_int(j, "dock_poll_ms", kDockPollMinMs, kDockPollMaxMs, &parsed.dock_poll_ms, error))
		return false;
	if (!detail::read_bounded_int(j, "video_stall_ms", kVideoStallMinMs, kVideoStallMaxMs, &parsed.video_stall_ms,
				      error))
		return false;
	if (!detail::read_bounded_int(j, "seen_people_max", kSeenPeopleMaxMin, kSeenPeopleMaxLimit,
				      &parsed.seen_people_max, error))
		return false;
	if (!detail::read_bounded_int(j, "plugin_log_ring_lines", kPluginLogRingMinLines, kPluginLogRingMaxLines,
				      &parsed.plugin_log_ring_lines, error))
		return false;
	if (!detail::read_bounded_int(j, "plugin_event_log_lines", kEventLogMinLines, kEventLogMaxLines,
				      &parsed.plugin_event_log_lines, error))
		return false;
	if (!detail::read_bounded_int(j, "record_space_check_interval_s", kRecordSpaceCheckIntervalMinS,
				      kRecordSpaceCheckIntervalMaxS, &parsed.record_space_check_interval_s, error))
		return false;
	if (!detail::read_bounded_int(j, "record_space_warn_minutes", kRecordSpaceWarnMinMinutes,
				      kRecordSpaceWarnMaxMinutes, &parsed.record_space_warn_minutes, error))
		return false;
	if (!detail::read_bounded_int(j, "audio_resync_ms", kAudioResyncMinMs, kAudioResyncMaxMs,
				      &parsed.audio_resync_ms, error))
		return false;
	if (!detail::read_bounded_int(j, "audio_drift_max_ppm", kAudioDriftMaxPpmMin, kAudioDriftMaxPpmMax,
				      &parsed.audio_drift_max_ppm, error))
		return false;
	if (!detail::read_bounded_int(j, "audio_drift_time_constant_ms", kAudioDriftTimeConstantMinMs,
				      kAudioDriftTimeConstantMaxMs, &parsed.audio_drift_time_constant_ms, error))
		return false;
	if (!detail::read_bounded_int(j, "voice_audio_resync_ms", kVoiceAudioResyncMinMs, kVoiceAudioResyncMaxMs,
				      &parsed.voice_audio_resync_ms, error))
		return false;
	if (!detail::read_bounded_int(j, "record_finalize_wait_on_exit_s", kRecordFinalizeWaitMinS,
				      kRecordFinalizeWaitMaxS, &parsed.record_finalize_wait_on_exit_s, error))
		return false;
	if (!detail::read_bounded_int(j, "record_queue_limit", kRecordQueueLimitMin, kRecordQueueLimitMax,
				      &parsed.record_queue_limit, error))
		return false;
	if (!detail::read_bounded_int(j, "record_clock_push_max_frames", kRecordClockPushMaxFramesMin,
				      kRecordClockPushMaxFramesMax, &parsed.record_clock_push_max_frames, error))
		return false;
	if (!detail::read_bounded_int(j, "record_clock_reanchor_max_ms", kRecordClockReanchorMinMs,
				      kRecordClockReanchorMaxMs, &parsed.record_clock_reanchor_max_ms, error))
		return false;
	// 長い辺の上限は「0 (制限なし) か 16 以上」の 2 つの範囲を持つので、まず 0 から上限までを読み、間の 1 から
	// 15 は別に拒む (上の定数のコメント)。
	if (!detail::read_bounded_int(j, "limit_max_long_edge", kLimitMaxLongEdgeNone, kLimitMaxLongEdgeMax,
				      &parsed.limit_max_long_edge, error))
		return false;
	if (parsed.limit_max_long_edge != kLimitMaxLongEdgeNone && parsed.limit_max_long_edge < kLimitMaxLongEdgeMin) {
		if (error)
			*error = "limit_max_long_edge is neither 0 nor >= " + std::to_string(kLimitMaxLongEdgeMin);
		return false;
	}
	if (!detail::read_bounded_int(j, "limit_fps_divisor", kLimitFpsDivisorMin, kLimitFpsDivisorMax,
				      &parsed.limit_fps_divisor, error))
		return false;
	if (!detail::read_bounded_int(j, "vencord_check_disconnected_s", kVencordCheckIntervalMinS,
				      kVencordCheckIntervalMaxS, &parsed.vencord_check_disconnected_s, error))
		return false;
	if (!detail::read_bounded_int(j, "vencord_check_connected_s", kVencordCheckIntervalMinS,
				      kVencordCheckIntervalMaxS, &parsed.vencord_check_connected_s, error))
		return false;

	// メインアカウントのユーザー ID は、利用者が入れた userId の文字列で、範囲の確認はしない (Discord の値なので、
	// cordmux が形を決めない)。文字列でないときだけ、形が違うとみなす。
	const auto broadcaster_it = j.find("broadcaster_user_id");
	if (broadcaster_it != j.end()) {
		if (!broadcaster_it->is_string()) {
			if (error)
				*error = "broadcaster_user_id の値が文字列でない";
			return false;
		}
		parsed.broadcaster_user_id = broadcaster_it->get<std::string>();
	}
	if (!detail::read_string(j, "self_frame_source_uuid", &parsed.self_frame_source_uuid, error))
		return false;
	if (!detail::read_string(j, "self_frame_source_name", &parsed.self_frame_source_name, error))
		return false;
	if (!detail::read_bool(j, "self_frame_on", &parsed.self_frame_on, error))
		return false;
	if (!detail::read_bool(j, "self_slot_first", &parsed.self_slot_first, error))
		return false;

	// 保存先も、利用者が入れたパスの文字列で、範囲の確認はしない (broadcaster_user_id と同じ扱い)。意味を持つのは
	// 空かどうかだけである (空なら既定の保存先を使う)。書き込める場所かどうかは、実際に開くまで分からないので、
	// ここでは判定しない。
	const auto record_dir_it = j.find("record_dir");
	if (record_dir_it != j.end()) {
		if (!record_dir_it->is_string()) {
			if (error)
				*error = "record_dir の値が文字列でない";
			return false;
		}
		parsed.record_dir = record_dir_it->get<std::string>();
	}
	if (!detail::read_bool(j, "allow_state_json_write", &parsed.allow_state_json_write, error))
		return false;
	if (!detail::read_bool(j, "feature_enabled", &parsed.feature_enabled, error))
		return false;
	if (!detail::read_bool(j, "self_voice_muted", &parsed.self_voice_muted, error))
		return false;

	// ---- メッセージ、名前ラベル、話している人の光、ダミー、録画ファイルの作成、ウィンドウのキー。読み方はほかのキーと同じ ----
	if (!detail::read_bounded_int(j, "notice_show_delay_ms", kNoticeDelayMinMs, kNoticeDelayMaxMs,
				      &parsed.notice_show_delay_ms, error))
		return false;
	if (!detail::read_bounded_int(j, "notice_clear_delay_ms", kNoticeDelayMinMs, kNoticeDelayMaxMs,
				      &parsed.notice_clear_delay_ms, error))
		return false;
	if (!detail::read_bounded_int(j, "first_frame_wait_ms", kFirstFrameWaitMinMs, kFirstFrameWaitMaxMs,
				      &parsed.first_frame_wait_ms, error))
		return false;
	if (!detail::read_bounded_int(j, "receive_restart_wait_ms", kReceiveRestartWaitMinMs, kReceiveRestartWaitMaxMs,
				      &parsed.receive_restart_wait_ms, error))
		return false;
	if (!detail::read_bounded_int(j, "link_lost_hold_ms", kLinkLostHoldMinMs, kLinkLostHoldMaxMs,
				      &parsed.link_lost_hold_ms, error))
		return false;
	if (!detail::read_bounded_int(j, "reconnect_layout_hold_ms", kReconnectLayoutHoldMinMs,
				      kReconnectLayoutHoldMaxMs, &parsed.reconnect_layout_hold_ms, error))
		return false;
	if (!detail::read_bounded_int(j, "call_action_timeout_ms", kCallActionTimeoutMinMs, kCallActionTimeoutMaxMs,
				      &parsed.call_action_timeout_ms, error))
		return false;
	if (!detail::read_bounded_int(j, "display_buffer_ms", kDisplayBufferMinMs, kDisplayBufferMaxMs,
				      &parsed.display_buffer_ms, error))
		return false;
	if (!detail::read_bounded_int(j, "tile_gap_px", kTileGapMinPx, kTileGapMaxPx, &parsed.tile_gap_px, error))
		return false;
	if (!detail::read_bool(j, "slot_background_show", &parsed.slot_background_show, error))
		return false;
	if (!detail::read_color(j, "slot_background_color", &parsed.slot_background_color, error))
		return false;
	if (!detail::read_bounded_int(j, "slot_background_opacity_pct", kSlotBackgroundOpacityMinPct,
				      kSlotBackgroundOpacityMaxPct, &parsed.slot_background_opacity_pct, error))
		return false;
	if (!detail::read_bounded_tenth_step(j, "label_em_ratio_pct", kLabelEmRatioMinPct, kLabelEmRatioMaxPct,
					     &parsed.label_em_ratio_pct, error))
		return false;
	if (!detail::read_bool(j, "label_show_name", &parsed.label_show_name, error))
		return false;
	if (!detail::read_bool(j, "label_show_avatar", &parsed.label_show_avatar, error))
		return false;
	if (!detail::read_bool(j, "label_show_speaking", &parsed.label_show_speaking, error))
		return false;
	if (!detail::read_color(j, "label_border_color", &parsed.label_border_color, error))
		return false;
	if (!detail::read_color(j, "label_plate_color", &parsed.label_plate_color, error))
		return false;
	if (!detail::read_bounded_int(j, "label_plate_opacity_pct", kLabelPlateOpacityMinPct, kLabelPlateOpacityMaxPct,
				      &parsed.label_plate_opacity_pct, error))
		return false;
	if (!detail::read_color(j, "label_text_color", &parsed.label_text_color, error))
		return false;
	if (!detail::read_string(j, "label_font_family", &parsed.label_font_family, error))
		return false;
	if (!detail::read_bounded_int(j, "label_font_weight", kLabelFontWeightMin, kLabelFontWeightMax,
				      &parsed.label_font_weight, error))
		return false;
	if (!detail::read_bounded_int(j, "label_border_rise_ms", kLabelBorderTimingMinMs, kLabelBorderTimingMaxMs,
				      &parsed.label_border_rise_ms, error))
		return false;
	if (!detail::read_bounded_int(j, "label_border_hold_ms", kLabelBorderTimingMinMs, kLabelBorderTimingMaxMs,
				      &parsed.label_border_hold_ms, error))
		return false;
	if (!detail::read_bounded_int(j, "label_border_fade_ms", kLabelBorderTimingMinMs, kLabelBorderTimingMaxMs,
				      &parsed.label_border_fade_ms, error))
		return false;
	if (!detail::read_bounded_double(j, "label_margin_pct", kLabelMarginMinPct, kLabelMarginMaxPct,
					 &parsed.label_margin_pct, error))
		return false;
	if (!detail::read_color(j, "label_outline_color", &parsed.label_outline_color, error))
		return false;
	if (!detail::read_bounded_double(j, "label_outline_ratio", kLabelOutlineRatioMin, kLabelOutlineRatioMax,
					 &parsed.label_outline_ratio, error))
		return false;
	if (!detail::read_bool(j, "label_show_outline", &parsed.label_show_outline, error))
		return false;
	if (!detail::read_bounded_int(j, "label_opacity_pct", kLabelOpacityMinPct, kLabelOpacityMaxPct,
				      &parsed.label_opacity_pct, error))
		return false;
	if (!detail::read_bool(j, "label_show_plate", &parsed.label_show_plate, error))
		return false;
	if (!detail::read_bounded_double(j, "label_avatar_ratio", kLabelAvatarRatioMin, kLabelAvatarRatioMax,
					 &parsed.label_avatar_ratio, error))
		return false;
	if (!detail::read_bool(j, "label_hide_when_single", &parsed.label_hide_when_single, error))
		return false;
	if (!detail::read_bool(j, "label_show_in_slot", &parsed.label_show_in_slot, error))
		return false;
	if (!detail::read_bool(j, "label_glow_frame", &parsed.label_glow_frame, error))
		return false;
	if (!detail::read_bool(j, "label_glow_avatar", &parsed.label_glow_avatar, error))
		return false;
	if (!detail::read_bool(j, "label_glow_name", &parsed.label_glow_name, error))
		return false;
	if (!detail::read_bounded_double(j, "label_glow_blur_ratio", kLabelGlowBlurRatioMin, kLabelGlowBlurRatioMax,
					 &parsed.label_glow_blur_ratio, error))
		return false;
	if (!detail::read_bounded_double(j, "label_glow_spread_ratio", kLabelGlowSpreadRatioMin,
					 kLabelGlowSpreadRatioMax, &parsed.label_glow_spread_ratio, error))
		return false;
	if (!detail::read_choice<LabelCorner>(j, "label_corner",
					      {{"tl", LabelCorner::TopLeft},
					       {"tr", LabelCorner::TopRight},
					       {"bl", LabelCorner::BottomLeft},
					       {"br", LabelCorner::BottomRight}},
					      &parsed.label_corner, error))
		return false;
	if (!detail::read_choice<AvatarSide>(
		    j, "label_avatar_side", {{"left", AvatarSide::Left}, {"right", AvatarSide::Right}},
		    &parsed.label_avatar_side, error))
		return false;
	if (!detail::read_choice<AvatarShape>(j, "label_avatar_shape",
					      {{"circle", AvatarShape::Circle},
					       {"rounded", AvatarShape::Rounded},
					       {"square", AvatarShape::Square}},
					      &parsed.label_avatar_shape, error))
		return false;
	if (!detail::read_bounded_step_int(j, "label_dim_pct", kLabelDimMinPct, kLabelDimMaxPct, kLabelDimStepPct,
					   &parsed.label_dim_pct, error))
		return false;
	if (!detail::read_string(j, "self_speaking_source_uuid", &parsed.self_speaking_source_uuid, error))
		return false;
	if (!detail::read_string(j, "self_speaking_source_name", &parsed.self_speaking_source_name, error))
		return false;
	if (!detail::read_bounded_int(j, "self_speaking_threshold_db", kSelfSpeakingThresholdMinDb,
				      kSelfSpeakingThresholdMaxDb, &parsed.self_speaking_threshold_db, error))
		return false;
	if (!detail::read_bool(j, "roster_show", &parsed.roster_show, error))
		return false;
	if (!detail::read_choice<RosterAnchor>(j, "roster_anchor",
					       {{"tl", RosterAnchor::TopLeft},
						{"tc", RosterAnchor::TopCenter},
						{"tr", RosterAnchor::TopRight},
						{"ml", RosterAnchor::MiddleLeft},
						{"mr", RosterAnchor::MiddleRight},
						{"bl", RosterAnchor::BottomLeft},
						{"bc", RosterAnchor::BottomCenter},
						{"br", RosterAnchor::BottomRight}},
					       &parsed.roster_anchor, error))
		return false;
	if (!detail::read_choice<RosterDirection>(
		    j, "roster_direction", {{"row", RosterDirection::Row}, {"column", RosterDirection::Column}},
		    &parsed.roster_direction, error))
		return false;
	// 位置と向きの合わない組は、位置に合う向きに修正する (形の違う値ではなく、人が書き間違えやすい組み合わせなので、
	// 設定をすべて既定値に戻さない)。
	parsed.roster_direction = roster_direction_for(parsed.roster_anchor, parsed.roster_direction);
	if (!detail::read_choice<AvatarSide>(
		    j, "roster_avatar_side", {{"left", AvatarSide::Left}, {"right", AvatarSide::Right}},
		    &parsed.roster_avatar_side, error))
		return false;
	if (!detail::read_bounded_twentieth_step(j, "roster_gap_ratio", kRosterGapRatioMin, kRosterGapRatioMax,
						 &parsed.roster_gap_ratio, error))
		return false;
	if (!detail::read_bounded_tenth_step(j, "roster_name_max_em", kRosterNameMaxEmMin, kRosterNameMaxEmMax,
					     &parsed.roster_name_max_em, error))
		return false;
	if (!detail::read_bool(j, "roster_show_bots", &parsed.roster_show_bots, error))
		return false;
	if (!detail::read_bounded_int(j, "roster_offset_x_px", kRosterOffsetMinPx, kRosterOffsetMaxPx,
				      &parsed.roster_offset_x_px, error))
		return false;
	if (!detail::read_bounded_int(j, "roster_offset_y_px", kRosterOffsetMinPx, kRosterOffsetMaxPx,
				      &parsed.roster_offset_y_px, error))
		return false;

	if (!detail::read_bounded_int(j, "dummy_cycle_ms", kDummyCycleMinMs, kDummyCycleMaxMs, &parsed.dummy_cycle_ms,
				      error))
		return false;
	if (!detail::read_bounded_int(j, "record_finalize_stall_s", kRecordFinalizeStallMinS, kRecordFinalizeStallMaxS,
				      &parsed.record_finalize_stall_s, error))
		return false;
	if (!detail::read_string(j, "settings_window_geometry", &parsed.settings_window_geometry, error))
		return false;
	if (!detail::read_string(j, "participants_window_geometry", &parsed.participants_window_geometry, error))
		return false;

	if (out)
		*out = parsed;
	return true;
}

// 現在の設定を settings.json の中身にする (書くのはドック)。
//
// parse_settings が読むキーをすべて書く (書いた文字列をもう一度読むと、同じ Settings に戻る)。
//
// トークンもポート番号も入らない (settings.json はそれらを持たないので、隠す処理は必要ない)。
inline std::string settings_to_json(const Settings &settings)
{
	nlohmann::json j = nlohmann::json::object();
	j["decoder"] = settings.decoder == DecoderChoice::D3D11VA ? "hardware" : "software";
	j["screen_audio_offset_ms"] = settings.screen_audio_offset_ms;
	j["voice_audio_delay_ms"] = settings.voice_audio_delay_ms;
	j["js_log_ring_lines"] = settings.js_log_ring_lines;
	j["js_event_log_lines"] = settings.js_event_log_lines;
	j["plugin_log_ring_lines"] = settings.plugin_log_ring_lines;
	j["plugin_event_log_lines"] = settings.plugin_event_log_lines;
	j["dock_poll_ms"] = settings.dock_poll_ms;
	j["video_stall_ms"] = settings.video_stall_ms;
	j["assign_mode"] = settings.assign_mode == AssignMode::Auto ? "auto" : "manual";
	j["slot_order"] = detail::choice_name(detail::kSlotOrderNames, settings.slot_order);
	j["record_with_stream"] = settings.record_with_stream;
	j["confirm_while_streaming"] = settings.confirm_while_streaming;
	j["receive_scope"] = detail::choice_name(detail::kReceiveScopeNames, settings.receive_scope);
	j["layout_hold_idle_ms"] = settings.layout_hold_idle_ms;
	j["dock_tab"] = detail::choice_name(detail::kDockTabNames, settings.dock_tab);
	j["seen_people_max"] = settings.seen_people_max;
	j["broadcaster_user_id"] = settings.broadcaster_user_id;
	j["self_frame_source_uuid"] = settings.self_frame_source_uuid;
	j["self_frame_source_name"] = settings.self_frame_source_name;
	j["self_frame_on"] = settings.self_frame_on;
	j["self_slot_first"] = settings.self_slot_first;
	j["allow_state_json_write"] = settings.allow_state_json_write;
	j["record_dir"] = settings.record_dir;
	j["record_output_pcm"] = settings.record_output_pcm == RecordPcmFormat::Int24 ? "in24" : "fl32";
	j["record_space_check_interval_s"] = settings.record_space_check_interval_s;
	j["record_space_warn_minutes"] = settings.record_space_warn_minutes;
	j["audio_resync_ms"] = settings.audio_resync_ms;
	j["audio_drift_max_ppm"] = settings.audio_drift_max_ppm;
	j["audio_drift_time_constant_ms"] = settings.audio_drift_time_constant_ms;
	j["voice_audio_resync_ms"] = settings.voice_audio_resync_ms;
	j["record_finalize_wait_on_exit_s"] = settings.record_finalize_wait_on_exit_s;
	j["record_queue_limit"] = settings.record_queue_limit;
	j["record_clock_push_max_frames"] = settings.record_clock_push_max_frames;
	j["record_clock_reanchor_max_ms"] = settings.record_clock_reanchor_max_ms;
	j["feature_enabled"] = settings.feature_enabled;
	j["self_voice_muted"] = settings.self_voice_muted;
	j["limit_max_long_edge"] = settings.limit_max_long_edge;
	j["limit_fps_divisor"] = settings.limit_fps_divisor;
	j["encoder_preference"] = settings.encoder_preference == EncoderPreference::Software ? "software" : "hardware";
	j["encode_priority"] = settings.encode_priority == EncodePriority::Quality ? "quality" : "speed";
	j["vencord_check_disconnected_s"] = settings.vencord_check_disconnected_s;
	j["vencord_check_connected_s"] = settings.vencord_check_connected_s;
	j["notice_show_delay_ms"] = settings.notice_show_delay_ms;
	j["notice_clear_delay_ms"] = settings.notice_clear_delay_ms;
	j["first_frame_wait_ms"] = settings.first_frame_wait_ms;
	j["receive_restart_wait_ms"] = settings.receive_restart_wait_ms;
	j["link_lost_hold_ms"] = settings.link_lost_hold_ms;
	j["reconnect_layout_hold_ms"] = settings.reconnect_layout_hold_ms;
	j["call_action_timeout_ms"] = settings.call_action_timeout_ms;
	j["display_buffer_ms"] = settings.display_buffer_ms;
	j["tile_gap_px"] = settings.tile_gap_px;
	j["slot_background_show"] = settings.slot_background_show;
	j["slot_background_color"] = settings.slot_background_color;
	j["slot_background_opacity_pct"] = settings.slot_background_opacity_pct;
	j["label_em_ratio_pct"] = settings.label_em_ratio_pct;
	j["label_show_name"] = settings.label_show_name;
	j["label_show_avatar"] = settings.label_show_avatar;
	j["label_show_speaking"] = settings.label_show_speaking;
	j["label_border_color"] = settings.label_border_color;
	j["label_plate_color"] = settings.label_plate_color;
	j["label_plate_opacity_pct"] = settings.label_plate_opacity_pct;
	j["label_text_color"] = settings.label_text_color;
	j["label_font_family"] = settings.label_font_family;
	j["label_font_weight"] = settings.label_font_weight;
	j["label_border_rise_ms"] = settings.label_border_rise_ms;
	j["label_border_hold_ms"] = settings.label_border_hold_ms;
	j["label_border_fade_ms"] = settings.label_border_fade_ms;
	j["label_margin_pct"] = settings.label_margin_pct;
	j["label_outline_color"] = settings.label_outline_color;
	j["label_outline_ratio"] = settings.label_outline_ratio;
	j["label_show_outline"] = settings.label_show_outline;
	j["label_opacity_pct"] = settings.label_opacity_pct;
	j["label_show_plate"] = settings.label_show_plate;
	j["label_avatar_ratio"] = settings.label_avatar_ratio;
	j["label_hide_when_single"] = settings.label_hide_when_single;
	j["label_show_in_slot"] = settings.label_show_in_slot;
	j["label_glow_frame"] = settings.label_glow_frame;
	j["label_glow_avatar"] = settings.label_glow_avatar;
	j["label_glow_name"] = settings.label_glow_name;
	j["label_glow_blur_ratio"] = settings.label_glow_blur_ratio;
	j["label_glow_spread_ratio"] = settings.label_glow_spread_ratio;
	{
		static const char *const corner_names[] = {"tl", "tr", "bl", "br"};
		j["label_corner"] = corner_names[static_cast<int>(settings.label_corner)];
	}
	j["label_avatar_side"] = settings.label_avatar_side == AvatarSide::Right ? "right" : "left";
	{
		static const char *const shape_names[] = {"circle", "rounded", "square"};
		j["label_avatar_shape"] = shape_names[static_cast<int>(settings.label_avatar_shape)];
	}
	j["label_dim_pct"] = settings.label_dim_pct;
	j["self_speaking_source_uuid"] = settings.self_speaking_source_uuid;
	j["self_speaking_source_name"] = settings.self_speaking_source_name;
	j["self_speaking_threshold_db"] = settings.self_speaking_threshold_db;
	j["roster_show"] = settings.roster_show;
	{
		static const char *const anchor_names[] = {"tl", "tc", "tr", "ml", "mr", "bl", "bc", "br"};
		j["roster_anchor"] = anchor_names[static_cast<int>(settings.roster_anchor)];
	}
	j["roster_direction"] = settings.roster_direction == RosterDirection::Column ? "column" : "row";
	j["roster_avatar_side"] = settings.roster_avatar_side == AvatarSide::Right ? "right" : "left";
	j["roster_gap_ratio"] = settings.roster_gap_ratio;
	j["roster_name_max_em"] = settings.roster_name_max_em;
	j["roster_show_bots"] = settings.roster_show_bots;
	j["roster_offset_x_px"] = settings.roster_offset_x_px;
	j["roster_offset_y_px"] = settings.roster_offset_y_px;
	j["dummy_cycle_ms"] = settings.dummy_cycle_ms;
	j["record_finalize_stall_s"] = settings.record_finalize_stall_s;
	j["settings_window_geometry"] = settings.settings_window_geometry;
	j["participants_window_geometry"] = settings.participants_window_geometry;
	return j.dump(2) + "\n";
}

// 設定ウィンドウから入る値を、最後に範囲に収める純粋関数。範囲の外の値は範囲の端に、刻みに合わない値は近い
// 刻みに丸め、形の違う色は既定の色に戻す。ウィンドウが返した値を dock/settings_fields.h の
// apply_settings_page_fields に渡す前に通す。ウィンドウの入力欄自体が範囲と刻みを持つので、普通は何も変えない
// (変えたら、入力欄の範囲がこの関数とずれている。ウィンドウの側の誤り)。ファイルを読むときには使わない。読む
// ときは、形の違う値が 1 つでもあれば、すべてを既定値に戻す (parse_settings。利用者が書いた値を、メッセージもログも
// 出さずに別の値にしない)。
//
//   - limit_max_long_edge の 1 から 15 は 16 に上げる。近いほうの 0 に下げると、0 は制限なしなので意味が逆になる
//     (小さい上限を選んだ人に、上限のない映像を送らせる)。
//   - 色の形が違えば、既定の色に戻す (形の違う文字列には、近い色がない)。
//   - 小数の NaN は既定値に戻す (NaN には大小がなく、どちらかの端にする根拠がない)。
//   - 画面共有していない参加者の位置に合わない向きは、位置に合う向きに修正する (roster_direction_for)。
//   範囲を持たない文字列 (メインアカウントのユーザー ID、保存先、ウィンドウの大きさと位置、名前ラベルのフォントの
//   名前) と、上の向き以外の列挙は変えない。
inline void clamp_settings(Settings &s)
{
	const Settings defaults{};
	const auto clamp_int = [](int &v, int lo, int hi) {
		if (v < lo)
			v = lo;
		else if (v > hi)
			v = hi;
	};
	const auto clamp_double = [](double &v, double lo, double hi, double fallback) {
		if (std::isnan(v))
			v = fallback;
		if (v < lo)
			v = lo;
		else if (v > hi)
			v = hi;
	};
	const auto clamp_tenth_step = [&clamp_double](double &v, double lo, double hi, double fallback) {
		clamp_double(v, lo, hi, fallback);
		v = std::round(v * 10.0) / 10.0;
		// 範囲の端が 0.1 刻みなので、丸めた後も範囲の中にある。
	};
	const auto fix_color = [](std::string &v, const char *fallback) {
		if (!detail::is_hex_color(v))
			v = fallback;
	};

	clamp_int(s.screen_audio_offset_ms, kScreenAudioOffsetMinMs, kScreenAudioOffsetMaxMs);
	clamp_int(s.voice_audio_delay_ms, kVoiceAudioDelayMinMs, kVoiceAudioDelayMaxMs);
	clamp_int(s.js_log_ring_lines, kJsLogRingMinLines, kJsLogRingMaxLines);
	clamp_int(s.js_event_log_lines, kEventLogMinLines, kEventLogMaxLines);
	clamp_int(s.dock_poll_ms, kDockPollMinMs, kDockPollMaxMs);
	clamp_int(s.video_stall_ms, kVideoStallMinMs, kVideoStallMaxMs);
	clamp_int(s.seen_people_max, kSeenPeopleMaxMin, kSeenPeopleMaxLimit);
	clamp_int(s.plugin_log_ring_lines, kPluginLogRingMinLines, kPluginLogRingMaxLines);
	clamp_int(s.plugin_event_log_lines, kEventLogMinLines, kEventLogMaxLines);
	clamp_int(s.record_space_check_interval_s, kRecordSpaceCheckIntervalMinS, kRecordSpaceCheckIntervalMaxS);
	clamp_int(s.record_space_warn_minutes, kRecordSpaceWarnMinMinutes, kRecordSpaceWarnMaxMinutes);
	clamp_int(s.audio_resync_ms, kAudioResyncMinMs, kAudioResyncMaxMs);
	clamp_int(s.audio_drift_max_ppm, kAudioDriftMaxPpmMin, kAudioDriftMaxPpmMax);
	clamp_int(s.audio_drift_time_constant_ms, kAudioDriftTimeConstantMinMs, kAudioDriftTimeConstantMaxMs);
	clamp_int(s.voice_audio_resync_ms, kVoiceAudioResyncMinMs, kVoiceAudioResyncMaxMs);
	clamp_int(s.record_finalize_wait_on_exit_s, kRecordFinalizeWaitMinS, kRecordFinalizeWaitMaxS);
	clamp_int(s.record_queue_limit, kRecordQueueLimitMin, kRecordQueueLimitMax);
	clamp_int(s.record_clock_push_max_frames, kRecordClockPushMaxFramesMin, kRecordClockPushMaxFramesMax);
	clamp_int(s.record_clock_reanchor_max_ms, kRecordClockReanchorMinMs, kRecordClockReanchorMaxMs);
	clamp_int(s.limit_max_long_edge, kLimitMaxLongEdgeNone, kLimitMaxLongEdgeMax);
	if (s.limit_max_long_edge != kLimitMaxLongEdgeNone && s.limit_max_long_edge < kLimitMaxLongEdgeMin)
		s.limit_max_long_edge = kLimitMaxLongEdgeMin;
	clamp_int(s.limit_fps_divisor, kLimitFpsDivisorMin, kLimitFpsDivisorMax);
	clamp_int(s.vencord_check_disconnected_s, kVencordCheckIntervalMinS, kVencordCheckIntervalMaxS);
	clamp_int(s.vencord_check_connected_s, kVencordCheckIntervalMinS, kVencordCheckIntervalMaxS);

	clamp_int(s.notice_show_delay_ms, kNoticeDelayMinMs, kNoticeDelayMaxMs);
	clamp_int(s.notice_clear_delay_ms, kNoticeDelayMinMs, kNoticeDelayMaxMs);
	clamp_int(s.first_frame_wait_ms, kFirstFrameWaitMinMs, kFirstFrameWaitMaxMs);
	clamp_int(s.link_lost_hold_ms, kLinkLostHoldMinMs, kLinkLostHoldMaxMs);
	clamp_int(s.reconnect_layout_hold_ms, kReconnectLayoutHoldMinMs, kReconnectLayoutHoldMaxMs);
	clamp_int(s.call_action_timeout_ms, kCallActionTimeoutMinMs, kCallActionTimeoutMaxMs);
	clamp_int(s.display_buffer_ms, kDisplayBufferMinMs, kDisplayBufferMaxMs);
	clamp_int(s.tile_gap_px, kTileGapMinPx, kTileGapMaxPx);
	fix_color(s.slot_background_color, kDefaultSlotBackgroundColor);
	clamp_int(s.slot_background_opacity_pct, kSlotBackgroundOpacityMinPct, kSlotBackgroundOpacityMaxPct);
	clamp_tenth_step(s.label_em_ratio_pct, kLabelEmRatioMinPct, kLabelEmRatioMaxPct, defaults.label_em_ratio_pct);
	fix_color(s.label_border_color, kDefaultLabelBorderColor);
	fix_color(s.label_plate_color, kDefaultLabelPlateColor);
	clamp_int(s.label_plate_opacity_pct, kLabelPlateOpacityMinPct, kLabelPlateOpacityMaxPct);
	fix_color(s.label_text_color, kDefaultLabelTextColor);
	clamp_int(s.label_font_weight, kLabelFontWeightMin, kLabelFontWeightMax);
	clamp_int(s.label_border_rise_ms, kLabelBorderTimingMinMs, kLabelBorderTimingMaxMs);
	clamp_int(s.label_border_hold_ms, kLabelBorderTimingMinMs, kLabelBorderTimingMaxMs);
	clamp_int(s.label_border_fade_ms, kLabelBorderTimingMinMs, kLabelBorderTimingMaxMs);
	clamp_double(s.label_margin_pct, kLabelMarginMinPct, kLabelMarginMaxPct, defaults.label_margin_pct);
	fix_color(s.label_outline_color, kDefaultLabelOutlineColor);
	clamp_double(s.label_outline_ratio, kLabelOutlineRatioMin, kLabelOutlineRatioMax, defaults.label_outline_ratio);
	clamp_int(s.label_opacity_pct, kLabelOpacityMinPct, kLabelOpacityMaxPct);
	clamp_double(s.label_avatar_ratio, kLabelAvatarRatioMin, kLabelAvatarRatioMax, defaults.label_avatar_ratio);
	clamp_double(s.label_glow_blur_ratio, kLabelGlowBlurRatioMin, kLabelGlowBlurRatioMax,
		     defaults.label_glow_blur_ratio);
	clamp_double(s.label_glow_spread_ratio, kLabelGlowSpreadRatioMin, kLabelGlowSpreadRatioMax,
		     defaults.label_glow_spread_ratio);
	clamp_int(s.label_dim_pct, kLabelDimMinPct, kLabelDimMaxPct);
	s.label_dim_pct = (s.label_dim_pct + kLabelDimStepPct / 2) / kLabelDimStepPct * kLabelDimStepPct;
	clamp_int(s.self_speaking_threshold_db, kSelfSpeakingThresholdMinDb, kSelfSpeakingThresholdMaxDb);
	clamp_double(s.roster_gap_ratio, kRosterGapRatioMin, kRosterGapRatioMax, defaults.roster_gap_ratio);
	s.roster_gap_ratio = std::round(s.roster_gap_ratio * 20.0) / 20.0; // 範囲の端が 0.05 刻みなので、丸めた後も範囲の中にある
	clamp_tenth_step(s.roster_name_max_em, kRosterNameMaxEmMin, kRosterNameMaxEmMax, defaults.roster_name_max_em);
	s.roster_direction = roster_direction_for(s.roster_anchor, s.roster_direction);
	clamp_int(s.roster_offset_x_px, kRosterOffsetMinPx, kRosterOffsetMaxPx);
	clamp_int(s.roster_offset_y_px, kRosterOffsetMinPx, kRosterOffsetMaxPx);
	clamp_int(s.layout_hold_idle_ms, kLayoutHoldIdleMinMs, kLayoutHoldIdleMaxMs);
	clamp_int(s.dummy_cycle_ms, kDummyCycleMinMs, kDummyCycleMaxMs);
	clamp_int(s.record_finalize_stall_s, kRecordFinalizeStallMinS, kRecordFinalizeStallMaxS);
}

} // namespace cordmux::config
