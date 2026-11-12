// SPDX-License-Identifier: Apache-2.0

// 声が二重に配信に入る構成を見つけるために、OBS のソースを読む (ledger/rules.cpp の DoubleAudio の入力)。OBS の
// 「アプリケーション音声キャプチャ」で Vesktop か Discord の音声を取り込んでいると、通話の声が、cordmux と同じ
// 配信に 2 回入る。cordmux はソースを読むだけで、削除も設定の変更もしない。
// window_setting_exe は libobs にも Qt にも依存しないので、テストから直接呼び出せる。read_double_audio_state の
// 実装 (double_audio.cpp) だけが libobs を呼ぶ。

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace cordmux::sources {

// アプリケーション音声キャプチャのソースの種類の ID と、取り込む先のウィンドウを持つ設定のキー
// (OBS 31.1.1 の plugins/win-wasapi/win-wasapi.cpp の info.id と OPT_WINDOW)。
inline constexpr const char *kAppAudioCaptureTypeId = "wasapi_process_output_capture";
inline constexpr const char *kAppAudioCaptureWindowKey = "window";

// ---- 純粋関数 (libobs に依存しない) ----

namespace detail {

// s の中の find を、前から順に重ならないように replace に置き換える。置き換えた後の文字列は、もう一度は
// 探さない (OBS 31.1.1 の libobs/util/dstr.c の dstr_replace と同じ)。
inline std::string replace_all(std::string s, std::string_view find, std::string_view replace)
{
	std::size_t pos = 0;
	while ((pos = s.find(find, pos)) != std::string::npos) {
		s.replace(pos, find.size(), replace);
		pos += replace.size();
	}
	return s;
}

} // namespace detail

// アプリケーション音声キャプチャの設定 kAppAudioCaptureWindowKey の値から、取り込む先の実行ファイルの名前を
// 取り出す。値は「タイトル:クラス:実行ファイルの名前」の形で、各フィールドの中の ':' は "#3A" に、'#' は "#22" に
// 置き換えてある。読み方は OBS 31.1.1 の libobs/util/windows/window-helpers.c の ms_build_window_strings と
// decode_str に合わせる。空のフィールドも 1 つと数え、4 つ目からのフィールドは読まない。フィールドが 3 つに
// 足りないときは、OBS も取り込む先を決めないので、空を返す。
inline std::string window_setting_exe(std::string_view window)
{
	const std::size_t first = window.find(':');
	if (first == std::string_view::npos)
		return {};
	const std::size_t second = window.find(':', first + 1);
	if (second == std::string_view::npos)
		return {};
	const std::size_t third = window.find(':', second + 1);
	const std::string_view exe =
		third == std::string_view::npos ? window.substr(second + 1) : window.substr(second + 1, third - second - 1);
	// decode_str と同じく "#3A" を先に戻す。"#22" を先に戻すと、タイトルの "#3A" という文字列 (保存すると
	// "#223A") が ':' になる。
	return detail::replace_all(detail::replace_all(std::string(exe), "#3A", ":"), "#22", "#");
}

// ---- OBS のソースを読む部分 (double_audio.cpp) ----

// read_double_audio_state が読んだ値。
struct DoubleAudioState {
	// 次の 3 つをすべて満たすアプリケーション音声キャプチャのソースの、音声ミキサーに表示される名前。配信の出力に
	// 出ている (obs_source_active)。ミュートでも音量 0% でもない。取り込む先の実行ファイルが、Vesktop か Discord の
	// もの (platform::is_discord_client_exe_name) である。2 つ以上あれば、obs_enum_sources が先に返したソースに
	// する。メッセージの見出しには 1 つしか入らず、利用者がそれをミュートすれば、次のポーリングで次のソースが入る。
	// なければ値を持たない。
	std::optional<std::string> discord_capture_name;
	// cordmux のソース (kCompositeSourceTypeId) が、配信の出力に出ているか (obs_source_active)。1 つもなければ
	// false。
	bool composite_active = false;
	// 出力に出ている cordmux のソースのうち、ミュートでも音量 0% でもないものが 1 つ以上あるか。cordmux の音が
	// 配信に出るのは、この値が true のときである。出力に出ていなければ false。
	bool composite_audible = false;
};

// 現在の OBS のソースを読む。ソースの一覧は obs_enum_sources で 1 回だけたどり、設定を読むのは、上の条件のうち
// 種類、配信の出力、ミュート、音量を満たしたアプリケーション音声キャプチャのソースだけにする。ドックのポーリングが
// 500 ms ごとに呼ぶためである。ソースもシーンも作らず、設定、音量、ミュートを変えない。UI スレッドから呼ぶ
// (dock/dock_poll.cpp)。
DoubleAudioState read_double_audio_state();

} // namespace cordmux::sources
