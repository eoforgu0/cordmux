// SPDX-License-Identifier: Apache-2.0

// 録画のセグメントを閉じる理由の判定、保存先の中のファイルとフォルダーの名前、録画が OFF の間に I/O の関数を
// 呼ばないための判定と、セッションのフォルダーと録画ログを開く処理。
// libobs、Qt、avformat に依存しない。Windows の名前 (test/lint/windows_names.txt) を書かない。
// パスの区切りは常に `/` で組み立てる (Windows の API は `/` をそのまま受け付けるので、`\` を書かずに済む)。

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "record/record_types.h"

namespace cordmux::record {

// ---------------------------------------------------------------------------
// セグメントを閉じる理由
// ---------------------------------------------------------------------------

// 直前に見た解像度と fps。Pipeline の stream_formats は configure を受け取るたびに丸ごと入れ替わり、前の値と
// 比べられないので、録画のコードが前の値を持つ。
struct StreamFormatMemo {
	int width = 0;
	int height = 0;
	int fps_num = 0;
	int fps_den = 0;

	// まだ 1 度も見ていない (比べる値がない)。
	bool empty() const { return width == 0 && height == 0 && fps_num == 0 && fps_den == 0; }
};

inline bool operator==(const StreamFormatMemo &a, const StreamFormatMemo &b)
{
	return a.width == b.width && a.height == b.height && a.fps_num == b.fps_num && a.fps_den == b.fps_den;
}

inline bool operator!=(const StreamFormatMemo &a, const StreamFormatMemo &b)
{
	return !(a == b);
}

// 1 つのストリーム (stream_id と user_id の組) について、今分かっていること。ストリームを表示するソースと枠の番号は
// 入れない。枠を並べ替えただけでセグメントを閉じないためである。
struct SegmentCutInput {
	bool connected = true;            // 接続が切れていないか
	bool present_in_configure = true; // configure にその stream がまだあるか
	StreamFormatMemo format;          // 現在の解像度と fps
	ClockStep clock = ClockStep::Forward; // タイムスタンプの判定 (record_clock.h の classify_pts)
	bool recording_enabled = true;    // 録画が ON か
	bool queue_overflowed = false;    // 録画のキューがあふれたか
};

// 開いているセグメントを閉じる理由を返す。閉じないときは値を持たない。
// 理由が重なったときは、外側の事情から先に見る (接続、configure、録画の OFF、キューのあふれ、解像度と fps、
// タイムスタンプの順)。接続が切れていれば configure の中身もキューも意味を持たないので、録画ログに残る理由が、
// いちばん外側で起きたことになる。
inline std::optional<CloseReason> segment_cut_reason(const StreamFormatMemo &previous, const SegmentCutInput &in)
{
	if (!in.connected)
		return CloseReason::Disconnected;
	if (!in.present_in_configure)
		return CloseReason::StreamGone;
	if (!in.recording_enabled)
		return CloseReason::RecordingToggled;
	if (in.queue_overflowed)
		return CloseReason::QueueOverflow;
	// 比べる値がないセグメント (開いた直後) では、解像度が変わったとは言えない。
	if (!previous.empty() && previous != in.format)
		return CloseReason::FormatChanged;
	if (in.clock == ClockStep::Jumped)
		return CloseReason::ClockJump;
	return std::nullopt;
}

// ---------------------------------------------------------------------------
// 保存先の中の名前
// ---------------------------------------------------------------------------

// 表示名をファイルの名前に使うときの長さの上限 (バイト)。パス全体の長さの上限は保存先によって変わるので、名前の側は
// 編集ソフトの一覧で読める長さで切る。
inline constexpr std::size_t kDisplayNameMaxBytes = 64;
// 名前が重なったときに後ろに付ける user_id の文字数 (resolve_name_collisions)。
inline constexpr std::size_t kCollisionIdPrefixLen = 6;
// セグメントの連番の桁数 (segment_file_name)。
inline constexpr int kSegmentSeqDigits = 6;

// 録画ログと保存先で使う種類の名前。録画ログの person の行の `kinds` と、セグメントファイルの名前が同じ語を使う。
inline const char *track_name(TrackKind track)
{
	switch (track) {
	case TrackKind::ScreenVideo:
		return "screen_video";
	case TrackKind::ScreenAudio:
		return "screen_audio";
	case TrackKind::Voice:
		return "voice";
	}
	return "voice";
}

inline std::optional<TrackKind> track_kind_from_name(const std::string &name)
{
	if (name == "screen_video")
		return TrackKind::ScreenVideo;
	if (name == "screen_audio")
		return TrackKind::ScreenAudio;
	if (name == "voice")
		return TrackKind::Voice;
	return std::nullopt;
}

// 区切りを 1 つだけ挟んでつなぐ (どちらかが空なら、もう一方をそのまま返す)。
inline std::string join_path(const std::string &base, const std::string &leaf)
{
	if (base.empty())
		return leaf;
	if (leaf.empty())
		return base;
	const char last = base.back();
	if (last == '/' || last == '\\')
		return base + leaf;
	return base + "/" + leaf;
}

namespace detail {

inline std::string two_digits(int value)
{
	if (value < 0)
		value = 0;
	std::string text = std::to_string(value);
	return text.size() >= 2 ? text : "0" + text;
}

inline std::string zero_pad(std::int64_t value, int digits)
{
	if (value < 0)
		value = 0;
	std::string text = std::to_string(value);
	while (static_cast<int>(text.size()) < digits)
		text.insert(text.begin(), '0');
	return text;
}

// UTF-8 の文字の途中で切らない長さ (max_bytes を超えない文字の境目のうち、最も後ろのもの)。
inline std::size_t utf8_truncate_len(const std::string &text, std::size_t max_bytes)
{
	if (text.size() <= max_bytes)
		return text.size();
	std::size_t len = max_bytes;
	while (len > 0 && (static_cast<unsigned char>(text[len]) & 0xC0) == 0x80)
		--len;
	return len;
}

} // namespace detail

// 保存先の中の、このセッションのフォルダーの名前 (「2000-01-01 00-00-00」)。OBS の録画の既定のファイル名
// (`%CCYY-%MM-%DD %hh-%mm-%ss`) と同じ形にする。
// 時差は引数で受け取る。ここで OS の設定を読むと、同じ入力でも結果が変わり、テストが環境に依存するためである。
// 日付の計算は Howard Hinnant の civil_from_days (ledger/rules.cpp の format_date_time_parts と同じ) で行い、
// <ctime> を使わない。
inline std::string session_dir_name(std::int64_t wall_us, int utc_offset_sec)
{
	constexpr std::int64_t kUsPerSec = 1000000;
	constexpr std::int64_t kSecPerDay = 86400;

	std::int64_t seconds = wall_us / kUsPerSec;
	if (wall_us % kUsPerSec < 0)
		--seconds; // 整数の除算は 0 方向に丸めるので、余りが負なら 1 秒引く
	seconds += utc_offset_sec;

	std::int64_t days = seconds / kSecPerDay;
	std::int64_t sec_of_day = seconds % kSecPerDay;
	if (sec_of_day < 0) {
		sec_of_day += kSecPerDay;
		--days;
	}

	const std::int64_t z = days + 719468;
	const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	const std::int64_t doe = z - era * 146097;
	const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const std::int64_t mp = (5 * doy + 2) / 153;
	const std::int64_t day = doy - (153 * mp + 2) / 5 + 1;
	const std::int64_t month = mp + (mp < 10 ? 3 : -9);
	const std::int64_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);

