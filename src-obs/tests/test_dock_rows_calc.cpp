// SPDX-License-Identifier: Apache-2.0

// dock/name_check.h の単体テスト (Qt を起動しない)。参加者の一覧の行の名前の見分け、2 行目の部分の残し方、
// 状態のテキストと解像度を表示する境目、名前と状態のテキストの幅の分け方、クリックを捨てる時間、[...] の周りの
// 間隔、状態のテキストの短い形を確認する。人の名前はすべて架空のもの。

#include "doctest.h"

#include "dock/name_check.h"
#include "ledger/text_ids.h"

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

using namespace cordmux::dock::rows;

namespace {

// 末尾を keep 文字 (UTF-16 の単位) 残して省いた見え方。keep が全文の長さ以上なら省かない。
NameShown shown(const std::u16string &full, std::size_t keep)
{
	NameShown s;
	s.full = full;
	if (keep >= full.size()) {
		s.visible_prefix = full;
		s.elided = false;
	} else {
		s.visible_prefix = full.substr(0, keep);
		s.elided = true;
	}
	return s;
}

} // namespace

TEST_CASE("見えているテキストが同じになる 2 行だけを、中央を省く表示に替える")
{
	const std::vector<NameShown> rows = {
		shown(u"やまだたろう", 4),
		shown(u"やまだたかし", 4),
		shown(u"すずき", 10),
	};
	const std::vector<bool> middle = middle_elide_rows(rows);
	REQUIRE(middle.size() == 3);
	CHECK(middle[0]);
	CHECK(middle[1]);
	CHECK_FALSE(middle[2]);
}

TEST_CASE("見えているテキストがほかの人の名前の先頭に一致する行も、中央を省く表示に替える")
{
	// 最小幅で表示される「Sagisawa_Yu...」は、もう 1 人の「Sagisawa_Yui_s...」の先頭でもある。
	const std::vector<NameShown> rows = {
		shown(u"Sagisawa_Yuki", 11),           // Sagisawa_Yu...
		shown(u"Sagisawa_Yui_streaming", 15),  // Sagisawa_Yui_st... (Sagisawa_Yuki の先頭ではない)
		shown(u"Taro", 10),
	};
	const std::vector<bool> middle = middle_elide_rows(rows);
	CHECK(middle[0]);
	CHECK_FALSE(middle[1]);
	CHECK_FALSE(middle[2]);
}

TEST_CASE("全文が見えている行と、省いていてもほかの人の名前の先頭に一致しない行は替えない")
{
	const std::vector<NameShown> rows = {
		shown(u"Sagisawa", 20),              // 全文 (ほかの人の名前の先頭でもあるが、全文なので見分けられる)
		shown(u"Sagisawa_Yui_streaming", 8), // Sagisawa... (1 行目の全文と同じ先頭) -> 替える
		shown(u"たなかはなこ", 3),           // たなか... (ほかの人の名前の先頭ではない)
	};
	const std::vector<bool> middle = middle_elide_rows(rows);
	CHECK_FALSE(middle[0]);
	CHECK(middle[1]);
	CHECK_FALSE(middle[2]);
}

TEST_CASE("1 人だけのときは省いていても替えず、同じ名前の 2 人が全文で見えているときも替えない (2 行目のユーザー名で見分ける)")
{
	CHECK_FALSE(middle_elide_rows({shown(u"やまだたろう", 2)})[0]);
	const std::vector<bool> same = middle_elide_rows({shown(u"はなこ", 10), shown(u"はなこ", 10)});
	CHECK_FALSE(same[0]);
	CHECK_FALSE(same[1]);
	CHECK(middle_elide_rows({}).empty());
}

TEST_CASE("2 行目の部分の種類は ini のキーから決まる")
{
	namespace k = cordmux::ledger::text::keys;
	CHECK(part_kind_of(k::kDockRowVoiceStatus) == PartKind::Voice);
	CHECK(part_kind_of(k::kDockRowVoiceAbsentStatus) == PartKind::Voice);
	CHECK(part_kind_of(k::kDockRowDiscordNameStatus) == PartKind::DiscordName);
	CHECK(part_kind_of(k::kDockRowUsernameStatus) == PartKind::Username);
	CHECK(part_kind_of(k::kDockRowReceivingAccountStatus) == PartKind::Other);
	CHECK(part_kind_of("") == PartKind::Other);
}

