// SPDX-License-Identifier: Apache-2.0

// composite_check.h の実装。

#include "composite_check.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QString>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "composite_scene.h"
#include "config/settings.h"
#include "sources/composite_draw_cpu.h"
#include "sources/composite_draw_plan.h"
#include "sources/composite_input.h"
#include "sources/composite_label.h"
#include "sources/composite_label_plan.h"
#include "sources/composite_layout.h"
#include "sources/composite_roster.h"
#include "sources/decor_paint.h"
#include "sources/decor_params.h"
#include "sources/label_border.h"

namespace cordmux::decor_preview {

namespace sources = cordmux::sources;
namespace config = cordmux::config;

namespace {

// 要求の 1 人。slot は 0 が自分の枠、1 から 4 が画面共有の枠の番号。
struct CheckPerson {
	int slot = 0;
	std::string user_id;
	std::string name;
	QString avatar_png;      // 空ならアバターの画像がない人 (頭文字の円になる)
	bool video = true;       // その枠に映像を描いたか。偽の枠には何も置かない
	bool hears_voice = true; // cordmux がその人の声を聞けるか (偽なら暗くしない)
};

// 名前ラベルを出す人 1 人 (製品のドックが合成のソースに渡す人。dock/build_observation.h の composite_roster_people)。製品と
// 同じく、名前ラベル付きで描くタイルがない人だけを並びに描く (composite_roster_shows)。タイルの人 (people) を入れると、その人は
// タイルに名前ラベルを描くときは並びに出ず、[1人のときは非表示] で隠すときは並びに出る。並べる順は製品と同じく参照が決める。
struct CheckRosterPerson {
	std::string user_id;
	std::string name;
	QString avatar_png;
	bool hears_voice = true;
};

// 参照の 1 枚。levels は people と、roster_levels は roster と同じ並びで、参加者ごとの話している明るさ (0 から 1。光と暗さが
// 使う明るさで、話している人の光の設定やミキサーで変える前の値)。mixer_audible は、OBS のミキサーで合成のソースが鳴っているか
// (偽はミュートか音量 0 で、光らせず暗くもしない)。
struct CheckRender {
	QString name;
	std::vector<double> levels;
	std::vector<double> roster_levels;
	bool mixer_audible = true;
	QString background_png; // 空なら要求の background_png
};

struct CheckRequest {
	int canvas_w = 1920;
	int canvas_h = 1080;
	QString settings_file;
	bool self_shown = false;
	std::vector<CheckPerson> people;
	std::vector<CheckRosterPerson> roster;
	QString background_png; // 空なら透明 (0, 0, 0, 0)
	std::vector<CheckRender> renders;
};

// 製品が設定と人から作る値。描いた画像はタイル (layout.tiles) の並びと、並びに置いた人 (roster_plan.placed) の並び。
struct Prepared {
	config::Settings settings;
	sources::CompositeLabelLook look;
	sources::CompositeLabelLook paint_look;
	sources::DecorLabelParams params;
	double name_px = 0.0;
	double margin_px = 0.0;
	sources::CompositeLayout layout;
	bool hide = false;
	sources::CompositeSpeakingParts parts;
	std::vector<int> tile_person;         // タイルに載る people の添字。いなければ -1
	std::vector<bool> tile_hears_voice;   // タイルごとの、声を聞けるか (composite_tile_hears_voice)
	std::vector<sources::CompositeLabel> labels;
	std::vector<QImage> frames;

	// 画面共有していない参加者。roster_drawn は製品が描く順 (composite_roster_drawn) の roster の添字で、roster_labels は
	// 同じ並び。roster_plan.placed の index は roster_drawn の添字。
	sources::CompositeRosterParams roster_params;
	int roster_cap_w = 0;
	int roster_gap_px = 0;
	std::vector<std::size_t> roster_drawn;
	std::vector<sources::CompositeLabel> roster_labels;
	sources::CompositeRosterPlan roster_plan;
};

void print_error(const QString &message)
{
	std::fprintf(stderr, "decor_preview: %s\n", message.toUtf8().constData());
}

// 撮った PNG と同じ約束で書く。PNG の 4 つの値に、乗算済みアルファの値をそのまま入れる (Qt にアルファで割り戻させ
// ない)。OBS の GetSourceScreenshot の PNG も、テクスチャの値をこの形で入れている見込みで、比べる側
// (test/tools/composite-check/compare.py) が撮った画像から確認する。
bool save_raw_png(const QImage &image, const QString &path)
{
	const QImage premultiplied = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	const QImage raw(premultiplied.constBits(), premultiplied.width(), premultiplied.height(),
			 premultiplied.bytesPerLine(), QImage::Format_ARGB32);
	return raw.save(path, "PNG");
}

// save_raw_png と同じ約束で読む。PNG の 4 つの値を、乗算済みアルファの値としてそのまま使う。
bool load_raw_png(const QString &path, int width, int height, QImage *out, QString *error)
{
	const QImage loaded(path);
	if (loaded.isNull()) {
		*error = QStringLiteral("PNG を読めません: %1").arg(path);
		return false;
	}
	if (loaded.width() != width || loaded.height() != height) {
		*error = QStringLiteral("PNG の大きさが %1x%2 ではありません: %3").arg(width).arg(height).arg(path);
		return false;
	}
	const QImage straight = loaded.convertToFormat(QImage::Format_ARGB32);
	*out = QImage(straight.constBits(), straight.width(), straight.height(), straight.bytesPerLine(),
		      QImage::Format_ARGB32_Premultiplied)
		       .copy();
	return true;
}

bool read_file(const QString &path, QByteArray *out, QString *error)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		*error = QStringLiteral("ファイルを読めません: %1").arg(path);
		return false;
	}
	*out = file.readAll();
	return true;
}