	std::string text = std::to_string(year);
	while (text.size() < 4)
		text.insert(text.begin(), '0');
	text += "-";
	text += detail::two_digits(static_cast<int>(month));
	text += "-";
	text += detail::two_digits(static_cast<int>(day));
	text += " ";
	text += detail::two_digits(static_cast<int>(sec_of_day / 3600));
	text += "-";
	text += detail::two_digits(static_cast<int>((sec_of_day % 3600) / 60));
	text += "-";
	text += detail::two_digits(static_cast<int>(sec_of_day % 60));
	return text;
}

// 同じ名前のセッションのフォルダーがすでにあるときに使う名前 (「2000-01-01 00-00-00 (2)」)。number が 1 以下なら
// そのまま返す。
// 同じ秒の中で録画を止めてまた始めると、そのままでは前のセッションのフォルダーで新しい録画ログを開き、録画ファイルの作成を
// 待っている前のセッションの録画ログを切り詰めてしまう。番号の形は、Windows がファイルのコピーに付ける「 (2)」に
// 合わせた。
inline std::string numbered_session_dir_name(const std::string &base_name, int number)
{
	if (number <= 1)
		return base_name;
	return base_name + " (" + std::to_string(number) + ")";
}

// 名前が録画ごとのフォルダー (session_dir_name の形「2000-01-01 00-00-00」、numbered_session_dir_name の「 (2)」付きを含む) か。
// 利用者が選んだ保存先は、録画のほかのファイルも入るフォルダーでありうる (D:\ など) ので、中身が録画か見分けるために使う。
inline bool is_session_dir_name(const std::string &name)
{
	constexpr const char *kShape = "dddd-dd-dd dd-dd-dd";
	constexpr std::size_t kLength = 19;
	if (name.size() < kLength)
		return false;
	for (std::size_t i = 0; i < kLength; ++i) {
		const char c = name[i];
		if (kShape[i] == 'd' ? (c < '0' || c > '9') : c != kShape[i])
			return false;
	}
	if (name.size() == kLength)
		return true;
	// 「 (2)」の形の番号。
	if (name.size() < kLength + 4 || name.compare(kLength, 2, " (") != 0 || name.back() != ')')
		return false;
	for (std::size_t i = kLength + 2; i + 1 < name.size(); ++i)
		if (name[i] < '0' || name[i] > '9')
			return false;
	return true;
}

