// SPDX-License-Identifier: Apache-2.0

// アンインストールで消す対象の表 (kRemovalTable)。アンインストールのコード (install/uninstall.h の
// run_uninstall) と、インストールの前後を比べるツール (test/tools/obs-run/install_snapshot.py) が、
// 同じこの表を読む。Python のツールはこのヘッダーを文字列として読むので、下の「Python が読む形」を守る。
// libobs にも Qt にも依存しない。

#pragma once

#include <cstddef>

// Python が読む形 (test/lint/removal_table.py が確認し、install_snapshot.py が同じ読み方を使う)。
// この形を崩すと、ツールが行を数え違えて失敗する。
//   - 表は、行頭が `// removal-table-begin` で始まる行と、`// removal-table-end` で始まる行の間にある
//     (目印の行は後ろにコメントが続くので、行全体ではなく行頭で見る)。
//   - 表の行は 1 行のソースに 1 つずつ書き、全体が次の正規表現に一致する (行末の // のコメントはよい)。
//       ^\s*\{RemovalItem::(\w+), Remover::(\w+), Locate::(\w+), UninstallStep::(\w+)\},\s*(//.*)?$
//   - 行の数は `kRemovalItemCount = <数>;` の 1 行で示す。
//   - `enum class RemovalItem { ... };` は 1 行に 1 つの列挙子を書く (テストがヘッダーを文字列として読んで
//     列挙子を数え、kRemovalItemCount と比べる。列挙子を追加して kRemovalItemCount と表への追加を忘れると、
//     行の数を比べるだけでは気付けないため)。
// 表の 4 つの列は、どれも列挙型で持つ (文字列にしない)。Python のツールが列挙子の名前をそのままキーにでき、
// C++ と Python で名前が食い違わない。Python のツールは探し方 (Locate) ごとに調べ方を 1 つ持ち、知らない
// 探し方があれば失敗する。
//
// ModuleConfigFiles の中身は settings.json、seen_people.json (以前のバージョンが書いた slots.json が残っていても、フォルダーごと消える)、
// vencord_dir_backup.json (バックアップ)、dock_shown_once (dock/dock_bind.cpp)、record_pending_dirs.json
// (record/record_search_dirs.h) などで、LocalDataFolder の中身は session.json (platform/platform.h)
// である。どちらもフォルダーの下を全部消すので、ファイルを追加しても表は変えなくてよい。バックアップは段階 (3)
// が読むので、段階 (1) では消さずに残す (段階 (3) で止まったときにバックアップが先に消えていると、OBS を再起動して
// アンインストールし直しても元に戻せない)。
//
// 診断情報の zip は表に載せない。利用者が保存先を選んで保存したもので、録画と同じく利用者のファイルであり、
// cordmux の状態ではないためである。録画ファイルの作成で使う一時ファイルと ffmpeg のログは、録画のセッションの
// フォルダーにある録画の中間ファイルのフォルダーに置くので、Recordings に含まれる (場所を決めるのは
// record/record_args.h の build_finalize_plan と、record/record_finalize.cpp の child_log_path)。

