// SPDX-License-Identifier: Apache-2.0

// dock/ui/dock_ui_calc.h、dock_metrics.h、text_arg.h の計算の単体テスト (Qt を起動しない)。コントラスト比、
// メッセージのレベル 1 のパネルの枠線、テキストを省くときと折り返すときの切れ目、候補の選び方、余白の計算、差し込む値、
// ダイアログの主文の折り返しと高さ、入力を止める範囲の判定、パスの親のたどり方を確認する。

#include "doctest.h"

#include "dock/ui/dock_metrics.h"
#include "dock/ui/dock_style.h"
#include "dock/ui/dock_ui_calc.h"
#include "dock/ui/text_arg.h"
#include "ledger/dock_view.h"

#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

using namespace cordmux::dock::ui;
using namespace cordmux::dock::ui::calc;

namespace {

double round2(double v)
{
	return std::round(v * 100.0) / 100.0;
}

const Rgb kWhite{255, 255, 255};
const Rgb kBlack{0, 0, 0};
const Rgb kYamiDanger{0xC0, 0x1C, 0x37}; // Yami の危険の色
const Rgb kSystemDanger{192, 0, 0};      // System の .text-danger
const Rgb kRed2{0xE3, 0x3B, 0x57};       // Yami の red2
const Rgb kBrightRed{0xFF, 0x6B, 0x6B};  // 利用者が追加したテーマの危険の色の例
const Rgb kYamiBase{0x27, 0x2A, 0x33};   // Yami の bg_base (Yami.obt の grey6)

bool same(Rgb a, Rgb b)
{
	return a.r == b.r && a.g == b.g && a.b == b.b;
}

// 見えているテキスト (cut 個) を返す。
std::u16string kept(const std::u16string &text, int cut)
{
	return text.substr(0, static_cast<std::size_t>(cut));
}

} // namespace

TEST_CASE("contrast_ratio は、テーマの色の組み合わせで期待どおりのコントラスト比になる")
{
	CHECK(round2(contrast_ratio(kWhite, kYamiDanger)) == doctest::Approx(6.06));
	CHECK(round2(contrast_ratio(kBlack, kYamiDanger)) == doctest::Approx(3.46));
	CHECK(round2(contrast_ratio(kWhite, kSystemDanger)) == doctest::Approx(6.48));
	CHECK(round2(contrast_ratio(kBlack, kSystemDanger)) == doctest::Approx(3.24));
	CHECK(round2(contrast_ratio(kWhite, kRed2)) == doctest::Approx(4.18));
	CHECK(round2(contrast_ratio(kBlack, kRed2)) == doctest::Approx(5.03));
	CHECK(round2(contrast_ratio(kWhite, kBrightRed)) == doctest::Approx(2.78));
	CHECK(round2(contrast_ratio(kBlack, kBrightRed)) == doctest::Approx(7.57));
	// 引数の順によらない。同じ色は 1、白と黒は 21。
	CHECK(contrast_ratio(kYamiDanger, kWhite) == doctest::Approx(contrast_ratio(kWhite, kYamiDanger)));
	CHECK(contrast_ratio(kYamiDanger, kYamiDanger) == doctest::Approx(1.0));
	CHECK(contrast_ratio(kWhite, kBlack) == doctest::Approx(21.0));
	// Yami の危険の色のパネルと背景は 2.36。
	CHECK(round2(contrast_ratio(kYamiDanger, kYamiBase)) == doctest::Approx(2.36));
}

TEST_CASE("black_or_white_on は背景とのコントラスト比が高い方を選び、どの色の上でも 4.58 以上になる")
{
	CHECK(same(black_or_white_on(kYamiDanger), kWhite));
	CHECK(same(black_or_white_on(kSystemDanger), kWhite));
	CHECK(same(black_or_white_on(kRed2), kBlack));
	CHECK(same(black_or_white_on(kBrightRed), kBlack));
	// 白とのコントラスト比と黒とのコントラスト比の積は 21 なので、選んだ方は 21 の平方根以上になる。
	const double floor = std::sqrt(21.0);
	for (int r = 0; r <= 255; r += 17) {
		for (int g = 0; g <= 255; g += 17) {
			for (int b = 0; b <= 255; b += 17) {
				const Rgb c{r, g, b};
				CHECK(contrast_ratio(kWhite, c) * contrast_ratio(kBlack, c) == doctest::Approx(21.0));
				CHECK(contrast_ratio(black_or_white_on(c), c) >= floor - 1e-9);
			}
		}
	}
}

TEST_CASE("メッセージのレベル 1 のパネルの枠線は、パネルと背景のコントラスト比が 3:1 に届かないときだけ描く")
{
	const DockStyle style;
	// Yami: 2.36 < 3 なので 2 px。
	CHECK(l1_border_px(kYamiDanger, kYamiBase, style.l1_border_px, style.l1_border_auto_px,
			   style.l1_border_contrast_min) == 2);
	// 白の背景 (Light に近い): 6.06 なので描かない。
	CHECK(l1_border_px(kYamiDanger, kWhite, style.l1_border_px, style.l1_border_auto_px,
			   style.l1_border_contrast_min) == 0);
	// 0、1、2 を指定したら、コントラスト比によらずその値にする (見た目を比べるための指定)。
	for (int fixed : {0, 1, 2}) {
		CHECK(l1_border_px(kYamiDanger, kYamiBase, fixed, 2, 3.0) == fixed);
		CHECK(l1_border_px(kYamiDanger, kWhite, fixed, 2, 3.0) == fixed);
	}
}

TEST_CASE("mix_rgb は、a から b へ割合だけ寄せた色を返し、割合は 0 から 100 に収める")
{
	const Rgb a{40, 42, 51};
	const Rgb b{255, 255, 255};
	CHECK(same(mix_rgb(a, b, 0), a));
	CHECK(same(mix_rgb(a, b, 100), b));
	CHECK(same(mix_rgb(a, b, -5), a));
	CHECK(same(mix_rgb(a, b, 250), b));
	CHECK(same(mix_rgb(Rgb{0, 0, 0}, Rgb{200, 100, 50}, 50), Rgb{100, 50, 25}));
	// 7% 寄せた色 (カードのパネルの色) は、背景と同じではなく、背景に近い。
	const Rgb panel = mix_rgb(a, b, 7);
	CHECK_FALSE(same(panel, a));
	CHECK(panel.r > a.r);
	CHECK(panel.r < 100);
	// 本文の色 (白) の側へ寄せるほど、背景とのコントラスト比は大きくなる。
	CHECK(contrast_ratio(mix_rgb(a, b, 20), a) > contrast_ratio(panel, a));
}

TEST_CASE("弱いテキストの色は、背景とのコントラスト比が 4.5:1 に届くときだけ使う")
{
	// OBS 32.2.2 の Light の弱いテキストの色 #646464 と、ダイアログの背景 rgb(211, 211, 211) は 3.95。
	const Rgb muted_light{0x64, 0x64, 0x64};
	const Rgb window_light{211, 211, 211};
	CHECK(round2(contrast_ratio(muted_light, window_light)) == doctest::Approx(3.95));
	CHECK_FALSE(muted_text_allowed(muted_light, window_light));
	// OBS 32.2.2 の Yami の弱いテキストの色 #969696 と、背景 #272A33 は 4.85。
	const Rgb muted_yami{0x96, 0x96, 0x96};
	CHECK(round2(contrast_ratio(muted_yami, kYamiBase)) == doctest::Approx(4.85));
	CHECK(muted_text_allowed(muted_yami, kYamiBase));
}