// ensure_session_open が、すでにある名前を飛ばしてセッションのフォルダーの名前を試す回数の上限。同じ秒に 100 の
// セッションがすでにあることはないので、上限に届くのは、すでにあると答え続ける壊れた関数のときだけである。
inline constexpr int kMaxSessionNameTries = 100;

// セグメントファイルの名前 `segments/<user_id>_<track>_<seq6>.mov` (中間ファイルのフォルダーからの相対パス)。
// seq は 6 桁の 0 詰めで、セッションの中で 1 から増える。stream_id は、再接続すると同じ番号が別の人に割り当てられる
// ので使わない。表示名は、まだ読めていない間に始まるセグメントがあるので使わない。録画ログの `file` と同じ相対パスで
// 返すので、呼び出し元が組み立て直さなくてよい。
inline std::string segment_file_name(const std::string &user_id, TrackKind track, std::int64_t seq)
{
	return std::string(kSegmentsDirName) + "/" + user_id + "_" + track_name(track) + "_" +
	       detail::zero_pad(seq, kSegmentSeqDigits) + ".mov";
}

// 黒埋め用のフレームのファイルの名前 `black/<user_id>_<encoder_instance_id>.mov` (中間ファイルのフォルダーからの
// 相対パス)。黒埋め用のフレームはエンコーダのインスタンスごとに 1 つなので、この名前で重ならない。
inline std::string black_file_name(const std::string &user_id, const std::string &encoder_instance_id)
{
	return std::string(kBlackDirName) + "/" + user_id + "_" + encoder_instance_id + ".mov";
}

namespace detail {

// ファイルの名前に使えない文字 (制御文字と `< > : " / \ | ? *`) を `_` に置き換え、前後の空白と点を除く (Windows の
// ファイルの名前は、末尾に点と空白を持てない)。長さは切り詰めない。
inline std::string clean_file_name_part(const std::string &source)
{
	std::string out;
	out.reserve(source.size());
	for (const char raw : source) {
		const unsigned char ch = static_cast<unsigned char>(raw);
		const bool forbidden = ch < 0x20 || ch == 0x7F || raw == '<' || raw == '>' || raw == ':' ||
				       raw == '"' || raw == '/' || raw == '\\' || raw == '|' || raw == '?' ||
				       raw == '*';
		out.push_back(forbidden ? '_' : raw);
	}
	std::size_t begin = 0;
	while (begin < out.size() && (out[begin] == ' ' || out[begin] == '.'))
		++begin;
	std::size_t end = out.size();
	while (end > begin && (out[end - 1] == ' ' || out[end - 1] == '.'))
		--end;
	return out.substr(begin, end - begin);
}

} // namespace detail

