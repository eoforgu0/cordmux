// SPDX-License-Identifier: Apache-2.0

// sources/composite_label_plan.h (合成のソースの名前ラベルと光を描くときの判断の純粋関数) のテスト。名前の大きさと
// 余白、描き直す鍵、描き直した回数、全面 1 枚のときに隠す判断、名前ラベルを描くタイルと並びに描く人、描く光の形、光の
// 明るさ (ミュートと音量 0)、暗さの倍率、画面共有していない参加者の描き直す鍵と、並びから消えた人を確認する。

#include "doctest.h"

#include "sources/composite_label_plan.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using cordmux::sources::BorderTiming;
using cordmux::sources::composite_hides_label;
using cordmux::sources::composite_label_dim_at;
using cordmux::sources::composite_roster_departed;
using cordmux::sources::CompositeRosterLabelKey;
using cordmux::sources::composite_margin_px;
using cordmux::sources::composite_name_px;
using cordmux::sources::composite_needs_repaint;
using cordmux::sources::composite_quantize;
using cordmux::sources::composite_speaking_level;
using cordmux::sources::composite_speaking_parts;
using cordmux::sources::CompositeFrameGlowKey;
using cordmux::sources::CompositeLabelKey;
using cordmux::sources::CompositeLabelLook;
using cordmux::sources::CompositeTile;
using cordmux::sources::make_composite_frame_glow_key;
using cordmux::sources::make_composite_label_key;
using cordmux::sources::PixelRect;
using cordmux::sources::SpeakingTimes;

namespace {

constexpr std::uint64_t kMs = 1000000ULL;

CompositeTile tile_at(int x, int y, int w, int h)
{
	CompositeTile tile;
	tile.slot = 1;
	tile.rect = PixelRect{x, y, w, h};
	return tile;
}

// 鍵が変わったときだけ描き直す呼び出し側の動き。描き直した回数を数える。
struct Painter {
	bool valid = false;
	CompositeLabelKey painted;
	int paints = 0;

	void draw(const CompositeLabelKey &key)
	{
		if (composite_needs_repaint(valid, painted, key)) {
			valid = true;
			painted = key;
			++paints;
		}
	}
};

CompositeLabelKey base_key()
{
	return make_composite_label_key(7, 3, "user-a", PixelRect{0, 0, 960, 540}, 31.32, 18.0);
}

} // namespace

TEST_CASE("名前の大きさと余白は、キャンバスの高さに割合を掛けたピクセルになる")
{
	CHECK(composite_name_px(1080, 0.029) == doctest::Approx(31.32));
	CHECK(composite_name_px(1440, 0.029) == doctest::Approx(41.76));
	CHECK(composite_margin_px(1080, 0.0167) == doctest::Approx(18.036));
	// 解像度が違っても、配信の画面の高さに対する割合は同じ。
	CHECK(composite_name_px(2160, 0.03) / 2160 == doctest::Approx(composite_name_px(720, 0.03) / 720));
}

TEST_CASE("名前の大きさと余白は、キャンバスの高さか割合が 0 以下なら 0 を返す")
{
	CHECK(composite_name_px(0, 0.03) == 0.0);
	CHECK(composite_name_px(-1, 0.03) == 0.0);
	CHECK(composite_name_px(1080, 0.0) == 0.0);
	CHECK(composite_name_px(1080, -0.5) == 0.0);
	CHECK(composite_name_px(1080, std::numeric_limits<double>::quiet_NaN()) == 0.0);
	CHECK(composite_margin_px(0, 0.02) == 0.0);
	CHECK(composite_margin_px(1080, 0.0) == 0.0);
}

TEST_CASE("丸めは 1/64 ピクセルで、浮動小数点数の誤差の差は同じ値になる")
{
	CHECK(composite_quantize(31.32) == composite_quantize(31.32 + 1e-9));
	CHECK(composite_quantize(31.32) != composite_quantize(31.34));
	CHECK(composite_quantize(0.0) == 0);
}

TEST_CASE("同じ鍵で繰り返し描いても、描き直すのは最初の 1 回だけ")
{
	Painter painter;
	for (int i = 0; i < 600; ++i) // 60 fps で 10 秒
		painter.draw(base_key());
	CHECK(painter.paints == 1);
}