TEST_CASE("数・時刻・人数の語の範囲を求める")
{
	SUBCASE("時刻、数、割合を 1 つの語にする")
	{
		const std::u16string t = u"開始 21:32";
		const auto spans = number_spans(t);
		REQUIRE(spans.size() == 1);
		CHECK(spans[0].start == 3);
		CHECK(spans[0].end == 8);
		CHECK(number_spans(u"1,024 件").at(0).end == 7);
		CHECK(number_spans(u"使用量 42% です").at(0).start == 4);
		CHECK(number_spans(u"使用量 42% です").at(0).end == 7);
	}
	SUBCASE("単位の語 (英字の 1 語か、漢字 1 字か 2 字) を数の語に含める")
	{
		CHECK(number_spans(u"1 person").at(0).end == 8);
		CHECK(number_spans(u"25 minutes left").at(0).end == 10);
		CHECK(number_spans(u"12 分").at(0).end == 4);
		CHECK(number_spans(u"8 本中 3 本").at(0).end == 4);
		// AM/PM の後に英字の 1 語が続けば、それも語に含める (正規表現と同じ)。
		CHECK(number_spans(u"10 AM").at(0).end == 5);
		CHECK(number_spans(u"10 AM today").at(0).end == 11);
	}
	SUBCASE("空白のない漢字の単位も語に含める (ja の ini の「%1人」「%2分間」)")
	{
		CHECK(number_spans(u"30分").at(0).end == 3);
		CHECK(number_spans(u"あと 2分間です").at(0).start == 3);
		CHECK(number_spans(u"あと 2分間です").at(0).end == 6);
		// 仮名は語に含めない (「2つ」の「つ」)。
		CHECK(number_spans(u"2つ").at(0).end == 1);
		bool paren = false;
		for (const Span &s : number_spans(u"人がいます (2人)")) {
			if (s.start == 6 && s.end == 10)
				paren = true;
		}
		CHECK(paren);
		// 省くときも単位ごと落とす (「30...」にしない)。
		const std::u16string t = u"残り 30分";
		CHECK(kept(t, safe_cut(t, 5)) == u"残り");
	}
	SUBCASE("「21:32 に」の仮名は語に含めない")
	{
		const auto spans = number_spans(u"21:32 に開始");
		REQUIRE(spans.size() == 1);
		CHECK(spans[0].end == 5);
	}
	SUBCASE("数字を含む括弧の組を 1 つの語にする")
	{
		const std::u16string t = u"表示中 (3 人)";
		bool paren = false;
		for (const Span &s : number_spans(t)) {
			if (s.start == 4 && s.end == 9)
				paren = true;
		}
		CHECK(paren);
		bool more = false;
		for (const Span &s : number_spans(u"たろう (+1 more)")) {
			if (s.start == 4 && s.end == 13)
				more = true;
		}
		CHECK(more);
		// 数字のない括弧は語にしない。
		CHECK(number_spans(u"声 (はなこ)").empty());
	}
	SUBCASE("全角の数字と、サロゲートペアの漢字も扱える")
	{
		CHECK(number_spans(u"３ 人").at(0).end == 3);
		// U+20BB7 (サロゲートペアの 2 単位) は、漢字の単位として語に含める。
		const std::u16string t = u"3 \U00020BB7x";
		CHECK(number_spans(t).at(0).end == 4);
	}
}

TEST_CASE("数の語と括弧の組を分けて求められる (折り返す側が括弧だけを外せるように)")
{
	// RecordSpaceLow の見出しの形 (ja)。語は「9分」だけで、括弧の組は括弧の全体。
	const std::u16string ja = u"空き容量が少なくなっています (録画できるのは残り約9分)";
	const auto words = number_word_spans(ja);
	REQUIRE(words.size() == 1);
	CHECK(kept(ja.substr(static_cast<std::size_t>(words[0].start)), words[0].end - words[0].start) == u"9分");
	const auto brackets = number_bracket_spans(ja);
	REQUIRE(brackets.size() == 1);
	CHECK(brackets[0].start == 15);
	CHECK(brackets[0].end == static_cast<int>(ja.size()));
	// en も同じ。語は「14 minutes」。
	const std::u16string en = u"Low space in the save folder (about 14 minutes of recording left)";
	const auto en_words = number_word_spans(en);
	REQUIRE(en_words.size() == 1);
	CHECK(kept(en.substr(static_cast<std::size_t>(en_words[0].start)), en_words[0].end - en_words[0].start) ==
	      u"14 minutes");
	CHECK(number_bracket_spans(en).size() == 1);
	// number_spans は両方をつないだもの (語が先、括弧が後。省く側はこれを使う)。
	const auto all = number_spans(ja);
	REQUIRE(all.size() == 2);
	CHECK(all[0].start == words[0].start);
	CHECK(all[1].start == brackets[0].start);
	// 割合は「%」が単位で、後ろの語を含めない (「0% volume」を 1 語にすると、折り返したときに行が増える)。
	const std::u16string pct = u"It was muted or at 0% volume";
	const auto pct_words = number_word_spans(pct);
	REQUIRE(pct_words.size() == 1);
	CHECK(kept(pct.substr(static_cast<std::size_t>(pct_words[0].start)), pct_words[0].end - pct_words[0].start) ==
	      u"0%");
	CHECK(number_word_spans(u"音量0%にしていた").at(0).end == 4);
	// 数字のない括弧は括弧の組にならず、括弧のないテキストには語だけがある。
	CHECK(number_bracket_spans(u"声 (はなこ)").empty());
	CHECK(number_bracket_spans(u"開始 21:32").empty());
	CHECK(number_word_spans(u"開始 21:32").size() == 1);
}

TEST_CASE("safe_cut は数の語の途中で切らない")
{
	SUBCASE("時刻の途中なら語の前まで戻し、末尾の空白も落とす")
	{
		const std::u16string t = u"開始 21:32";
		CHECK(kept(t, safe_cut(t, 5)) == u"開始");
		CHECK(kept(t, safe_cut(t, 7)) == u"開始");
	}
	SUBCASE("人数の括弧の途中なら括弧ごと落とす (「表示中 (3...」にしない)")
	{
		const std::u16string t = u"表示中 (3 人)";
		CHECK(kept(t, safe_cut(t, 6)) == u"表示中");
	}
	SUBCASE("語の外の切れ目はそのままにする")
	{
		const std::u16string t = u"21:32 に開始しました";
		CHECK(safe_cut(t, 7) == 7);
		// 「21:32 」で切るときは、末尾の空白だけを落とす。
		CHECK(safe_cut(t, 6) == 5);
	}
	SUBCASE("単位の語の途中なら、数と単位の語ごと落とす")
	{
		const std::u16string t = u"Waiting for 1 person";
		CHECK(kept(t, safe_cut(t, 17)) == u"Waiting for");
	}
	SUBCASE("すべてが語なら 0 を返す (Qt の省略に任せる)")
	{
		const std::u16string t = u"21:32";
		CHECK(safe_cut(t, 3) == 0);
	}
	SUBCASE("範囲の外の cut も扱える")
	{
		const std::u16string t = u"abc";
		CHECK(safe_cut(t, 0) == 0);
		CHECK(safe_cut(t, 3) == 3);
		CHECK(safe_cut(t, 10) == 3);
	}
	SUBCASE("サロゲートペアの間で切らない")
	{
		const std::u16string t = u"ab\U0001F600cd";
		CHECK(safe_cut(t, 3) == 2);
	}
}

TEST_CASE("safe_cut は en の短縮形の語の途中で切らない (否定の n't を落として逆の意味にしない)")
{
	const std::u16string t = u"The participant settings haven't been saved";
	SUBCASE("短縮形の語の途中なら語ごと落とし、末尾の空白も落とす")
	{
		// 「The participant settings have...」にしない。
		CHECK(kept(t, safe_cut(t, 29)) == u"The participant settings");
		// アポストロフィの直後 (「haven'...」) も同じ。
		CHECK(kept(t, safe_cut(t, 31)) == u"The participant settings");
	}
	SUBCASE("語の後ろで切るなら否定は残る")
	{
		CHECK(kept(t, safe_cut(t, 32)) == u"The participant settings haven't");
		// 次の語 been の途中は、Qt の 1 文字ずつの省略のまま。
		CHECK(kept(t, safe_cut(t, 35)) == u"The participant settings haven't be");
	}
	SUBCASE("右引用符の形のアポストロフィと、所有格の形も守る")
	{
		const std::u16string curly = u"Vesktop isn\u2019t running";
		CHECK(kept(curly, safe_cut(curly, 10)) == u"Vesktop");
		const std::u16string owner = u"This doesn't affect cordmux's video";
		CHECK(kept(owner, safe_cut(owner, 27)) == u"This doesn't affect");
	}
	SUBCASE("アポストロフィのない語と、英字にはさまれないアポストロフィは守らない")
	{
		const std::u16string plain = u"Disconnected from Vesktop";
		CHECK(safe_cut(plain, 15) == 15);
		const std::u16string quoted = u"the 'cordmux' folder";
		CHECK(contraction_spans(quoted).empty());
		CHECK(safe_cut(quoted, 8) == 8);
	}
	SUBCASE("すべてが 1 つの短縮形の語なら 0 を返す (Qt の省略に任せる)")
	{
		const std::u16string word = u"haven't";
		CHECK(safe_cut(word, 4) == 0);
	}
}

