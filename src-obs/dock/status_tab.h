// SPDX-License-Identifier: Apache-2.0

// 状態タブの中身。上から「現在のメッセージ」「cordmuxの録画」「映像の受信」「Vesktop」「履歴」の組を縦に 1 列に並べる
// (「Vesktop」は、Vesktop の設定が同梱の Vencord を指していて、Vesktop が起動していない間だけ)。
// 並びと値は ledger の View (DockViewV2::notices と StatusTabView) が決めたもので、ここは描くだけである。
// 判断は持たない。押せるかは View の disabled_reason、外から届いた値で入力欄を書き換えるかは ledger::look_field_follows が決める。
//
// 位置を動かす変化 (メッセージの増減、表の行の増減、録画の節の高さの変化) は、並びの保留の間 (allow_moves が偽) は行わず、
// 保留が終わった後の update() で反映する。数字の更新と履歴の追加は保留しない (履歴はタブの最後にあり、伸びても上の
// ウィジェットは動かない)。
//
// 表は、行のウィジェットを残して値だけを書き換える。バッファの入力欄を動かすと、表の遅延と最大を、その値ですぐ計算し直す。
// どの入力欄もホイールを受け取らず、スクロールに渡す。幅は状態で変えず、最小の幅でも中身が横にはみ出さない (名前の列は省略する)。

#pragma once

#include <QString>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "dock/dock_look.h"
#include "dock/notice_list.h"
#include "ledger/dock_view.h"

class QFormLayout;
class QGridLayout;
class QLabel;
class QPushButton;
class QTimer;
class QVBoxLayout;

namespace cordmux::dock {

namespace ui {
class ElideLabel;
class SliderSpin;
class WrapLabel;
} // namespace ui

class StatusTab : public QWidget {
public:
	explicit StatusTab(QWidget *parent = nullptr);
	~StatusTab() override;

	void set_look(const DockLook &look);

	// View を反映する。notices は DockViewV2::notices。allow_moves が偽の間は位置を動かす変化を行わず、ためておいて true を
	// 返す (呼び出し元は保留が解けたら、もう一度呼ぶ)。
	bool update(const std::vector<ledger::dockview::Notice> &notices, const ledger::dockview::StatusTabView &view,
		    bool allow_moves);

	// メッセージのカードと、[途切れの累計をリセット] を押したときの呼び出し先。
	void set_on_press(NoticeButtonCallback cb);
	// [フォルダーを開く] を押したときの呼び出し先 (メッセージの場所の行と、最後の録画)。
	void set_folder_opener(FolderOpener opener);
	void set_busy(const std::set<ledger::ActionId> &busy);
	NoticeList *notices() const { return notices_; }

	// ---- 映像のバッファ ----
	// 設定の値を入力欄へ反映する。周期ごとに呼んでよい。操作している間と、変えた値をまだ渡していない間は書き換えない。
	void set_buffer_value(int ms);
	// 値を変えて入力が止まったときに呼ぶ (ledger::kStatusBufferIdleMs)。引数は入力欄の値 (ms)。動かしている間は呼ばず、表の計算だけを先に行う。
	void set_on_buffer_changed(std::function<void(int)> callback) { on_buffer_changed_ = std::move(callback); }
	// 変えたまま渡していない値があれば、今渡す (OBS を閉じるとき、タブを離れるとき)。
	void flush();
	bool has_unreflected() const { return dirty_; }
	int buffer_ms() const;
	ui::SliderSpin *buffer_input() const { return buffer_; }

	// テストとプレビューツールが読む。
	QPushButton *reset_button() const { return reset_; }
	QPushButton *open_folder_button() const { return open_folder_; }
	// 「Vesktop」の組の [Vesktopから削除] と、その下の押せない理由の文。
	QPushButton *remove_from_vesktop_button() const { return vesktop_remove_; }
	QLabel *vesktop_reason_label() const;
	QWidget *table_widget() const { return table_; }
	// 表のセルのウィジェットを、行 (0 が見出し) と列 (名前、途切れ、合計、遅延、最大の 0 から 4) で返す。
	QWidget *cell(int row, int column) const;
	int table_rows() const { return static_cast<int>(rows_.size()); }
	// 履歴に表示している行の数。
	int history_lines() const { return static_cast<int>(history_shown_.size()); }
	// 録画の節の現在の状態。
	ledger::dockview::StatusRecordingSection recording_section() const { return shown_.recording.section; }
	// 位置を動かす変化をためているか。
	bool moves_pending() const { return moves_pending_; }

private:
	struct Row {
		std::string user_id;
		ui::ElideLabel *name = nullptr;
		QLabel *gaps = nullptr;
		QLabel *total = nullptr;
		QLabel *late = nullptr;
		QLabel *max = nullptr;
	};