bool write_json(const QJsonObject &root, const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return false;
	file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
	return true;
}

QJsonArray rect_json(const sources::PixelRect &r)
{
	return QJsonArray{r.x, r.y, r.w, r.h};
}

// 製品 (decor_filter.cpp の set_person_decor と、並びの人の表) と同じく、受け取った PNG をそのまま読む。path が空なら
// 画像のない人 (頭文字の円) にする。
bool load_avatar(const QString &path, QImage *out, QString *error)
{
	*out = QImage();
	if (path.isEmpty())
		return true;
	QByteArray png;
	if (!read_file(path, &png, error))
		return false;
	out->loadFromData(png, "PNG");
	if (out->isNull()) {
		*error = QStringLiteral("アバターの PNG を読めません: %1").arg(path);
		return false;
	}
	return true;
}

const char *kind_name(sources::CompositeDrawKind kind)
{
	switch (kind) {
	case sources::CompositeDrawKind::FrameGlow:
		return "frame_glow";
	case sources::CompositeDrawKind::LabelGlow:
		return "label_glow";
	case sources::CompositeDrawKind::Label:
		return "label";
	case sources::CompositeDrawKind::RosterGlow:
		return "roster_glow";
	case sources::CompositeDrawKind::RosterLabel:
		return "roster_label";
	}
	return "label";
}

// 真偽の値の欄を読む。欄がなければ fallback。真偽でない値は誤りにする (toBool は数や文字列を黙って既定の値にするので、
// 書き損じた要求のまま比べてしまう)。
bool read_bool(const QJsonObject &object, const QString &key, bool fallback, bool *out, const QString &where,
	       QString *error)
{
	const QJsonValue value = object.value(key);
	if (value.isUndefined()) {
		*out = fallback;
		return true;
	}
	if (!value.isBool()) {
		*error = QStringLiteral("%1 の %2 は true か false にします").arg(where, key);
		return false;
	}
	*out = value.toBool();
	return true;
}

// 明るさの並びを読む。count 個の 0 から 1 の数であること。
bool read_levels(const QJsonArray &levels, std::size_t count, std::vector<double> *out, const QString &what,
		 QString *error)
{
	if (levels.size() != static_cast<qsizetype>(count)) {
		*error = QStringLiteral("%1 の数が合いません (%2 個にします)").arg(what).arg(count);
		return false;
	}
	for (const QJsonValue &level : levels) {
		if (!level.isDouble() || level.toDouble() < 0.0 || level.toDouble() > 1.0) {
			*error = QStringLiteral("%1 は 0 から 1 の数にします").arg(what);
			return false;
		}
		out->push_back(level.toDouble());
	}
	return true;
}