TEST_CASE("2 行目の部分がすべて入れば、全文のままにする")
{
	const std::vector<PartIn> parts = {{PartKind::Voice, 50, -1}, {PartKind::DiscordName, 80, -1}};
	const std::vector<PartOut> out = fit_line2(parts, 50 + 6 + 80, 6);
	REQUIRE(out.size() == 2);
	CHECK(out[0].shown);
	CHECK(out[1].shown);
	CHECK(out[0].elide == ElideMode::None);
	CHECK(out[1].elide == ElideMode::None);
	CHECK(out[0].width_px == 50);
	CHECK(out[1].width_px == 80);
}

TEST_CASE("2 行目が入らなければ、Discord での名前の部分を先に落とす")
{
	const std::vector<PartIn> parts = {{PartKind::DiscordName, 120, -1}, {PartKind::Username, 110, 60}};
	const std::vector<PartOut> out = fit_line2(parts, 120, 6);
	CHECK_FALSE(out[0].shown);
	CHECK(out[1].shown);
	CHECK_FALSE(out[1].use_short);
	CHECK(out[1].width_px == 110);
}

TEST_CASE("2 行目がまだ入らなければ、ユーザー名の部分を値だけの短い形にする")
{
	const std::vector<PartIn> parts = {{PartKind::DiscordName, 120, -1}, {PartKind::Username, 110, 60}};
	const std::vector<PartOut> out = fit_line2(parts, 80, 6);
	CHECK_FALSE(out[0].shown);
	CHECK(out[1].use_short);
	CHECK(out[1].width_px == 60);
	CHECK(out[1].elide == ElideMode::None);
}

TEST_CASE("2 行目がそれでも入らなければ、ユーザー名の中央を省く")
{
	const std::vector<PartIn> parts = {{PartKind::DiscordName, 120, -1}, {PartKind::Username, 110, 60}};
	const std::vector<PartOut> out = fit_line2(parts, 40, 6);
	CHECK_FALSE(out[0].shown);
	CHECK(out[1].use_short);
	CHECK(out[1].width_px == 40);
	CHECK(out[1].elide == ElideMode::Middle);
}

TEST_CASE("2 行目に Discord での名前しかなければ、落とさずに末尾を省く (2 行目そのものが消えないように)")
{
	const std::vector<PartIn> parts = {{PartKind::DiscordName, 120, -1}};
	const std::vector<PartOut> out = fit_line2(parts, 70, 6);
	CHECK(out[0].shown);
	CHECK(out[0].width_px == 70);
	CHECK(out[0].elide == ElideMode::End);
}

TEST_CASE("2 行目の声の部分は落とさず、短い声は全文のまま残して、余りをユーザー名に与える")
{
	const std::vector<PartIn> parts = {
		{PartKind::Voice, 40, -1}, {PartKind::DiscordName, 100, -1}, {PartKind::Username, 150, 120}};
	const std::vector<PartOut> out = fit_line2(parts, 100, 6);
	CHECK(out[0].shown);
	CHECK(out[0].width_px == 40);
	CHECK(out[0].elide == ElideMode::None);
	CHECK_FALSE(out[1].shown);
	CHECK(out[2].use_short);
	CHECK(out[2].width_px == 100 - 6 - 40);
	CHECK(out[2].elide == ElideMode::Middle);
}

TEST_CASE("2 行目の声とユーザー名がどちらも長いときは等しく分け、声は末尾を、ユーザー名は中央を省く")
{
	const std::vector<PartIn> parts = {{PartKind::Voice, 200, -1}, {PartKind::Username, 220, 180}};
	const std::vector<PartOut> out = fit_line2(parts, 106, 6);
	CHECK(out[0].shown);
	CHECK(out[1].shown);
	CHECK(out[0].width_px == 50);
	CHECK(out[1].width_px == 50);
	CHECK(out[0].elide == ElideMode::End);
	CHECK(out[1].elide == ElideMode::Middle);
	// 幅の合計が、使える幅を超えない。
	CHECK(out[0].width_px + 6 + out[1].width_px <= 106);
}

