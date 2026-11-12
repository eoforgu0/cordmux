// SPDX-License-Identifier: Apache-2.0

// composite_sample.h の実装。

#include "composite_sample.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QRect>
#include <QString>

#include <algorithm>
#include <array>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "composite_scene.h"
#include "config/settings.h"
#include "sources/composite_label.h"
#include "sources/composite_layout.h"

namespace cordmux::decor_preview {

namespace sources = cordmux::sources;
namespace config = cordmux::config;

namespace {

constexpr int kPeopleCount = kSamplePeopleCount;

struct Args {
	QString out_dir;
	QString tag;
	int canvas_w = 1920;
	int canvas_h = 1080;
	int speaker = 1; // グローを最大の明るさで描く枠の位置 (0 左上、1 右上、2 左下、3 右下)。-1 なら誰も話していない
	sources::DecorCorner corner = sources::DecorCorner::BottomLeft;
	config::Settings settings;
	QString glow_text = QStringLiteral("avatar,name"); // 出力のファイル名に使う。--glow がなければ設定の既定値の形
	// 席 (0 は自分の枠、1 から 3 は画面共有の枠) ごとの人 (scene_people() の添字)。--people で替える。
	std::array<int, kPeopleCount> people{0, 1, 2, 3};
	// 枠の位置 (左上、右上、左下、右下) ごとに、模様の代わりに描く画像。--tile-images で読む。空なら模様を描く。
	std::array<QImage, kPeopleCount> tile_images;
};

// 1 枚のキャンバスを描く入力。
struct RenderInput {
	int canvas_w = 1920;
	int canvas_h = 1080;
	config::Settings settings;
	int speaker = 1;    // 話している枠の位置 (-1 ならだれも話していない)
	double level = 1.0; // 話している枠の明るさ
	sources::DecorCorner corner = sources::DecorCorner::BottomLeft;
	// 席 (0 は自分の枠、1 から 3 は画面共有の枠の番号) ごとの人 (scene_people() の添字) と、枠の位置ごとの背景の種類。
	std::array<int, kPeopleCount> people{0, 1, 2, 3};
	std::array<int, kPeopleCount> backgrounds{kDarkGame, kBrightSky, kStripes, kSnowyField};
	// 枠の位置ごとに、backgrounds の模様の代わりに描く画像 (空なら模様を描く)。枠を覆うように拡大し、はみ出た分を
	// 中央で切る (製品の映像の枠の収め方とは別の、見本のための描き方)。
	std::array<QImage, kPeopleCount> background_images;
};

struct RenderOutput {
	QImage canvas;
	double name_px = 0.0;
	double margin_px = 0.0;
	std::vector<sources::CompositeTile> tiles;
	std::vector<sources::CompositeLabel> labels;
};

// 製品の配置で枠を並べ、背景 (映像の代わり) を描いた後、製品の計画のとおりに名前ラベルと光を CPU で重ねる
// (composite_scene.h)。名前の大きさと余白、光の明るさの式も製品と同じ。
RenderOutput render_composite(const RenderInput &in)
{
	SceneRequest request;
	request.canvas_w = in.canvas_w;
	request.canvas_h = in.canvas_h;
	request.settings = in.settings;
	switch (in.corner) {
	case sources::DecorCorner::TopLeft:
		request.settings.label_corner = config::LabelCorner::TopLeft;
		break;
	case sources::DecorCorner::TopRight:
		request.settings.label_corner = config::LabelCorner::TopRight;
		break;
	case sources::DecorCorner::BottomRight:
		request.settings.label_corner = config::LabelCorner::BottomRight;
		break;
	case sources::DecorCorner::BottomLeft:
		request.settings.label_corner = config::LabelCorner::BottomLeft;
		break;
	}
	for (const int person : in.people)
		request.seats.push_back({person});
	const ScenePlan plan = plan_scene(request);

	RenderOutput out;
	out.name_px = plan.name_px;
	out.margin_px = plan.margin_px;
	out.tiles = plan.layout.tiles;
	out.canvas = QImage(in.canvas_w, in.canvas_h, QImage::Format_ARGB32_Premultiplied);
	out.canvas.fill(Qt::black);

	std::vector<SceneTileImages> shown;
	for (std::size_t index = 0; index < plan.layout.tiles.size(); ++index) {
		const sources::CompositeTile &tile = plan.layout.tiles[index];
		{
			QPainter painter(&out.canvas);
			const std::size_t position = std::min<std::size_t>(index, kPeopleCount - 1);
			const QImage &image = in.background_images[position];
			if (image.isNull()) {
				painter.drawImage(tile.rect.x, tile.rect.y,
						  make_scene_background(in.backgrounds[position], tile.rect.w, tile.rect.h));
			} else {
				const QImage scaled = image.scaled(tile.rect.w, tile.rect.h, Qt::KeepAspectRatioByExpanding,
								   Qt::SmoothTransformation);
				painter.drawImage(QRect(tile.rect.x, tile.rect.y, tile.rect.w, tile.rect.h), scaled,
						  QRect((scaled.width() - tile.rect.w) / 2, (scaled.height() - tile.rect.h) / 2,
							tile.rect.w, tile.rect.h));
			}
		}
		SceneTileImages images;
		images.label = paint_scene_label(plan, index);
		images.label_owner = plan.tile_owner[index];
		images.frame = paint_scene_frame(plan, index);
		const bool speaking = in.speaker == static_cast<int>(index) && plan.params.show_speaking;
		images.level = speaking ? in.level : 0.0;
		out.labels.push_back(images.label);
		shown.push_back(images);
	}
	// 画面共有していない参加者の並び。この見本の並びに出るのは、名前ラベルを枠に描かない設定 (--no-slot-labels) のときの席の
	// 人だけである (計画が、名前ラベルを描かないタイルの席の人を並びに入れる)。話している明るさは、その人の席で決める。
	std::vector<SceneRosterImages> roster;
	for (std::size_t r = 0; r < plan.roster_content.size(); ++r) {
		SceneRosterImages images;
		images.label = paint_scene_roster_label(plan, r);
		images.label_owner = plan.roster_owner[r];
		const bool speaking = in.speaker == plan.roster_track[r] && plan.params.show_speaking;
		images.level = speaking ? in.level : 0.0;
		roster.push_back(images);
	}
	draw_scene(&out.canvas, plan, shown, roster);
	return out;
}

// 光らせる対象の文字列 (--glow の値) を設定に反映する。
bool apply_glow_option(const QString &text, config::Settings *s, QString *error)
{
	s->label_glow_frame = false;
	s->label_glow_avatar = false;
	s->label_glow_name = false;
	if (text == QStringLiteral("none"))
		return true;
	for (const QString &token : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
		if (token == QStringLiteral("frame") || token == QStringLiteral("all"))
			s->label_glow_frame = true;
		if (token == QStringLiteral("avatar") || token == QStringLiteral("all"))
			s->label_glow_avatar = true;
		if (token == QStringLiteral("name") || token == QStringLiteral("all"))
			s->label_glow_name = true;
		if (token != QStringLiteral("frame") && token != QStringLiteral("avatar") &&
		    token != QStringLiteral("name") && token != QStringLiteral("all")) {
			*error = QStringLiteral("--glow must be none or a comma list of frame, avatar, name, all");
			return false;
		}
	}
	return true;
}

bool parse_args(const QStringList &args, Args *out, QString *error)
{
	config::Settings &s = out->settings;
	for (int i = 1; i < args.size(); ++i) {
		const QString a = args[i];
		if (a == QStringLiteral("--composite-sample"))
			continue;
		const auto value = [&](QString *v) {
			if (i + 1 >= args.size()) {
				*error = a + QStringLiteral(" needs a value");
				return false;
			}
			*v = args[++i];
			return true;
		};
		const auto number = [&](double *v) {
			QString text;
			if (!value(&text))
				return false;
			bool ok = false;
			*v = text.toDouble(&ok);
			if (!ok)
				*error = a + QStringLiteral(" must be a number");
			return ok;
		};
		QString text;
		double n = 0.0;
		if (a == QStringLiteral("--backend") || a == QStringLiteral("--system-font")) {
			// main が Qt を起動する前に読んでいる。ここでは値ごと読み飛ばす。
			if (!value(&text))
				return false;
		} else if (a == QStringLiteral("--out")) {
			if (!value(&out->out_dir))
				return false;
		} else if (a == QStringLiteral("--tag")) {
			if (!value(&out->tag))
				return false;
		} else if (a == QStringLiteral("--people")) {
			// 席の順 (自分の枠、画面共有の枠 1 から 3) に、scene_people() の id を 4 つ。
			if (!value(&text))
				return false;
			const QStringList ids = text.split(QLatin1Char(','));
			if (ids.size() != kPeopleCount) {
				*error = QStringLiteral("--people needs %1 ids separated by commas").arg(kPeopleCount);
				return false;
			}
			const std::vector<ScenePerson> &people = scene_people();
			for (int seat = 0; seat < kPeopleCount; ++seat) {
				const auto found = std::find_if(people.begin(), people.end(), [&](const ScenePerson &person) {
					return ids[seat] == QString::fromUtf8(person.id);
				});
				if (found == people.end()) {
					*error = QStringLiteral("--people has an unknown id: %1").arg(ids[seat]);
					return false;
				}
				out->people[static_cast<std::size_t>(seat)] = static_cast<int>(found - people.begin());
			}
		} else if (a == QStringLiteral("--tile-images")) {
			// 枠の位置の順 (左上、右上、左下、右下) に、画像のファイルを 4 つ。
			if (!value(&text))
				return false;
			const QStringList paths = text.split(QLatin1Char(','));
			if (paths.size() != kPeopleCount) {
				*error = QStringLiteral("--tile-images needs %1 files separated by commas").arg(kPeopleCount);
				return false;
			}
			for (int position = 0; position < kPeopleCount; ++position) {
				QImage image(paths[position]);
				if (image.isNull()) {
					*error = QStringLiteral("--tile-images cannot read %1").arg(paths[position]);
					return false;
				}
				out->tile_images[static_cast<std::size_t>(position)] = image;
			}
		} else if (a == QStringLiteral("--canvas")) {
			if (!value(&text))
				return false;
			const QStringList parts = text.split(QLatin1Char('x'));
			bool ok_w = false;
			bool ok_h = false;
			out->canvas_w = parts.size() == 2 ? parts[0].toInt(&ok_w) : 0;
			out->canvas_h = parts.size() == 2 ? parts[1].toInt(&ok_h) : 0;
			if (!ok_w || !ok_h || out->canvas_w < 320 || out->canvas_h < 180 || out->canvas_w > 7680 ||
			    out->canvas_h > 4320) {
				*error = QStringLiteral("--canvas must be WxH (320x180 to 7680x4320)");
				return false;
			}
		} else if (a == QStringLiteral("--name-pct")) {
			if (!number(&n))
				return false;
			s.label_em_ratio_pct = n;
		} else if (a == QStringLiteral("--glow")) {
			if (!value(&out->glow_text) || !apply_glow_option(out->glow_text, &s, error))
				return false;
		} else if (a == QStringLiteral("--speaker")) {
			if (!value(&text))
				return false;
			bool ok = false;
			out->speaker = text == QStringLiteral("none") ? -1 : text.toInt(&ok);
			const bool in_range = ok && out->speaker >= 0 && out->speaker < kPeopleCount;
			if (text != QStringLiteral("none") && !in_range) {
				*error = QStringLiteral("--speaker must be 0 to 3 (0 top-left, 1 top-right, 2 bottom-left, "
							"3 bottom-right) or none");
				return false;
			}
		} else if (a == QStringLiteral("--corner")) {
			if (!value(&text))
				return false;
			if (text == QStringLiteral("tl"))
				out->corner = sources::DecorCorner::TopLeft;
			else if (text == QStringLiteral("tr"))
				out->corner = sources::DecorCorner::TopRight;
			else if (text == QStringLiteral("bl"))
				out->corner = sources::DecorCorner::BottomLeft;
			else if (text == QStringLiteral("br"))
				out->corner = sources::DecorCorner::BottomRight;
			else {
				*error = QStringLiteral("--corner must be tl, tr, bl, or br");
				return false;
			}
		} else if (a == QStringLiteral("--plate")) {
			s.label_show_plate = true;
		} else if (a == QStringLiteral("--no-avatar")) {
			s.label_show_avatar = false;
		} else if (a == QStringLiteral("--no-slot-labels")) {
			// 名前ラベルを枠に描かない (label_show_in_slot)。枠の人は画面共有していない参加者の並びに描く。
			s.label_show_in_slot = false;
		} else if (a == QStringLiteral("--margin-pct")) {
			if (!number(&s.label_margin_pct))
				return false;
		} else if (a == QStringLiteral("--outline-ratio")) {
			if (!number(&s.label_outline_ratio))
				return false;
		} else if (a == QStringLiteral("--opacity-pct")) {
			if (!number(&n))
				return false;
			s.label_opacity_pct = static_cast<int>(n);
		} else if (a == QStringLiteral("--avatar-ratio")) {
			if (!number(&s.label_avatar_ratio))
				return false;
		} else if (a == QStringLiteral("--glow-blur-ratio")) {
			if (!number(&s.label_glow_blur_ratio))
				return false;
		} else if (a == QStringLiteral("--glow-spread-ratio")) {
			if (!number(&s.label_glow_spread_ratio))
				return false;
		} else if (a == QStringLiteral("--font-family")) {
			if (!value(&text))
				return false;
			s.label_font_family = text.toStdString();
		} else if (a == QStringLiteral("--font-weight")) {
			if (!number(&n))
				return false;
			s.label_font_weight = static_cast<int>(n);
		} else {
			*error = QStringLiteral("unknown option %1 for --composite-sample").arg(a);
			return false;
		}
	}
	if (out->out_dir.isEmpty()) {
		*error = QStringLiteral("--composite-sample needs --out DIR");
		return false;
	}
	// 範囲の外や刻みに合わない値は、設定の画面から入る値と同じ丸め方で変わってしまうので、受け付けない。
	config::Settings clamped = s;
	config::clamp_settings(clamped);
	if (config::settings_to_json(clamped) != config::settings_to_json(s)) {
		*error = QStringLiteral("an option is outside the range or the step of the setting it sets");
		return false;
	}
	return true;
}

bool save_png(const QImage &image, const QString &path)
{
	QImage rgb = image.convertToFormat(QImage::Format_RGB32);
	return rgb.save(path);
}

// ---- つまみごとの見本の一覧 ----

// つまみの 1 つの値。label は一覧に書き込む値の文字、apply は既定の設定をその値に変える。
struct KnobStep {
	QString label;
	std::function<void(config::Settings &, double &level)> apply;
};

struct Knob {
	const char *key;   // ファイル名に使う名前
	QString title;     // 一覧の見出し
	std::vector<KnobStep> steps;
};

// 値の文字。既定値と同じなら「(既定)」を付ける。
QString step_label(const QString &value, bool is_default)
{
	return is_default ? value + QStringLiteral(" (既定)") : value;
}

std::vector<Knob> all_knobs()
{
	const config::Settings d;
	std::vector<Knob> knobs;
	const auto number_steps = [&](std::vector<double> values, double current, const char *suffix,
				      std::function<void(config::Settings &, double)> set) {
		std::vector<KnobStep> steps;
		for (const double v : values) {
			const QString text = QString::number(v, 'g', 4) + QString::fromUtf8(suffix);
			steps.push_back({step_label(text, v == current),
					 [set, v](config::Settings &s, double &) { set(s, v); }});
		}
		return steps;
	};
	const auto color_steps = [&](std::vector<const char *> values, const std::string &current,
				     std::function<void(config::Settings &, const std::string &)> set) {
		std::vector<KnobStep> steps;
		for (const char *v : values) {
			const std::string color = v;
			steps.push_back({step_label(QString::fromUtf8(v), color == current),
					 [set, color](config::Settings &s, double &) { set(s, color); }});
		}
		return steps;
	};

	knobs.push_back({"name_size", QStringLiteral("名前の大きさ (配信の画面の高さに対する %)"),
			 number_steps({1.0, 2.5, 4.0, 5.5, 7.0, 8.5}, d.label_em_ratio_pct, "%",
				      [](config::Settings &s, double v) { s.label_em_ratio_pct = v; })});
	knobs.push_back({"margin", QStringLiteral("余白 (配信の画面の高さに対する %)"),
			 number_steps({0.0, 0.5, 1.0, 3.0, 7.0, 13.9}, d.label_margin_pct, "%",
				      [](config::Settings &s, double v) { s.label_margin_pct = v; })});
	knobs.push_back({"outline_width", QStringLiteral("縁取りの太さ (名前の大きさに対する倍率)"),
			 number_steps({0.0, 0.05, 0.1, 0.2, 0.3, 0.4}, d.label_outline_ratio, "",
				      [](config::Settings &s, double v) { s.label_outline_ratio = v; })});
	knobs.push_back({"outline_color", QStringLiteral("縁取りの色"),
			 color_steps({"#000000", "#FFFFFF", "#5865F2", "#ED4245"}, d.label_outline_color,
				     [](config::Settings &s, const std::string &c) { s.label_outline_color = c; })});
	knobs.push_back({"text_color", QStringLiteral("文字の色"),
			 color_steps({"#FFFFFF", "#000000", "#FEE75C", "#57F287"}, d.label_text_color,
				     [](config::Settings &s, const std::string &c) { s.label_text_color = c; })});
	knobs.push_back({"opacity", QStringLiteral("名前ラベル全体の不透明度 (%)"),
			 number_steps({0, 25, 50, 75, 100}, d.label_opacity_pct, "%",
				      [](config::Settings &s, double v) { s.label_opacity_pct = static_cast<int>(v); })});
	{
		Knob avatar{"avatar", QStringLiteral("アバターの表示と大きさ (名前の大きさに対する倍率)"), {}};
		avatar.steps.push_back({QStringLiteral("表示しない"),
					[](config::Settings &s, double &) { s.label_show_avatar = false; }});
		for (KnobStep &step : number_steps({0.4, 1.0, 1.5, 2.0, 3.0}, d.label_avatar_ratio, "",
						   [](config::Settings &s, double v) { s.label_avatar_ratio = v; }))
			avatar.steps.push_back(step);
		knobs.push_back(avatar);
	}
	{
		Knob plate{"plate", QStringLiteral("背景の表示と色と不透明度"), {}};
		plate.steps.push_back({step_label(QStringLiteral("表示しない"), !d.label_show_plate),
				       [](config::Settings &s, double &) { s.label_show_plate = false; }});
		struct PlateStep {
			const char *color;
			int opacity;
		};
		for (const PlateStep p : {PlateStep{"#000000", 25}, PlateStep{"#000000", 50}, PlateStep{"#000000", 75},
					  PlateStep{"#000000", 100}, PlateStep{"#FFFFFF", 50}, PlateStep{"#5865F2", 50}}) {
			const std::string color = p.color;
			const int opacity = p.opacity;
			const bool is_default =
				d.label_show_plate && color == d.label_plate_color && opacity == d.label_plate_opacity_pct;
			const QString label =
				QStringLiteral("表示する %1 %2%").arg(QString::fromUtf8(p.color)).arg(opacity);
			plate.steps.push_back({step_label(label, is_default),
					       [color, opacity](config::Settings &s, double &) {
						       s.label_show_plate = true;
						       s.label_plate_color = color;
						       s.label_plate_opacity_pct = opacity;
					       }});
		}
		knobs.push_back(plate);
	}
	{
		Knob shape{"glow_shape", QStringLiteral("光らせる対象"), {}};
		struct ShapeStep {
			const char *label;
			bool frame;
			bool avatar;
			bool name;
		};
		for (const ShapeStep p :
		     {ShapeStep{"光: アバターと名前", false, true, true}, ShapeStep{"光: アバター", false, true, false},
		      ShapeStep{"光: 名前", false, false, true}, ShapeStep{"光: 枠", true, false, false},
		      ShapeStep{"光: 枠とアバターと名前", true, true, true},
		      ShapeStep{"光: なし", false, false, false}}) {
			const bool is_default = p.frame == d.label_glow_frame && p.avatar == d.label_glow_avatar &&
						p.name == d.label_glow_name;
			shape.steps.push_back({step_label(QString::fromUtf8(p.label), is_default),
					       [p](config::Settings &s, double &) {
						       s.label_glow_frame = p.frame;
						       s.label_glow_avatar = p.avatar;
						       s.label_glow_name = p.name;
					       }});
		}
		knobs.push_back(shape);
	}
	knobs.push_back({"glow_color", QStringLiteral("光の色"),
			 color_steps({"#57F287", "#FFFFFF", "#ED4245", "#5865F2", "#FEE75C"}, d.label_border_color,
				     [](config::Settings &s, const std::string &c) { s.label_border_color = c; })});
	knobs.push_back({"glow_blur", QStringLiteral("光のぼかし (名前の大きさに対する倍率)"),
			 number_steps({0.0, 0.1, 0.5, 1.0, 2.0, 3.0}, d.label_glow_blur_ratio, "",
				      [](config::Settings &s, double v) { s.label_glow_blur_ratio = v; })});
	knobs.push_back({"glow_spread", QStringLiteral("光の広がり (名前の大きさに対する倍率)"),
			 number_steps({0.0, 0.1, 0.25, 0.5, 0.75, 1.0}, d.label_glow_spread_ratio, "",
				      [](config::Settings &s, double v) { s.label_glow_spread_ratio = v; })});
	{
		Knob level{"level", QStringLiteral("話している間の明るさ (立ち上がりと消える途中の値)"), {}};
		for (const double v : {0.0, 0.25, 0.5, 0.75, 1.0})
			level.steps.push_back({QString::number(v, 'g', 3), [v](config::Settings &, double &l) { l = v; }});
		knobs.push_back(level);
	}
	return knobs;
}

// 一覧の 1 つの見本の入力。左上の枠に名前の長さが中くらいの人を置いて話させ、下の枠も同じ明るさの背景にする。
RenderInput sweep_input(const KnobStep &step, int column, int canvas_w, int canvas_h)
{
	const std::array<int, 2> speaker_backgrounds{kBrightSky, kDarkGame};
	const int background = speaker_backgrounds[static_cast<std::size_t>(column)];
	RenderInput in;
	in.canvas_w = canvas_w;
	in.canvas_h = canvas_h;
	in.speaker = 0;
	in.people = {1, 0, 2, 3};
	in.backgrounds = {background, kBrightSky, background, kDarkGame};
	step.apply(in.settings, in.level);
	return in;
}

// 一覧の切り抜き。つまみのどの値でも同じ範囲にして見比べられるよう、全部の値の、話している左上の枠の名前ラベルと
// 光の範囲を合わせた矩形に余白を足す。下の枠との境を必ずまたがせる (余白が小さいときに、光がセルの境を越えて下の
// 枠へ出ているかが見える)。
QRect sweep_crop(const Knob &knob, int canvas_w, int canvas_h)
{
	QRect area;
	for (const KnobStep &step : knob.steps) {
		const RenderOutput out = render_composite(sweep_input(step, 0, canvas_w, canvas_h));
		if (out.labels.empty())
			continue;
		const sources::CompositeLabel &label = out.labels.front();
		if (!label.label.isNull())
			area = area.united(QRect(label.label_x, label.label_y, label.label.width(), label.label.height()));
		if (!label.glow.isNull())
			area = area.united(QRect(label.glow_x, label.glow_y, label.glow.width(), label.glow.height()));
	}
	const int pad = 24;
	const int boundary = canvas_h / 2;
	if (area.isNull())
		area = QRect(0, boundary - 120, 360, 120);
	area = area.adjusted(-pad, -pad, pad, pad);
	area.setBottom(std::max(area.bottom(), boundary + pad));
	area.setRight(std::max(area.right(), 360));
	return area.intersected(QRect(0, 0, canvas_w, canvas_h));
}

bool write_sweep(const Knob &knob, int canvas_w, int canvas_h, const QString &path)
{
	const QRect crop = sweep_crop(knob, canvas_w, canvas_h);
	// 小さい名前ラベルは 2 倍に拡大して並べる (ピクセルを補わずに拡大し、縁の 1 ピクセルの違いも見えるようにする)。
	const int scale = crop.width() * 2 <= 1200 && crop.height() * 2 <= 800 ? 2 : 1;
	const int cell_w = crop.width() * scale;
	const int cell_h = crop.height() * scale;
	const int caption_h = 34;
	const int gap = 12;
	const int columns = 2; // 明るい背景と暗い背景
	const int header_h = 72;
	const int rows = static_cast<int>(knob.steps.size());
	QImage sheet(gap + columns * (cell_w + gap), header_h + rows * (caption_h + cell_h + gap),
		     QImage::Format_ARGB32_Premultiplied);
	sheet.fill(QColor(48, 48, 52));
	QPainter painter(&sheet);
	QFont title_font = painter.font();
	title_font.setPixelSize(24);
	title_font.setBold(true);
	QFont caption_font = painter.font();
	caption_font.setPixelSize(20);
	painter.setPen(QColor(240, 240, 240));
	painter.setFont(title_font);
	painter.drawText(QRect(gap, 4, sheet.width() - gap * 2, 32), Qt::AlignVCenter | Qt::AlignLeft, knob.title);
	painter.setFont(caption_font);
	painter.drawText(QRect(gap, 36, sheet.width() - gap * 2, 32), Qt::AlignVCenter | Qt::AlignLeft,
			 QStringLiteral("左: 明るい背景、右: 暗い背景。左上の枠が話している。%1 倍。"
					"切り抜きはキャンバス (%2x%3) の (%4, %5) から %6x%7")
					      .arg(scale)
					      .arg(canvas_w)
					      .arg(canvas_h)
					      .arg(crop.x())
					      .arg(crop.y())
					      .arg(crop.width())
					      .arg(crop.height()));

	for (int row = 0; row < rows; ++row) {
		const KnobStep &step = knob.steps[static_cast<std::size_t>(row)];
		const int top = header_h + row * (caption_h + cell_h + gap);
		painter.setFont(caption_font);
		painter.setPen(QColor(240, 240, 240));
		painter.drawText(QRect(gap, top, sheet.width() - gap * 2, caption_h), Qt::AlignVCenter | Qt::AlignLeft,
				 step.label);
		for (int column = 0; column < columns; ++column) {
			const RenderOutput out = render_composite(sweep_input(step, column, canvas_w, canvas_h));
			QImage part = out.canvas.copy(crop);
			if (scale != 1)
				part = part.scaled(cell_w, cell_h, Qt::IgnoreAspectRatio, Qt::FastTransformation);
			painter.drawImage(gap + column * (cell_w + gap), top + caption_h, part);
		}
	}
	painter.end();
	return save_png(sheet, path);
}

} // namespace

int run_composite_sample(const QStringList &args)
{
	Args a;
	QString error;
	if (!parse_args(args, &a, &error)) {
		std::fprintf(stderr, "decor_preview: %s\n", error.toUtf8().constData());
		return 2;
	}
	RenderInput in;
	in.canvas_w = a.canvas_w;
	in.canvas_h = a.canvas_h;
	in.settings = a.settings;
	in.speaker = a.speaker;
	in.corner = a.corner;
	in.people = a.people;
	in.background_images = a.tile_images;
	const RenderOutput out = render_composite(in);

	QJsonArray tiles_json;
	QImage speaker_crop;
	for (std::size_t index = 0; index < out.tiles.size(); ++index) {
		const sources::CompositeTile &tile = out.tiles[index];
		const sources::CompositeLabel &label = out.labels[index];
		const bool speaking = a.speaker == static_cast<int>(index);
		QJsonObject t;
		t[QStringLiteral("index")] = static_cast<int>(index);
		t[QStringLiteral("rect")] = QJsonArray{tile.rect.x, tile.rect.y, tile.rect.w, tile.rect.h};
		t[QStringLiteral("speaking")] = speaking;
		t[QStringLiteral("name_drawn")] = label.name_drawn;
		t[QStringLiteral("box")] = QJsonArray{label.box_x, label.box_y, label.box_w, label.box_h};
		t[QStringLiteral("elided")] = label.elided;
		t[QStringLiteral("avatar_dropped")] = label.avatar_dropped;
		t[QStringLiteral("name_hidden")] = label.name_hidden;
		t[QStringLiteral("initial_drawn")] = label.initial_drawn;
		t[QStringLiteral("font_family_used")] = label.font_family_used;
		tiles_json.append(t);
		if (speaking || (a.speaker < 0 && index == 0))
			speaker_crop = out.canvas.copy(tile.rect.x, tile.rect.y, tile.rect.w, tile.rect.h);
	}

	if (a.tag.isEmpty()) {
		a.tag = QStringLiteral("n%1_%2_s%3")
				.arg(a.settings.label_em_ratio_pct, 0, 'f', 1)
				.arg(QString(a.glow_text).replace(QLatin1Char(','), QLatin1Char('-')))
				.arg(a.speaker < 0 ? QStringLiteral("none") : QString::number(a.speaker));
	}
	if (!QDir().mkpath(a.out_dir)) {
		std::fprintf(stderr, "decor_preview: cannot create %s\n", a.out_dir.toUtf8().constData());
		return 1;
	}
	const QString base = a.out_dir + QStringLiteral("/composite_") + a.tag;
	if (!save_png(out.canvas, base + QStringLiteral(".png")) ||
	    (!speaker_crop.isNull() && !save_png(speaker_crop, base + QStringLiteral("_tile.png")))) {
		std::fprintf(stderr, "decor_preview: cannot write the images under %s\n", a.out_dir.toUtf8().constData());
		return 1;
	}

	QJsonObject root;
	root[QStringLiteral("canvas")] = QJsonArray{a.canvas_w, a.canvas_h};
	root[QStringLiteral("name_px")] = out.name_px;
	root[QStringLiteral("margin_px")] = out.margin_px;
	root[QStringLiteral("speaker")] = a.speaker;
	const std::string settings_text = config::settings_to_json(a.settings);
	root[QStringLiteral("settings")] = QJsonDocument::fromJson(settings_text.c_str()).object();
	root[QStringLiteral("tiles")] = tiles_json;
	QFile file(base + QStringLiteral(".json"));
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		std::fprintf(stderr, "decor_preview: cannot write %s\n", file.fileName().toUtf8().constData());
		return 1;
	}
	file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
	file.close();
	std::printf("%s\n", QDir::toNativeSeparators(base + QStringLiteral(".png")).toUtf8().constData());
	return 0;
}