TEST_CASE("safe_cut は ja の打ち消しの述語の途中で切らず、述語の前の助詞の後まで戻す")
{
	// 切れ目は、見えているテキスト (先頭の文字の並び) で渡す。それが全文の先頭であることも確認する。
	const auto cut_after = [](const std::u16string &text, const std::u16string &head) {
		REQUIRE(text.compare(0, head.size(), head) == 0);
		return safe_cut(text, static_cast<int>(head.size()));
	};
	SUBCASE("打ち消しの述語の途中なら、その前の助詞の後まで戻す")
	{
		const std::u16string added = u"Vesktopにcordmuxを追加できませんでした";
		CHECK(kept(added, cut_after(added, u"Vesktopにcordmuxを追加できま")) == u"Vesktopにcordmuxを");
		CHECK(kept(added, cut_after(added, u"Vesktopにcordmuxを追")) == u"Vesktopにcordmuxを");
		const std::u16string not_added = u"Vesktopにcordmuxが追加されていません";
		CHECK(kept(not_added, cut_after(not_added, u"Vesktopにcordmuxが追加されて")) == u"Vesktopにcordmuxが");
		const std::u16string roster = u"参加者の一覧を更新できません";
		CHECK(kept(roster, cut_after(roster, u"参加者の一覧を更新できま")) == u"参加者の一覧を");
		const std::u16string unsaved = u"参加者の設定を保存できていません";
		CHECK(kept(unsaved, cut_after(unsaved, u"参加者の設定を保存できて")) == u"参加者の設定を");
		const std::u16string stopped = u"アンインストールを完了できませんでした";
		CHECK(kept(stopped, cut_after(stopped, u"アンインストールを完了でき")) == u"アンインストールを");
		// 「まだ」も述語に含める (「cordmuxのシーンがまだ作成されて...」は「作成されている」とも読める)。
		const std::u16string scenes = u"cordmuxのシーンがまだ作成されていません";
		CHECK(kept(scenes, cut_after(scenes, u"cordmuxのシーンがまだ作")) == u"cordmuxのシーンが");
		// 2 文字の助詞「から」の後ろ。
		const std::u16string session = u"Vesktopから接続できません";
		CHECK(kept(session, cut_after(session, u"Vesktopから接続できま")) == u"Vesktopから");
		const std::u16string ports = u"Vesktopからの接続を受け付けられません";
		CHECK(kept(ports, cut_after(ports, u"Vesktopからの接続を受け付けられ")) == u"Vesktopからの接続を");
		const std::u16string start = u"cordmuxの録画を開始できませんでした";
		CHECK(kept(start, cut_after(start, u"cordmuxの録画を開")) == u"cordmuxの録画を");
	}
	SUBCASE("連体形の打ち消し (「届いていない人」) も述語として守る")
	{
		const std::u16string t = u"画面共有の映像が届いていない人がいます (2人)";
		CHECK(kept(t, cut_after(t, u"画面共有の映像が届い")) == u"画面共有の映像が");
		CHECK(kept(t, cut_after(t, u"画面共有の映像が届いていな")) == u"画面共有の映像が");
		// 打ち消しの後ろで切れるなら戻さない (肯定の「います」の途中は Qt の省略のまま)。
		CHECK(kept(t, cut_after(t, u"画面共有の映像が届いていない人がい")) == u"画面共有の映像が届いていない人がい");
		// 人数の括弧の途中は、数の語のガードで括弧ごと落とす (2 つのガードが一緒に適用される)。
		CHECK(kept(t, cut_after(t, u"画面共有の映像が届いていない人がいます (2")) == u"画面共有の映像が届いていない人がいます");
	}
	SUBCASE("数の語と打ち消しの述語が並ぶときは、述語だけを落として数は残し、数の途中では切らない")
	{
		const std::u16string t = u"cordmuxの録画ファイルを作成するには、保存先の空き容量が約1.2 GB足りません";
		CHECK(kept(t, cut_after(t, u"cordmuxの録画ファイルを作成するには、保存先の空き容量が約1.2 GB足りま")) ==
		      u"cordmuxの録画ファイルを作成するには、保存先の空き容量が約1.2 GB");
		CHECK(kept(t, cut_after(t, u"cordmuxの録画ファイルを作成するには、保存先の空き容量が約1.2 G")) ==
		      u"cordmuxの録画ファイルを作成するには、保存先の空き容量が約");
	}
	SUBCASE("前の節の中で切れる形は守らず (短い形で防ぐ)、打ち消しの節の途中なら読点ごと戻す")
	{
		const std::u16string t = u"Vesktopは起動していますが、接続されていません";
		// 「Vesktopは起動していま...」は肯定に読めるが、打ち消しの述語の外なので動かさない
		// (vesktop_not_connected の短い形で防ぐ)。この確認は、ガードが壊れたと読み違えないためのもの。
		CHECK(safe_cut(t, static_cast<int>(std::u16string(u"Vesktopは起動していま").size())) ==
		      static_cast<int>(std::u16string(u"Vesktopは起動していま").size()));
		// 述語の途中なら「、」の後まで戻り、末尾の「、」も落とす (「...ますが、...」にしない)。
		CHECK(kept(t, cut_after(t, u"Vesktopは起動していますが、接続されて")) == u"Vesktopは起動していますが");
	}
	SUBCASE("述語の前で切れるときと、助詞の直後で切れるときは動かさない")
	{
		const std::u16string t = u"参加者の設定を保存できていません";
		CHECK(kept(t, cut_after(t, u"参加者の設")) == u"参加者の設");
		CHECK(kept(t, cut_after(t, u"参加者の設定を")) == u"参加者の設定を");
		CHECK(safe_cut(t, static_cast<int>(t.size())) == static_cast<int>(t.size()));
	}
	SUBCASE("述語が文の先頭から始まる (助詞がない) なら 0 を返す (Qt の省略に任せる)")
	{
		const std::u16string t = u"画面共有していません";
		CHECK(safe_cut(t, 5) == 0);
	}
	SUBCASE("打ち消しでない「ない」は述語にしない")
	{
		// 名前の中の「ない」(後ろが平仮名)。
		const std::u16string name = u"ないとうさん";
		CHECK(ja_negated_predicate_spans(name).empty());
		CHECK(safe_cut(name, 3) == 3);
		// 形容詞「少ない」(前が漢字)。
		const std::u16string few = u"空き容量が少ない";
		CHECK(ja_negated_predicate_spans(few).empty());
		// 肯定の文には範囲がない。
		CHECK(ja_negated_predicate_spans(u"Vesktopのcordmuxが上書きされています").empty());
		// en のテキストにも範囲がない。
		CHECK(ja_negated_predicate_spans(u"The cordmux settings haven't been saved").empty());
	}
	SUBCASE("範囲は、述語の先頭から「ませ」までと、後ろの「でした」の 2 つになる")
	{
		const std::u16string t = u"Vesktopにcordmuxを追加できませんでした";
		const std::vector<Span> s = ja_negated_predicate_spans(t);
		REQUIRE(s.size() == 2);
		CHECK(s[0].start == static_cast<int>(std::u16string(u"Vesktopにcordmuxを").size()));
		CHECK(s[0].end == static_cast<int>(std::u16string(u"Vesktopにcordmuxを追加できませ").size()));
		CHECK(s[1].start == static_cast<int>(std::u16string(u"Vesktopにcordmuxを追加できません").size()));
		CHECK(s[1].end == static_cast<int>(t.size()));
	}
	SUBCASE("次の文字が平仮名でない「か」は助詞とみなし (「誰の映像か確認できません」)、送り仮名の「か」は述語の中とみなす")
	{
		const std::u16string t = u"表示を止めています (誰の映像か確認できません)";
		CHECK(kept(t, cut_after(t, u"表示を止めています (誰の映像か確")) == u"表示を止めています (誰の映像か");
		const std::u16string w = u"名前が分からない人";
		CHECK(kept(w, cut_after(w, u"名前が分か")) == u"名前が");
	}
	SUBCASE("「ませ」の後ろで切れるなら打ち消しと読めるので動かさない")
	{
		const std::u16string slots = u"空いている枠がありません。";
		CHECK(kept(slots, cut_after(slots, u"空いている枠がありませ")) == u"空いている枠がありませ");
		CHECK(kept(slots, cut_after(slots, u"空いている枠がありま")) == u"空いている枠が");
		// 「ない」は「な」だけでは打ち消しと決まらないので、「な」の後ろでも戻す。
		const std::u16string video = u"画面共有の映像が届いていない人がいます (2人)";
		CHECK(kept(video, cut_after(video, u"画面共有の映像が届いていな")) == u"画面共有の映像が");
	}
	SUBCASE("「でした」だけが落ちる切れ目は打ち消しが残るので、「ません」の後ろまでにとどめる")
	{
		// 「cordmuxの設定を読み込めません...」を「cordmuxの設定を...」にしない。
		const std::u16string t = u"cordmuxの設定を読み込めませんでした";
		CHECK(kept(t, cut_after(t, u"cordmuxの設定を読み込めませんで")) == u"cordmuxの設定を読み込めません");
		CHECK(kept(t, cut_after(t, u"cordmuxの設定を読み込めません")) == u"cordmuxの設定を読み込めません");
		CHECK(kept(t, cut_after(t, u"cordmuxの設定を読み込めま")) == u"cordmuxの設定を");
	}
}