	void build();
	void apply_look_to_widgets();
	void apply_recording(const ledger::dockview::StatusRecordingView &view);
	void apply_vesktop(const std::optional<ledger::dockview::StatusVesktopView> &view);
	// 位置が動く変化を行ったときは true を返し、保留で見送ったときは pending に true を入れる。
	void apply_receive(const ledger::dockview::StatusTabView &view, bool allow_moves, bool *pending);
	void apply_history(const std::vector<ledger::dockview::StatusHistoryLine> &lines);
	void fill_row(Row &row, const ledger::dockview::StatusReceiveRow &data);
	void refill_rows();
	Row make_row(const std::string &user_id);
	void relayout_rows(const std::vector<std::string> &order);
	void retire(QWidget *w);
	void arm_guards();
	void on_buffer_input_changed();
	void request_reflect();
	void reflect_now();
	void style_muted(QLabel *label);

	DockLook look_;
	bool has_look_ = false;
	ledger::dockview::StatusTabView shown_; // 録画の節と合成のソースの有無について、画面に反映した値
	std::map<std::string, ledger::dockview::StatusReceiveRow> data_; // 表の行ごとの、最後に反映した値
	bool has_shown_ = false;
	bool moves_pending_ = false;
	std::vector<ledger::dockview::StatusHistoryLine> history_shown_;
	bool history_none_shown_ = false;

	QVBoxLayout *root_ = nullptr;
	std::vector<QLabel *> headings_;
	QLabel *notices_heading_ = nullptr;
	NoticeList *notices_ = nullptr;
	QLabel *notices_none_ = nullptr;

	QLabel *recording_heading_ = nullptr;
	QLabel *recording_now_ = nullptr;
	QLabel *recording_none_ = nullptr;
	ui::WrapLabel *recording_last_ = nullptr;
	QPushButton *open_folder_ = nullptr;
	std::string open_folder_path_;

	QLabel *receive_heading_ = nullptr;
	QWidget *table_ = nullptr;
	QGridLayout *grid_ = nullptr;
	std::vector<QLabel *> headers_;
	std::vector<Row> rows_;
	ui::WrapLabel *receive_empty_ = nullptr;
	ui::WrapLabel *receive_no_composite_ = nullptr;
	ui::WrapLabel *receive_note_ = nullptr;
	QWidget *receive_note_box_ = nullptr; // receive_note_ の入れ物 (ui::note_box)
	QPushButton *reset_ = nullptr;
	ledger::dockview::ViewButton reset_view_;
	ui::SliderSpin *buffer_ = nullptr;
	QLabel *buffer_label_ = nullptr;
	// [映像のバッファ] の行。上の表と [途切れの累計をリセット] とは意味が違うので、上に意味の違う部分の間の余白を空ける。
	QFormLayout *buffer_form_ = nullptr;

	QLabel *vesktop_heading_ = nullptr;
	ui::WrapLabel *vesktop_text_ = nullptr;
	QPushButton *vesktop_remove_ = nullptr;
	ui::WrapLabel *vesktop_reason_ = nullptr;
	std::optional<ledger::dockview::StatusVesktopView> shown_vesktop_; // 画面に反映した「Vesktop」の組

	QLabel *history_heading_ = nullptr;
	QWidget *history_box_ = nullptr;
	QGridLayout *history_grid_ = nullptr;
	std::vector<QLabel *> history_when_;
	std::vector<ui::WrapLabel *> history_text_;
	QLabel *history_none_ = nullptr;

	NoticeButtonCallback on_press_;
	FolderOpener opener_;
	std::function<void(int)> on_buffer_changed_;
	QTimer *reflect_timer_ = nullptr;
	bool loading_ = false;
	bool dirty_ = false;
};

} // namespace cordmux::dock