TEST_CASE("2 行目のすべての部分の先頭が入る幅なら、どの部分も先頭の幅を下回らない (「声: ...」にしない)")
{
	// Yami、日本語、最小幅のドックの形 (2 行目 74 px、間隔 7 px): 声「声: kenji」は 48 px (先頭「声: k...」は
	// 36 px)、ユーザー名の値だけの形は 115 px (先頭「t...2」は 23 px)。等しい水位で分けると 33 px と 34 px になり、
	// 声が「声: ...」になる。
	const std::vector<PartIn> parts = {{PartKind::Voice, 48, -1, 36}, {PartKind::Username, 140, 115, 23}};
	const std::vector<PartOut> out = fit_line2(parts, 74, 7);
	REQUIRE(out.size() == 2);
	CHECK(out[0].shown);
	CHECK(out[1].shown);
	CHECK(out[1].use_short);
	CHECK(out[0].width_px >= 36);
	CHECK(out[1].width_px >= 23);
	CHECK(out[0].width_px + 7 + out[1].width_px <= 74);
	CHECK(out[0].width_px + 7 + out[1].width_px >= 73); // 余りを捨てない
	CHECK(out[0].elide == ElideMode::End);
	CHECK(out[1].elide == ElideMode::Middle);
}

TEST_CASE("2 行目の先頭の幅の下限は、水位より低ければ動作しない (広い幅では等しい水位で分ける)")
{
	const std::vector<PartIn> parts = {{PartKind::Voice, 200, -1, 30}, {PartKind::Username, 220, 180, 20}};
	const std::vector<PartOut> out = fit_line2(parts, 106, 6);
	CHECK(out[0].width_px == 50);
	CHECK(out[1].width_px == 50);
}

TEST_CASE("2 行目のすべての先頭も入らない幅では、ユーザー名を丸ごと落とし、声だけを残す")
{
	// 先頭の幅の合計は 36 + 7 + 23 = 66 > 50。等しい水位で分けると「声: ...」と「t...2」になり、誰の声かも、
	// 誰の行かも読めない。声は落とさず、1 行目に名前があるユーザー名を落とす。
	const std::vector<PartIn> parts = {{PartKind::Voice, 48, -1, 36}, {PartKind::Username, 140, 115, 23}};
	const std::vector<PartOut> out = fit_line2(parts, 50, 7);
	REQUIRE(out.size() == 2);
	CHECK(out[0].shown);
	CHECK(out[0].width_px == 48); // 部分の間隔も不要になる。全文が入るので省かない
	CHECK(out[0].elide == ElideMode::None);
	CHECK_FALSE(out[1].shown);
	CHECK(out[1].width_px == 0);
}

TEST_CASE("2 行目の声だけを残しても全文が入らなければ、声の末尾を省く")
{
	const std::vector<PartIn> parts = {{PartKind::Voice, 48, -1, 36}, {PartKind::Username, 140, 115, 23}};
	const std::vector<PartOut> out = fit_line2(parts, 40, 7);
	CHECK(out[0].shown);
	CHECK(out[0].width_px == 40);
	CHECK(out[0].elide == ElideMode::End);
	CHECK_FALSE(out[1].shown);
}

TEST_CASE("2 行目に声とユーザー名とほかの部分があり、先頭が入らない幅では、声だけを残す")
{
	const std::vector<PartIn> parts = {
		{PartKind::Voice, 48, -1, 36}, {PartKind::Username, 140, 115, 23}, {PartKind::Other, 90, -1, 40}};
	const std::vector<PartOut> out = fit_line2(parts, 50, 7);
	CHECK(out[0].shown);
	CHECK(out[0].width_px == 48);
	CHECK_FALSE(out[1].shown);
	CHECK_FALSE(out[2].shown);
}

TEST_CASE("2 行目に声がなければ、先頭が入らない幅ではユーザー名だけを残す")
{
	const std::vector<PartIn> parts = {{PartKind::Username, 140, 115, 23}, {PartKind::Other, 90, -1, 40}};
	const std::vector<PartOut> out = fit_line2(parts, 50, 7);
	CHECK_FALSE(out[1].shown);
	CHECK(out[0].shown);
	CHECK(out[0].use_short);
	CHECK(out[0].width_px == 50);
	CHECK(out[0].elide == ElideMode::Middle);
}

TEST_CASE("2 行目の先頭の幅の合計が入らなければ、ユーザー名の値だけの形が入る幅でも、声を残す")
{
	// 先頭の幅の合計は 40 + 7 + 30 = 77 > 70。ユーザー名の値だけの形 (60 px) は入るが、声を落とさない。
	const std::vector<PartIn> parts = {{PartKind::Voice, 90, -1, 40}, {PartKind::Username, 100, 60, 30}};
	const std::vector<PartOut> out = fit_line2(parts, 70, 7);
	CHECK(out[0].shown);
	CHECK(out[0].width_px == 70);
	CHECK(out[0].elide == ElideMode::End);
	CHECK_FALSE(out[1].shown);
}