bool parse_request(const QByteArray &text, CheckRequest *out, QString *error)
{
	QJsonParseError parse_error;
	const QJsonDocument doc = QJsonDocument::fromJson(text, &parse_error);
	if (!doc.isObject()) {
		*error = QStringLiteral("要求の JSON を読めません: %1").arg(parse_error.errorString());
		return false;
	}
	const QJsonObject root = doc.object();
	const QJsonArray canvas = root.value(QStringLiteral("canvas")).toArray();
	if (canvas.size() != 2 || canvas[0].toInt() <= 0 || canvas[1].toInt() <= 0) {
		*error = QStringLiteral("canvas は [幅, 高さ] の正の整数にします");
		return false;
	}
	out->canvas_w = canvas[0].toInt();
	out->canvas_h = canvas[1].toInt();
	out->settings_file = root.value(QStringLiteral("settings_file")).toString();
	if (out->settings_file.isEmpty()) {
		*error = QStringLiteral("settings_file (製品が使った settings.json のパス) が要ります");
		return false;
	}
	out->background_png = root.value(QStringLiteral("background_png")).toString();

	const QJsonArray people = root.value(QStringLiteral("people")).toArray();
	if (people.isEmpty() || people.size() > sources::kCompositeCellCount + 1) {
		*error = QStringLiteral("people は 1 人から 5 人にします");
		return false;
	}
	bool has_self = false;
	for (const QJsonValue &value : people) {
		const QJsonObject p = value.toObject();
		CheckPerson person;
		person.slot = p.value(QStringLiteral("slot")).toInt(-1);
		if (person.slot < 0 || person.slot > sources::kCompositeCellCount) {
			*error = QStringLiteral("people の slot は 0 (自分の枠) から 4 にします");
			return false;
		}
		const auto same_slot = [&](const CheckPerson &other) { return other.slot == person.slot; };
		const bool duplicated = std::any_of(out->people.begin(), out->people.end(), same_slot);
		if (duplicated) {
			*error = QStringLiteral("people に同じ slot が 2 つあります: %1").arg(person.slot);
			return false;
		}
		// 名前ラベルの隅は、製品と同じく設定の label_corner を全員に使う。参加者ごとの隅を書いた要求は、製品と違う画像を
		// 参照にしてしまうので断る。
		if (p.contains(QStringLiteral("corner"))) {
			*error = QStringLiteral("people の corner は使えません (名前ラベルの隅は settings.json の label_corner で、全員一律です)");
			return false;
		}
		has_self = has_self || person.slot == 0;
		person.user_id = p.value(QStringLiteral("user_id")).toString().toStdString();
		person.name = p.value(QStringLiteral("name")).toString().toStdString();
		person.avatar_png = p.value(QStringLiteral("avatar_png")).toString();
		if (!read_bool(p, QStringLiteral("video"), true, &person.video, QStringLiteral("people"), error) ||
		    !read_bool(p, QStringLiteral("hears_voice"), true, &person.hears_voice, QStringLiteral("people"), error))
			return false;
		out->people.push_back(person);
	}
	out->self_shown = root.value(QStringLiteral("self_shown")).toBool(has_self);
	if (out->self_shown && !has_self) {
		*error = QStringLiteral("self_shown が真なら、people に slot 0 の人が要ります");
		return false;
	}

	for (const QJsonValue &value : root.value(QStringLiteral("roster")).toArray()) {
		const QJsonObject p = value.toObject();
		CheckRosterPerson person;
		person.user_id = p.value(QStringLiteral("user_id")).toString().toStdString();
		person.name = p.value(QStringLiteral("name")).toString().toStdString();
		if (person.user_id.empty() || person.name.empty()) {
			*error = QStringLiteral("roster の人には user_id と name が要ります");
			return false;
		}
		person.avatar_png = p.value(QStringLiteral("avatar_png")).toString();
		if (!read_bool(p, QStringLiteral("hears_voice"), true, &person.hears_voice, QStringLiteral("roster"), error))
			return false;
		out->roster.push_back(person);
	}

	const QRegularExpression name_pattern(QStringLiteral("^[A-Za-z0-9_-]+$"));
	const QJsonArray renders = root.value(QStringLiteral("renders")).toArray();
	if (renders.isEmpty()) {
		*error = QStringLiteral("renders に 1 枚以上の参照を書きます");
		return false;
	}
	for (const QJsonValue &value : renders) {
		const QJsonObject r = value.toObject();
		CheckRender render;
		render.name = r.value(QStringLiteral("name")).toString();
		if (!name_pattern.match(render.name).hasMatch()) {
			*error = QStringLiteral("renders の name は英数字と _ と - だけにします: %1").arg(render.name);
			return false;
		}
		const QString where = QStringLiteral("renders (%1)").arg(render.name);
		if (!read_levels(r.value(QStringLiteral("levels")).toArray(), out->people.size(), &render.levels,
				 where + QStringLiteral(" の levels"), error))
			return false;
		if (r.contains(QStringLiteral("roster_levels"))) {
			if (!read_levels(r.value(QStringLiteral("roster_levels")).toArray(), out->roster.size(),
					 &render.roster_levels, where + QStringLiteral(" の roster_levels"), error))
				return false;
		} else {
			render.roster_levels.assign(out->roster.size(), 0.0);
		}
		if (!read_bool(r, QStringLiteral("mixer_audible"), true, &render.mixer_audible, where, error))
			return false;
		render.background_png = r.value(QStringLiteral("background_png")).toString();
		out->renders.push_back(render);
	}
	return true;
}