TEST_CASE("fit_forms は長い順の候補から、幅に入る最初のものを選ぶ")
{
	// 「通話中 (4 人) - 表示中 (3 人)」(210) と、後ろの区切りを落とした「通話中 (4 人)」(100)。
	const std::vector<int> widths{210, 100};
	CHECK(fit_forms(widths, 210).index == 0);
	CHECK_FALSE(fit_forms(widths, 210).elide);
	// 長い形が入らなければ、短い形を丸ごと表示する (途中で切らない)。
	CHECK(fit_forms(widths, 209).index == 1);
	CHECK(fit_forms(widths, 100).index == 1);
	CHECK_FALSE(fit_forms(widths, 100).elide);
	// いちばん短い形も入らないときだけ、その末尾を省く。
	CHECK(fit_forms(widths, 50).index == 1);
	CHECK(fit_forms(widths, 50).elide);
	CHECK(fit_forms({}, 50).index == -1);
	// 3 つの形で、真ん中が入る。
	CHECK(fit_forms({170, 110, 50}, 115).index == 1);
	// 候補が 1 つなら、入らなければそれを省く。
	CHECK(fit_forms({80}, 40).index == 0);
	CHECK(fit_forms({80}, 40).elide);
}

TEST_CASE("elide_form_index は、守る語の途中でしか切れない短い形の代わりに、1 つ前の候補を省く")
{
	const std::u16string full = u"画面共有の映像が届いていない人がいます (1人)";
	const std::u16string shorter = u"1人の映像が届いていません";
	const std::vector<std::u16string> forms{full, shorter};
	SUBCASE("短い形の Qt の切れ目が数と単位の間 (「1...」) なら全文を省く")
	{
		// 短い形は、数の語「1人」の先頭まで戻ると何も残らない (safe_cut が 0)。
		CHECK(safe_cut(shorter, 1) == 0);
		CHECK(elide_form_index(forms, {6, 1}) == 0);
		CHECK(kept(full, safe_cut(full, 6)) == u"画面共有の映");
	}
	SUBCASE("短い形を守って省けるなら、短い形を選ぶ")
	{
		CHECK(elide_form_index(forms, {6, 4}) == 1);
		CHECK(elide_form_index(forms, {0, 4}) == 1);
	}
	SUBCASE("どの候補も守って省けなければ、最後の候補を選ぶ (Qt の省略のまま)")
	{
		CHECK(elide_form_index(forms, {0, 1}) == 1);
		CHECK(elide_form_index(forms, {0, 0}) == 1);
		// 全文も、打ち消しの述語の途中でしか切れない (先頭まで戻る) 形。
		const std::vector<std::u16string> neg{u"追加できませんでした", u"12人"};
		CHECK(safe_cut(neg[0], 3) == 0);
		CHECK(elide_form_index(neg, {3, 1}) == 1);
	}
	SUBCASE("Qt が省かずに表示できる候補 (切れ目が文字列の長さ) は、そのまま選べる")
	{
		CHECK(elide_form_index(forms, {6, static_cast<int>(shorter.size())}) == 1);
	}
	SUBCASE("候補が 1 つのときと、候補がないときも扱える")
	{
		CHECK(elide_form_index({u"12:41"}, {1}) == 0);
		CHECK(elide_form_index({}, {}) == -1);
	}
}

TEST_CASE("余白と幅と高さは、本文のフォントの大きさから計算する")
{
	const DockStyle style;
	const DockMetrics m10 = compute_dock_metrics(10.0, 96.0, 16, style);
	CHECK(m10.em_px == doctest::Approx(13.333).epsilon(0.001));
	CHECK(m10.small_px == 3);
	CHECK(m10.large_px == 7);
	CHECK(m10.group_gap_px == 10);
	CHECK(m10.name_floor_px == 53);
	CHECK(m10.paragraph_max_px == 533);
	CHECK(m10.dialog_max_px == 600);
	CHECK(m10.icon_px == 16);
	CHECK(m10.row_min_height_px == 24);
	CHECK(m10.focus_ring_px == 2);
	CHECK(m10.focus_ring_gap_px == 1);
	// [フォントサイズ] 14 に追従する。
	const DockMetrics m14 = compute_dock_metrics(14.0, 96.0, 16, style);
	CHECK(m14.small_px == 5);
	CHECK(m14.large_px == 9);
	CHECK(m14 != m10);
	// 極端に小さいフォントでも余白は 0 にしない。読めない値は既定の値にする。
	const DockMetrics tiny = compute_dock_metrics(1.0, 96.0, 0, style);
	CHECK(tiny.small_px >= 1);
	CHECK(tiny.large_px >= 2);
	CHECK(tiny.icon_px == 16);
	CHECK(compute_dock_metrics(-1.0, 0.0, 16, style) == m10);
}

// ---- 差し込む値 ----

namespace {

// s に制御文字 (U+0000 から U+001F、DEL、U+0080 から U+009F) が含まれていれば true を返す。
bool has_control(const std::string &s)
{
	for (std::size_t i = 0; i < s.size(); ++i) {
		const auto c = static_cast<unsigned char>(s[i]);
		if (c < 0x20 || c == 0x7F)
			return true;
		if (c == 0xC2 && i + 1 < s.size()) {
			const auto d = static_cast<unsigned char>(s[i + 1]);
			if (d >= 0x80 && d <= 0x9F)
				return true;
		}
	}
	return false;
}

} // namespace

TEST_CASE("plan_arg は 2 種類の日時のマーカー (時刻、日付と時刻) を読む")
{
	namespace dv = cordmux::ledger::dockview;
	const std::int64_t at = 946728000000000; // 2000-01-01 12:00 (協定世界時)
	const ArgPlan t = plan_arg(dv::time_arg(at));
	CHECK(t.kind == ArgKind::Time);
	CHECK(t.epoch_us == at);
	const ArgPlan d = plan_arg(dv::date_time_arg(at));
	CHECK(d.kind == ArgKind::DateTime);
	CHECK(d.epoch_us == at);
	// 負の時刻も読める (1970 年より前。形は壊れていない)。
	CHECK(plan_arg(dv::time_arg(-5)).kind == ArgKind::Time);
}

TEST_CASE("plan_arg は、壊れたマーカーの値の制御文字を画面に表示しない")
{
	const std::string mark(1, cordmux::ledger::dockview::kTimeArgMark);
	struct Case {
		std::string arg;
		std::string text;
	};
	const Case cases[] = {
		{mark + "Y12ab", "12ab"},  // 数でない
		{mark + "Y", ""},         // 数がない
		{mark + "Q123", "123"},   // 知らない種類の文字は、種類の文字ごと除く
		{mark + "-123", "-123"},  // 種類の文字がない (数の先頭は除かない)
		{mark, ""},               // マーカーだけ
		{mark + "T123" + mark + "4", "1234"}, // 途中のマーカーも除く
	};
	for (const Case &c : cases) {
		const ArgPlan p = plan_arg(c.arg);
		CHECK(p.kind == ArgKind::Plain);
		CHECK(p.text == c.text);
		CHECK_FALSE(has_control(p.text));
	}
	// マーカーのない文字列はそのまま。ただし制御文字は除く (U+0000 から U+001F、DEL、U+0085)。日本語と
	// NBSP (U+00A0) は残す。
	CHECK(plan_arg("たろう").text == "たろう");
	CHECK(plan_arg("a\tb\nc\x7f" "d").text == "abcd");
	CHECK(plan_arg("x\xC2\x85y").text == "xy");
	CHECK(plan_arg("x\xC2\xA0y").text == "x\xC2\xA0y");
	CHECK(plan_arg("").text.empty());
}