// 表示名を、ファイルの名前に使える形にする。使えない文字を `_` に置き換え、前後の空白と点を除き、
// kDisplayNameMaxBytes で切る (多バイトの文字の途中では切らない)。空 (まだ読めていない) なら user_id を仮の名前に
// する。予約されたデバイス名 (CON など) は避けない。録画ファイルの名前には必ず種類の語が付くので、予約された名前
// そのものにはならない。
inline std::string sanitize_display_name(const std::string &display_name, const std::string &user_id)
{
	auto clean = [](const std::string &source) {
		std::string out = detail::clean_file_name_part(source);
		out.resize(detail::utf8_truncate_len(out, kDisplayNameMaxBytes));
		// 切った後の末尾に、また空白や点が来ることがある。
		while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
			out.pop_back();
		return out;
	};

	std::string name = clean(display_name);
	if (!name.empty())
		return name;
	name = clean(user_id);
	if (!name.empty())
		return name;
	// 表示名も user_id も空になることは想定していないが、名前のないファイルは作れない。
	return "unnamed";
}

namespace detail {

// 書式の %1、%2、%3 を 1 回の走査で置き換える (置き換えた文字列の中の `%2` を、もう 1 度置き換えない)。
// ほかの指定 (%4 など) と単独の % はそのまま残す。
inline std::string fill_three(const std::string &format, const std::string &a1, const std::string &a2,
			      const std::string &a3)
{
	std::string out;
	out.reserve(format.size() + a1.size() + a2.size() + a3.size());
	for (std::size_t i = 0; i < format.size(); ++i) {
		if (format[i] == '%' && i + 1 < format.size()) {
			const char next = format[i + 1];
			if (next == '1' || next == '2' || next == '3') {
				out += next == '1' ? a1 : (next == '2' ? a2 : a3);
				++i;
				continue;
			}
		}
		out.push_back(format[i]);
	}
	return out;
}

} // namespace detail

// 録画ファイルの名前。人の名前、種類の語、セッションの日時を書式で並べ、`.mov` を付ける。
// 書式と語は、呼び出し元が ini から読んで渡す。語の並びと区切りは言語によって変わるので、コードに書かない。空なら
// record_types.h の既定 (en-US.ini と同じ文字列) を使う。置き換えた後の全体からも、ファイルの名前に使えない文字を
// 除く (語や書式に使えない文字が入っていてもファイルを作れるようにするため)。長さは切り詰めない。日時まで切ると
// どのセッションか分からなくなり、名前の部分はすでに kDisplayNameMaxBytes で切ってある。
// sanitized_name は、sanitize_display_name と resolve_name_collisions を通した後の名前。
inline std::string output_file_name_for_session(const std::string &format, const std::string &sanitized_name,
						 const std::string &kind_word, const std::string &session_label)
{
	const std::string used_format = format.empty() ? std::string(kDefaultOutputNameFormat) : format;
	std::string stem =
		detail::clean_file_name_part(detail::fill_three(used_format, sanitized_name, kind_word, session_label));
	if (stem.empty())
		stem = sanitized_name.empty() ? std::string("unnamed") : sanitized_name;
	return stem + ".mov";
}

// ファイルの名前が同じになった人どうしを分ける。names と user_ids は同じ順に並べる。
// 重なった名前には、`_` と user_id の先頭の kCollisionIdPrefixLen 文字を後ろに付ける。後ろに付けるのは、利用者が
// 一覧で読みたい表示名を先に置くためである。重なった全員に付ける (片方だけに付けると、どちらが誰か名前から分からない)。
// それでも重なれば user_id を全部使う。
// 長さが食い違う呼び出しは何もせずにそのまま返す (呼び出し元の誤りを、ここで隠さない)。
inline std::vector<std::string> resolve_name_collisions(std::vector<std::string> names,
							const std::vector<std::string> &user_ids)
{
	if (names.size() != user_ids.size())
		return names;

	auto count_same = [](const std::vector<std::string> &list, const std::string &value) {
		std::size_t count = 0;
		for (const std::string &item : list) {
			if (item == value)
				++count;
		}
		return count;
	};

	const std::vector<std::string> original = names;
	for (std::size_t i = 0; i < names.size(); ++i) {
		if (count_same(original, original[i]) < 2)
			continue;
		const std::string &id = user_ids[i];
		const std::string prefix = id.size() > kCollisionIdPrefixLen ? id.substr(0, kCollisionIdPrefixLen) : id;
		names[i] = original[i] + "_" + prefix;
	}

	// 先頭の文字だけでは分かれなかった人 (user_id の先頭が同じ) は、user_id を全部使う。
	const std::vector<std::string> once = names;
	for (std::size_t i = 0; i < names.size(); ++i) {
		if (count_same(once, once[i]) < 2)
			continue;
		names[i] = original[i] + "_" + user_ids[i];
	}
	return names;
}