// 製品と同じ関数で、設定から見た目の値を、人から配置と名前ラベルと枠の光を、画面共有していない参加者から並びの名前ラベルと
// 位置を作る。製品の描画のスレッドの手順 (sources/composite_decor.cpp の draw、update_tile、update_roster) と、ドックが
// 渡す値 (dock/dock_poll_layout.cpp の push_composite_input と distribute_person_decor) と同じ値を同じ順で渡す。
bool prepare(const CheckRequest &request, Prepared *out, QString *error)
{
	QByteArray settings_text;
	if (!read_file(request.settings_file, &settings_text, error))
		return false;
	std::string parse_error;
	// 製品は読めない settings.json を既定値で使うが、参照がそれに合わせると、書き損じた設定のまま比べてしまうので
	// 止める。
	const std::string_view settings_view(settings_text.constData(), static_cast<std::size_t>(settings_text.size()));
	if (!config::parse_settings(settings_view, &out->settings, &parse_error)) {
		*error = QStringLiteral("settings.json を製品が読めません (%1): %2")
				 .arg(QString::fromStdString(parse_error), request.settings_file);
		return false;
	}
	const config::Settings &s = out->settings;
	out->look = sources::composite_label_look_from_settings(s);
	out->params = sources::decor_label_params_from_settings(s);
	out->name_px = sources::composite_name_px(request.canvas_h, out->params.em_ratio);
	out->margin_px = sources::composite_margin_px(request.canvas_h, out->look.margin_ratio);
	out->parts = sources::composite_speaking_parts(out->look);
	// 光を描かない形のときは、光の画像を作らない (composite_decor.cpp の update_tile と同じ)。
	out->paint_look = out->look;
	if (!out->parts.label_glow) {
		out->paint_look.glow_avatar = false;
		out->paint_look.glow_name = false;
	}

	// 配置。ドックは見えている画面共有の枠と、全面にする 1 人の決まり (真) と、すき間の設定と、参加者ごとの声を聞けるかを
	// 渡す。
	std::vector<int> visible_slots;
	std::vector<sources::SlotPerson> slot_people;
	bool self_hears_voice = false;
	std::string self_user_id;
	for (const CheckPerson &person : request.people) {
		if (person.slot == 0) {
			self_hears_voice = person.hears_voice;
			self_user_id = person.user_id;
			continue;
		}
		visible_slots.push_back(person.slot);
		slot_people.push_back({person.slot, person.name, person.user_id, person.hears_voice});
	}
	sources::CompositeInput input =
		sources::make_composite_input(visible_slots, slot_people, request.self_shown, true, s.tile_gap_px);
	input.self_hears_voice = self_hears_voice;
	input.self_user_id = self_user_id;
	out->layout = sources::plan_composite_layout(
		sources::make_layout_request(input, request.canvas_w, request.canvas_h));
	out->tile_hears_voice = sources::composite_tile_hears_voice(input, out->layout.tiles);
	out->hide = sources::composite_hides_label(out->look, request.canvas_w, request.canvas_h, out->layout.tiles) ||
		    !(out->name_px > 0.0);
	// タイルに載る人と、タイルごとに名前ラベルを描くか (CompositeDecor::draw と同じ composite_tiles_labeled)。名前ラベル付きで
	// 描くタイルの人は、並びに描かない (composite_roster_shows)。
	std::vector<bool> tile_video;
	for (const sources::CompositeTile &tile : out->layout.tiles) {
		int index = -1;
		for (std::size_t i = 0; i < request.people.size(); ++i) {
			if (request.people[i].slot == (tile.self ? 0 : tile.slot))
				index = static_cast<int>(i);
		}
		out->tile_person.push_back(index);
		tile_video.push_back(index >= 0 && request.people[static_cast<std::size_t>(index)].video);
	}
	const std::vector<bool> tile_labeled = sources::composite_tiles_labeled(out->look, request.canvas_w, request.canvas_h,
										 out->layout.tiles, tile_video);
	const std::vector<std::string> labeled_users =
		sources::composite_labeled_tile_users(tile_labeled, sources::composite_tile_user_ids(input, out->layout.tiles));

	// 画面共有していない参加者 (refresh_roster と update_roster と同じ手順)。出さない形 (roster_show が偽か、名前の
	// 大きさが 0) では描かない。描く人の名前は、画面共有の枠の名前ラベルでも、ほかの人と見分けるために使う
	// (CompositeDecor::draw の roster_names)。
	out->roster_params = sources::composite_roster_params_from_settings(s);
	out->roster_cap_w = sources::composite_roster_cap_width(request.canvas_w, out->margin_px);
	out->roster_gap_px = sources::composite_roster_gap_px(out->roster_params.gap_ratio, out->name_px);
	const bool show_roster = out->roster_params.show && out->name_px > 0.0;
	std::vector<std::string> roster_names;
	if (show_roster) {
		std::vector<sources::CompositeRosterPerson> roster_people;
		for (const CheckRosterPerson &person : request.roster)
			roster_people.push_back({person.name, person.user_id, person.hears_voice});
		for (const std::size_t index : sources::composite_roster_drawn(roster_people)) {
			if (sources::composite_roster_shows(roster_people[index], labeled_users))
				out->roster_drawn.push_back(index);
		}
		for (const std::size_t index : out->roster_drawn)
			roster_names.push_back(request.roster[index].name);
	}

	// 並びの名前ラベルが、ほかの人と見分けるために使う、名前ラベル付きで描くタイルの人の名前 (update_roster の tile_names)。
	std::vector<std::string> tile_names;
	for (std::size_t t = 0; t < out->layout.tiles.size(); ++t) {
		const sources::CompositeTile &tile = out->layout.tiles[t];
		const int index = out->tile_person[t];
		sources::CompositeLabel label;
		// 名前ラベルを枠に描かない設定 (label_show_in_slot が偽) では、製品の合成のソースと同じく、タイルの名前ラベルの画像を
		// 作らない (枠の光だけを置く)。
		if (out->look.show_in_slot && index >= 0 && !request.people[static_cast<std::size_t>(index)].user_id.empty()) {
			const CheckPerson &person = request.people[static_cast<std::size_t>(index)];
			if (tile_labeled[t] && out->name_px > 0.0)
				tile_names.push_back(person.name);
			sources::LabelContent content;
			content.name = person.name;
			// 名前ラベルの隅は全員一律の設定 (distribute_person_decor と同じ)。
			content.corner = sources::decor_corner_of(s.label_corner);
			if (!load_avatar(person.avatar_png, &content.avatar, error))
				return false;
			// 同じ画面に見えているほかの人の名前。映像を描いている人 (本人を除く。distribute_person_decor と同じ) と、
			// 画面共有していない参加者 (update_tile が足す)。
			for (const CheckPerson &other : request.people) {
				if (other.slot != person.slot && other.video && !other.name.empty() &&
				    (other.slot != 0 || request.self_shown))
					content.other_visible_names.push_back(other.name);
			}
			content.other_visible_names.insert(content.other_visible_names.end(), roster_names.begin(),
							   roster_names.end());
			label = sources::paint_composite_label(content, out->paint_look, out->name_px, out->margin_px,
							       tile.rect.w, tile.rect.h);
		}
		out->labels.push_back(label);
		out->frames.push_back(out->parts.frame_glow
					      ? sources::paint_frame_glow(tile.rect.w, tile.rect.h, out->look.glow_argb,
									  out->look.glow_blur_ratio * out->name_px,
									  out->look.glow_spread_ratio * out->name_px)
					      : QImage());
	}

	if (!show_roster)
		return true;
	sources::CompositeLabelLook roster_look = out->paint_look;
	roster_look.avatar_side = out->roster_params.avatar_side;
	sources::CompositeRosterRequest layout;
	layout.canvas_w = request.canvas_w;
	layout.canvas_h = request.canvas_h;
	layout.margin_px = static_cast<int>(std::lround(out->margin_px));
	layout.gap_px = out->roster_gap_px;
	layout.anchor = out->roster_params.anchor;
	layout.direction = out->roster_params.direction;
	layout.avatar_side = out->roster_params.avatar_side;
	layout.offset_x_px = out->roster_params.offset_x_px;
	layout.offset_y_px = out->roster_params.offset_y_px;
	for (std::size_t k = 0; k < out->roster_drawn.size(); ++k) {
		const CheckRosterPerson &person = request.roster[out->roster_drawn[k]];
		// 名前の幅の上限の幅と、キャンバスの高さのセルに、余白 0 で左上の隅に描く。ほかの人と見分けるための名前は、
		// 見えている名前の全部 (名前ラベル付きのタイルの人と、並びの人) から本人を除いたもの (update_roster と同じ
		// composite_roster_others)。
		sources::LabelContent content;
		content.name = person.name;
		content.corner = sources::DecorCorner::TopLeft;
		if (!load_avatar(person.avatar_png, &content.avatar, error))
			return false;
		std::vector<std::string> visible_names = tile_names;
		for (const std::size_t drawn : out->roster_drawn)
			visible_names.push_back(request.roster[drawn].name);
		content.other_visible_names = sources::composite_roster_others(visible_names, person.name);
		const sources::CompositeLabel label = sources::paint_composite_label(
			content, roster_look, out->name_px, 0.0, out->roster_cap_w, request.canvas_h);
		sources::CompositeRosterBox box;
		if (!label.label.isNull()) {
			box.w = label.box_w;
			box.h = label.box_h;
		}
		layout.boxes.push_back(box);
		out->roster_labels.push_back(label);
	}
	out->roster_plan = sources::plan_composite_roster(layout);
	return true;
}