namespace cordmux::install {

// 消す対象の種類。値を追加したら、表にも行を追加し、kRemovalItemCount も増やす (テストと Python のツールが
// 比べる)。値は明示せず、末尾に追加する (テストが値を表の添字として使う)。
enum class RemovalItem {
	CordmuxSources,         // cordmux の型のソース (合成のソース。利用者が「+」から追加したもの)
	ModuleConfigFiles,      // obs_module_config_path の下の、cordmux のファイル全部
	LocalDataFolder,        // %LOCALAPPDATA% の下の cordmux (接続のトークンとポート番号)
	VencordDirValue,        // state.json の vencordDir (previous に戻す。キーがなかったならキーごと消す)
	ExtractedFolder,        // 展開したフォルダー (data/vencord を含む)
	Recordings,             // 録画
	ObsDockLayout,          // OBS が保存するドックの配置
	ObsLogs,                // OBS のログ
	ObsPluginManagerRecord, // OBS 32 のプラグインマネージャーが保存する、読み込んだモジュールの記録
};

// 誰が消すか。
enum class Remover {
	Product, // cordmux がアンインストールで消す
	User,    // 利用者が消す (アンインストールの後に場所を表示する)
	Nobody,  // 消さない (理由は表の行のコメント)
};

// 探し方。Python のツールは、この名前ごとに調べ方を 1 つ持つ。
enum class Locate {
	SourceTypeId,         // ソースの型 id で探す。名前では探さない (sources/cordmux_removal.cpp)
	ModuleConfigDir,      // obs_module_config_path の場所の下を全部消し、最後にフォルダーごと消す
	LocalDataDir,         // platform/ が返す %LOCALAPPDATA% の下の cordmux (フォルダーごと)
	StateJsonKey,         // state.json の vencordDir のキー (値はバックアップの previous と比べる)
	ModuleFolder,         // cordmux の DLL の場所から上へたどった、展開したフォルダー
	RecordDir,            // 録画の保存先 (settings の record_dir か既定の場所)
	ObsProfileDockState,  // OBS の全体の設定に残るドックの配置
	ObsLogDir,            // OBS のログのフォルダー
	ObsPluginManagerFile, // OBS の設定のフォルダーの plugin_manager/modules.json (キーによらず全部)
};

// アンインストールの段階。値は段階の番号で、run_uninstall はこの順に実行する。段階 (0) は表の行を持たない
// (消す対象ではなく、段階 (1) 以降で消したファイルが書き戻されないようにする準備である)。
enum class UninstallStep {
	MarkUninstalling = 0,  // (0) アンインストール中のフラグを立て、Vesktop からの接続の待ち受けを止める
	SettingsAndSecret = 1, // (1) 設定とトークン
	SourcesAndScenes = 2,  // (2) ソース (ほかのシーンコレクションも)
	RestoreVencordDir = 3, // (3) vencordDir を元に戻す
	ShowFolder = 4,        // (4) 消してほしいフォルダーの場所を表示する
	NotRemoved = 9,        // 消さない行 (段階を持たない)
};

struct RemovalRow {
	RemovalItem item;
	Remover remover;
	Locate locate;
	UninstallStep step;
};

// 表の行の数。行を追加したら、ここも増やす (テストが列挙子と、Python のツールが読めた行と比べる)。
inline constexpr std::size_t kRemovalItemCount = 9;

// 消す対象の表。1 つの行を 1 行のソースに書く (上の「Python が読む形」)。
// ドックの配置 (ObsDockLayout) は OBS 自身の状態なので消さない。残った配置に見える影響があるかと、OBS の API で
// 消せるかは確認していない (どちらも当てはまれば Product にする)。
// clang-format off
// removal-table-begin (表の始まり。Python が読む目印)
inline constexpr RemovalRow kRemovalTable[] = {
	{RemovalItem::CordmuxSources, Remover::Product, Locate::SourceTypeId, UninstallStep::SourcesAndScenes},
	{RemovalItem::ModuleConfigFiles, Remover::Product, Locate::ModuleConfigDir, UninstallStep::SettingsAndSecret},
	{RemovalItem::LocalDataFolder, Remover::Product, Locate::LocalDataDir, UninstallStep::SettingsAndSecret},
	{RemovalItem::VencordDirValue, Remover::Product, Locate::StateJsonKey, UninstallStep::RestoreVencordDir},
	{RemovalItem::ExtractedFolder, Remover::User, Locate::ModuleFolder, UninstallStep::ShowFolder}, // 読み込まれている DLL は自分では消せない
	{RemovalItem::Recordings, Remover::Nobody, Locate::RecordDir, UninstallStep::NotRemoved}, // 利用者が作ったファイル
	{RemovalItem::ObsDockLayout, Remover::Nobody, Locate::ObsProfileDockState, UninstallStep::NotRemoved}, // OBS 自身の状態
	{RemovalItem::ObsLogs, Remover::Nobody, Locate::ObsLogDir, UninstallStep::NotRemoved}, // OBS 自身のログ
	// OBS 32 のプラグインマネージャーは、読み込んだモジュールを OBS の設定のフォルダーの
	// plugin_manager/modules.json に保存する。cordmux の DLL を消して OBS を再起動しても、cordmux の項目は残る
	// (OBS 32.2.2 では、インストールの後に 13 個、アンインストールの後に 10 個のキーがあった。OBS 31.1.1 には
	// このファイルがない)。ドックの配置と同じく、OBS 自身の状態なので消さない。
	{RemovalItem::ObsPluginManagerRecord, Remover::Nobody, Locate::ObsPluginManagerFile, UninstallStep::NotRemoved}, // OBS 自身の状態
};
// removal-table-end (表の終わり。Python が読む目印)
// clang-format on

inline constexpr std::size_t kRemovalTableRows = sizeof(kRemovalTable) / sizeof(kRemovalTable[0]);

} // namespace cordmux::install