// ---------------------------------------------------------------------------
// 録画が OFF の間に I/O の関数を呼ばないための判定
// ---------------------------------------------------------------------------

// 録画が ON のときだけ I/O の関数へ進む、という判定。録画が OFF の間は、ファイルもスレッドもハンドルも増やさない。
// テスト (test_record_gate.cpp) は、この判定を常に true を返す関数に差し替えると、OFF のままでも open_file が
// 呼ばれることも確認する。判定を外しても失敗しないテストでは、OFF の間に I/O の関数を呼ばないことを確認したことに
// ならないためである。判定を外すフラグを製品のコードに置かず、テストの側が関数を差し替える形にするのは、そのフラグ
// 自身が誤って切り替わる経路になるためである。
inline bool recording_gate(bool recording_enabled)
{
	return recording_enabled;
}

// 判定を差し替えるための関数の型 (製品は既定の recording_gate を使う)。
using RecordingGateFn = bool (*)(bool);

// このセッションのフォルダーと録画ログ。開いていない間は、RecordPlatformOps の関数を 1 つも呼んでいない。
struct SessionState {
	bool open = false;
	std::string session_id;    // 「2000-01-01 00-00-00」(同じ秒の 2 つ目には「 (2)」が付く)
	std::string session_dir;   // `<保存先>/<session_id>` (UTF-8 の絶対パス)
	std::string materials_dir; // `<session_dir>/<kMaterialsDirName>` (録画ログ、セグメント、黒埋め用のフレーム)
	FileHandle ledger = kInvalidFile;
	std::int64_t started_at_wall_us = 0;
	std::int64_t started_at_steady_us = 0;
	// 直前に開いたセッションの日時の名前と、その名前で何番目か (numbered_session_dir_name)。
	// セッションをまたいで残す (close_session は消さない)。
	std::string last_base_name;
	int same_name_count = 0;
	// 最後に開くのに失敗したときの、セッションのフォルダーを作るフォルダー (`<保存先>/cordmux`)。開けたセッションで
	// 空に戻す。
	std::string failed_root;
};

// セッションのフォルダーの中の並び (録画ファイルのフォルダーと、録画ログとセグメントのフォルダー)。
struct SessionLayout {
	std::string session_dir;   // セッションのフォルダー (録画ファイルを置く)
	std::string materials_dir; // 録画ログとセグメントのフォルダー
};

// セッションの並び (中間ファイルは 1 つ下のフォルダーに置く)。
inline SessionLayout session_layout_of(const std::string &session_dir)
{
	SessionLayout layout;
	layout.session_dir = session_dir;
	layout.materials_dir = join_path(session_dir, kMaterialsDirName);
	return layout;
}