TEST_CASE("2 行目にユーザー名がなければ、先頭が入らない幅でも落とさずに等しい水位で分ける")
{
	// 見分けに使う部分がないので、どちらかを落とす理由がない (声と、cordmux.dock.row_receiving_account.status の
	// テキストなど)。
	const std::vector<PartIn> parts = {{PartKind::Voice, 48, -1, 36}, {PartKind::Other, 140, -1, 40}};
	const std::vector<PartOut> out = fit_line2(parts, 50, 7);
	CHECK(out[0].shown);
	CHECK(out[1].shown);
	CHECK(out[0].width_px + 7 + out[1].width_px <= 50);
	CHECK(std::abs(out[0].width_px - out[1].width_px) <= 1);
	for (const PartOut &o : out)
		CHECK(o.width_px >= 0);
}

TEST_CASE("2 行目がユーザー名だけなら、先頭が入らない幅でも残して中央を省く")
{
	const std::vector<PartIn> parts = {{PartKind::Username, 140, 115, 23}};
	const std::vector<PartOut> out = fit_line2(parts, 15, 7);
	CHECK(out[0].shown);
	CHECK(out[0].width_px == 15);
	CHECK(out[0].elide == ElideMode::Middle);
}

TEST_CASE("2 行目の先頭の幅が全文より広い部分も、全文の幅を超えない")
{
	const std::vector<PartIn> parts = {{PartKind::Voice, 20, -1, 30}, {PartKind::Username, 140, 115, 23}};
	const std::vector<PartOut> out = fit_line2(parts, 60, 7);
	CHECK(out[0].width_px == 20);
	CHECK(out[0].elide == ElideMode::None);
	CHECK(out[1].width_px == 60 - 7 - 20);
}

TEST_CASE("2 行目の幅が 0 でも、負の幅を返さない")
{
	const std::vector<PartIn> parts = {{PartKind::Voice, 50, -1}, {PartKind::Other, 70, -1}};
	const std::vector<PartOut> out = fit_line2(parts, 0, 6);
	for (const PartOut &o : out)
		CHECK(o.width_px >= 0);
	CHECK(fit_line2({}, 100, 6).empty());
}

TEST_CASE("行の状態のテキストは計算した境目から表示し、解像度は 32 em から表示する")
{
	// [密度] が [通常] でフォントサイズが 10 のときの形: 1 em は 13.33 px、チェックボックスのインジケーターと
	// 空き 20、名前の幅の下限 53、アイコン 16、[...] 22、小さい余白 3、大きい余白 7、フォーカスリング 2 と空き 1。
	const int dots_gap = dots_gap_px(7, 2, 1);
	const int min_row = min_row_width(20, 53, 16, 22, 3, 7, dots_gap);
	const int keep = 80; // 状態のテキストに残す幅の例 (式を確認するための値。製品の値は次のテストで確認する)
	const int status_min = status_text_min_width(min_row, keep, 7);
	// 境目 = チェックと空き + 名前の幅の下限 + 大 + アイコン + 小 + 残す幅 + 大 + 共有のアイコン + 共有のアイコンと
	// [...] の間 + [...]。
	CHECK(status_min == 20 + 53 + 7 + 16 + 3 + 80 + 7 + 16 + dots_gap + 22);
	const int res_min = 427; // 32 em
	CHECK_FALSE(width_rules(status_min - 1, status_min, res_min).status_text);
	CHECK(width_rules(status_min, status_min, res_min).status_text);
	CHECK_FALSE(width_rules(res_min - 1, status_min, res_min).resolution);
	CHECK(width_rules(res_min, status_min, res_min).resolution);
	// 267 px のドック (行は約 256 px) ではテキストを表示し、230 px のドック (行は約 219 px) では表示しない。
	CHECK(width_rules(256, status_min, res_min).status_text);
	CHECK_FALSE(width_rules(219, status_min, res_min).status_text);
	// 境目は 20 em (267 px) より狭い。
	CHECK(status_min < 267);
}