TEST_CASE("short_clock_format は秒を除き、日本語の画面では 24 時間制にする")
{
	// en-US の地域の設定 (Qt が Windows の h:mm:ss tt を変換した形)。
	CHECK(short_clock_format("h:mm:ss AP", false) == "h:mm AP");
	CHECK(short_clock_format("h:mm:ss AP", true) == "H:mm");
	CHECK(short_clock_format("h:mm AP", true) == "H:mm");
	CHECK(short_clock_format("M/d/yyyy h:mm AP", false) == "M/d/yyyy h:mm AP");
	CHECK(short_clock_format("M/d/yyyy h:mm AP", true) == "M/d/yyyy H:mm");
	// 午前と午後の表示が先頭に来る地域 (ko-KR など)。
	CHECK(short_clock_format("AP h:mm", true) == "H:mm");
	CHECK(short_clock_format("yyyy-MM-dd AP h:mm", true) == "yyyy-MM-dd H:mm");
	CHECK(short_clock_format("AP h:mm", false) == "AP h:mm");
	// ja-JP と de-DE はもともと 24 時間制。
	CHECK(short_clock_format("yyyy/MM/dd H:mm", true) == "yyyy/MM/dd H:mm");
	CHECK(short_clock_format("HH:mm:ss", false) == "HH:mm");
	CHECK(short_clock_format("dd.MM.yyyy HH:mm", false) == "dd.MM.yyyy HH:mm");
	// 1/1000 秒と、1 文字の午前と午後の表示 (a)。
	CHECK(short_clock_format("HH:mm:ss.zzz", false) == "HH:mm");
	CHECK(short_clock_format("h:mm a", true) == "H:mm");
	// 引用符の中の文字は変えない ('' は引用符そのもの)。
	CHECK(short_clock_format("h 'h' mm's'", true) == "H 'h' mm's'");
	CHECK(short_clock_format("h' o''clock' AP", true) == "H' o''clock'");
	// 日本語の文字はそのまま通す。
	CHECK(short_clock_format("H\xE6\x99\x82mm\xE5\x88\x86", true) == "H\xE6\x99\x82mm\xE5\x88\x86");
}

TEST_CASE("phrase_break_choice は、2 つの行が列に入る折り返す位置のうち、行の長さがいちばん近いものを選ぶ")
{
	// 1 行に入るなら折り返さない。
	CHECK(phrase_break_choice({100}, 400, 465, 0) == -1);
	// 切り替えの確認のダイアログの主文の形: 読点が 1 つ。1 行目が約 270、全体が約 670、列が 465。
	CHECK(phrase_break_choice({270}, 670, 465, 116) == 0);
	// 2 行目が列に入らない (読点が前すぎる) なら折り返さない。
	CHECK(phrase_break_choice({100}, 670, 465, 0) == -1);
	// 1 行目が列に入らない (読点が後ろすぎる) なら折り返さない。
	CHECK(phrase_break_choice({500}, 670, 465, 0) == -1);
	// 2 行目が最後の行の長さの下限より短いなら、折り返さない。
	CHECK(phrase_break_choice({450}, 500, 465, 116) == -1);
	// 合う位置が 2 つあるときは、行の長さが近い方 (広い方の行が短い方) を選ぶ。
	CHECK(phrase_break_choice({250, 400}, 600, 465, 0) == 0);
	CHECK(phrase_break_choice({200, 320}, 600, 465, 0) == 1);
	// 列が 0 以下なら折り返さない。
	CHECK(phrase_break_choice({100}, 670, 0, 0) == -1);
	// 候補がなければ折り返さない。
	CHECK(phrase_break_choice({}, 670, 465, 0) == -1);
}

TEST_CASE("dialog_height_fit は、ダイアログが画面の作業領域に入らないときだけ本文をスクロールし、アイコンと主文とボタンを画面に残す")
{
	// 作業領域の高さ (論理 px) = 画面の高さ / 拡大率 - タスクバー (Windows 10 の既定で 40 論理 px)。
	//   1366x768 で 125%: 768 / 1.25 = 614 -> 574。1920x1080 で 175%: 1080 / 1.75 = 617 -> 577。
	//   1920x1080 で 100%: 1080 - 40 = 1040。
	// 枠 (タイトルバーと上下の枠) は、Windows 10 で約 39。
	constexpr int kFrame = 39;
	constexpr int kWorkArea125 = 574;
	constexpr int kWorkArea175 = 577;
	constexpr int kWorkAreaPo = 1040;
	constexpr int kMinBody = 60;
	// 手順書と違う置き方のアンインストールが終わったダイアログの本来の高さ (テーマ、言語、フォントサイズを変えて測った値)
	// と、本文以外の高さ (外周、主文、間隔、ボタン) の例 110。
	for (const int natural : {593, 610, 633, 650, 910}) {
		const int body = natural - 110;
		for (const int area : {kWorkArea125, kWorkArea175}) {
			const DialogHeightFit fit = dialog_height_fit(natural, body, area, kFrame, kMinBody);
			CAPTURE(natural);
			CAPTURE(area);
			CHECK(fit.scroll);
			// ダイアログの全体 (枠 + 本文以外の高さ + 見える本文) が、作業領域にちょうど入る。
			CHECK(kFrame + (natural - body) + fit.body_px == area);
			CHECK(fit.body_px < body);
			CHECK(fit.body_px >= kMinBody);
		}
		// 1920x1080 で 100% の画面ではスクロールしない (見た目を変えない)。
		CHECK_FALSE(dialog_height_fit(natural, body, kWorkAreaPo, kFrame, kMinBody).scroll);
	}
	// 入るダイアログはスクロールしない (アンインストールが終わったときのダイアログの、ja の 271 など)。ちょうど入る
	// 高さもスクロールしない。1 px でもはみ出せばスクロールする。
	CHECK_FALSE(dialog_height_fit(271, 160, kWorkArea125, kFrame, kMinBody).scroll);
	CHECK_FALSE(dialog_height_fit(kWorkArea125 - kFrame, 400, kWorkArea125, kFrame, kMinBody).scroll);
	{
		const DialogHeightFit fit = dialog_height_fit(kWorkArea125 - kFrame + 1, 400, kWorkArea125, kFrame, kMinBody);
		CHECK(fit.scroll);
		CHECK(fit.body_px == 399);
	}
	// 作業領域が小さすぎて本文以外の部分も入らないときは、本文を最小の高さで見せる (ダイアログは作業領域を超える)。
	{
		const DialogHeightFit fit = dialog_height_fit(600, 490, 150, kFrame, kMinBody);
		CHECK(fit.scroll);
		CHECK(fit.body_px == kMinBody);
	}
	// スクロールしても低くならない (本文が最小の高さ以下) なら、スクロールしない。
	CHECK_FALSE(dialog_height_fit(600, 50, kWorkArea125, kFrame, kMinBody).scroll);
	// 作業領域の高さが分からない (0 以下) とき、スクロールする本文がないとき、本来の高さがないときは、
	// スクロールしない。
	CHECK_FALSE(dialog_height_fit(610, 500, 0, kFrame, kMinBody).scroll);
	CHECK_FALSE(dialog_height_fit(610, 500, -1, kFrame, kMinBody).scroll);
	CHECK_FALSE(dialog_height_fit(610, 0, kWorkArea125, kFrame, kMinBody).scroll);
	CHECK_FALSE(dialog_height_fit(0, 0, kWorkArea125, kFrame, kMinBody).scroll);
	// 負の枠は 0 と同じ。最小の高さが 0 以下でも 1 px は見せる。
	CHECK(dialog_height_fit(610, 500, 600, -5, kMinBody).body_px == 490);
	CHECK(dialog_height_fit(610, 500, 50, 0, 0).body_px == 1);
}