TEST_CASE("鍵のどの要素が変わっても描き直し、元に戻した鍵でもその回数だけ描き直す")
{
	Painter painter;
	painter.draw(base_key());
	CHECK(painter.paints == 1);

	CompositeLabelKey key = base_key();
	key.version = 8;
	painter.draw(key);
	CHECK(painter.paints == 2);

	key = base_key();
	key.look_version = 4;
	painter.draw(key);
	CHECK(painter.paints == 3);

	key = base_key();
	key.owner = "user-b";
	painter.draw(key);
	CHECK(painter.paints == 4);

	key = base_key();
	key.cell_w = 640;
	painter.draw(key);
	CHECK(painter.paints == 5);

	key = base_key();
	key.cell_h = 360;
	painter.draw(key);
	CHECK(painter.paints == 6);

	key = base_key();
	key.name_q += 64;
	painter.draw(key);
	CHECK(painter.paints == 7);

	key = base_key();
	key.margin_q += 64;
	painter.draw(key);
	CHECK(painter.paints == 8);

	// 画面共有していない参加者が入る、出る、名前が変わる。
	key = base_key();
	key.roster_names = {"Ken"};
	painter.draw(key);
	CHECK(painter.paints == 9);

	key.roster_names = {"Ken", "Sakura"};
	painter.draw(key);
	CHECK(painter.paints == 10);

	key.roster_names = {"Ken", "Sakura M"};
	painter.draw(key);
	CHECK(painter.paints == 11);

	painter.draw(key);
	CHECK(painter.paints == 11);
}

TEST_CASE("名前の大きさのごく小さな揺れでは描き直さず、1/64 ピクセルを超える変化では描き直す")
{
	Painter painter;
	const PixelRect cell{0, 0, 960, 540};
	painter.draw(make_composite_label_key(1, 1, "a", cell, 31.32, 18.0));
	painter.draw(make_composite_label_key(1, 1, "a", cell, 31.32 + 1e-7, 18.0 - 1e-7));
	CHECK(painter.paints == 1);
	painter.draw(make_composite_label_key(1, 1, "a", cell, 31.4, 18.0));
	CHECK(painter.paints == 2);
}

TEST_CASE("枠の光の鍵は、セルの大きさ、色、ぼかし、広がりだけで決まる")
{
	CompositeLabelLook look;
	look.glow_argb = 0xFF57F287u;
	look.glow_blur_ratio = 0.75;
	look.glow_spread_ratio = 0.10;
	const PixelRect cell{960, 540, 960, 540};
	const CompositeFrameGlowKey key = make_composite_frame_glow_key(cell, look, 31.32);

	// 位置や、枠の光に関係ない見た目の値が変わっても、鍵は変わらない。
	CompositeLabelLook other = look;
	other.opacity = 0.5;
	other.show_plate = false;
	other.glow_name = false;
	CHECK(make_composite_frame_glow_key(PixelRect{0, 0, 960, 540}, other, 31.32) == key);

	CHECK(make_composite_frame_glow_key(PixelRect{960, 540, 640, 540}, look, 31.32) != key);
	CHECK(make_composite_frame_glow_key(PixelRect{960, 540, 960, 360}, look, 31.32) != key);
	CompositeLabelLook color = look;
	color.glow_argb = 0xFFFF0000u;
	CHECK(make_composite_frame_glow_key(cell, color, 31.32) != key);
	CompositeLabelLook blur = look;
	blur.glow_blur_ratio = 1.0;
	CHECK(make_composite_frame_glow_key(cell, blur, 31.32) != key);
	CompositeLabelLook spread = look;
	spread.glow_spread_ratio = 0.2;
	CHECK(make_composite_frame_glow_key(cell, spread, 31.32) != key);
	// 名前の大きさが変わると、ぼかしと広がりのピクセルが変わる。
	CHECK(make_composite_frame_glow_key(cell, look, 40.0) != key);
}

TEST_CASE("枠の光は、同じ鍵なら 1 度だけ描く")
{
	CompositeLabelLook look;
	bool valid = false;
	CompositeFrameGlowKey painted;
	int paints = 0;
	for (int i = 0; i < 100; ++i) {
		const CompositeFrameGlowKey key = make_composite_frame_glow_key(PixelRect{0, 0, 960, 540}, look, 31.32);
		if (composite_needs_repaint(valid, painted, key)) {
			valid = true;
			painted = key;
			++paints;
		}
	}
	CHECK(paints == 1);
}