TEST_CASE("状態のテキストに残す幅は日本語の先頭がすべて入る 77 px で、267 px のドックではスクロールバーが出てもテキストを表示する")
{
	// Yami、[密度] が [通常]、フォントサイズ 10 のときに、プレビューツールで測った値: 1 em は 13.33 px、
	// チェックボックスのインジケーターと空き 21、名前の幅の下限 53、アイコン 16、[...] 26、小さい余白 3、
	// 大きい余白 7、フォーカスリング 2 と空き 1、ドックの外周 11、スクロールバー 12。
	const double em = 10.0 * 96.0 / 72.0;
	const int keep = static_cast<int>(kStatusTextKeepEm * em + 0.5);
	// ラベルの幅を 1 px ずつ変えて描くと、日本語の先頭「映像を待って...」が入るいちばん狭い幅は 77 px。これより
	// 狭いと状態の先頭が読めなくなり、広いと下の 267 px の場合にテキストが消える。
	CHECK(keep == 77);
	const int min_row = min_row_width(21, 53, 16, 26, 3, 7, dots_gap_px(7, 2, 1));
	const int status_min = status_text_min_width(min_row, keep, 7);
	const int res_min = static_cast<int>(kResolutionMinEm * em + 0.5);
	const int edge = 11;
	const int bar = 12;
	// 267 px: スクロールバーがないとき (行 245) も、あるとき (行 233。メッセージが高くなって一覧をスクロールできる
	// 形) もテキストを表示する。
	CHECK(width_rules(267 - 2 * edge, status_min, res_min).status_text);
	CHECK(width_rules(267 - 2 * edge - bar, status_min, res_min).status_text);
	// 230 px: スクロールバーの有無によらず、アイコンだけを表示する。
	CHECK_FALSE(width_rules(230 - 2 * edge, status_min, res_min).status_text);
	CHECK_FALSE(width_rules(230 - 2 * edge - bar, status_min, res_min).status_text);
	// 境目では、名前は下限 (53)、状態のテキストは残す幅 (77) を取る。名前と状態のテキストが分け合う幅は、境目から、
	// 名前の外のウィジェットの幅 (最小幅 - 名前の幅の下限) と、状態のテキストの後の大きい余白を引いた残り。
	const NameStatusSplit at_floor = split_name_status(status_min - (min_row - 53) - 7, 120, 189, 53, keep);
	CHECK(at_floor.name_px == 53);
	CHECK(at_floor.status_px == keep);
}

TEST_CASE("行が現れたか動いた後、ダブルクリックの時間の間だけクリックを捨てる")
{
	CHECK(press_blocked(1000, 1000, 500));
	CHECK(press_blocked(1000, 1499, 500));
	CHECK_FALSE(press_blocked(1000, 1500, 500));
	CHECK_FALSE(press_blocked(0, 100, 500)); // 時刻がない
	CHECK_FALSE(press_blocked(1000, 1100, 0));
}

TEST_CASE("[...] の中心に置いた直径 24 px の円が、チェックの押せる範囲と交わらないかを判定する")
{
	// [密度] が [通常] の btn-tool は約 22 px 四方。中心から左へ 12 px は、ボタンの半分の 11 px より 1 px 外に出る
	// ので、1 px 空ければ足りる。
	CHECK(dots_circle_clear(100, 101, 22));
	CHECK_FALSE(dots_circle_clear(100, 100, 22));
	// [クラシック] で 16 px なら 4 px 必要。
	CHECK(dots_circle_clear(100, 104, 16));
	CHECK_FALSE(dots_circle_clear(100, 103, 16));
}

TEST_CASE("一覧の 1 行の最小幅は、各ウィジェットの幅と間の余白の和になる")
{
	// チェックボックスのインジケーターと空き 20 + 名前の幅の下限 53 + (大 7 + 状態のアイコン 16) +
	// (小 3 + 共有のアイコン 16) + (共有のアイコンと [...] の間 7 + [...] 22)。名前とその右のまとまりの間は
	// 大きい余白 (別の情報の境)、共有のアイコンと [...] の間は、フォーカスリングの外に背景が残る幅。
	CHECK(min_row_width(20, 53, 16, 22, 3, 7, 7) == 20 + 53 + 7 + 16 + 3 + 16 + 7 + 22);
	// 大きい余白が小さい余白より広い分だけ、最小幅が広がる (間隔を数え落とすと、いちばん狭いドックで [...] が
	// 切れる)。
	CHECK(min_row_width(20, 53, 16, 22, 3, 7, 3) - min_row_width(20, 53, 16, 22, 3, 3, 3) == 4);
}