// 録画が ON なら、このセッションのフォルダー (`<日時>/` と、その下の中間ファイルのフォルダー、`segments/`、
// `black/`) を作り、録画ログを開く。OFF の間は RecordPlatformOps の関数を 1 度も呼ばない。すでに開いていれば何もせずに
// true を返す。開けなかったときは、書けなかった場所を画面に表示するために、`state.failed_root` にセッションの
// フォルダーを作るフォルダーを残す。別の場所へは移さない (移すと、利用者が録画の場所を見失う)。
inline bool ensure_session_open(SessionState &state, const RecordPlatformOps &ops, const std::string &base_dir,
				bool recording_enabled, int utc_offset_sec, RecordingGateFn gate = &recording_gate)
{
	if (!gate(recording_enabled))
		return false;
	if (state.open)
		return true;
	if (!ops.make_directories || !ops.open_file || !ops.now_wall_us || !ops.now_steady_us)
		return false;

	const std::int64_t wall_us = ops.now_wall_us();
	const std::string base_name = session_dir_name(wall_us, utc_offset_sec);
	int number = base_name == state.last_base_name ? state.same_name_count + 1 : 1;
	const std::string root = base_dir; // 保存先そのものが、セッションのフォルダーを作る場所

	if (base_dir.empty()) {
		state.failed_root.clear(); // 保存先を解決できていない (相対パスでは作らない。場所は表示できない)
		return false;
	}
	state.failed_root = root;

	// セッションのフォルダーには、まだない名前だけを使う。すでにあるフォルダー (同じ秒に始めたもう 1 つの OBS の
	// セッション、時計が戻った後の同じ秒の名前) を選ぶと、下の open_file (すでにあれば中身を捨てる) がその
	// セッションの録画ログを消してしまう。そこで、すでにあれば作らない関数でフォルダーを 1 階層だけ作り、
	// すでにあれば「 (2)」「 (3)」... の名前を順に試す。作れたと答えるのは、同じ名前を同時に作った 2 つのプロセスの
	// 片方だけなので、2 つの OBS が同じ秒に始めても取り違えない。上限 (kMaxSessionNameTries) を超えたら始めない。
	// あるかどうかを先に確認してから作る形にしないのは、確認してから作るまでの間に、もう 1 つの OBS が同じ名前を
	// 作りうるためである。録画ログを「すでにあれば開かない」形で開く方法も使わない。中間ファイルのフォルダーに
	// 録画ログがないフォルダー (残っていたフォルダー) を選んでしまい、ほかのセッションのファイルと同じフォルダーに
	// セグメントが混ざるためである。使用中のマーカーでも
	// 防げない (マーカーはセッションのフォルダーの中にあるので、フォルダーを取り違えた後では遅い)。
	if (ops.make_new_directory) {
		if (!ops.make_directories(root))
			return false;
		bool created = false;
		for (int tries = 0; tries < kMaxSessionNameTries; ++tries, ++number) {
			bool already_exists = false;
			if (ops.make_new_directory(join_path(root, numbered_session_dir_name(base_name, number)),
						   &already_exists)) {
				created = true;
				break;
			}
			if (!already_exists)
				return false; // 作れない (書けない保存先)。StartFailed にして場所を表示する
		}
		if (!created)
			return false;
	}
	const std::string session_id = numbered_session_dir_name(base_name, number);
	const std::string session_dir = join_path(root, session_id);
	const SessionLayout layout = session_layout_of(session_dir);
	if (!ops.make_directories(session_dir))
		return false;
	if (!ops.make_directories(layout.materials_dir))
		return false;
	if (!ops.make_directories(join_path(layout.materials_dir, kSegmentsDirName)))
		return false;
	if (!ops.make_directories(join_path(layout.materials_dir, kBlackDirName)))
		return false;

	const FileHandle ledger = ops.open_file(join_path(layout.materials_dir, kLedgerFileName));
	if (ledger == kInvalidFile)
		return false;

	state.failed_root.clear();
	state.last_base_name = base_name;
	state.same_name_count = number;
	state.open = true;
	state.session_id = session_id;
	state.session_dir = session_dir;
	state.materials_dir = layout.materials_dir;
	state.ledger = ledger;
	state.started_at_wall_us = wall_us;
	state.started_at_steady_us = ops.now_steady_us();
	return true;
}

// 録画ログに 1 行追加する (行末の改行はここで付ける)。OFF の間は RecordPlatformOps の関数を呼ばない
// (ensure_session_open と同じ判定を通す)。
inline bool append_ledger_line(const SessionState &state, const RecordPlatformOps &ops, const std::string &line,
			       bool recording_enabled, RecordingGateFn gate = &recording_gate)
{
	if (!gate(recording_enabled))
		return false;
	if (!state.open || state.ledger == kInvalidFile || !ops.append)
		return false;
	std::string text = line;
	if (text.empty() || text.back() != '\n')
		text.push_back('\n');
	return ops.append(state.ledger, text.data(), text.size());
}

// 録画ログを閉じる (止めるときと、次のセッションを開く前)。録画が ON かの判定は通さない。開いたものを閉じる処理は、
// 録画が OFF になった後にも必要なためである。
inline void close_session(SessionState &state, const RecordPlatformOps &ops)
{
	if (state.ledger != kInvalidFile && ops.close)
		ops.close(state.ledger);
	state.ledger = kInvalidFile;
	state.open = false;
}

} // namespace cordmux::record