TEST_CASE("全面 1 枚のときだけ隠す: 設定が真で、タイルが 1 つでキャンバス全体を覆うとき")
{
	CompositeLabelLook look;
	look.hide_when_single = true;
	const std::vector<CompositeTile> full{tile_at(0, 0, 1920, 1080)};
	CHECK(composite_hides_label(look, 1920, 1080, full));

	// 設定が偽なら隠さない。
	look.hide_when_single = false;
	CHECK_FALSE(composite_hides_label(look, 1920, 1080, full));
}

TEST_CASE("1 枚でも 4 等分の左上に置いているとき (全面にしない設定) と、2 枚以上のときは隠さない")
{
	CompositeLabelLook look;
	look.hide_when_single = true;
	CHECK_FALSE(composite_hides_label(look, 1920, 1080, {tile_at(0, 0, 960, 540)}));
	CHECK_FALSE(composite_hides_label(look, 1920, 1080, {tile_at(0, 0, 960, 540), tile_at(960, 0, 960, 540)}));
	// タイルがないときも隠す対象がない。
	CHECK_FALSE(composite_hides_label(look, 1920, 1080, {}));
	// 全面の大きさでも、キャンバスの大きさが違えば全面ではない。
	CHECK_FALSE(composite_hides_label(look, 3840, 2160, {tile_at(0, 0, 1920, 1080)}));
}

TEST_CASE("タイルに名前ラベルを描くのは、映像を描いた大きさのあるタイルで、全面の 1 枚を隠すときは描かない")
{
	using cordmux::sources::composite_tiles_labeled;
	CompositeLabelLook look;
	look.hide_when_single = true;
	const std::vector<CompositeTile> two{tile_at(0, 0, 960, 540), tile_at(960, 0, 960, 540)};
	CHECK(composite_tiles_labeled(look, 1920, 1080, two, {true, true}) == std::vector<bool>{true, true});
	// 映像がまだないタイルには描かない。has_video が足りない分も描かない。
	CHECK(composite_tiles_labeled(look, 1920, 1080, two, {true, false}) == std::vector<bool>{true, false});
	CHECK(composite_tiles_labeled(look, 1920, 1080, two, {true}) == std::vector<bool>{true, false});
	// 大きさのないタイルには描かない。
	CHECK(composite_tiles_labeled(look, 1920, 1080, {tile_at(0, 0, 0, 540)}, {true}) == std::vector<bool>{false});
	// 全面の 1 枚は、設定が真なら隠し、偽なら描く (composite_hides_label と同じ判断)。
	const std::vector<CompositeTile> full{tile_at(0, 0, 1920, 1080)};
	CHECK(composite_tiles_labeled(look, 1920, 1080, full, {true}) == std::vector<bool>{false});
	look.hide_when_single = false;
	CHECK(composite_tiles_labeled(look, 1920, 1080, full, {true}) == std::vector<bool>{true});
}

TEST_CASE("名前ラベルを枠に描かない設定では、どのタイルにも名前ラベルを描かないが、枠の光は描けるタイルのままにする")
{
	using cordmux::sources::composite_tiles_framed;
	using cordmux::sources::composite_tiles_labeled;
	CompositeLabelLook look;
	look.hide_when_single = true;
	const std::vector<CompositeTile> two{tile_at(0, 0, 960, 540), tile_at(960, 0, 960, 540)};
	// 既定 (枠に描く) は、名前ラベルと枠の光の判断が同じ。
	CHECK(composite_tiles_framed(look, 1920, 1080, two, {true, false}) == std::vector<bool>{true, false});
	CHECK(composite_tiles_labeled(look, 1920, 1080, two, {true, false}) == std::vector<bool>{true, false});
	look.show_in_slot = false;
	CHECK(composite_tiles_labeled(look, 1920, 1080, two, {true, true}) == std::vector<bool>{false, false});
	// 枠の光は、映像を描いた大きさのあるタイルに描ける (名前ラベルの設定によらない)。
	CHECK(composite_tiles_framed(look, 1920, 1080, two, {true, true}) == std::vector<bool>{true, true});
	CHECK(composite_tiles_framed(look, 1920, 1080, two, {true, false}) == std::vector<bool>{true, false});
	// 全面の 1 枚を [1人のときは非表示] で隠すときは、前と同じく枠の光も描かない。
	const std::vector<CompositeTile> full{tile_at(0, 0, 1920, 1080)};
	CHECK(composite_tiles_framed(look, 1920, 1080, full, {true}) == std::vector<bool>{false});
	look.hide_when_single = false;
	CHECK(composite_tiles_framed(look, 1920, 1080, full, {true}) == std::vector<bool>{true});
	CHECK(composite_tiles_labeled(look, 1920, 1080, full, {true}) == std::vector<bool>{false});
}