TEST_CASE("共有のアイコンと [...] の間は大きい余白で、それがリングと空きと 1 px の和より狭いときはその和にする")
{
	CHECK(dots_gap_px(7, 2, 1) == 7); // フォントサイズ 10: 大 7 > リング 2 + 空き 1 + 背景 1
	CHECK(dots_gap_px(5, 2, 1) == 5); // フォントサイズ 8
	CHECK(dots_gap_px(3, 2, 1) == 4); // 大きい余白が 4 px を下回るフォントでも、リングの外に背景を 1 px 残す
	CHECK(dots_gap_px(2, 3, 1) == 5);
}

TEST_CASE("行どうしの間は小さい余白で、それがリングと空きと 1 px の和より狭いときはその和にする")
{
	CHECK(row_gap_px(3, 2, 1) == 4); // フォントサイズ 10: 小さい余白 3 はリングの 3 px で使い切るので 4
	CHECK(row_gap_px(2, 2, 1) == 4); // 小さい余白が 2 px に丸まるフォント
	CHECK(row_gap_px(5, 2, 1) == 5); // 大きいフォントでは小さい余白のまま (リング + 1 px を超えている)
	CHECK(row_gap_px(4, 2, 1) == 4); // ちょうど
	CHECK(row_gap_px(3, 3, 2) == 6); // リングの太さと空きが変われば、それに合わせる
	CHECK(row_gap_px(3, -1, -1) == 3); // リングの値が負でも、小さい余白を下回らない
}

TEST_CASE("名前と状態のテキストの両方が全文で入れば全文にし、余りは名前に与える")
{
	const NameStatusSplit s = split_name_status(300, 100, 150, 53, 80);
	CHECK(s.status_px == 150);
	CHECK(s.name_px == 150);
	// 状態のテキストを表示していない行は、すべてを名前に与える。
	const NameStatusSplit none = split_name_status(120, 400, 0, 53, 80);
	CHECK(none.name_px == 120);
	CHECK(none.status_px == 0);
}

TEST_CASE("名前と状態のテキストが入らなければ、状態のテキストを下限まで先に縮め、名前の全文を残す")
{
	// 26 em のドックで、名前が下限 (4 em) まで縮み、状態のテキストが 189 px を取る形にしない。
	const NameStatusSplit s = split_name_status(250, 120, 189, 53, 80);
	CHECK(s.name_px == 120);
	CHECK(s.status_px == 130);
	// 名前がもっと長いと、状態のテキストは下限 (80) まで縮み、名前が残りを取る (名前の末尾を省く)。
	const NameStatusSplit longer = split_name_status(250, 400, 189, 53, 80);
	CHECK(longer.status_px == 80);
	CHECK(longer.name_px == 170);
	// 状態のテキストが下限より短ければ、下限は状態のテキストの全文になる。
	const NameStatusSplit short_status = split_name_status(250, 400, 60, 53, 80);
	CHECK(short_status.status_px == 60);
	CHECK(short_status.name_px == 190);
}

TEST_CASE("名前は下限 (4 em) を下回らず、下回るほど狭ければ状態のテキストが先に縮む")
{
	const NameStatusSplit s = split_name_status(100, 400, 189, 53, 80);
	CHECK(s.name_px == 53);
	CHECK(s.status_px == 47);
	const NameStatusSplit tiny = split_name_status(40, 400, 189, 53, 80);
	CHECK(tiny.name_px == 53);
	CHECK(tiny.status_px == 0);
	// 負の幅を返さない。
	const NameStatusSplit neg = split_name_status(-10, 400, 189, 53, 80);
	CHECK(neg.status_px == 0);
	CHECK(neg.name_px >= 0);
}

TEST_CASE("名前と状態のテキストが分け合う幅を広げても、名前も状態のテキストも狭くならない")
{
	const int names[] = {20, 53, 60, 120, 165, 400};
	const int statuses[] = {30, 79, 80, 81, 189, 500};
	for (int name : names) {
		for (int status : statuses) {
			NameStatusSplit before = split_name_status(0, name, status, 53, 80);
			for (int room = 1; room <= 800; ++room) {
				const NameStatusSplit now = split_name_status(room, name, status, 53, 80);
				CHECK(now.name_px >= before.name_px);
				CHECK(now.status_px >= before.status_px);
				CHECK(now.status_px <= status);
				before = now;
			}
		}
	}
}

