// SPDX-License-Identifier: Apache-2.0

// アンインストールの段階の順序と止め方 (run_uninstall)、段階ごとの処理、アンインストール中のフラグ。
// 段階ごとの処理は UninstallOps などの関数として注入する (ws::PlatformOps と同じ形)。このヘッダーと
// uninstall.cpp は libobs にも Qt にも依存しないので、テストから直接呼び出せる。実際の platform/ と OBS の
// 関数をまとめる make_real_uninstall_ops は uninstall_real.cpp にあり、製品のビルドにだけ入る。
// シーンコレクションの中のソースとシーンを消すのは sources/cordmux_removal.h である。
// アンインストールの流れは docs/architecture.md の「配布、インストール、更新、アンインストール」にある。

#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "install/removal_table.h"
#include "install/vencord_dir.h"
#include "platform/platform.h"

namespace cordmux::install {

// ---- アンインストール中のフラグ ----
//
// フラグはメモリにだけ置き、立てたら OBS を終了するまで下ろさない。フラグが立つと、枠の自動作成、settings.json、
// seen_people.json の書き込み、起動時の書き直し、接続の待ち受けが止まり、OBS を終了するまで何も書かない。
// フラグをファイルに保存しないのは、アンインストールした後に cordmux のファイルを残さないためである。そのため、
// フォルダーを消さずに OBS を起動し直すと、cordmux はまた動き始める。

// フラグの実体。どのスレッドから読み書きしてもよい。
// プロセスに 1 つの実体 (mark_uninstalling など) と別に作れるのは、テストが自分の実体で確認するためである。
// プロセスに 1 つの実体は下ろせないので、テストがそれを立てると、同じ実行ファイルのほかのテストが読む値まで
// 変わる。
class UninstallMark {
public:
	// フラグを立て、実行中の write_unless_marked が終わるまで待つ (write_unless_uninstalling のコメント)。
	void mark();
	bool is_marked() const { return marked_.load(); }
	// フラグが立っていなければ write を呼び出してその戻り値を返し、立っていれば呼び出さずに false を返す。
	// フラグの確認と書き込みは 1 つのロックの下で行う。write の中から mark を呼び出さない (同じロックで止まる)。
	bool write_unless_marked(const std::function<bool()> &write);

private:
	std::atomic<bool> marked_{false};
	std::mutex write_mu_;
};

// プロセスに 1 つのフラグを立てる。立てたら OBS を終了するまで下ろさない (下ろす関数はない)。どのスレッドから
// 呼び出してもよい。
void mark_uninstalling();

// プロセスに 1 つのフラグが立っているか。どのスレッドから呼び出してもよい。読むのは、枠の自動作成 (sources/ の
// ensure_* とシーンを作る関数、pipeline/ と plugin-main.cpp)、settings.json、seen_people.json の書き込み、
// 起動時の書き直し、接続の待ち受けである。
bool is_uninstalling();

// プロセスに 1 つのフラグで write_unless_marked を行う。ほかのスレッドから設定のフォルダーに書く処理は、この関数を
// 通して書く (pipeline/pipeline.cpp が、録画ファイルの作成の済んでいない録画の保存先の一覧 (record/record_search_dirs.h) を書く)。
// アンインストールは録画ファイルの作成の途中でも始められる。フラグを見るだけだと、確認を通り過ぎた書き込みが段階 (1)
// の後にファイルを作り直し、段階 (3) でフォルダーを消せずに止まる。確認と書き込みを 1 つのロックの下で行い、
// mark() はフラグを立てた後に同じロックを 1 回取るので、mark() が戻った後に始まる書き込みは必ずフラグを見る
// (実行中だった書き込みは、段階 (1) より前に終わる)。
// 段階 (0) で書き込むスレッドを止める方法は使わない。アンインストールが録画のコンポーネントを知ることになり、
// 録画ファイルの作成を止めずにアンインストールできるという現在の動作も変わる。
bool write_unless_uninstalling(const std::function<bool()> &write);

// ---- 結果の型 ----

// 残っているもの 1 つ。item は表の Remover::Product の行のどれか。
struct UninstallLeftover {
	RemovalItem item = RemovalItem::ModuleConfigFiles;
	std::string where; // ファイルの場所か、シーンコレクションの表示名 (分からなければ空)
	// where がファイル 1 つの場所か。false なら、フォルダー、シーンコレクションの表示名、または空。
	// 削除できなかったものを知っているのは段階の処理なので、段階の処理が見たとおりに入れる。delete_file が
	// 失敗したもの、フォルダーを指さないシンボリックリンク、バックアップ、state.json はファイルで、
	// remove_directory_if_empty が失敗したもの、たどる深さを超えたもの、保存先のフォルダーそのものはフォルダー
	// である。結果のダイアログとドックのカードは、ファイルの行に [ファイルの場所を開く]
	// (cordmux.dialog.open_file_location.button) を置く。
	// 場所の文字列 (拡張子の有無など) から推測しない。拡張子のないファイルも、ドットを含むフォルダーも作れる。
	bool is_file = false;
};

// 1 つの段階の結果。leftovers、collections_not_opened、collections_duplicate_names の 3 つとも空なら、その段階は
// 残りなく終わった。run_uninstall が止まるのは、leftovers が空でないときだけである。
struct UninstallStepResult {
	std::vector<UninstallLeftover> leftovers;
	// 段階 (2) だけが使う。開けなかったシーンコレクションと、同じ表示名が 2 つ以上あったシーンコレクションの名前。
	// 結果には入れるが、アンインストールは止めない。どちらもアンインストールのやり直しで修正できるものではなく、
	// 止めると段階 (3) が実行されず、Vesktop が cordmux のフォルダーを指したままになる (その後にフォルダーを
	// 消すと、Vesktop が公式の Vencord をダウンロードする)。残るのは、そのシーンコレクションの中の、読み込む
	// プラグインのなくなったソースだけである。
	std::vector<std::string> collections_not_opened;
	std::vector<std::string> collections_duplicate_names;
	// 段階 (2) だけが使う。シーンコレクションのフォルダーを読めず、同じ表示名のシーンコレクションがないか確認できなかった。
	// collections_not_opened と同じく、結果には入れるが、アンインストールは止めない。
	bool collection_folder_unreadable = false;
	// 段階 (3) だけが入れる。元に戻せずに止まったときに、次のアンインストールが使うバックアップを残した、cordmux の
	// 設定のフォルダー (restore_vencord_dir のコメント)。残りではない (止めるかの判断に入れない)。
	std::string settings_dir;
};

// 1 回のアンインストールの結果。ドックはこれを ledger::UninstallFacts に入れる。
struct UninstallReport {
	// 最後に終えた段階。completed が false なら、この次の段階で止まった (または始めていない)。
	UninstallStep reached = UninstallStep::MarkUninstalling;
	bool completed = false; // 段階 (4) まで終えたか