TEST_CASE("名前ラベル付きで描く枠の人は、描くタイルの人で、空の user_id は入れない")
{
	using cordmux::sources::composite_labeled_tile_users;
	CHECK(composite_labeled_tile_users({true, false, true}, {"a", "b", "c"}) == std::vector<std::string>{"a", "c"});
	CHECK(composite_labeled_tile_users({true, true}, {"", "b"}) == std::vector<std::string>{"b"});
	// 並びの長さが違えば、短い方まで。
	CHECK(composite_labeled_tile_users({true, true}, {"a"}) == std::vector<std::string>{"a"});
	CHECK(composite_labeled_tile_users({}, {"a"}).empty());
}

TEST_CASE("並びに描くのは、名前ラベル付きで描く枠がない人で、渡された順のまま")
{
	using cordmux::sources::composite_roster_shown;
	using cordmux::sources::composite_roster_shows;
	using cordmux::sources::CompositeRosterPerson;
	const CompositeRosterPerson a{"あおい", "a", true};
	const CompositeRosterPerson b{"ken", "b", true};
	const CompositeRosterPerson c{"sakura", "c", true};
	CHECK(composite_roster_shows(a, {}));
	CHECK(composite_roster_shows(a, {"b"}));
	CHECK_FALSE(composite_roster_shows(b, {"b"}));
	const std::vector<CompositeRosterPerson> roster{b, a, c};
	const std::vector<CompositeRosterPerson> shown = composite_roster_shown(roster, {"b", "c"});
	REQUIRE(shown.size() == 1);
	CHECK(shown[0].user_id == "a");
	const std::vector<CompositeRosterPerson> all = composite_roster_shown(roster, {});
	REQUIRE(all.size() == 3);
	CHECK(all[0].user_id == "b");
	CHECK(all[1].user_id == "a");
	CHECK(all[2].user_id == "c");
}

TEST_CASE("描く光は、対象に応じて枠とアバターと名前の光になる")
{
	CompositeLabelLook look;
	look.glow_frame = true;
	look.glow_avatar = true;
	look.glow_name = true;
	auto parts = composite_speaking_parts(look);
	CHECK(parts.frame_glow);
	CHECK(parts.label_glow);

	look.glow_frame = false;
	look.glow_avatar = false;
	look.glow_name = true;
	parts = composite_speaking_parts(look);
	CHECK_FALSE(parts.frame_glow);
	CHECK(parts.label_glow);

	look.glow_name = false;
	parts = composite_speaking_parts(look);
	CHECK_FALSE(parts.frame_glow);
	CHECK_FALSE(parts.label_glow);

	look.glow_frame = true;
	parts = composite_speaking_parts(look);
	CHECK(parts.frame_glow);
	CHECK_FALSE(parts.label_glow);
}

TEST_CASE("光の明るさは発言の時刻から決まり、立ち上がり、保持、消えるまでの形を持つ")
{
	const BorderTiming timing{200, 500, 400};
	const SpeakingTimes talking{1000 * kMs, 0};
	// 話し始めの前は 0、立ち上がりの途中、最大。
	CHECK(composite_speaking_level(talking, 999 * kMs, timing, true, false, 1.0f) == 0.0);
	CHECK(composite_speaking_level(talking, 1100 * kMs, timing, true, false, 1.0f) == doctest::Approx(0.5));
	CHECK(composite_speaking_level(talking, 1500 * kMs, timing, true, false, 1.0f) == doctest::Approx(1.0));

	// 話し終わりの後、保持の間は最大のまま、そのあと直線で 0 になる。
	const SpeakingTimes stopped{1000 * kMs, 2000 * kMs};
	CHECK(composite_speaking_level(stopped, 2400 * kMs, timing, true, false, 1.0f) == doctest::Approx(1.0));
	CHECK(composite_speaking_level(stopped, 2700 * kMs, timing, true, false, 1.0f) == doctest::Approx(0.5));
	CHECK(composite_speaking_level(stopped, 2900 * kMs, timing, true, false, 1.0f) == 0.0);
}

