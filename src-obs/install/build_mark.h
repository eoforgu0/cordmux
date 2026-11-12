// SPDX-License-Identifier: Apache-2.0

// マーカーのファイル cordmux-build.txt を読み書きし、data/vencord の状態を判定する (classify_shown_bundle)。
// CI が data/vencord-src と data/vencord の両方に置き、cordmux.dll が起動時と、Vesktop の設定を調べるたびに読む。
// ファイルも時計も読まない (読むのは呼び出し元)。libobs にも Qt にも依存しない。

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace cordmux::install {

// マーカーのファイルの名前。data/vencord-src (コピー元) と data/vencord (Vesktop が読むフォルダー) の両方に、
// 同じ名前で置く。
inline constexpr const char *kBuildMarkFileName = "cordmux-build.txt";

// キーの名前。CI のツールもこの名前で書く。
//
// ファイルの書式 (CI が書き、cordmux.dll が読む):
//   - UTF-8、改行は LF で、1 行に 1 つの「キー: 値」を書く。
//   - キーは build_id (ビルドの識別。<version>+<短いコミット>)、vencord_commit
//     (src-vencord/vencord.lock.json の固定コミット)、vencord_tag (同じファイルの tag。Vencord のバージョンの
//     文字列で、「バージョン情報」のページの「同梱のVencord」(cordmux.settings.version_bundled_vencord.label)
//     に表示する) の 3 つで、この順に書く。例:
//       build_id: 0.1.0+abc1234
//       vencord_commit: 0123456789abcdef0123456789abcdef01234567
//       vencord_tag: v1.15.9
//   - 読む側は、書き方の細かい違いを許す。行末の CR、知らないキーの行、":" のない行、空の行は無視し、キーと
//     値の前後の空白 (半角の空白とタブ) を除く (":" の後の空白は 0 個でも 2 個以上でもよい)。ファイルの先頭の
//     UTF-8 の BOM も除く (Windows のツールで書き直すと付くことがある)。build_id と vencord_commit と
//     vencord_tag のどれかがないか、値が空なら「読めない」(nullopt)。
//   - 同じキーが 2 回以上あれば「読めない」にする (前の行も後の行も使わない)。CI はそういうファイルを書かない
//     ので、あれば壊れたか手で書き換えたもので、どちらの値が正しいかは分からない。読めないときは、画面に
//     「-」を表示する。
//   - キーの大文字と小文字は区別する (書くのは CI だけで、キーの名前はこのファイルの定数にしかない)。
//   vencord_tag は見せるだけの文字列で、更新と Force Update の見分け (operator==) にもバージョンの食い違いの判定にも
//   使わない (どちらも build_id と vencord_commit のまま)。
inline constexpr const char *kBuildMarkKeyBuildId = "build_id";
inline constexpr const char *kBuildMarkKeyVencordCommit = "vencord_commit";
inline constexpr const char *kBuildMarkKeyVencordTag = "vencord_tag";

struct BuildMark {
	std::string build_id;
	std::string vencord_commit;
	// Vencord のバージョンの文字列 (例 v1.15.9)。読めた印では空でない。
	std::string vencord_tag;
};

// 比べるのは build_id と vencord_commit だけである。vencord_tag は vencord_commit から決まる文字列なので、比べても
// 分かることは増えない。
inline bool operator==(const BuildMark &a, const BuildMark &b)
{
	return a.build_id == b.build_id && a.vencord_commit == b.vencord_commit;
}

inline bool operator!=(const BuildMark &a, const BuildMark &b)
{
	return !(a == b);
}

// ファイルの中身を読む。読めなければ nullopt を返す (読み方は上の書式のコメントにある)。
std::optional<BuildMark> parse_build_mark(std::string_view text);

// ファイルの中身を作る (キーは上の順で、各行の末尾は LF)。parse_build_mark で読むと同じマーカーに戻る。
// 値は 3 つとも空にしない (空だと、読んでも同じマーカーに戻らない)。値に改行を含めない (含めると、読んでも
// 同じマーカーに戻らない。CI が書く値は、1 行の識別と 16 進数とバージョンの文字列である)。
std::string build_mark_to_text(const BuildMark &mark);

// data/vencord の判定の結果。
enum class ShownBundleVerdict {
	// 判定できない。data/vencord-src のマーカーが読めないか、data/vencord-src と data/vencord を比べられなかった
	// (platform::BundleState::Unknown)。何もしない (分からないときに、違うと表示しないため)。
	Unknown,
	// data/vencord-src と一致している。何もしない。
	Matches,
	// マーカーが違い、data/vencord のマーカーは cordmux のもの。前のバージョンの cordmux のファイルが残っている
	// (展開が途中で止まったときなど)。メッセージを表示せずに data/vencord-src からコピーし、ログに 1 行書く
	// (更新のたびに警告を表示しないため)。
	StaleOwnBundle,
	// マーカーが同じでハッシュが違うか、data/vencord にマーカーがなくハッシュが違う。ほかのツール (Vesktop の
	// Force Update など) が上書きした。違うファイルの名前を表示し、[再インストール]
	// (cordmux.dock.reinstall.button) を勧める。
	Overwritten,
};

// data/vencord を判定する。
//   original     - data/vencord-src のマーカー (読めなければ nullopt)
//   shown        - data/vencord のマーカー (ないか読めなければ nullopt。読めないマーカーは cordmux のものと
//                  言えないので、ないのと同じに扱う)
//   hashes_match - Vesktop が読む 4 つのファイルを比べた結果 (true なら一致、false なら違う、nullopt なら
//                  比べられなかった = platform::BundleState::Unknown)
//
//   original   shown              hashes_match   結果
//   ---------  -----------------  -------------  --------------
//   nullopt    (どれでも)         (どれでも)     Unknown
//   あり       (どれでも)         nullopt        Unknown
//   あり       あり != original   true / false   StaleOwnBundle
//   あり       あり == original   false          Overwritten
//   あり       nullopt            false          Overwritten
//   あり       あり == original   true           Matches
//   あり       nullopt            true           Matches
//
// マーカーが違えば、ハッシュによらず StaleOwnBundle にする。Force Update はマーカーのファイルを知らずに
// 上書きするので、マーカーが cordmux の別のバージョンのものであることは、上書きではなく、別のバージョンの
// ファイルが残っていることを示す (ハッシュが一致していても、コピーすればマーカーまでそろい、次から Matches に
// なる)。data/vencord にマーカーがなくハッシュが一致するときは、Vesktop が読む 4 つのファイルが
// data/vencord-src と同じで、コピーし直す必要がないので、Matches にする。
ShownBundleVerdict classify_shown_bundle(const std::optional<BuildMark> &original,
					 const std::optional<BuildMark> &shown, std::optional<bool> hashes_match);

} // namespace cordmux::install