TEST_CASE("行の状態のテキストは、映像待ち、打ち消しの状態、画質を下げている状態が短い形のキーを持つ")
{
	namespace k = cordmux::ledger::text::keys;
	const std::string not_receiving = k::kDockRowNotReceivingStatus;
	const std::string not_sharing = k::kDockRowNotSharingStatus;
	const std::string cant_show = k::kDockRowCantShowStatus;
	REQUIRE(status_short_key_for(not_receiving) != nullptr);
	REQUIRE(status_short_key_for(not_sharing) != nullptr);
	REQUIRE(status_short_key_for(cant_show) != nullptr);
	CHECK(std::string(status_short_key_for(not_receiving)) == k::kDockRowNotReceivingShortStatus);
	CHECK(std::string(status_short_key_for(not_sharing)) == k::kDockRowNotSharingShortStatus);
	// 「表示できない」は画質のテキストの側のキー (View の quality) だが、同じ関数で引ける。
	CHECK(std::string(status_short_key_for(cant_show)) == k::kDockRowCantShowShortStatus);
	const char *self_video_short = status_short_key_for(k::kDockRowSelfVideoMissingStatus);
	REQUIRE(self_video_short != nullptr);
	CHECK(std::string(self_video_short) == k::kDockRowSelfVideoMissingShortStatus);

	// 映像待ちと、画質を下げている 4 つの状態 (内訳が 3 つと、どの段階か分からないとき) も短い形を持つ。
	const std::pair<const char *, const char *> shorts[] = {
		{k::kDockRowWaitingStatus, k::kDockRowWaitingShortStatus},
		{k::kDockRowFpsReducedStatus, k::kDockRowFpsReducedShortStatus},
		{k::kDockRowResolutionReducedStatus, k::kDockRowResolutionReducedShortStatus},
		{k::kDockRowBothReducedStatus, k::kDockRowBothReducedShortStatus},
		{k::kDockRowQualityReducedStatus, k::kDockRowQualityReducedShortStatus}};
	for (const auto &[full, expected] : shorts) {
		const char *got = status_short_key_for(full);
		REQUIRE(got != nullptr);
		CHECK(std::string(got) == expected);
		// 短い形のキーから、さらに短い形を引かない (連鎖させない)。
		CHECK(status_short_key_for(expected) == nullptr);
	}
	// Vesktop のアカウントの行の「サブアカウント」は状態ではなく、短い形を持たない。
	CHECK(status_short_key_for(k::kDockRowReceivingAccountStatus) == nullptr);
	// 短い形のキーから、さらに短い形を引かない (連鎖させない)。空のキーも引かない。
	CHECK(status_short_key_for(k::kDockRowNotReceivingShortStatus) == nullptr);
	CHECK(status_short_key_for(k::kDockRowNotSharingShortStatus) == nullptr);
	CHECK(status_short_key_for(k::kDockRowCantShowShortStatus) == nullptr);
	CHECK(status_short_key_for(k::kDockRowSelfVideoMissingShortStatus) == nullptr);
	CHECK(status_short_key_for("") == nullptr);
}

TEST_CASE("ミュートのボタン 2 つの幅は、ボタンごとに小さい余白を足し、画面共有の音のボタンも数える")
{
	// 画面共有の音のボタンは、その人が画面共有の音を持たない間も場所を空けるので、最小の幅は 2 つ分で決まる。
	CHECK(audio_buttons_px(24, 3) == 2 * (24 + 3));
	CHECK(audio_buttons_px(0, 0) == 0);
	// ボタンが広がった分だけ最小の幅が 2 倍で広がる (数え落とすと、いちばん狭いドックでボタンが切れる)。
	CHECK(audio_buttons_px(26, 3) - audio_buttons_px(24, 3) == 4);
}

TEST_CASE("アバターは状態のテキストより広い幅でだけ表示する")
{
	// 名前に残す幅の下限が 52 px なら、アバターのある行は 39 px まで狭くしてよいので、境目は 13 px 下がる。
	CHECK(name_floor_with_avatar(52) == 39);
	CHECK(avatar_min_width(200, 20, 3, 52) == 210);
	CHECK(width_rules(210, 200, 500, 210).avatar);
	CHECK_FALSE(width_rules(209, 200, 500, 210).avatar);
	CHECK(width_rules(209, 200, 500, 210).status_text);
	// 境目を渡さないときは常に表示する。
	CHECK(width_rules(10, 200, 500).avatar);
}