TEST_CASE("OBS のミキサーでミュートか音量 0 の間と、発言の表示を切った設定では、話していても光らせない")
{
	const BorderTiming timing{200, 500, 400};
	const SpeakingTimes talking{1000 * kMs, 0};
	const std::uint64_t now = 1500 * kMs;
	CHECK(composite_speaking_level(talking, now, timing, true, false, 1.0f) == doctest::Approx(1.0));
	// ミュート。
	CHECK(composite_speaking_level(talking, now, timing, true, true, 1.0f) == 0.0);
	// 音量 0、負の値、読めない値 (NaN)。
	CHECK(composite_speaking_level(talking, now, timing, true, false, 0.0f) == 0.0);
	CHECK(composite_speaking_level(talking, now, timing, true, false, -1.0f) == 0.0);
	CHECK(composite_speaking_level(talking, now, timing, true, false, std::numeric_limits<float>::quiet_NaN()) == 0.0);
	// 音量が小さくても 0 でなければ光る (明るさは音量に比例させない)。
	CHECK(composite_speaking_level(talking, now, timing, true, false, 0.01f) == doctest::Approx(1.0));
	// 1 を超える音量 (ミキサーで増幅) も光る。
	CHECK(composite_speaking_level(talking, now, timing, true, false, 2.0f) == doctest::Approx(1.0));
	// 発言を表示しない設定。
	CHECK(composite_speaking_level(talking, now, timing, false, false, 1.0f) == 0.0);
}

TEST_CASE("一度も話していない人は光らない")
{
	const BorderTiming timing{200, 500, 400};
	CHECK(composite_speaking_level(SpeakingTimes{}, 5000 * kMs, timing, true, false, 1.0f) == 0.0);
}

TEST_CASE("見た目の値の比較は、1 つでも違えば違うと判断する")
{
	CompositeLabelLook a;
	CompositeLabelLook b;
	CHECK(a == b);
	b.glow_blur_ratio += 0.1;
	CHECK(a != b);
	b = a;
	b.glow_frame = !a.glow_frame;
	CHECK(a != b);
	b = a;
	b.hide_when_single = !a.hide_when_single;
	CHECK(a != b);
	b = a;
	b.font_family = "Some Font";
	CHECK(a != b);
	b = a;
	b.margin_ratio = 0.05;
	CHECK(a != b);
	b = a;
	b.opacity = 0.5;
	CHECK(a != b);
}

// ---- 新しい画像ができるまでの間に、前の画像を描くか ----

TEST_CASE("前の名前ラベルは、描く人が同じでセルに収まるときだけ描く")
{
	using cordmux::sources::composite_keeps_previous_label;
	// 見た目や大きさだけが変わった (描く人は同じ) ときは、前の画像を描く
	CHECK(composite_keeps_previous_label(true, "u1", "u1", 10, 20, 100, 30, 400, 300));
	// 描く人が替わったときは、違う人の名前を見せない
	CHECK_FALSE(composite_keeps_previous_label(true, "u1", "u2", 10, 20, 100, 30, 400, 300));
	// 描く人が空に替わったときも描かない
	CHECK_FALSE(composite_keeps_previous_label(true, "u1", "", 10, 20, 100, 30, 400, 300));
	// 前の画像がない
	CHECK_FALSE(composite_keeps_previous_label(false, "u1", "u1", 10, 20, 100, 30, 400, 300));
}

TEST_CASE("前の名前ラベルは、セルからはみ出すなら描かない")
{
	using cordmux::sources::composite_keeps_previous_label;
	// ちょうど収まる
	CHECK(composite_keeps_previous_label(true, "u1", "u1", 0, 0, 400, 300, 400, 300));
	// セルが小さくなって、右にはみ出す、下にはみ出す
	CHECK_FALSE(composite_keeps_previous_label(true, "u1", "u1", 10, 20, 100, 30, 100, 300));
	CHECK_FALSE(composite_keeps_previous_label(true, "u1", "u1", 10, 20, 100, 30, 400, 40));
	// 位置が負、大きさが 0
	CHECK_FALSE(composite_keeps_previous_label(true, "u1", "u1", -1, 20, 100, 30, 400, 300));
	CHECK_FALSE(composite_keeps_previous_label(true, "u1", "u1", 10, 20, 0, 30, 400, 300));
}