TEST_CASE("owner_hold_action は止める範囲への入力だけを捨て、範囲の外への入力はすべて通す")
{
	const HoldEvent all[] = {HoldEvent::PointerPress, HoldEvent::PointerOther, HoldEvent::Wheel,
				 HoldEvent::KeyPress,     HoldEvent::KeyOther,     HoldEvent::ContextMenu,
				 HoldEvent::Hint,         HoldEvent::HoverIn,      HoldEvent::TouchBegin,
				 HoldEvent::TouchOther,   HoldEvent::Drag,         HoldEvent::Close,
				 HoldEvent::Activate,     HoldEvent::Other};
	// 範囲の外 (OBS のメインウィンドウ、ドック、失敗のダイアログそのもの) は、どのイベントも通す。
	for (HoldEvent e : all) {
		CHECK(owner_hold_action(e, false, true) == HoldAction::Pass);
		CHECK(owner_hold_action(e, false, false) == HoldAction::Pass);
	}
	// 範囲の中: クリック、キー、コンテキストメニュー、タッチの開始は、捨ててダイアログを前面に出す。
	CHECK(owner_hold_action(HoldEvent::PointerPress, true, true) == HoldAction::SwallowAndRaise);
	CHECK(owner_hold_action(HoldEvent::KeyPress, true, true) == HoldAction::SwallowAndRaise);
	CHECK(owner_hold_action(HoldEvent::ContextMenu, true, true) == HoldAction::SwallowAndRaise);
	CHECK(owner_hold_action(HoldEvent::TouchBegin, true, true) == HoldAction::SwallowAndRaise);
	// 続きの入力は捨てるだけ (前面に出すのは、1 回の操作に 1 回)。
	CHECK(owner_hold_action(HoldEvent::PointerOther, true, true) == HoldAction::Swallow);
	CHECK(owner_hold_action(HoldEvent::Wheel, true, true) == HoldAction::Swallow);
	CHECK(owner_hold_action(HoldEvent::KeyOther, true, true) == HoldAction::Swallow);
	CHECK(owner_hold_action(HoldEvent::Hint, true, true) == HoldAction::Swallow);
	CHECK(owner_hold_action(HoldEvent::HoverIn, true, true) == HoldAction::Swallow);
	CHECK(owner_hold_action(HoldEvent::TouchOther, true, true) == HoldAction::Swallow);
	CHECK(owner_hold_action(HoldEvent::Drag, true, true) == HoldAction::Swallow);
	// 閉じる操作: 利用者の操作 ([x]、Alt+F4) は捨てる。コードからの close (OBS の終了処理) は通す。
	CHECK(owner_hold_action(HoldEvent::Close, true, true) == HoldAction::SwallowAndRaise);
	CHECK(owner_hold_action(HoldEvent::Close, true, false) == HoldAction::Pass);
	// 範囲がアクティブになったら、届けたうえで、ダイアログをその上に出す。
	CHECK(owner_hold_action(HoldEvent::Activate, true, true) == HoldAction::PassAndRaise);
	CHECK(owner_hold_action(HoldEvent::Activate, true, false) == HoldAction::PassAndRaise);
	// 描画、フォーカス、マウスが出る (ホバーを戻す) などのイベントは止めない。
	CHECK(owner_hold_action(HoldEvent::Other, true, true) == HoldAction::Pass);
}

TEST_CASE("keep_numbers_break は、数と単位の語の途中の折り返す位置を、語の先頭へ戻す")
{
	// 「...人がいます (2人)」: Qt が「(2」の後で折り返そうとしたら、括弧の先頭まで戻す。
	const std::u16string t = u"話している人がいます (2人)";
	const auto spans = number_spans(t);
	CHECK(keep_numbers_break(spans, 0, 13) == 11);
	// 語の外の位置はそのまま。
	CHECK(keep_numbers_break(spans, 0, 6) == 6);
	// 語が行の先頭から始まる (戻すと行が空になる) なら戻さない。
	CHECK(keep_numbers_break(spans, 11, 13) == 13);
	// 「30分」の数と単位の間。
	const std::u16string m = u"あと 30分";
	CHECK(keep_numbers_break(number_spans(m), 0, 5) == 3);
}

TEST_CASE("button_name_spans は角かっこで囲んだボタンの名前の範囲を返し、keep_numbers_break はその途中で折り返さない")
{
	// ContactUnavailable の本文の形 (ja)。「[Vesktopに追加]」は添字 12 から 24 (「]」の次) まで。
	const std::u16string t = u"もう一度押すと戻ります。[Vesktopに追加] を押してください。";
	const auto spans = button_name_spans(t);
	REQUIRE(spans.size() == 1);
	CHECK(spans[0].start == 12);
	CHECK(t.substr(static_cast<std::size_t>(spans[0].start), static_cast<std::size_t>(spans[0].end - spans[0].start)) ==
	      u"[Vesktopに追加]");
	// Qt が「追」と「加]」の間 (添字 22) で折り返そうとしたら、名前の先頭まで戻す。
	CHECK(keep_numbers_break(spans, 0, 22) == 12);
	// 名前が行の先頭から始まる (1 行に入らない) なら戻さない (WrapLabel は 1 行に入らない名前を範囲に入れないが、念のため)。
	CHECK(keep_numbers_break(spans, 12, 22) == 22);
	// en と、2 つの名前。
	const std::u16string en = u"Quit Vesktop, then select [Remove from Vesktop] and [OK].";
	const auto en_spans = button_name_spans(en);
	REQUIRE(en_spans.size() == 2);
	CHECK(en.substr(static_cast<std::size_t>(en_spans[1].start), static_cast<std::size_t>(en_spans[1].end - en_spans[1].start)) ==
	      u"[OK]");
	// 閉じていない「[」、空の組、入れ子の外側は範囲にしない (内側の閉じた組だけ)。
	CHECK(button_name_spans(u"[表示 を押す").empty());
	CHECK(button_name_spans(u"[] と [").empty());
	const auto nested = button_name_spans(u"[a [b] c]");
	REQUIRE(nested.size() == 1);
	CHECK(nested[0].start == 3);
	CHECK(nested[0].end == 6);
}

TEST_CASE("path_break は区切りの直後で折り返し、区切りがないときだけ文字の間で折り返す")
{
	const std::u16string p = u"C:\\ProgramData\\obs-studio\\plugins\\cordmux";
	// 幅に「C:\ProgramData\obs-st」まで入るなら、「C:\ProgramData\」の後で折り返す。
	CHECK(path_break(p, 0, 21) == 15);
	// 区切りの直後までちょうど入るなら、そこで折り返す。
	CHECK(path_break(p, 0, 15) == 15);
	// 行の中に区切りもつなぎもない (1 つの区切りの間が幅に入らない) なら、文字の間で折り返す。
	CHECK(path_break(p, 3, 9) == 9);
	// 行の先頭の直前の区切りは数えない (空の行を作らない)。区切りがない行では「-」の直後で折り返す (「obs-」と
	// 「studio」)。
	CHECK(path_break(p, 15, 20) == 19);
	// 「/」も区切り。
	const std::u16string q = u"C:/Users/a/Videos";
	CHECK(path_break(q, 0, 12) == 11);
}

TEST_CASE("path_break は区切りが入らない名前を空白、次に「-」「_」「.」で折り返し、数字の並びの途中では折り返さない")
{
	// 録画のセッションのフォルダーの名前の行に「2000-01-01 12-34-5」まで入る形。空白の後で折り返し、「12-34-56」を
	// 次の行へ送る (「-」を空白と同じ段階にすると「12-34-」と「56」に折れ、最後の行が 2 文字になる)。
	const std::u16string s = u"C:\\Videos\\cordmux\\2000-01-01 12-34-56";
	const int name = 18; // 「2000」の先頭
	CHECK(s.substr(name, 4) == u"2000");
	CHECK(path_break(s, name, name + 18) == name + 11);
	CHECK(s.substr(name + 11) == u"12-34-56");
	// 「2000-01-01」まで入り、続く空白が入らない幅でも、空白の後で折り返す (行の末尾の空白は見えない)。
	CHECK(path_break(s, name, name + 10) == name + 11);
	// 区切りが入る行では、名前の中のつなぎより区切りを先に選ぶ。
	const std::u16string u = u"C:\\Users\\example_streaming_01\\Videos";
	CHECK(path_break(u, 0, 19) == 9);
	// 名前が丸ごと入り、続く区切りだけが入らないなら、名前の中のつなぎより、名前の後ろ (区切りの前) で折り返す。
	CHECK(path_break(u, 9, 29) == 29);
	CHECK(u.substr(29, 1) == u"\\");
	// 空白がなければ、「-」「_」「.」のいちばん右。
	const std::u16string d = u"2000-01-01_12-34-56";
	CHECK(path_break(d, 0, 18) == 17);
	CHECK(path_break(u"example_streaming_01", 0, 19) == 18);
	CHECK(path_break(u"recording.mkv", 0, 12) == 10);
	// 数と数の間の「.」(「1.5」) では折り返さず、ほかのつなぎを探す。
	CHECK(path_break(u"v-1.5", 0, 4) == 2);
	// つなぎもなければ文字の間。数字の並びの途中なら、並びの先頭まで戻す。
	CHECK(path_break(u"abc12345", 0, 6) == 3);
	CHECK(path_break(u"abcdefgh", 0, 6) == 6);
	// 数字の並びが行の先頭から始まる (戻すと空の行になる) なら、戻さない。
	CHECK(path_break(u"12345678", 0, 6) == 6);
	// 全角の空白も空白。
	CHECK(path_break(u"配信\u3000録画", 0, 4) == 3);
}