	// 途中で止まったときに残っているもの。止まった段階のぶんだけを持つ (それより後の段階は始めていない)。
	std::vector<UninstallLeftover> leftovers;
	// 段階 (2) の結果。completed が true でも空でないことがある (UninstallStepResult のコメント)。
	std::vector<std::string> collections_not_opened;
	std::vector<std::string> collections_duplicate_names;
	bool collection_folder_unreadable = false; // 段階 (2) の結果 (UninstallStepResult と同じ意味)

	// 段階 (4) で表示する、消してほしいフォルダー。1 つに決まらない置き方 (DLL のフォルダーが plugins\<名前> でない) では空
	// (extracted_folder_from_module_dir)。途中で止まったときも入れる。止まったときの結果のダイアログは、この
	// 場所を削除しないでほしいフォルダーとしてだけ表示し、消すフォルダーとしては表示しない
	// (dock/install_dialog_plan.cpp。cordmux.dialog.uninstall_folder.text の行は、終わったときだけ表示する)。
	// 削除しないでほしいという注意の直後に限るので、段階 (3) より先にフォルダーを消させることにはならない。
	// この場所を表示しないと、注意が指すフォルダーを、同じダイアログに並ぶ 2 つの cordmux の設定のフォルダーと
	// 見分けられない。
	// 止まったとき用のフィールドを別に作らないのは、同じ関数から同じ値が入るフィールドが 2 つになるためである。
	std::string folder_to_delete;
	// 止まった段階が返した、バックアップを残した cordmux の設定のフォルダー (UninstallStepResult::settings_dir の
	// コピー)。値を持つのは、段階 (3) で元に戻せずに止まり、バックアップを見たときだけで、それ以外 (終わったとき、
	// 段階 (1) と (2) で止まったとき、バックアップがないとき) は空。
	std::string settings_dir;
	// cordmux の録画の保存先。completed のときで、フォルダーがあり、中に 1 つ以上のものがあるときだけ値を持つ
	// (record_dir_if_has_files)。
	std::string record_dir;
};

// ---- 段階ごとの処理 (注入する) ----

struct UninstallOps {
	// (0) アンインストール中のフラグを立て、接続の待ち受けを止める。
	std::function<void()> mark_and_stop_listening;
	// (1) 設定とトークン (表の SettingsAndSecret の行)。
	std::function<UninstallStepResult()> remove_settings_and_secret;
	// (2) ソースとシーン (表の SourcesAndScenes の行。ほかのシーンコレクションも順に開いて消し、最後に元の
	// シーンコレクションに戻る)。
	std::function<UninstallStepResult()> remove_sources_and_scenes;
	// (3) vencordDir を元に戻す (表の RestoreVencordDir の行。install/vencord_dir.h の plan_restore)。
	std::function<UninstallStepResult()> restore_vencord_dir;
	// (4) 消してほしいフォルダー (表の ShowFolder の行) と、録画の保存先。folder_to_delete は途中で止まったときにも
	// 呼び出す (UninstallReport::folder_to_delete のコメント)。record_dir は completed のときだけ呼び出し、あって
	// 空でない場所だけを返す (実際の関数は record_dir_if_has_files を使う)。
	std::function<std::string()> folder_to_delete;
	std::function<std::string()> record_dir;
};

// 録画の保存先を結果のダイアログに表示するかを決める。dir があり、中に 1 つ以上のものがあるときだけ dir を返し、
// それ以外 (空の文字列、ない、読めない、空のフォルダー) は空を返す。list は platform::list_directory と同じ約束
// (ないか読めなければ false)。
// 録画の保存先のフォルダーは、録画のセッションを始めるときにだけ作る (record/record_segment.h の
// make_directories)。あるかどうかを見ないと、一度も録画していない利用者に、ない場所を
// cordmux.dialog.recordings_here.text として表示し、[フォルダーを開く] でも開けない。空でないことまで見るのは、
// 利用者が録画を手で消した空のフォルダーを表示しないためである。保存先は利用者が選んだ場所そのもの
// (D:\ のように、録画のほかのファイルも入る場所でありうる) なので、中に開始日時の名前のフォルダー
// (record::is_session_dir_name) が 1 つもなければ、録画の場所として表示しない。中はたどらず 1 階層だけ見る。保存先の下は
// 録画のセッションごとのフォルダーで、セッションを始めるときに録画の中間ファイルのフォルダーと録画ログを作る
// (record/record_segment.h の ensure_session_open) ので、一度でも録画していれば空ではない。利用者がセッションの
// フォルダーの中だけを手で消し、セッションのフォルダーを残したときは表示する (中をたどると、録画の多い利用者で
// 結果のダイアログが遅くなる。空のセッションのフォルダーも、利用者のものとして残っている)。
// 見出しを変えていつも表示する方法は使わない (録画のない利用者に不要な場所を表示し、開けないボタンも残る)。
std::string record_dir_if_has_files(
	const std::string &dir, const std::function<bool(const std::string &, std::vector<platform::DirEntry> &)> &list);

// ドックが知っていて、実際の段階の処理に必要なもの。フィールドは追加してよい (追加するだけなら、呼び出し元の
// 現在の行は壊れない)。パスはどれも UTF-8。
struct UninstallContext {
	std::string module_config_dir; // obs_module_config_path の場所 (表の ModuleConfigFiles)
	std::string backup_json_path;  // バックアップ vencord_dir_backup.json (plan_restore が読む)
	std::string record_dir;        // 録画の保存先 (表の Recordings。表示するだけで消さない)
	std::size_t max_json_bytes = 0; // state.json とバックアップを読む大きさの上限 (platform/os_probe.h と同じ値)
	std::function<void()> stop_listening; // 接続の待ち受けを止める (pipeline/ の関数を渡す)