TEST_CASE("前の枠の光は、セルと同じ大きさのときだけ描く")
{
	using cordmux::sources::composite_keeps_previous_frame_glow;
	CHECK(composite_keeps_previous_frame_glow(true, 960, 540, 960, 540));
	CHECK_FALSE(composite_keeps_previous_frame_glow(true, 1920, 1080, 960, 540));
	CHECK_FALSE(composite_keeps_previous_frame_glow(true, 960, 540, 960, 541));
	CHECK_FALSE(composite_keeps_previous_frame_glow(false, 960, 540, 960, 540));
}

TEST_CASE("暗さの倍率は、明るさ 0 で 1 - 暗さ、0.5 で半分、1 で 1 になる")
{
	using cordmux::sources::composite_label_dim_factor;
	CHECK(composite_label_dim_factor(0.0, 30, true) == doctest::Approx(0.7));
	CHECK(composite_label_dim_factor(0.5, 30, true) == doctest::Approx(0.85));
	CHECK(composite_label_dim_factor(1.0, 30, true) == doctest::Approx(1.0));
	CHECK(composite_label_dim_factor(0.0, 90, true) == doctest::Approx(0.1));
	CHECK(composite_label_dim_factor(0.5, 90, true) == doctest::Approx(0.55));
	CHECK(composite_label_dim_factor(0.25, 40, true) == doctest::Approx(0.7));
}

TEST_CASE("暗さ 0 と、声を聞けない人と、読めない明るさでは、暗くしない")
{
	using cordmux::sources::composite_label_dim_factor;
	// 暗さ 0 (暗くしない設定)。負の値も同じ。
	CHECK(composite_label_dim_factor(0.0, 0, true) == 1.0);
	CHECK(composite_label_dim_factor(0.0, -5, true) == 1.0);
	// cordmux がその人の声を聞けない (自分の枠の人、声の枠が埋まった人): 話していなくても暗くしない。
	CHECK(composite_label_dim_factor(0.0, 30, false) == 1.0);
	CHECK(composite_label_dim_factor(0.0, 90, false) == 1.0);
	// 明るさが NaN: 話している可能性があるので暗くしない。
	CHECK(composite_label_dim_factor(std::numeric_limits<double>::quiet_NaN(), 30, true) == 1.0);
}

TEST_CASE("暗さの倍率は、範囲の外の明るさと暗さを収めて、0 から 1 の間に保つ")
{
	using cordmux::sources::composite_label_dim_factor;
	CHECK(composite_label_dim_factor(-3.0, 30, true) == doctest::Approx(0.7));
	CHECK(composite_label_dim_factor(7.0, 30, true) == doctest::Approx(1.0));
	CHECK(composite_label_dim_factor(0.0, 250, true) == doctest::Approx(0.0));
}

TEST_CASE("暗さに使う明るさは、光の表示を切っていても話した時刻から決まり、ミュートと音量 0 では話していない人として扱う")
{
	using cordmux::sources::composite_label_dim_factor;
	using cordmux::sources::composite_label_dim_level;
	const BorderTiming timing{200, 500, 400};
	const SpeakingTimes talking{1000 * kMs, 0};
	const std::uint64_t now = 1500 * kMs;
	// 光の表示を切る設定 (show_speaking が偽) では光の明るさは 0 だが、暗さに使う明るさは 1 のまま。
	CHECK(composite_speaking_level(talking, now, timing, false, false, 1.0f) == 0.0);
	CHECK(composite_label_dim_level(talking, now, timing, false, 1.0f) == doctest::Approx(1.0));
	CHECK(composite_label_dim_factor(composite_label_dim_level(talking, now, timing, false, 1.0f), 30, true) ==
	      doctest::Approx(1.0));
	// 話していない人は暗い。
	CHECK(composite_label_dim_level(SpeakingTimes{}, now, timing, false, 1.0f) == 0.0);
	// 声が配信に出ていない (ミュート、音量 0) 間は、話していても暗い人と同じ扱い。
	CHECK(composite_label_dim_level(talking, now, timing, true, 1.0f) == 0.0);
	CHECK(composite_label_dim_level(talking, now, timing, false, 0.0f) == 0.0);
}

TEST_CASE("アバターの位置が違う見た目は、違う見た目として比べる")
{
	CompositeLabelLook a;
	CompositeLabelLook b = a;
	CHECK(a == b);
	b.avatar_side = cordmux::config::AvatarSide::Right;
	CHECK(a != b);
}