sources::CompositeDrawImage image_of(const QImage &image, int x, int y)
{
	sources::CompositeDrawImage drawn;
	drawn.present = !image.isNull();
	drawn.x = x;
	drawn.y = y;
	drawn.w = image.width();
	drawn.h = image.height();
	return drawn;
}

// 話している明るさ level から、光の明るさを決める。製品の composite_speaking_level は、話している人の光を表示しない設定か、
// ミキサーで鳴っていない (ミュートか音量 0) ときはいつも 0 を返す。
double glow_level(const Prepared &p, double level, bool mixer_audible)
{
	return p.params.show_speaking && mixer_audible ? std::clamp(level, 0.0, 1.0) : 0.0;
}

// 名前ラベルの色に掛ける倍率。製品の composite_label_dim_at と同じく、明るさは話している人の光の設定に依らない値を使う。
double label_dim(const Prepared &p, double level, bool hears_voice, bool mixer_audible)
{
	return sources::composite_label_dim_factor(std::clamp(level, 0.0, 1.0), p.params.dim_pct, hears_voice,
						   mixer_audible);
}

// 1 枚の参照の、置く物の計画。並びの画像の位置は、名前ラベルを描いたセルの左上から見た位置なので、矩形の左上 (box_x、
// box_y) と置く位置の差だけずらす (update_roster と同じ)。
std::vector<sources::CompositeDraw> plan_draws(const CheckRequest &request, const Prepared &p,
					       const CheckRender &render)
{
	sources::CompositeDrawRequest draw_request;
	draw_request.canvas_w = request.canvas_w;
	draw_request.canvas_h = request.canvas_h;
	draw_request.hide = p.hide;
	draw_request.parts = p.parts;
	for (std::size_t i = 0; i < p.layout.tiles.size(); ++i) {
		const int index = p.tile_person[i];
		const CheckPerson *person = index >= 0 ? &request.people[static_cast<std::size_t>(index)] : nullptr;
		const sources::CompositeLabel &label = p.labels[i];
		const double level = person != nullptr ? render.levels[static_cast<std::size_t>(index)] : 0.0;
		const bool hears = i < p.tile_hears_voice.size() && p.tile_hears_voice[i];
		sources::CompositeDrawTile tile;
		tile.cell = p.layout.tiles[i].rect;
		tile.has_video = person != nullptr && person->video;
		tile.level = glow_level(p, level, render.mixer_audible);
		tile.label_dim = label_dim(p, level, hears, render.mixer_audible);
		tile.label = image_of(label.label, label.label_x, label.label_y);
		tile.glow = image_of(label.glow, label.glow_x, label.glow_y);
		tile.label_current = true;
		tile.label_owner = person != nullptr ? person->user_id : std::string();
		tile.owner = tile.label_owner;
		tile.frame = image_of(p.frames[i], 0, 0);
		tile.frame_current = true;
		draw_request.tiles.push_back(tile);
	}
	for (const sources::CompositeRosterPlaced &spot : p.roster_plan.placed) {
		const sources::CompositeLabel &label = p.roster_labels[spot.index];
		const std::size_t person = p.roster_drawn[spot.index];
		const double level = render.roster_levels[person];
		const int dx = spot.x - label.box_x;
		const int dy = spot.y - label.box_y;
		sources::CompositeDrawRosterEntry entry;
		entry.label = image_of(label.label, label.label_x + dx, label.label_y + dy);
		entry.glow = image_of(label.glow, label.glow_x + dx, label.glow_y + dy);
		entry.level = glow_level(p, level, render.mixer_audible);
		entry.dim = label_dim(p, level, request.roster[person].hears_voice, render.mixer_audible);
		draw_request.roster.push_back(entry);
	}
	return sources::plan_composite_draws(draw_request);
}