int run_composite_sweep(const QStringList &args)
{
	QString out_dir;
	QString only;
	for (int i = 1; i < args.size(); ++i) {
		const QString a = args[i];
		if (a == QStringLiteral("--composite-sweep"))
			continue;
		if ((a == QStringLiteral("--out") || a == QStringLiteral("--knob")) && i + 1 < args.size()) {
			(a == QStringLiteral("--out") ? out_dir : only) = args[++i];
			continue;
		}
		std::fprintf(stderr, "decor_preview: --composite-sweep が知らない引数です: %s\n", a.toUtf8().constData());
		return 2;
	}
	if (out_dir.isEmpty()) {
		std::fprintf(stderr, "decor_preview: --composite-sweep には --out DIR が要ります\n");
		return 2;
	}
	if (!QDir().mkpath(out_dir)) {
		std::fprintf(stderr, "decor_preview: フォルダーを作れません: %s\n", out_dir.toUtf8().constData());
		return 1;
	}
	int written = 0;
	for (const Knob &knob : all_knobs()) {
		if (!only.isEmpty() && only != QString::fromUtf8(knob.key))
			continue;
		const QString path = out_dir + QStringLiteral("/sweep_") + QString::fromUtf8(knob.key) + QStringLiteral(".png");
		if (!write_sweep(knob, 1920, 1080, path)) {
			std::fprintf(stderr, "decor_preview: 書き出せません: %s\n", path.toUtf8().constData());
			return 1;
		}
		std::printf("%s\n", QDir::toNativeSeparators(path).toUtf8().constData());
		++written;
	}
	if (written == 0) {
		std::fprintf(stderr, "decor_preview: --knob の名前が一覧にありません: %s\n", only.toUtf8().constData());
		return 2;
	}
	return 0;
}

} // namespace cordmux::decor_preview