TEST_CASE("OBS のミキサーでミュートか音量 0 の間は、話していない人も暗くしない")
{
	using cordmux::sources::composite_label_dim_factor;
	CHECK(composite_label_dim_factor(0.0, 30, true, false) == 1.0);
	CHECK(composite_label_dim_factor(0.0, 90, true, false) == 1.0);
	CHECK(composite_label_dim_factor(0.5, 90, true, false) == 1.0);
	// 聞こえている間は今までどおり。
	CHECK(composite_label_dim_factor(0.0, 30, true, true) == doctest::Approx(0.7));
}

TEST_CASE("合成のソースが名前ラベルに掛ける倍率は、話した時刻と暗さと声を聞けるかで決まり、ミュートと音量 0 では暗くしない")
{
	const BorderTiming timing{200, 500, 400};
	const SpeakingTimes talking{1000 * kMs, 0};
	const SpeakingTimes silent{};
	const std::uint64_t now = 1500 * kMs;
	// 話している人は 1、話していない人は 1 - 暗さ。
	CHECK(composite_label_dim_at(talking, now, timing, 30, true, false, 1.0f) == doctest::Approx(1.0));
	CHECK(composite_label_dim_at(silent, now, timing, 30, true, false, 1.0f) == doctest::Approx(0.7));
	// 立ち上がりの途中 (100 ms / 200 ms) は、明るさ 0.5 の倍率。
	const SpeakingTimes rising{1400 * kMs, 0};
	CHECK(composite_label_dim_at(rising, now, timing, 30, true, false, 1.0f) == doctest::Approx(0.85));
	// 暗さ 0、声を聞けない人は暗くしない。
	CHECK(composite_label_dim_at(silent, now, timing, 0, true, false, 1.0f) == 1.0);
	CHECK(composite_label_dim_at(silent, now, timing, 30, false, false, 1.0f) == 1.0);
	// OBS のミキサーでミュート、音量 0、読めない音量の間は、話していない人も暗くしない。
	CHECK(composite_label_dim_at(silent, now, timing, 30, true, true, 1.0f) == 1.0);
	CHECK(composite_label_dim_at(silent, now, timing, 30, true, false, 0.0f) == 1.0);
	CHECK(composite_label_dim_at(silent, now, timing, 30, true, false, std::numeric_limits<float>::quiet_NaN()) ==
	      1.0);
	// 音量が 0 より大きければ、小さくても今までどおり暗くする。
	CHECK(composite_label_dim_at(silent, now, timing, 30, true, false, 0.01f) == doctest::Approx(0.7));
}

TEST_CASE("並びの名前ラベルの鍵は、どれか 1 つの値が違えば違う鍵になる")
{
	CompositeRosterLabelKey base;
	base.user_id = "alice";
	base.person_version = 3;
	base.look_version = 2;
	base.avatar_side = cordmux::config::AvatarSide::Right;
	base.name_q = composite_quantize(23.76);
	base.cell_w = 938;
	base.cell_h = 1080;
	base.others = {"bob", "carol"};
	CHECK(base == base);
	std::vector<CompositeRosterLabelKey> changed(8, base);
	changed[0].user_id = "bob";
	changed[1].person_version = 4;
	changed[2].look_version = 3;
	changed[3].avatar_side = cordmux::config::AvatarSide::Left;
	changed[4].name_q = composite_quantize(24.0);
	changed[5].cell_w = 937;
	changed[6].cell_h = 720;
	changed[7].others = {"bob"};
	for (const CompositeRosterLabelKey &key : changed) {
		CHECK(key != base);
		CHECK(composite_needs_repaint(true, base, key));
	}
	// 名前の順が違うだけでも、違う鍵として描き直す (並びの順が変わったときだけ起きる)。
	CompositeRosterLabelKey reordered = base;
	reordered.others = {"carol", "bob"};
	CHECK(reordered != base);
}

TEST_CASE("並びから消えた人は、前に画像を持っていて、いま描く人にいない人")
{
	CHECK(composite_roster_departed({"a", "b", "c"}, {"b", "d"}) == std::vector<std::string>{"a", "c"});
	CHECK(composite_roster_departed({"a", "b"}, {"b", "a"}).empty());
	// 並びを出さない形 (描く人がいない) では、全員の画像を捨てる。
	CHECK(composite_roster_departed({"a", "b"}, {}) == std::vector<std::string>{"a", "b"});
	CHECK(composite_roster_departed({}, {"a"}).empty());
}