TEST_CASE("parent_path は文字列で 1 つ上のフォルダーを返し、ドライブのルート、「/」、UNC の共有のルートでは空を返す")
{
	CHECK(parent_path(u"C:/a/b") == u"C:/a");
	CHECK(parent_path(u"C:/a") == u"C:/");
	CHECK(parent_path(u"C:/").empty());
	CHECK(parent_path(u"/a") == u"/");
	CHECK(parent_path(u"/").empty());
	CHECK(parent_path(u"//srv/share/a/b") == u"//srv/share/a");
	CHECK(parent_path(u"//srv/share/a") == u"//srv/share");
	// 共有のルートより上 ("//srv") は、エクスプローラーで開けるフォルダーではない。
	CHECK(parent_path(u"//srv/share").empty());
	CHECK(parent_path(u"//srv").empty());
	CHECK(parent_path(u"").empty());
	CHECK(parent_path(u"name").empty());
}

TEST_CASE("nearest_folder は、存在するフォルダー、存在するファイルがあるフォルダー、存在するいちばん近い親のどれかを返す")
{
	// テスト用のツリー。一覧にないパスは存在しない。
	const std::map<std::u16string, PathKind> tree = {
		{u"C:/", PathKind::Folder},
		{u"C:/Users", PathKind::Folder},
		{u"C:/Users/a", PathKind::Folder},
		{u"C:/Users/a/Videos", PathKind::Folder},
		{u"C:/Users/a/Videos/f.mkv", PathKind::File},
		{u"C:/top.txt", PathKind::File},
		{u"//srv/share", PathKind::Folder},
		{u"//srv/share/rec", PathKind::Folder},
	};
	int asked = 0;
	const auto kind_of = [&tree, &asked](const std::u16string &p) {
		++asked;
		const auto it = tree.find(p);
		return it == tree.end() ? PathKind::Missing : it->second;
	};
	// 存在するフォルダーは、そのフォルダー (親を開いて選択する形にしない)。
	CHECK(nearest_folder(u"C:/Users/a/Videos", kind_of) == u"C:/Users/a/Videos");
	// 存在するファイルは、そのファイルがあるフォルダー。ルートの直下のファイルはルート。
	CHECK(nearest_folder(u"C:/Users/a/Videos/f.mkv", kind_of) == u"C:/Users/a/Videos");
	CHECK(nearest_folder(u"C:/top.txt", kind_of) == u"C:/");
	// 1 階層だけ存在しない。
	CHECK(nearest_folder(u"C:/Users/a/Videos/cordmux", kind_of) == u"C:/Users/a/Videos");
	// 2 階層以上が存在しない (録画の日付のフォルダーごと削除されたとき)。QDir::cdUp でたどると、ここで止まって
	// 何も開けない。
	CHECK(nearest_folder(u"C:/Users/a/Videos/cordmux/2000-01-01/taro", kind_of) == u"C:/Users/a/Videos");
	// ルートの下がすべて存在しない。
	CHECK(nearest_folder(u"C:/gone/deeper", kind_of) == u"C:/");
	// 存在しないドライブは、ルートまでさかのぼっても存在するフォルダーがないので、空を返す (何もしない)。
	asked = 0;
	CHECK(nearest_folder(u"Q:/no/such", kind_of).empty());
	CHECK(asked == 3); // "Q:/no/such"、"Q:/no"、"Q:/" で止まる (ルートの上へたどらない)
	// UNC: 共有のルートまではたどり、その上 ("//srv") へは行かない。
	CHECK(nearest_folder(u"//srv/share/rec/gone/x", kind_of) == u"//srv/share/rec");
	CHECK(nearest_folder(u"//srv/other/x", kind_of).empty());
	// 空のパスは開かない。
	CHECK(nearest_folder(u"", kind_of).empty());
}

TEST_CASE("1 行目の無効の形は、大きいテキストと長いボタンから順に、入るものを選ぶ")
{
	using cordmux::dock::ui::calc::disabled_line_fit;
	// テキストは大きいとき 170 px、ボタンは長い形で 160 px、短い形で 80 px、間は 3 px。
	auto fit = disabled_line_fit(400, 170, 160, 80, 3);
	CHECK_FALSE(fit.small_text);
	CHECK_FALSE(fit.short_button);
	fit = disabled_line_fit(300, 170, 160, 80, 3);
	CHECK_FALSE(fit.small_text);
	CHECK(fit.short_button);
	fit = disabled_line_fit(200, 170, 160, 80, 3);
	CHECK(fit.small_text);
	CHECK(fit.short_button);
	// どれも入らなくても、テキストとボタンを残す形を返す。
	fit = disabled_line_fit(100, 170, 160, 80, 3);
	CHECK(fit.small_text);
	CHECK(fit.short_button);
}

TEST_CASE("確認のボタンは、元のボタンの場所が足りれば同じ幅で 2 つに分ける")
{
	using cordmux::dock::ui::calc::confirm_layout;
	const auto layout = confirm_layout(100, 200, 80, 70, 4, false, 0);
	CHECK(layout.left == 100);
	CHECK(layout.width == 200);
	CHECK_FALSE(layout.cancel_on_left);
}

TEST_CASE("確認のボタンのテキストが入らないときは、右端をそろえて左へ広げる")
{
	using cordmux::dock::ui::calc::confirm_layout;
	const auto layout = confirm_layout(100, 90, 50, 70, 4, false, 0);
	// やめる側 (70) が [確定] (50) より狭くないので、必要な幅は 2 つの幅の和 (50 + 4 + 70)。
	CHECK(layout.width == 124);
	CHECK(layout.left + layout.width == 190);
	CHECK(layout.confirm_width + 4 + layout.cancel_width == layout.width);
	CHECK(layout.cancel_width >= 70);
	// やめる側が狭いときは、同じ幅で 2 つに分ける (広い方の 2 倍が必要)。
	const auto narrow = confirm_layout(100, 90, 70, 50, 4, false, 0);
	CHECK(narrow.width == 144);
	CHECK(narrow.confirm_width + 4 + narrow.cancel_width == narrow.width);
}

TEST_CASE("[キャンセル] はポインターの下に来る側に置く")
{
	using cordmux::dock::ui::calc::confirm_layout;
	CHECK(confirm_layout(100, 200, 80, 70, 4, true, 120).cancel_on_left);
	CHECK_FALSE(confirm_layout(100, 200, 80, 70, 4, true, 280).cancel_on_left);
	// ポインターが場所の外にあるときと、位置が分からないときは右に置く。
	CHECK_FALSE(confirm_layout(100, 200, 80, 70, 4, true, 20).cancel_on_left);
	CHECK_FALSE(confirm_layout(100, 200, 80, 70, 4, false, 120).cancel_on_left);
}

TEST_CASE("並びの保留は、ポインターが一覧の上で動いている間と、押している間と、メニューを開いている間だけ続く")
{
	using cordmux::dock::ui::calc::layout_held;
	CHECK(layout_held(true, true, false, false));
	CHECK_FALSE(layout_held(true, false, false, false)); // 止まって時間がたった
	CHECK_FALSE(layout_held(false, true, false, false)); // 一覧の外
	CHECK(layout_held(false, false, true, false));
	CHECK(layout_held(false, false, false, true));
	CHECK_FALSE(layout_held(false, false, false, false));
}