	// data/vencord の場所 (dock/ の DockPollConfig::bundled_vencord_dir と同じ値)。段階 (3) が、バックアップが
	// ないのに現在の値がここを指しているか、利用者が変えた値がここを指しているかを見分けるのに使う
	// (restore_vencord_dir のコメント)。空なら見分けない。
	std::string bundled_vencord_dir;

	// 設定の allow_state_json_write をそのまま渡す。false なら、段階 (3) は state.json を書かず、元に戻せなかった
	// ものとして結果に入れる。実機テストの実行スクリプト (test/tools/obs-run/seed-config.py) は、テストによっては
	// このキーを false にして、state.json を書かないようにする。既定が false なのは、渡し忘れたときに書かない側に
	// するためである。
	bool allow_state_json_write = false;
};

// ---- 表から段階ごとの対象を取り出す ----

// 表 (kRemovalTable) のうち、Remover::Product で段階が `step` の行の種類を、表の順のまま返す。段階 (0) と (4) と
// NotRemoved では常に空。どの段階も、ここで返す種類だけを扱い、表にないものは消さない。行を追加すると、その行は
// どれかの段階に必ず現れる (テストが確認する)。
std::vector<RemovalItem> product_items_for_step(UninstallStep step);

// 種類の探し方 (表の Locate の列)。表にない種類なら nullopt。
std::optional<Locate> locate_of(RemovalItem item);

// ---- ファイルの段階 (1) と (3) ----

// ファイルと Vesktop の設定を扱う関数 (注入する)。実際の関数は platform/ の同じ名前の関数。
// 空のフィールドがあれば、それを使う行を「消せなかった」として結果に入れる。
struct UninstallFileOps {
	std::function<bool(const std::string &)> delete_file;               // もうなければ true
	std::function<bool(const std::string &)> remove_directory_if_empty; // もうなければ true
	std::function<bool(const std::string &, std::vector<platform::DirEntry> &)> list_directory;
	std::function<bool(const std::string &, std::string &, std::size_t)> read_file_bytes;
	std::function<bool(const std::string &, const std::string &)> write_file_atomic;
	std::function<bool(std::string &)> vesktop_state_json_path;
	std::function<bool(std::string &)> vesktop_settings_json_path; // 空なら Vencord の設定は触らない
	std::function<std::string()> local_data_dir; // 解決できなければ空
	// トークンの保存先を、今ほかの OBS が使っているか。空なら使っていないとみなす。使っていれば消さずに結果に
	// 入れる (同じ PC の 2 つ目の OBS からアンインストールしたときに、1 つ目の OBS のトークンを消して接続を
	// 壊さないため)。
	std::function<bool()> local_data_used_by_other;
	// 元に戻す書き込みの計画 (実際の関数は install::plan_restore。テストが差し替えられるように関数にした)。
	std::function<RestorePlan(const std::string &, const Backup &)> plan_restore;
	// Vesktop が今起動しているか (実際の関数は platform::vesktop_running_now)。調べられなければ nullopt。
	// [Vesktopから削除] (remove_from_vesktop) とアンインストールの段階 (3) (restore_vencord_dir) が、書く直前に見る。
	// 空なら調べられないとみなし、書かない。
	std::function<std::optional<bool>()> vesktop_running;
};

// フォルダーの中をたどる深さの上限 (これより深いものは消さずに結果に入れる)。cordmux が書くのは保存先の 1 階層目
// だけで、上限はフォルダーのループに入ったときに止まるためのもの。目安の値で、測って決めたものではない。
inline constexpr int kMaxFolderDepth = 8;

// 段階 (1)。表の SettingsAndSecret の行 (ModuleConfigFiles と LocalDataFolder) を消す。フォルダーの中身を
// 消してからフォルダーを消す (深さは kMaxFolderDepth まで)。
// シンボリックリンクとジャンクション (platform::DirEntry::is_link) の中へは入らず、リンクそのものだけを消す。
// 指す先は、cordmux の保存先の外にある利用者のファイルかもしれないためである (platform/platform.h の約束)。
// バックアップ (context.backup_json_path) だけは消さずに残す。段階 (3) が元の値を読むのはバックアップだけなので、
// ここで消すと、段階 (1) の途中で止まってやり直したときに、元の値が失われる。トークンの保存先は、ほかの OBS が
// 使っているときは消さずに結果に入れる (UninstallFileOps::local_data_used_by_other)。
UninstallStepResult remove_settings_and_secret(const UninstallFileOps &ops, const UninstallContext &context);

// 段階 (3)。表の RestoreVencordDir の行。バックアップを読み、plan_restore の計画のとおりに state.json を
// write_file_atomic (一時ファイルに書いてから置き換える) で書き、元に戻し終えたら、バックアップを消して設定の
// フォルダーを消す。元に戻せなかったときはバックアップを残す (手で戻すための情報を消さない)。
//
// plan_restore が Refuse(ForeignChange) を返したとき (インストールの後に利用者が選んだ値) は、その値を残し、
// 現在の値が data/vencord (context.bundled_vencord_dir) を指していなければ残りに数えない (理由は
// install/vencord_dir.h の plan_restore のコメントにある)。指していれば、フォルダーを消すと Vesktop が公式の
// Vencord をダウンロードするので、結果に入れて止める。バックアップがないのに現在の値が data/vencord を指している
// ときも同じである。
//
// 元に戻せずに止まり、バックアップを見たとき (バックアップがある、またはあるか分からない) は、バックアップを
// 残した cordmux の設定のフォルダー (context.module_config_dir) を UninstallStepResult::settings_dir に入れる。
// 結果のダイアログとドックのカードは、元に戻すまで削除しないでほしいフォルダーとして、プラグインのフォルダーの
// 後にこの場所を並べる (ledger/rules_dock.cpp の uninstall_result)。このフォルダーを消すと、次のアンインストールの
// 段階 (3) でバックアップがなく、Vesktop の設定を戻す方法がなくなる。削除しないでほしいフォルダーを 1 つだけ
// 挙げると、挙げないほうは消してよいと読める。
// 「あるか分からない」は、一覧の読み込みも空のフォルダーの削除も失敗した (フォルダーがあり、中身がある) ときで、
// 2 つの関数があるときに限る。関数がない (渡し忘れた) ときは何も見ていないので入れない。バックアップがないときは、
// フォルダーは段階 (1) が消した後なので、入れると開けない場所を表示することになる。元に戻し終えた後の、
// バックアップやフォルダーの削除の失敗も入れない (もう戻すものがなく、次のアンインストールにバックアップは
// 必要ない)。
// 段階 (1) で止まったときもバックアップは残るが、settings_dir は入れない。そのときの結果のダイアログは、
// アンインストールのやり直しだけを勧め、手で消す方法を示さない (ledger/rules_dock.cpp の uninstall_result)。
// 止まったときにいつも module_config_dir を入れる方法は使わない。バックアップのないときにも、ないフォルダーを
// 表示することになる。
//
// state.json を書く直前に ops.vesktop_running で Vesktop が起動していないことを確認する ([Vesktopに追加] と
// [Vesktopから削除] と同じ)。ドックは押す前の確認のダイアログの前と後で Vesktop の状態を見ているが、その後に
// 起動されることがある。起動中の Vesktop は state.json をメモリの値で書き直すので、戻した値が消える。起動して
// いたら (調べられなくても) 書かず、書けなかったときと同じく、元に戻せなかった設定として結果に入れて止める
// (バックアップは残す)。
//
// vencordDir を元に戻し終えた後 (残りなく終わったとき) に、Vencord の設定 (Vesktop の settings/settings.json) の
// plugins の cordmux の項を、バックアップの plugin_entry (cordmux を入れる前の項。install::Backup) に合わせて戻す
// (install::plan_restore_plugin_entry)。項がなかったなら消し、あったならその値に戻す。Vencord は必須のプラグインの
// 項を必ず保存するので、消さないとアンインストールの後も cordmux の項が残る。利用者が前から使っている別のバージョンの
// cordmux も同じ名前の項を持つので、無条件に消すと、そのバージョンの設定 (接続のトークンなど) が失われる。
// バックアップがない、読めない、plugin_entry が分からない (古いバックアップに項がない、cordmux を入れたときに
// 設定が読めなかった) ときは、触らない。書く前に Vesktop が起動していないことを確認する (state.json を書いたときは
// その確認で足りるので、繰り返さない)。起動していたら (調べられなくても) 書かない。Vesktop は Vencord の設定もメモリの
// 値で書き直す。ファイルがない、読めない、もう戻っている、Vesktop が起動している、
// context.allow_state_json_write が false のときは何も書かず、残りにも数えない。Vesktop の動作に関わらない
// データなので、アンインストールを止める理由にしない。
UninstallStepResult restore_vencord_dir(const UninstallFileOps &ops, const UninstallContext &context);

// ---- [Vesktopから削除] ----

// [Vesktopから削除] (ledger::ActionId::RemoveFromVesktop) の結果。
enum class VesktopRemoveOutcome {
	Restored,         // state.json の vencordDir を cordmux を追加する前の値に戻した
	AlreadyRestored,  // もう元の値だった (state.json は書いていない)
	VesktopRunning,   // 書く直前に Vesktop が起動していた (何も書いていない)
	VesktopUnknown,   // 書く直前に Vesktop が起動しているかを調べられなかった (何も書いていない)
	StatePathUnknown, // state.json の場所が分からない
	StateUnreadable,  // state.json を読めない
	NoBackup,         // バックアップを読めない (ないときを含む)
	BackupInvalid,    // バックアップの形が違う
	Refused,          // plan_restore が戻さないと決めた (現在の値が cordmux の書いた値でない、壊れた JSON など)
	WritesDisabled,   // context.allow_state_json_write が false
	StateWriteFailed, // state.json に書けなかった
};

// [Vesktopから削除] の本体。アンインストールの段階 (3) (restore_vencord_dir) と同じ処理で、Vesktop の設定を cordmux を
// 追加する前の値に戻す。state.json の vencordDir はバックアップの previous に戻し (plan_restore)、Vencord の設定の
// cordmux の項はバックアップの plugin_entry に合わせて戻す (plan_restore_plugin_entry)。
// 段階 (3) と違い、cordmux のファイル (data\vencord など) と設定 (バックアップ vencord_dir_backup.json と設定の
// フォルダー) には触らない。バックアップを残すので、後で [Vesktopに追加] を押すと、state.json が previous に戻って
// いる (plan_write の分岐 4) ので、previous と plugin_entry を引き継ぎ、バックアップは cordmux の場所で上書き
// されない。後のアンインストールは、cordmux を追加する前の値に戻す。
// Vesktop の画面が表示されないとき (Discord の更新に同梱の Vencord が合わない) に、利用者が cordmux を外して Vesktop を
// 使えるようにするための操作である。
//
// 書く直前に ops.vesktop_running で Vesktop が起動していないことを確認し、起動しているか調べられなければ何も書かない
// (ledger/actions.cpp の vesktop_busy_reason。起動中の Vesktop は state.json をメモリの値で書き直すので、書いた値が
// 消える)。確認は、読み込みと計画の後、書く直前に 1 回だけ行う。
// Vencord の設定の項は、state.json を戻せた (か、もう戻っていた) ときだけ戻し、失敗しても結果に入れない (段階 (3) と
// 同じ。Vesktop が cordmux を読まなくなるのは vencordDir を戻したときで、項は Vesktop の動作に関わらない)。
VesktopRemoveOutcome remove_from_vesktop(const UninstallFileOps &ops, const UninstallContext &context);

// ---- 段階 (4) の場所 ----

// cordmux の DLL の場所 (platform::module_dir()) から、展開したフォルダー (表の ExtractedFolder) を求める。
// 「<...>/plugins/<module_name>」の形 (区切りは / と \ のどちらでもよい。末尾の区切りは無視する。名前の大文字と
// 小文字は区別しない。plugins の上に少なくとも 1 階層ある) のときだけ、そのフォルダーを返し、ほかの形では nullopt を
// 返す。module_name は DLL の名前から拡張子を除いたもの (OBS 33 以上はフォルダーの名前を DLL の名前として探すので、
// 2 つは同じ文字列になる)。この plugins フォルダーは、インストール版の OBS では C:\ProgramData\obs-studio\plugins、
// ポータブル版の OBS では OBS のフォルダーの plugins である。OBS 自身の obs-plugins/64bit や、OBS 31 と 32 の
// bin/64bit の形で上のフォルダーを返すと、OBS のフォルダーそのものや無関係なフォルダーを消させることになるので、
// 形が合わなければ返さない。
std::optional<std::string> extracted_folder_from_module_dir(std::string_view module_dir, std::string_view module_name);

// ---- 段階 (2) シーンコレクションを順に開く ----

// 1 つの種類の、消した後に残っている数。
struct RemovalCount {
	RemovalItem item = RemovalItem::CordmuxSources;
	std::size_t remaining = 0;
};

// 1 つのシーンコレクションを開いて消した結果。
struct CollectionVisit {
	std::string name;                    // 表示名 (分からなければ空)
	std::vector<RemovalCount> remaining; // 消した後に残っている数
};

// どの順に開くか。
struct CollectionVisitPlan {
	std::string original;                 // 現在のシーンコレクション (最後に戻る先。空なら分からない)
	std::vector<std::string> others;      // 順に開くもの (original を除く。順は OBS が返した順)
	std::vector<std::string> not_visited; // 戻る先が分からないので開かないもの (結果に入れる)
};

// 開く順を決める純粋関数。current が空 (現在の名前が分からない) なら、ほかを 1 つも開かない (元に戻れないまま、
// 利用者を別のシーンコレクションに置いたままにしない)。listed の中の同じ文字列は 1 回だけ数える。
CollectionVisitPlan plan_collection_visits(const std::vector<std::string> &listed, const std::string &current);

// シーンコレクションのフォルダーの中の 1 つのファイル。
struct CollectionFile {
	std::string file_name;    // フォルダーの中の名前 (例 "Untitled.json")
	std::string name_in_file; // ファイルの中の "name" の値 (なければ空)
};

// OBS がそのファイルに付ける表示名。拡張子がちょうど ".json" のファイルだけが対象で、ほかは nullopt。"name" が
// 空なら、ファイルの名前から ".json" を除いた文字列 (OBS 31.1.1 の
// frontend/widgets/OBSBasic_SceneCollections.cpp の RefreshSceneCollectionCache と同じ規則)。
std::optional<std::string> collection_display_name(const CollectionFile &file);

// 同じ表示名が 2 つ以上あるものの表示名 (見つけた順に 1 回ずつ)。OBS は同じ表示名の 2 つ目を一覧に載せない
// (上と同じ関数の try_emplace) ので、2 つ目を開く方法がない。
std::vector<std::string> duplicate_collection_names(const std::vector<CollectionFile> &files);

// シーンコレクションを開いて消す関数 (注入する)。実際の関数は OBS の frontend API と sources/cordmux_removal.h。
struct CollectionWalkOps {
	std::function<std::string()> current_collection;              // 現在の表示名 (分からなければ空)
	std::function<std::vector<std::string>()> list_collections;   // OBS が一覧に載せている表示名
	std::function<bool(const std::string &)> open_collection;     // 開いて、現在の名前が一致したら true
	std::function<std::vector<RemovalCount>(const std::vector<RemovalItem> &)> remove_here;
	// フォルダーを読めなければ nullopt (確認できなかったものとして結果に入れる)。読めて 1 つもなければ空の配列。
	std::function<std::optional<std::vector<CollectionFile>>()> list_collection_files;
	std::function<void()> save;                                    // 現在のシーンコレクションを保存させる
};

// 段階 (2) の本体。現在のシーンコレクションで消し、ほかを 1 つずつ開いて消し、元のシーンコレクションに戻って
// もう一度消して数え (切り替えたときに作り直されたものも拾う)、保存させる。items には
// product_items_for_step(SourcesAndScenes) を渡す。
UninstallStepResult walk_collections(const CollectionWalkOps &ops, const std::vector<RemovalItem> &items);

// 開いた結果をまとめる純粋関数。remaining が 1 以上の種類を残っているものにし、数が返らなかった種類も残っている
// ものとして数える (結果から落とさない)。
UninstallStepResult summarize_collection_walk(const std::vector<CollectionVisit> &visits,
					      const std::vector<RemovalItem> &items,
					      const std::vector<std::string> &not_opened,
					      const std::vector<std::string> &duplicate_names,
					      bool folder_unreadable = false);

// ---- 実際の関数をまとめる (uninstall_real.cpp。製品のビルドにだけ入る) ----

// 段階 (4) で表示する、消してほしいフォルダー (platform::module_dir と DLL の名前から
// extracted_folder_from_module_dir で求める)。1 つに決まらない置き方 (DLL のフォルダーが plugins\<名前> でない) では空。
// make_real_uninstall_ops の段階 (4) と、確認のダイアログの文を選ぶドック (dock/dock_poll_install.cpp) が、
// 同じこの関数を呼び出す (確認のダイアログと結果のダイアログの形を、1 つの判定で選ぶため)。
// ログは書かない (書くのは段階 (4) の関数だけ。確認のダイアログの前と段階 (4) で、同じことを 2 行書かないため)。
std::string real_folder_to_delete();

// platform/ の実際の関数を詰める。
UninstallFileOps make_real_uninstall_file_ops();

// 実際の段階の処理を詰める。段階 (0) は mark_uninstalling と context.stop_listening、段階 (1) と (3) は
// make_real_uninstall_file_ops の関数で上の 2 つの関数、段階 (2) は OBS の frontend API と
// sources/cordmux_removal.h で walk_collections、段階 (4) は real_folder_to_delete と context.record_dir を使う。
UninstallOps make_real_uninstall_ops(const UninstallContext &context);

// 段階 (0) から (4) を順に呼び出し、1 つの結果にまとめる。ある段階が残り (leftovers) を返したら、そこで止めて
// 残りを結果に入れる。段階の順序には意味があるためである (たとえば、段階 (3) で vencordDir を元に戻す前に
// 段階 (4) のフォルダーを消すと、Vesktop がそこに公式の Vencord をダウンロードする)。
// 段階の関数が空なら、その段階の Product の行を残っているものとして結果に入れて止める (渡し忘れを、残りなく
// 終わったと言わないため)。段階 (0) の関数が空なら、何もせずに表の Product の行を全部結果に入れる (フラグを
// 立てずに先へ進まない)。
// ドックは、アンインストールの確認のダイアログで [アンインストール] (cordmux.dialog.uninstall.button) を選んだ
// ときだけ、make_real_uninstall_ops の結果を渡して呼び出し、UninstallReport を ledger::Observation の
// UninstallFacts に入れる。OBS の UI のスレッドから呼び出す (段階 (2) のシーンコレクションの切り替えは、UI の
// スレッドで同期して行われる)。
UninstallReport run_uninstall(const UninstallOps &ops);

} // namespace cordmux::install