std::vector<sources::CompositeTileImages> tile_images(const Prepared &p)
{
	std::vector<sources::CompositeTileImages> images;
	for (std::size_t i = 0; i < p.layout.tiles.size(); ++i)
		images.push_back({p.labels[i].label, p.labels[i].glow, p.frames[i]});
	return images;
}

// 並びに置いた人の画像。計画の RosterGlow と RosterLabel の tile (置いた順の添字) と同じ並び。
std::vector<sources::CompositeTileImages> roster_images(const Prepared &p)
{
	std::vector<sources::CompositeTileImages> images;
	for (const sources::CompositeRosterPlaced &spot : p.roster_plan.placed)
		images.push_back({p.roster_labels[spot.index].label, p.roster_labels[spot.index].glow, QImage()});
	return images;
}

QJsonObject draw_json(const sources::CompositeDraw &draw)
{
	QJsonObject d;
	d[QStringLiteral("kind")] = QString::fromLatin1(kind_name(draw.kind));
	d[QStringLiteral("tile")] = static_cast<int>(draw.tile);
	d[QStringLiteral("image")] = rect_json(draw.image);
	d[QStringLiteral("clip")] = rect_json(draw.clip);
	d[QStringLiteral("level")] = draw.level;
	d[QStringLiteral("dim")] = draw.dim;
	return d;
}

// 名前ラベルの描画の結果のうち、比べる側と人が読む値。
void put_label_result(QJsonObject *t, const sources::CompositeLabel &label)
{
	(*t)[QStringLiteral("name_drawn")] = label.name_drawn;
	(*t)[QStringLiteral("elided")] = label.elided;
	(*t)[QStringLiteral("elided_middle")] = label.elided_middle;
	(*t)[QStringLiteral("avatar_dropped")] = label.avatar_dropped;
	(*t)[QStringLiteral("name_hidden")] = label.name_hidden;
	(*t)[QStringLiteral("initial_drawn")] = label.initial_drawn;
	(*t)[QStringLiteral("font_family_used")] = label.font_family_used;
}

QJsonObject tile_json(const CheckRequest &request, const Prepared &p, std::size_t i)
{
	const sources::CompositeTile &tile = p.layout.tiles[i];
	const sources::CompositeLabel &label = p.labels[i];
	QJsonObject t;
	t[QStringLiteral("index")] = static_cast<int>(i);
	t[QStringLiteral("self")] = tile.self;
	t[QStringLiteral("slot")] = tile.slot;
	t[QStringLiteral("rect")] = rect_json(tile.rect);
	const int index = p.tile_person[i];
	t[QStringLiteral("person")] = index;
	if (index >= 0)
		t[QStringLiteral("user_id")] =
			QString::fromStdString(request.people[static_cast<std::size_t>(index)].user_id);
	t[QStringLiteral("hears_voice")] = i < p.tile_hears_voice.size() && p.tile_hears_voice[i];
	// 名前ラベルと光の画像の位置は、キャンバスのピクセルで書く (計画の image と同じ目盛り)。
	const auto placed = [&](const QImage &image, int x, int y) {
		return image.isNull() ? QJsonValue() : QJsonValue(QJsonArray{tile.rect.x + x, tile.rect.y + y,
									    image.width(), image.height()});
	};
	t[QStringLiteral("label")] = placed(label.label, label.label_x, label.label_y);
	t[QStringLiteral("glow")] = placed(label.glow, label.glow_x, label.glow_y);
	t[QStringLiteral("frame")] = placed(p.frames[i], 0, 0);
	t[QStringLiteral("box")] =
		QJsonArray{tile.rect.x + label.box_x, tile.rect.y + label.box_y, label.box_w, label.box_h};
	t[QStringLiteral("avatar")] =
		QJsonArray{tile.rect.x + label.avatar_x, tile.rect.y + label.avatar_y, label.avatar_d, label.avatar_d};
	put_label_result(&t, label);
	return t;
}

// 並びに置いた 1 人。index は計画の RosterGlow と RosterLabel の tile と同じ添字、person は要求の roster の添字。
QJsonObject roster_json(const CheckRequest &request, const Prepared &p, std::size_t placed_index)
{
	const sources::CompositeRosterPlaced &spot = p.roster_plan.placed[placed_index];
	const sources::CompositeLabel &label = p.roster_labels[spot.index];
	const std::size_t person = p.roster_drawn[spot.index];
	const int dx = spot.x - label.box_x;
	const int dy = spot.y - label.box_y;
	QJsonObject r;
	r[QStringLiteral("index")] = static_cast<int>(placed_index);
	r[QStringLiteral("order")] = static_cast<int>(spot.index);
	r[QStringLiteral("person")] = static_cast<int>(person);
	r[QStringLiteral("user_id")] = QString::fromStdString(request.roster[person].user_id);
	r[QStringLiteral("hears_voice")] = request.roster[person].hears_voice;
	const auto placed = [&](const QImage &image, int x, int y) {
		return image.isNull() ? QJsonValue() : QJsonValue(QJsonArray{x + dx, y + dy, image.width(), image.height()});
	};
	r[QStringLiteral("label")] = placed(label.label, label.label_x, label.label_y);
	r[QStringLiteral("glow")] = placed(label.glow, label.glow_x, label.glow_y);
	r[QStringLiteral("box")] = QJsonArray{spot.x, spot.y, label.box_w, label.box_h};
	r[QStringLiteral("avatar")] =
		QJsonArray{label.avatar_x + dx, label.avatar_y + dy, label.avatar_d, label.avatar_d};
	put_label_result(&r, label);
	return r;
}