TEST_CASE("メーターの色の境目は、-20 dB から黄、-9 dB から赤で、メーターの全幅に対して固定である")
{
	using cordmux::dock::ui::calc::meter_segments;
	using cordmux::dock::ui::calc::MeterSegments;
	const double yellow_from = (-20.0 + 48.0) / 48.0;
	const double red_from = (-9.0 + 48.0) / 48.0;
	CHECK(yellow_from == doctest::Approx(0.5833).epsilon(0.001));
	CHECK(red_from == doctest::Approx(0.8125).epsilon(0.001));

	// 緑の中: 緑だけ。
	MeterSegments s = meter_segments(0.3);
	CHECK(s.green_end == doctest::Approx(0.3));
	CHECK(s.yellow_end == doctest::Approx(0.3));
	CHECK(s.red_end == doctest::Approx(0.3));

	// 黄の中: 緑は -20 dB まで、黄は値まで。
	s = meter_segments(0.7);
	CHECK(s.green_end == doctest::Approx(yellow_from));
	CHECK(s.yellow_end == doctest::Approx(0.7));
	CHECK(s.red_end == doctest::Approx(0.7));

	// 赤の中: 緑は -20 dB まで、黄は -9 dB まで、赤は値まで。
	s = meter_segments(0.95);
	CHECK(s.green_end == doctest::Approx(yellow_from));
	CHECK(s.yellow_end == doctest::Approx(red_from));
	CHECK(s.red_end == doctest::Approx(0.95));

	// ちょうど境目の値は、次の色を塗らない (幅 0)。
	s = meter_segments(yellow_from);
	CHECK(s.green_end == doctest::Approx(yellow_from));
	CHECK(s.yellow_end == doctest::Approx(yellow_from));
	s = meter_segments(red_from);
	CHECK(s.yellow_end == doctest::Approx(red_from));
	CHECK(s.red_end == doctest::Approx(red_from));
}

TEST_CASE("メーターの色の境目は、範囲の外の値と NaN を端に収める")
{
	using cordmux::dock::ui::calc::meter_segments;
	CHECK(meter_segments(-1.0).red_end == 0.0);
	CHECK(meter_segments(std::nan("")).red_end == 0.0);
	CHECK(meter_segments(2.0).red_end == 1.0);
	CHECK(meter_segments(1.0).red_end == 1.0);
}

TEST_CASE("メーターの塗りつぶす幅は px に四捨五入し、変わらない値では塗り直さない")
{
	using cordmux::dock::ui::calc::meter_fill_px;
	CHECK(meter_fill_px(0.0, 100) == 0);
	CHECK(meter_fill_px(-0.5, 100) == 0);
	CHECK(meter_fill_px(0.5, 100) == 50);
	CHECK(meter_fill_px(0.504, 100) == 50);
	CHECK(meter_fill_px(0.506, 100) == 51);
	CHECK(meter_fill_px(1.0, 100) == 100);
	CHECK(meter_fill_px(3.0, 100) == 100);
	CHECK(meter_fill_px(0.5, 0) == 0);
	CHECK(meter_fill_px(std::nan(""), 100) == 0);
	// 1 px に満たない違いは、同じ幅になる (塗り直さない)。
	CHECK(meter_fill_px(0.5001, 100) == meter_fill_px(0.5002, 100));
}

TEST_CASE("readable_on は、読めない色を本文の色へ 10% ずつ寄せて、4.5:1 に届いた最初の色にする")
{
	const Rgb classic_warn{0xB8, 0x8A, 0x16}; // Classic の注意の色
	const Rgb classic_bg{0x3A, 0x39, 0x3A};   // Classic のドックの背景
	const Rgb text{0xFF, 0xFF, 0xFF};
	// 前提: 元の色は 3.66:1 で読めない。
	REQUIRE(round2(contrast_ratio(classic_warn, classic_bg)) == doctest::Approx(3.66));

	const Rgb got = readable_on(classic_warn, classic_bg, text);
	CHECK(contrast_ratio(got, classic_bg) >= kTextContrastMin);
	// 最初の色である。1 つ手前 (10% 少ない寄せ方) は 4.5:1 に届かない。
	int percent = 0;
	while (percent <= 100 && !same(mix_rgb(classic_warn, text, percent), got))
		percent += 10;
	REQUIRE(percent > 0);
	REQUIRE(percent <= 100);
	CHECK(contrast_ratio(mix_rgb(classic_warn, text, percent - 10), classic_bg) < kTextContrastMin);
}

TEST_CASE("readable_on は、すでに読める色はそのまま返し、本文の色でも届かないときは本文の色を返す")
{
	const Rgb ground{0x27, 0x2A, 0x33};
	const Rgb readable{0xFF, 0xD0, 0x40};
	REQUIRE(contrast_ratio(readable, ground) >= kTextContrastMin);
	CHECK(same(readable_on(readable, ground, Rgb{255, 255, 255}), readable));

	// 本文の色と背景が近く、どこまで寄せても届かない。
	const Rgb dim_text{0x50, 0x50, 0x50};
	const Rgb dim_ground{0x48, 0x48, 0x48};
	CHECK(same(readable_on(Rgb{0x40, 0x40, 0x40}, dim_ground, dim_text), dim_text));
}

TEST_CASE("rule_color は、背景との比が下限に届いた最初の色を返す (暗い背景と明るい背景)")
{
	const Rgb dark_ground{0x27, 0x2A, 0x33};
	const Rgb dark_text{0xFF, 0xFF, 0xFF};
	const Rgb light_ground{0xE5, 0xE5, 0xE5};
	const Rgb light_text{0x00, 0x00, 0x00};
	// 下限の低い罫線の例 (1.5:1)。製品は kStrongRuleContrastMin だけを使うが、下限によらず同じ決め方になることを確かめる。
	constexpr double kLowerMin = 1.5;
	for (const auto &[ground, text] : {std::pair<Rgb, Rgb>{dark_ground, dark_text}, std::pair<Rgb, Rgb>{light_ground, light_text}}) {
		const Rgb strong = rule_color(ground, text, kStrongRuleContrastMin);
		const Rgb light = rule_color(ground, text, kLowerMin);
		CHECK(contrast_ratio(strong, ground) >= kStrongRuleContrastMin);
		CHECK(contrast_ratio(light, ground) >= kLowerMin);
		// 下限の高い罫線は、下限の低い罫線より背景から遠い。
		CHECK(contrast_ratio(strong, ground) > contrast_ratio(light, ground));
		// 下限の低い罫線は、1 つ手前 (5% 少ない寄せ方) では下限に届かない最初の色である。
		int percent = 5;
		while (percent <= 100 && !same(mix_rgb(ground, text, percent), light))
			percent += 5;
		REQUIRE(percent <= 100);
		if (percent > 5)
			CHECK(contrast_ratio(mix_rgb(ground, text, percent - 5), ground) < kLowerMin);
	}
	// 本文の色と背景が近く、どこまで寄せても届かないときは本文の色を返す。
	CHECK(same(rule_color(Rgb{0x48, 0x48, 0x48}, Rgb{0x50, 0x50, 0x50}, kStrongRuleContrastMin), Rgb{0x50, 0x50, 0x50}));
}

TEST_CASE("name_column_px: 名前の欄は 3 段で広がる (つまみ最小、つまみのゆとり、名前の全文)")
{
	// 名前の最小 73、上限 120、つまみの最小 40、ゆとり 80、全員の名前の全文の幅の最大 200。
	const auto px = [](int available) { return name_column_px(available, 200, 73, 120, 40, 80); };
	// 狭いうちは名前の欄が最小。つまみの最小を引いても足りないときも、最小を下回らない。
	CHECK(px(50) == 73);
	CHECK(px(113) == 73);
	// 1 段目: つまみは最小のまま、名前の欄が上限まで広がる。
	CHECK(px(140) == 100);
	CHECK(px(160) == 120);
	// 2 段目: 名前の欄は上限のまま、つまみが最小からゆとりまで広がる (160 から 200)。
	CHECK(px(180) == 120);
	CHECK(px(200) == 120);
	// 3 段目: つまみがゆとりの幅のまま、名前の欄が全文の幅まで広がる。
	CHECK(px(201) == 121);
	CHECK(px(250) == 170);
	CHECK(px(280) == 200);
	// 全文より広くなっても、名前の欄は全文の幅のまま (残りはつまみ)。
	CHECK(px(400) == 200);
	// 全文が上限より短いなら、上限までも広げない。
	CHECK(name_column_px(300, 100, 73, 120, 40, 80) == 100);
	CHECK(name_column_px(140, 100, 73, 120, 40, 80) == 100);
	CHECK(name_column_px(120, 100, 73, 120, 40, 80) == 80);
	// 全文が最小より短いなら最小。
	CHECK(name_column_px(300, 10, 73, 120, 40, 80) == 73);
	// 1 段目では名前の欄とつまみの幅の和が available に収まる。
	for (int a = 113; a <= 300; ++a)
		CHECK(px(a) + 40 <= a);
}
