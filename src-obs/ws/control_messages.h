// SPDX-License-Identifier: Apache-2.0

// ワイヤー形式の制御用の JSON (WebSocket のテキストメッセージ) の型と、組み立てる関数と読む関数。メッセージの
// 種類とフィールドは docs/wire_format.md の「制御用の JSON」にある。キーは snake_case で書く。
// 読む関数は、壊れた入力にも悪意のある入力にも例外を投げず、nullopt を返す。不正なメッセージ 1 つで OBS が
// クラッシュしないようにするためである。
// libobs に依存しない。nlohmann/json (ws/ws_deps.cmake が取得する) にだけ依存する。

#pragma once

// nlohmann/json は外部のライブラリなので、そのヘッダーが出す警告は表示しない。
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <nlohmann/json.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace cordmux::ws {

// "type" フィールドの値。
namespace control_type {
inline constexpr const char *kHello = "hello";
inline constexpr const char *kHelloOk = "hello_ok";
inline constexpr const char *kHelloNg = "hello_ng";
inline constexpr const char *kPing = "ping";
inline constexpr const char *kPong = "pong";
inline constexpr const char *kConfigure = "configure";
inline constexpr const char *kKeyframeRequest = "keyframe_request";
// 画面共有の映像が届かないストリームを確認して、必要なら読み出しとエンコードを始め直す頼み (受信側 -> 送信側) と、
// その結果 (送信側 -> 受信側)。
inline constexpr const char *kStreamCheck = "stream_check";
inline constexpr const char *kStreamCheckResult = "stream_check_result";
// 録画中かどうか (受信側 -> 送信側)。
inline constexpr const char *kRecording = "recording";
// 映像と音声を読み出してエンコードしてよいか (受信側 -> 送信側)。
inline constexpr const char *kMedia = "media";
// 画質の上限。すべての画面共有に同じ値を使う (受信側 -> 送信側)。
inline constexpr const char *kLimits = "limits";
// メインアカウントの userId (受信側 -> 送信側)。送信側が、メインアカウントがいる通話を読むために使う。
inline constexpr const char *kMainAccount = "main_account";
// 視聴する人の範囲 (受信側 -> 送信側)。
inline constexpr const char *kReceiveScope = "receive_scope";
// [移動] と [退出] を押したこと (受信側 -> 送信側)。
inline constexpr const char *kMoveToMainCall = "move_to_main_call";
inline constexpr const char *kLeaveCall = "leave_call";
inline constexpr const char *kLog = "log";
inline constexpr const char *kFacts = "facts";
inline constexpr const char *kProfiles = "profiles";
// 1 人のアバターの画像 (送信側 -> 受信側)。参加者ごとに、avatar_key が変わったときだけ届く。
inline constexpr const char *kAvatar = "avatar";
} // namespace control_type

// ワイヤー形式のバージョン。hello と hello_ok の wire に入れる。上げる条件は docs/wire_format.md の「バージョンと拡張性」に
// ある。
inline constexpr int kWireVersion = 1;

// ---- hello、hello_ok、hello_ng (接続して最初に交わすメッセージ) ----

// 送信側の時計の種類 (hello の任意のキー clock_base)。
enum class SenderClockBase {
	// キーがないか、知らない値。受信側は、送信側の時計と OBS の時計の差を hello と pong の clock_us から決める
	// (pipeline/sender_clock.h)。
	Unspecified,
	// "qpc"。送信側の時計は QueryPerformanceCounter の us で、OBS の時計 (os_gettime_ns / 1000) と同じ値である
	// (src-vencord/encode/sender_clock.ts)。受信側は差を 0 にする。
	Qpc,
};

struct HelloMessage {
	int wire = 0;         // 送信側のワイヤー形式のバージョン
	std::string version;  // 送信側のバージョン。表示にだけ使い、値を信用しない
	std::string secret;   // トークン (16 進の 64 文字のはずだが、ここでは長さを確認しない)
	// Vesktop のバージョン (任意のキー vesktop)。version と同じく、表示にだけ使い、値を信用しない。
	// 送信側が読めなかったときは値がなく、画面には分からないことを示す "-" を表示する。キーがあっても
	// 文字列でなければ、値がないものとして扱う (parse_hello)。
	std::optional<std::string> vesktop;
	// 送信側が hello を送る前に読んだ送信側の時計 (任意のキー clock_us。timestamp_us と同じ時計の us)。受信側は、
	// これで送信側の時計と OBS の時計の差を決める (pipeline/sender_clock.h)。0 以上の整数でなければ、値がないものとして
	// 扱う。
	std::optional<std::int64_t> clock_us;
	// 送信側の時計の種類 (任意のキー clock_base)。文字列の "qpc" のときだけ Qpc で、キーがないか、ほかの値か、文字列で
	// なければ Unspecified (古い受信側と同じ扱い)。
	SenderClockBase clock_base = SenderClockBase::Unspecified;
};

enum class HelloNgReason { BadSecret, BadWire, Malformed };

inline const char *to_string(HelloNgReason r)
{
	switch (r) {
	case HelloNgReason::BadSecret:
		return "bad_secret";
	case HelloNgReason::BadWire:
		return "bad_wire";
	case HelloNgReason::Malformed:
		return "malformed";
	}
	return "malformed";
}

// ---- media、limits (受信側 -> 送信側) ----
//
// hello_ok がこの 2 つの初期値を任意のキーとして持つので、hello_ok より前に定義する。

// 送信側に、映像と音声を読み出してエンコードしてよいかを伝える。false の間、送信側は読み出しもエンコードも
// アバターの取得もしない。値を決めるのは受信側 (pipeline/pipeline.h の decide_media) である。
// 送るのは recording と同じく、値が変わったときと、接続した直後である。接続していなければ捨てる。
// on は必須で、真偽値でなければそのメッセージを捨てる (recording と同じ)。
struct MediaMessage {
	bool on = false;
};

// 画質の上限と、エンコードの設定。すべての画面共有に同じ値を使う。参加者ごと、枠ごとの値はない。
// どのキーも任意で、省いたキーは制限しない (max_long_edge を省くと長い辺の上限なし、fps_divisor を省くと
// 1 と同じでフレームレートをそのまま、encoder_preference を省くとハードウェア、latency_mode を省くと realtime)。
// キーがあるときの範囲は次のとおり。
//   max_long_edge:      正の整数 (ピクセル)。0 と負の値は範囲の外 (制限しないときはキーを省く)。
//   fps_divisor:        1 か 2。
//   encoder_preference: "hardware" か "software"。
//   latency_mode:       "realtime" か "quality"。
// エンコードの設定の 2 つは、続いている画面共有には適用されず、次に始まる画面共有から反映される (送信側の encode/limits.ts)。
// 範囲の外の値と型の違う値を含むメッセージは、まるごと捨てて数える。送信側の
// src-vencord/transport/control.ts と同じ基準で、範囲の外の値が送信側のエンコードの処理に届いて RangeError に
// ならないようにする。設定の limit_max_long_edge の 0 (制限なし) を、キーを省いた形にするのは呼び出し元である。
struct LimitsMessage {
	std::optional<uint32_t> max_long_edge;
	std::optional<int> fps_divisor;
	std::optional<std::string> encoder_preference;
	std::optional<std::string> latency_mode;
};

inline bool operator==(const LimitsMessage &a, const LimitsMessage &b)
{
	return a.max_long_edge == b.max_long_edge && a.fps_divisor == b.fps_divisor &&
	       a.encoder_preference == b.encoder_preference && a.latency_mode == b.latency_mode;
}

inline bool operator!=(const LimitsMessage &a, const LimitsMessage &b)
{
	return !(a == b);
}

// limits の値が範囲の中にあるか (基準は LimitsMessage のコメント)。組み立てる側と読む側の両方が使う。
inline bool limits_in_range(const LimitsMessage &m)
{
	if (m.max_long_edge.has_value() && *m.max_long_edge == 0)
		return false;
	if (m.fps_divisor.has_value() && *m.fps_divisor != 1 && *m.fps_divisor != 2)
		return false;
	if (m.encoder_preference.has_value() && *m.encoder_preference != "hardware" &&
	    *m.encoder_preference != "software")
		return false;
	if (m.latency_mode.has_value() && *m.latency_mode != "realtime" && *m.latency_mode != "quality")
		return false;
	return true;
}

struct HelloOkMessage {
	int wire = kWireVersion;
	std::string version; // cordmux.dll のバージョン (表示専用の値)

	// ---- 任意のキー ----
	// media と limits の初期値。値がなければキーを省く。hello_ok に入れておくと、送信側は接続した直後に、
	// 読み出しを始めてすぐに止めることも、上限なしでエンコードを始めることもしないで済む。ワイヤー形式では
	// 知らないキーを無視するので、このキーを知らない送信側にも送ってよい。hello_ok に入れても、接続した直後の
	// media と limits のメッセージは別に送る (recording と同じタイミング)。
	std::optional<bool> media;
	std::optional<LimitsMessage> limits;
};

struct HelloNgMessage {
	HelloNgReason reason = HelloNgReason::Malformed;
	// 受け取った secret と version は、理由の文字列に入れない (トークンをログと画面に出さないため)。
};

// ---- ping、pong (ハートビート。間隔と切断までの回数は ws/listener.h の ListenerConfig) ----

struct PingMessage {
	uint64_t seq = 0;
};

struct PongMessage {
	uint64_t seq = 0;
	// 送信側が pong を送る前に読んだ送信側の時計 (任意のキー clock_us)。hello の clock_us と同じ扱い。
	std::optional<std::int64_t> clock_us;
};

// ---- configure (ストリームごとの解像度、フレームレート、音声の有無。割り当て表への入力) ----

enum class StreamKind { Screen, Voice };

inline const char *to_string(StreamKind k) { return k == StreamKind::Screen ? "screen" : "voice"; }

struct StreamConfig {
	uint32_t stream_id = 0;
	std::string user_id;
	StreamKind kind = StreamKind::Screen;
	uint32_t width = 0;
	uint32_t height = 0;
	double fps = 0.0;
	bool audio = false;
};

struct ConfigureMessage {
	std::vector<StreamConfig> streams;
};

// ---- keyframe_request (受信側 -> 送信側。キーフレームの要求) ----

struct KeyframeRequestMessage {
	uint32_t stream_id = 0;
};

// ---- stream_check (受信側 -> 送信側。映像が届かないストリームの確認の頼み) ----
//
// 枠に載せている人の映像が届かないまま待つ時間 (settings の receive_restart_wait_ms) がたったときに送る。送信側は、
// そのストリームのトラックに RTP が届いているかなどの証拠で、読み出しとエンコードを始め直すかを決め、結果を
// stream_check_result で返す (docs/wire_format.md の「メッセージの種類」の stream_check の扱い)。
struct StreamCheckMessage {
	uint32_t stream_id = 0;
};

// ---- stream_check_result (送信側 -> 受信側。確認の結果) ----
//
// outcome は送信側が決めた結果の名前 (restarted、superseded、not_receiving、sending、ended、unknown_stream)。知らない
// 名前も文字列のまま受け取る (結果の種類を増やしても壊れないようにするため)。detail は任意の短い説明で、ログにだけ書く。
struct StreamCheckResultMessage {
	uint32_t stream_id = 0;
	std::string outcome;
	std::string detail;
};

// ---- recording (受信側 -> 送信側。録画中かどうか) ----
//
// 送信側は、録画中のときだけ黒埋め用のフレームを作るので、録画の状態を知る必要がある。送るのは、値が変わった
// ときと、接続した直後である。接続していなければ捨てる。接続が切れても、送信側は録画の状態を OFF に戻さない。
struct RecordingMessage {
	bool on = false;
};

// ---- main_account (受信側 -> 送信側。メインアカウントの userId) ----
//
// 送信側は、Vesktop のアカウントから見える通話の中で、メインアカウントがいる通話の名前と、参加できるかを facts の
// main_call で返す。そのため、メインアカウントの userId を知る必要がある。送るのは、値が変わったときと、接続した
// 直後である。接続していなければ捨てる。user_id が空の文字列なら、メインアカウントを使わない構成で、送信側は
// main_call を返さない。
struct MainAccountMessage {
	std::string user_id;
};

// ---- receive_scope (受信側 -> 送信側。視聴する人の範囲) ----
//
// slots_only が false なら、画面共有している全員を視聴する (user_ids は持たない)。true なら、user_ids の人だけを視聴する
// (空なら誰も視聴しない)。user_ids には、枠に載っている人を、userId の昇順で重複なく入れる。送るのは、範囲が変わった
// ときと、接続した直後である。接続していなければ捨てる。
struct ReceiveScopeMessage {
	bool slots_only = false;
	std::vector<std::string> user_ids;
};

inline bool operator==(const ReceiveScopeMessage &a, const ReceiveScopeMessage &b)
{
	return a.slots_only == b.slots_only && a.user_ids == b.user_ids;
}

inline bool operator!=(const ReceiveScopeMessage &a, const ReceiveScopeMessage &b)
{
	return !(a == b);
}

// ---- move_to_main_call、leave_call (受信側 -> 送信側。利用者が [移動] と [退出] を押したこと) ----
//
// どちらも、利用者が押したときだけ送る。キーを持たない。移動先は運ばず、送信側が、押された時点のメインアカウントが
// いる通話を読み直して決める (受信側から任意の通話を指定させない)。接続していなければ捨て、送り直さない。
struct MoveToMainCallMessage {};
struct LeaveCallMessage {};

// ---- log (送信側のログの行。src-vencord/core/logbook.ts から取り出した行) ----
//
// lines は出来事の行、alerts は異常に関わる出来事の行 (受信側は OBS のログに LOG_WARNING で書く)、stats は数秒ごとの
// 統計の行である。alerts と stats は任意で、古い送信側は lines だけを送る (そのときは全部が出来事の行になる)。
struct LogMessage {
	std::vector<std::string> lines;
	std::vector<std::string> alerts;
	std::vector<std::string> stats;
};

// ---- facts (送信側の状態の要約。中身は ws/ では読まない) ----

struct FactsMessage {
	nlohmann::json facts; // 中身は src-vencord/core が決める。ws/ はそのまま渡す
};

// facts の中身 (stages を含む) は、ここでは読まない。ws/ は facts のキーがあることだけを確認してそのまま渡し、
// キーごとの読み方は pipeline/facts_state.h にだけ置く。2 か所で読むと、キーがないときや壊れているときの扱いが
// 食い違う。

// ---- profiles (参加者ごとの表示名、ユーザー名、アバターのバージョン、Bot の印。facts と違い、ws/ が形を確認する) ----
//
// アバターの画像は profiles に載らず、avatar で参加者ごとに届く (下の AvatarMessage)。

struct ProfileEntry {
	std::string user_id;
	std::optional<std::string> display_name;
	// Discord のユーザー名 (任意)。Discord の中で 1 人に 1 つだけの名前で、表示名が同じ人を見分けるために、
	// 参加者の一覧の行の 2 行目に表示する。送信側が読めなかったときは値がない。空の文字列も値がないものとする。
	std::optional<std::string> username;
	// アバターのバージョン (Discord の user.avatar の値。任意)。アバターを設定していない人は空の文字列で、送信側がまだ
	// 読めていない人は値がない。空の文字列と値がないことを区別する (前者はアバターがない、後者は分からない)。
	std::optional<std::string> avatar_key;
	// Bot のアカウントか。キーがなければ false。
	bool bot = false;
};

struct ProfilesMessage {
	std::vector<ProfileEntry> profiles;
};

// ---- avatar (送信側 -> 受信側。1 人のアバターの画像) ----
//
// avatar_png は base64 の文字列のまま持ち、デコードしない (デコードするのは pipeline/profiles_state.h)。3 つのキーは
// どれも省けない。avatar_png が kMaxAvatarPngBase64Bytes を超えるメッセージは捨てる。
struct AvatarMessage {
	std::string user_id;
	std::string avatar_key; // この画像を取ったときの Discord の user.avatar の値
	std::string avatar_png;
};

// avatar_png の長さの上限 (バイト)。デコードした PNG の大きさではなく、base64 の文字列の長さで数える。
// 捨てるかどうかを、デコードせずに決められるようにするためである。
inline constexpr std::size_t kMaxAvatarPngBase64Bytes = 32 * 1024;

// ---- 組み立てる関数 (cordmux.dll が送るメッセージ) ----

// limits の 4 つのキーだけを持つオブジェクトを作る (値がないキーは書かない)。limits のメッセージと、hello_ok の
// 任意のキー limits の両方が使う。
inline nlohmann::json limits_fields_to_json(const LimitsMessage &m)
{
	nlohmann::json j = nlohmann::json::object();
	if (m.max_long_edge.has_value())
		j["max_long_edge"] = *m.max_long_edge;
	if (m.fps_divisor.has_value())
		j["fps_divisor"] = *m.fps_divisor;
	if (m.encoder_preference.has_value())
		j["encoder_preference"] = *m.encoder_preference;
	if (m.latency_mode.has_value())
		j["latency_mode"] = *m.latency_mode;
	return j;
}

inline std::string build_hello_ok(const HelloOkMessage &m)
{
	nlohmann::json j;
	j["type"] = control_type::kHelloOk;
	j["wire"] = m.wire;
	j["version"] = m.version;
	// 任意のキーは値があるときだけ書く。範囲の外の limits は書かない (送信側が捨てる値を送らない。limits の
	// メッセージでも同じで、Listener::send_limits が範囲の外の値を送らない)。
	if (m.media.has_value())
		j["media"] = *m.media;
	if (m.limits.has_value() && limits_in_range(*m.limits))
		j["limits"] = limits_fields_to_json(*m.limits);
	return j.dump();
}

inline std::string build_media(const MediaMessage &m)
{
	nlohmann::json j;
	j["type"] = control_type::kMedia;
	j["on"] = m.on;
	return j.dump();
}

// 範囲は確認しない。範囲の外の値を送らないのは、呼び出し元 (Listener::send_limits) の役目である。
inline std::string build_limits(const LimitsMessage &m)
{
	nlohmann::json j = limits_fields_to_json(m);
	j["type"] = control_type::kLimits;
	return j.dump();
}

inline std::string build_hello_ng(const HelloNgMessage &m)
{
	nlohmann::json j;
	j["type"] = control_type::kHelloNg;
	j["reason"] = to_string(m.reason);
	return j.dump();
}

inline std::string build_ping(const PingMessage &m)
{
	nlohmann::json j;
	j["type"] = control_type::kPing;
	j["seq"] = m.seq;
	return j.dump();
}

inline std::string build_pong(const PongMessage &m)
{
	nlohmann::json j;
	j["type"] = control_type::kPong;
	j["seq"] = m.seq;
	if (m.clock_us.has_value())
		j["clock_us"] = *m.clock_us;
	return j.dump();
}

inline std::string build_keyframe_request(const KeyframeRequestMessage &m)
{
	nlohmann::json j;
	j["type"] = control_type::kKeyframeRequest;
	j["stream_id"] = m.stream_id;
	return j.dump();
}

inline std::string build_stream_check(const StreamCheckMessage &m)
{
	nlohmann::json j;
	j["type"] = control_type::kStreamCheck;
	j["stream_id"] = m.stream_id;
	return j.dump();
}

inline std::string build_recording(const RecordingMessage &m)
{
	nlohmann::json j;
	j["type"] = control_type::kRecording;
	j["on"] = m.on;
	return j.dump();
}

inline std::string build_main_account(const MainAccountMessage &m)
{
	nlohmann::json j;
	j["type"] = control_type::kMainAccount;
	j["user_id"] = m.user_id;
	return j.dump();
}

// slots_only が false のとき、user_ids は書かない。
inline std::string build_receive_scope(const ReceiveScopeMessage &m)
{
	nlohmann::json j;
	j["type"] = control_type::kReceiveScope;
	j["scope"] = m.slots_only ? "slots" : "everyone";
	if (m.slots_only)
		j["user_ids"] = m.user_ids;
	return j.dump();
}

inline std::string build_move_to_main_call()
{
	nlohmann::json j;
	j["type"] = control_type::kMoveToMainCall;
	return j.dump();
}

inline std::string build_leave_call()
{
	nlohmann::json j;
	j["type"] = control_type::kLeaveCall;
	return j.dump();
}

inline std::string build_hello(const HelloMessage &m)
{
	// 単体テストが送信側の代わりに使う。製品の送信側の実装は src-vencord/transport にある。
	nlohmann::json j;
	j["type"] = control_type::kHello;
	j["wire"] = m.wire;
	j["version"] = m.version;
	j["secret"] = m.secret;
	// 任意のキーは値があるときだけ書く (src-vencord/transport/client.ts の buildHello と同じ形)。
	if (m.vesktop.has_value())
		j["vesktop"] = *m.vesktop;
	if (m.clock_us.has_value())
		j["clock_us"] = *m.clock_us;
	if (m.clock_base == SenderClockBase::Qpc)
		j["clock_base"] = "qpc";
	return j.dump();
}

// ---- 読む関数 (受け取ったメッセージ)。どれも例外を投げず、読めなければ nullopt を返す ----

// テキストを JSON として読む。読めなければ nullopt を返す (allow_exceptions を false にした parse を使う)。
inline std::optional<nlohmann::json> try_parse_json(const std::string &text)
{
	nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
	if (j.is_discarded())
		return std::nullopt;
	return j;
}

// "type" フィールドの値を返す。ないか文字列でなければ nullopt を返す。
inline std::optional<std::string> peek_type(const nlohmann::json &j)
{
	if (!j.is_object())
		return std::nullopt;
	const auto it = j.find("type");
	if (it == j.end() || !it->is_string())
		return std::nullopt;
	return it->get<std::string>();
}

namespace detail {

// 任意のキー clock_us を読む。0 以上の整数で、int64_t に収まるときだけ値を返す。それ以外は、キーがないものとして扱い、
// メッセージは捨てない (hello の vesktop と同じ扱い)。
inline std::optional<std::int64_t> optional_clock_us(const nlohmann::json &j)
{
	const auto it = j.find("clock_us");
	if (it == j.end() || !it->is_number_integer())
		return std::nullopt;
	if (it->is_number_unsigned()) {
		const std::uint64_t v = it->get<std::uint64_t>();
		if (v > std::uint64_t(INT64_MAX))
			return std::nullopt;
		return std::int64_t(v);
	}
	const std::int64_t v = it->get<std::int64_t>();
	if (v < 0)
		return std::nullopt;
	return v;
}

} // namespace detail

inline std::optional<HelloMessage> parse_hello(const nlohmann::json &j)
{
	if (!j.is_object())
		return std::nullopt;
	const auto wire_it = j.find("wire");
	const auto secret_it = j.find("secret");
	const auto version_it = j.find("version");
	if (wire_it == j.end() || !wire_it->is_number_integer())
		return std::nullopt;
	if (secret_it == j.end() || !secret_it->is_string())
		return std::nullopt;
	// version は必須のキーである (src-vencord/transport/control.ts と同じ扱い)。
	if (version_it == j.end() || !version_it->is_string())
		return std::nullopt;
	HelloMessage m;
	m.wire = wire_it->get<int>();
	m.secret = secret_it->get<std::string>();
	m.version = version_it->get<std::string>();
	// 任意のキー vesktop が文字列でなければ、そのキーだけをないものとし、hello は捨てない。表示にだけ使う値の
	// 誤り 1 つで、接続を止めないためである (hello_ok の任意のキーと同じ扱いで、src-vencord/transport/control.ts の
	// validateControlMessage と同じ基準)。
	if (const auto vesktop_it = j.find("vesktop"); vesktop_it != j.end() && vesktop_it->is_string())
		m.vesktop = vesktop_it->get<std::string>();
	m.clock_us = detail::optional_clock_us(j);
	// 任意のキー clock_base は、文字列の "qpc" だけを読む。ほかの値は、キーがないものとして扱い、hello は捨てない
	// (送信側が新しい値を足しても、古い受信側は今までどおり差を決めて接続を続ける)。
	if (const auto base_it = j.find("clock_base");
	    base_it != j.end() && base_it->is_string() && base_it->get<std::string>() == "qpc")
		m.clock_base = SenderClockBase::Qpc;
	return m;
}

inline std::optional<PongMessage> parse_pong(const nlohmann::json &j)
{
	if (!j.is_object())
		return std::nullopt;
	const auto seq_it = j.find("seq");
	if (seq_it == j.end() || !seq_it->is_number_unsigned())
		return std::nullopt;
	PongMessage m;
	m.seq = seq_it->get<uint64_t>();
	m.clock_us = detail::optional_clock_us(j);
	return m;
}

// recording を受け取るのは送信側 (src-vencord/transport/control.ts) で、cordmux.dll は送るだけである。読む関数を
// ここに置くのは、単体テストが、送信側に届いたテキストを読んで確認するためである (build_hello と同じ理由)。
// on が真偽値でなければ nullopt を返す。
inline std::optional<RecordingMessage> parse_recording(const nlohmann::json &j)
{
	if (!j.is_object())
		return std::nullopt;
	const auto on_it = j.find("on");
	if (on_it == j.end() || !on_it->is_boolean())
		return std::nullopt;
	RecordingMessage m;
	m.on = on_it->get<bool>();
	return m;
}

// ---- main_account、receive_scope を読む関数 ----
//
// どちらも受け取るのは送信側で、ここに置く理由は parse_recording と同じである。基準は src-vencord/transport/control.ts と
// 同じにする。

inline std::optional<MainAccountMessage> parse_main_account(const nlohmann::json &j)
{
	if (!j.is_object())
		return std::nullopt;
	const auto it = j.find("user_id");
	if (it == j.end() || !it->is_string())
		return std::nullopt;
	MainAccountMessage m;
	m.user_id = it->get<std::string>();
	return m;
}

inline std::optional<ReceiveScopeMessage> parse_receive_scope(const nlohmann::json &j)
{
	if (!j.is_object())
		return std::nullopt;
	const auto scope = j.find("scope");
	if (scope == j.end() || !scope->is_string())
		return std::nullopt;
	ReceiveScopeMessage m;
	const std::string &value = scope->get_ref<const std::string &>();
	if (value == "everyone")
		return m;
	if (value != "slots")
		return std::nullopt;
	const auto ids = j.find("user_ids");
	if (ids == j.end() || !ids->is_array())
		return std::nullopt;
	for (const auto &id : *ids) {
		if (!id.is_string())
			return std::nullopt;
		m.user_ids.push_back(id.get<std::string>());
	}
	m.slots_only = true;
	return m;
}

// ---- media、limits、hello_ok を読む関数 ----
//
// どれも受け取るのは送信側 (src-vencord/transport/control.ts) で、ここに置く理由は parse_recording と同じである。
// 基準は control.ts と同じにする。片方だけ緩めると、単体テストが通るのに送信側が捨てる値ができる。

inline std::optional<MediaMessage> parse_media(const nlohmann::json &j)
{
	if (!j.is_object())
		return std::nullopt;
	const auto on_it = j.find("on");
	if (on_it == j.end() || !on_it->is_boolean())
		return std::nullopt;
	MediaMessage m;
	m.on = on_it->get<bool>();
	return m;
}

// オブジェクトから limits の 4 つのキーを読む (limits のメッセージと、hello_ok のキー limits の共通の部分)。
// 型の違う値か範囲の外の値が 1 つでもあれば nullopt を返す (基準は LimitsMessage のコメント)。知らないキーは
// 無視する。
inline std::optional<LimitsMessage> parse_limits_fields(const nlohmann::json &j)
{
	if (!j.is_object())
		return std::nullopt;
	LimitsMessage m;
	if (const auto it = j.find("max_long_edge"); it != j.end()) {
		if (!it->is_number_unsigned())
			return std::nullopt;
		const std::uint64_t raw = it->get<std::uint64_t>();
		if (raw == 0 || raw > UINT32_MAX)
			return std::nullopt;
		m.max_long_edge = static_cast<uint32_t>(raw);
	}
	if (const auto it = j.find("fps_divisor"); it != j.end()) {
		if (!it->is_number_integer())
			return std::nullopt;
		const std::int64_t raw = it->get<std::int64_t>();
		if (raw != 1 && raw != 2)
			return std::nullopt;
		m.fps_divisor = static_cast<int>(raw);
	}
	if (const auto it = j.find("encoder_preference"); it != j.end()) {
		if (!it->is_string())
			return std::nullopt;
		std::string v = it->get<std::string>();
		if (v != "hardware" && v != "software")
			return std::nullopt;
		m.encoder_preference = std::move(v);
	}
	if (const auto it = j.find("latency_mode"); it != j.end()) {
		if (!it->is_string())
			return std::nullopt;
		std::string v = it->get<std::string>();
		if (v != "realtime" && v != "quality")
			return std::nullopt;
		m.latency_mode = std::move(v);
	}
	return m;
}

inline std::optional<LimitsMessage> parse_limits(const nlohmann::json &j)
{
	return parse_limits_fields(j);
}

// hello_ok を読む。wire (整数) と version (文字列) は必須である。
// 任意のキー (media と limits) の形が壊れていても hello_ok は捨てず、そのキーだけをないものとして扱う
// (src-vencord/transport/control.ts と同じ扱い)。hello_ok を捨てると接続できなくなり、1 つの値の誤りで接続全体が
// 止まる。同じ値は接続した直後の media と limits のメッセージでも届き、壊れていればそちらで捨てて数えられる。
inline std::optional<HelloOkMessage> parse_hello_ok(const nlohmann::json &j)
{
	if (!j.is_object())
		return std::nullopt;
	const auto wire_it = j.find("wire");
	const auto version_it = j.find("version");
	if (wire_it == j.end() || !wire_it->is_number_integer())
		return std::nullopt;
	if (version_it == j.end() || !version_it->is_string())
		return std::nullopt;
	HelloOkMessage m;
	m.wire = wire_it->get<int>();
	m.version = version_it->get<std::string>();
	if (const auto media_it = j.find("media"); media_it != j.end() && media_it->is_boolean())
		m.media = media_it->get<bool>();
	if (const auto limits_it = j.find("limits"); limits_it != j.end())
		m.limits = parse_limits_fields(*limits_it);
	return m;
}

inline std::optional<StreamKind> parse_stream_kind(const nlohmann::json &j)
{
	if (!j.is_string())
		return std::nullopt;
	const std::string s = j.get<std::string>();
	if (s == "screen")
		return StreamKind::Screen;
	if (s == "voice")
		return StreamKind::Voice;
	return std::nullopt;
}

inline std::optional<StreamConfig> parse_stream_config(const nlohmann::json &j)
{
	if (!j.is_object())
		return std::nullopt;
	const auto id_it = j.find("stream_id");
	const auto user_it = j.find("user_id");
	const auto kind_it = j.find("kind");
	if (id_it == j.end() || !id_it->is_number_unsigned())
		return std::nullopt;
	if (user_it == j.end() || !user_it->is_string())
		return std::nullopt;
	if (kind_it == j.end())
		return std::nullopt;
	const auto kind = parse_stream_kind(*kind_it);
	if (!kind)
		return std::nullopt;

	StreamConfig c;
	c.stream_id = id_it->get<uint32_t>();
	c.user_id = user_it->get<std::string>();
	c.kind = *kind;

	// width、height、fps、audio は任意のキーだが、キーがあるのに型が違うときと、width、height、fps が 0 以下のときは、
	// この要素が壊れているものとして nullopt を返す (呼び出し元の parse_configure は、1 つでも壊れた要素があれば
	// configure 全体を捨てる)。意味のない値を受け入れないためで、src-vencord/transport/control.ts の
	// isConfigureStream と同じ基準である。
	if (const auto w = j.find("width"); w != j.end()) {
		if (!w->is_number_unsigned() || w->get<uint32_t>() == 0)
			return std::nullopt;
		c.width = w->get<uint32_t>();
	}
	if (const auto h = j.find("height"); h != j.end()) {
		if (!h->is_number_unsigned() || h->get<uint32_t>() == 0)
			return std::nullopt;
		c.height = h->get<uint32_t>();
	}
	if (const auto f = j.find("fps"); f != j.end()) {
		if (!f->is_number() || !(f->get<double>() > 0.0))
			return std::nullopt;
		c.fps = f->get<double>();
	}
	if (const auto a = j.find("audio"); a != j.end()) {
		if (!a->is_boolean())
			return std::nullopt;
		c.audio = a->get<bool>();
	}
	return c;
}

inline std::optional<ConfigureMessage> parse_configure(const nlohmann::json &j)
{
	if (!j.is_object())
		return std::nullopt;
	const auto streams_it = j.find("streams");
	if (streams_it == j.end() || !streams_it->is_array())
		return std::nullopt;

	ConfigureMessage m;
	m.streams.reserve(streams_it->size());
	// stream_id は、画面のストリームと声のストリームで共通の番号なので、同じ configure の中に同じ stream_id が
	// 2 つ以上あれば、種類にかかわらず壊れた configure である。受信側はそれを 2 つの経路に分けられず、画面共有の
	// 音が声の枠に出てしまう。1 つでも重複していれば、壊れた要素と同じく configure 全体を捨てる。どの値が正しいかを
	// 決められないので、どれも使わない。
	std::set<uint32_t> seen_stream_ids;
	for (const auto &item : *streams_it) {
		const auto c = parse_stream_config(item);
		if (!c)
			return std::nullopt; // 1 つでも壊れていれば、全体を不正とする
		if (!seen_stream_ids.insert(c->stream_id).second)
			return std::nullopt; // stream_id が重複している (壊れた configure)
		m.streams.push_back(*c);
	}
	return m;
}

namespace detail {

// 文字列の配列を読む。配列でないか、文字列でない要素が 1 つでもあれば false を返す。
inline bool read_string_array(const nlohmann::json &value, std::vector<std::string> *out)
{
	if (!value.is_array())
		return false;
	out->reserve(value.size());
	for (const auto &item : value) {
		if (!item.is_string())
			return false;
		out->push_back(item.get<std::string>());
	}
	return true;
}

} // namespace detail

inline std::optional<LogMessage> parse_log(const nlohmann::json &j)
{
	if (!j.is_object())
		return std::nullopt;
	const auto lines_it = j.find("lines");
	if (lines_it == j.end())
		return std::nullopt;

	LogMessage m;
	if (!detail::read_string_array(*lines_it, &m.lines))
		return std::nullopt;
	// alerts と stats は任意。あるのに形が違えば、メッセージの全体を捨てる (lines と同じ扱い)。
	const auto alerts_it = j.find("alerts");
	if (alerts_it != j.end() && !detail::read_string_array(*alerts_it, &m.alerts))
		return std::nullopt;
	const auto stats_it = j.find("stats");
	if (stats_it != j.end() && !detail::read_string_array(*stats_it, &m.stats))
		return std::nullopt;
	return m;
}

inline std::optional<StreamCheckResultMessage> parse_stream_check_result(const nlohmann::json &j)
{
	if (!j.is_object())
		return std::nullopt;
	const auto id_it = j.find("stream_id");
	const auto outcome_it = j.find("outcome");
	if (id_it == j.end() || outcome_it == j.end() || !id_it->is_number_unsigned() || !outcome_it->is_string())
		return std::nullopt;
	const std::uint64_t id = id_it->get<std::uint64_t>();
	if (id > 0xFFFFFFFFull)
		return std::nullopt;
	StreamCheckResultMessage m;
	m.stream_id = static_cast<uint32_t>(id);
	m.outcome = outcome_it->get<std::string>();
	// detail は任意。文字列でなければ、このキーだけをないものとする (ログにだけ使う値で、捨てるほどの誤りではない)。
	const auto detail_it = j.find("detail");
	if (detail_it != j.end() && detail_it->is_string())
		m.detail = detail_it->get<std::string>();
	return m;
}

inline std::optional<FactsMessage> parse_facts(const nlohmann::json &j)
{
	if (!j.is_object())
		return std::nullopt;
	const auto facts_it = j.find("facts");
	if (facts_it == j.end())
		return std::nullopt;
	FactsMessage m;
	m.facts = *facts_it;
	return m;
}

// user_id、display_name、avatar_key の型が違うか、bot が真偽値でなければ nullopt を返す (呼び出し元はメッセージ
// 全体を捨てる)。知らないキー (アバターの画像を profiles に載せていた送信側の avatar_png など) は読まない。
inline std::optional<ProfileEntry> parse_profile_entry(const nlohmann::json &j)
{
	if (!j.is_object())
		return std::nullopt;
	const auto id_it = j.find("user_id");
	if (id_it == j.end() || !id_it->is_string())
		return std::nullopt;

	ProfileEntry e;
	e.user_id = id_it->get<std::string>();

	if (const auto name_it = j.find("display_name"); name_it != j.end()) {
		if (!name_it->is_string())
			return std::nullopt;
		e.display_name = name_it->get<std::string>();
	}

	// avatar_key は空の文字列も値として持つ (アバターを設定していない人。ProfileEntry のコメント)。
	if (const auto key_it = j.find("avatar_key"); key_it != j.end()) {
		if (!key_it->is_string())
			return std::nullopt;
		e.avatar_key = key_it->get<std::string>();
	}

	if (const auto bot_it = j.find("bot"); bot_it != j.end()) {
		if (!bot_it->is_boolean())
			return std::nullopt;
		e.bot = bot_it->get<bool>();
	}

	// username の扱いは display_name と同じで、キーがあるのに文字列でなければ要素が壊れている (呼び出し元は
	// profiles 全体を捨てる)。空の文字列は、読めなかったときと同じく値がないものとする。送信側は読めなかった
	// ときにキーを省くので空の文字列を送らないはずで、空の名前では人を見分けられない。
	if (const auto username_it = j.find("username"); username_it != j.end()) {
		if (!username_it->is_string())
			return std::nullopt;
		std::string username = username_it->get<std::string>();
		if (!username.empty())
			e.username = std::move(username);
	}
	return e;
}

inline std::optional<ProfilesMessage> parse_profiles(const nlohmann::json &j)
{
	if (!j.is_object())
		return std::nullopt;
	const auto profiles_it = j.find("profiles");
	if (profiles_it == j.end() || !profiles_it->is_array())
		return std::nullopt;

	ProfilesMessage m;
	m.profiles.reserve(profiles_it->size());
	for (const auto &item : *profiles_it) {
		const auto e = parse_profile_entry(item);
		if (!e)
			return std::nullopt; // 1 つでも壊れた要素があれば、全体を不正とする。
		m.profiles.push_back(*e);
	}
	return m;
}

// avatar を読む。3 つのキーのどれかがないか文字列でないとき、avatar_png が kMaxAvatarPngBase64Bytes を超えるときは
// nullopt を返す (呼び出し元が捨てて数える)。
inline std::optional<AvatarMessage> parse_avatar(const nlohmann::json &j)
{
	if (!j.is_object())
		return std::nullopt;
	const auto id_it = j.find("user_id");
	const auto key_it = j.find("avatar_key");
	const auto png_it = j.find("avatar_png");
	if (id_it == j.end() || !id_it->is_string())
		return std::nullopt;
	if (key_it == j.end() || !key_it->is_string())
		return std::nullopt;
	if (png_it == j.end() || !png_it->is_string())
		return std::nullopt;
	if (png_it->get_ref<const std::string &>().size() > kMaxAvatarPngBase64Bytes)
		return std::nullopt;
	AvatarMessage m;
	m.user_id = id_it->get<std::string>();
	m.avatar_key = key_it->get<std::string>();
	m.avatar_png = png_it->get<std::string>();
	return m;
}

} // namespace cordmux::ws