bool parse_size(const QString &text, int *w, int *h)
{
	const QStringList parts = text.split(QLatin1Char('x'));
	bool ok_w = false;
	bool ok_h = false;
	*w = parts.size() == 2 ? parts[0].toInt(&ok_w) : 0;
	*h = parts.size() == 2 ? parts[1].toInt(&ok_h) : 0;
	return ok_w && ok_h && *w > 0 && *h > 0 && *w <= 7680 && *h <= 4320;
}

// args から、オプションの名前の後の値を順に集める。--backend と --system-font は main が Qt を起動する前に読んで
// いるので、値ごと読み飛ばす。
bool collect_args(const QStringList &args, const QString &command, int value_count, QStringList *values,
		  QString *out_dir, QString *error)
{
	for (int i = 1; i < args.size(); ++i) {
		const QString a = args[i];
		if (a == QStringLiteral("--backend") || a == QStringLiteral("--system-font")) {
			++i;
			continue;
		}
		if (a == command) {
			if (i + value_count >= args.size()) {
				*error = QStringLiteral("%1 には値が %2 つ要ります").arg(command).arg(value_count);
				return false;
			}
			for (int k = 0; k < value_count; ++k)
				values->append(args[++i]);
			continue;
		}
		if (out_dir != nullptr && a == QStringLiteral("--out") && i + 1 < args.size()) {
			*out_dir = args[++i];
			continue;
		}
		*error = QStringLiteral("%1 が知らない引数です: %2").arg(command, a);
		return false;
	}
	return true;
}

} // namespace

int run_composite_check(const QStringList &args)
{
	QStringList values;
	QString out_dir;
	QString error;
	if (!collect_args(args, QStringLiteral("--composite-check"), 1, &values, &out_dir, &error)) {
		print_error(error);
		return 2;
	}
	if (out_dir.isEmpty()) {
		print_error(QStringLiteral("--composite-check には --out DIR が要ります"));
		return 2;
	}
	QByteArray request_text;
	CheckRequest request;
	Prepared prepared;
	if (!read_file(values[0], &request_text, &error) || !parse_request(request_text, &request, &error) ||
	    !prepare(request, &prepared, &error)) {
		print_error(error);
		return 2;
	}
	QImage default_background(request.canvas_w, request.canvas_h, QImage::Format_ARGB32_Premultiplied);
	default_background.fill(Qt::transparent);
	if (!request.background_png.isEmpty() &&
	    !load_raw_png(request.background_png, request.canvas_w, request.canvas_h, &default_background, &error)) {
		print_error(error);
		return 2;
	}
	if (!QDir().mkpath(out_dir)) {
		print_error(QStringLiteral("フォルダーを作れません: %1").arg(out_dir));
		return 1;
	}
	const std::vector<sources::CompositeTileImages> images = tile_images(prepared);
	const std::vector<sources::CompositeTileImages> rosters = roster_images(prepared);

	// 参照の画像。参加者ごとの明るさで計画を作り、背景の上に CPU で重ねる。
	QJsonArray renders_json;
	for (const CheckRender &render : request.renders) {
		QImage canvas = default_background;
		if (!render.background_png.isEmpty() &&
		    !load_raw_png(render.background_png, request.canvas_w, request.canvas_h, &canvas, &error)) {
			print_error(error);
			return 2;
		}
		const std::vector<sources::CompositeDraw> draws = plan_draws(request, prepared, render);
		sources::draw_composite_plan(&canvas, draws, images, rosters);
		const QString file = QStringLiteral("ref-%1.png").arg(render.name);
		if (!save_raw_png(canvas, out_dir + QLatin1Char('/') + file)) {
			print_error(QStringLiteral("書き出せません: %1/%2").arg(out_dir, file));
			return 1;
		}
		QJsonArray draws_json;
		for (const sources::CompositeDraw &draw : draws)
			draws_json.append(draw_json(draw));
		QJsonArray levels_json;
		for (const double level : render.levels)
			levels_json.append(level);
		QJsonArray roster_levels_json;
		for (const double level : render.roster_levels)
			roster_levels_json.append(level);
		QJsonObject r;
		r[QStringLiteral("name")] = render.name;
		r[QStringLiteral("file")] = file;
		r[QStringLiteral("levels")] = levels_json;
		r[QStringLiteral("roster_levels")] = roster_levels_json;
		r[QStringLiteral("mixer_audible")] = render.mixer_audible;
		r[QStringLiteral("draws")] = draws_json;
		renders_json.append(r);
	}

	// 置く物を 1 つずつ、透明の上に最大の明るさで置いた画像 (名前ラベルは暗くしない)。比べる側が、ピクセルごとに重なる物の
	// 数と、明るさと暗さを掛ける前の値を読む。
	QJsonArray objects_json;
	CheckRender brightest;
	brightest.levels.assign(request.people.size(), 1.0);
	brightest.roster_levels.assign(request.roster.size(), 1.0);
	const std::vector<sources::CompositeDraw> full = plan_draws(request, prepared, brightest);
	for (std::size_t j = 0; j < full.size(); ++j) {
		QImage canvas(request.canvas_w, request.canvas_h, QImage::Format_ARGB32_Premultiplied);
		canvas.fill(Qt::transparent);
		sources::draw_composite_plan(&canvas, {full[j]}, images, rosters);
		const QString file = QStringLiteral("obj-%1.png").arg(j);
		if (!save_raw_png(canvas, out_dir + QLatin1Char('/') + file)) {
			print_error(QStringLiteral("書き出せません: %1/%2").arg(out_dir, file));
			return 1;
		}
		QJsonObject o = draw_json(full[j]);
		o[QStringLiteral("file")] = file;
		objects_json.append(o);
	}

	QJsonArray tiles_json;
	for (std::size_t i = 0; i < prepared.layout.tiles.size(); ++i)
		tiles_json.append(tile_json(request, prepared, i));
	QJsonArray roster_json_list;
	for (std::size_t i = 0; i < prepared.roster_plan.placed.size(); ++i)
		roster_json_list.append(roster_json(request, prepared, i));
	QJsonObject root;
	root[QStringLiteral("canvas")] = QJsonArray{request.canvas_w, request.canvas_h};
	const std::string settings_text = config::settings_to_json(prepared.settings);
	root[QStringLiteral("settings")] = QJsonDocument::fromJson(QByteArray::fromStdString(settings_text)).object();
	root[QStringLiteral("name_px")] = prepared.name_px;
	root[QStringLiteral("margin_px")] = prepared.margin_px;
	root[QStringLiteral("hide")] = prepared.hide;
	root[QStringLiteral("show_speaking")] = prepared.params.show_speaking;
	root[QStringLiteral("dim_pct")] = prepared.params.dim_pct;
	root[QStringLiteral("parts")] = QJsonObject{{QStringLiteral("frame_glow"), prepared.parts.frame_glow},
						    {QStringLiteral("label_glow"), prepared.parts.label_glow}};
	root[QStringLiteral("layout_log_line")] = QString::fromStdString(
		sources::composite_layout_log_line(request.canvas_w, request.canvas_h, prepared.layout.tiles));
	root[QStringLiteral("tiles")] = tiles_json;
	root[QStringLiteral("roster")] = roster_json_list;
	root[QStringLiteral("roster_drawn")] = static_cast<int>(prepared.roster_drawn.size());
	root[QStringLiteral("roster_not_placed")] = prepared.roster_plan.not_placed;
	root[QStringLiteral("roster_cap_w")] = prepared.roster_cap_w;
	root[QStringLiteral("roster_gap_px")] = prepared.roster_gap_px;
	root[QStringLiteral("renders")] = renders_json;
	root[QStringLiteral("objects")] = objects_json;
	if (!write_json(root, out_dir + QStringLiteral("/check.json"))) {
		print_error(QStringLiteral("書き出せません: %1/check.json").arg(out_dir));
		return 1;
	}
	std::printf("%s\n", QDir::toNativeSeparators(out_dir + QStringLiteral("/check.json")).toUtf8().constData());
	return 0;
}

int run_composite_check_background(const QStringList &args)
{
	QStringList values;
	QString error;
	if (!collect_args(args, QStringLiteral("--composite-check-background"), 3, &values, nullptr, &error)) {
		print_error(error);
		return 2;
	}
	static const struct {
		const char *name;
		int kind;
	} kinds[] = {{"dark-game", kDarkGame},
		     {"bright-sky", kBrightSky},
		     {"stripes", kStripes},
		     {"snowy-field", kSnowyField}};
	int kind = -1;
	for (const auto &k : kinds) {
		if (values[0] == QLatin1String(k.name))
			kind = k.kind;
	}
	int w = 0;
	int h = 0;
	if (kind < 0 || !parse_size(values[1], &w, &h)) {
		print_error(QStringLiteral("--composite-check-background <dark-game|bright-sky|stripes|snowy-field> "
					   "<幅>x<高さ> <PNG> の形で渡します"));
		return 2;
	}
	if (!save_raw_png(make_scene_background(kind, w, h), values[2])) {
		print_error(QStringLiteral("書き出せません: %1").arg(values[2]));
		return 1;
	}
	std::printf("%s\n", QDir::toNativeSeparators(values[2]).toUtf8().constData());
	return 0;
}

int run_composite_check_avatar(const QStringList &args)
{
	QStringList values;
	QString error;
	if (!collect_args(args, QStringLiteral("--composite-check-avatar"), 2, &values, nullptr, &error)) {
		print_error(error);
		return 2;
	}
	bool ok = false;
	const int hue = values[0].toInt(&ok);
	if (!ok || hue < 0 || hue > 359) {
		print_error(QStringLiteral("--composite-check-avatar の色相は 0 から 359 にします"));
		return 2;
	}
	if (!save_raw_png(make_scene_avatar(hue), values[1])) {
		print_error(QStringLiteral("書き出せません: %1").arg(values[1]));
		return 1;
	}
	std::printf("%s\n", QDir::toNativeSeparators(values[1]).toUtf8().constData());
	return 0;
}

} // namespace cordmux::decor_preview
