// SPDX-License-Identifier: Apache-2.0

// ドックのインストールに関わる操作 ([Vesktopに追加]、[再インストール]、アンインストール) のうち、Qt を使わない
// 判断だけを切り出した純粋関数。ダイアログを開くかや、どのファイルの名前を表示するかを決める。ダイアログを開き、
// ファイルを読み書きするのは dock_poll_install.cpp である。
// dock/build_observation.* と同じく libobs にも Qt にも依存せず、単体テスト (align_tests) に入る (dock/dock.cmake の
// cordmux_add_dock_observation)。

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "dock/dock_poll_config.h"
#include "install/reinstall_plan.h"
#include "install/removal_table.h"
#include "install/uninstall.h"
#include "ledger/observation.h"
#include "platform/bundle_check.h"

namespace cordmux::dock {

// ---- ほかの Vencord から切り替える確認のダイアログ ----

// [Vesktopに追加] を押したときに、ほかの Vencord から切り替える確認のダイアログを開くか。
//   vencord_dir  - 押した時点の state.json の vencordDir (キーがなければ nullopt)
//   bundled_dir  - 同梱の Vencord のフォルダー (data/vencord)
//   last_written - バックアップの written (cordmux が最後に書いた値。バックアップがなければ nullopt)
// 開くのは、キーがあり、同梱のフォルダーでも cordmux が最後に書いたフォルダーでもないときだけ。比べ方は、
// OsProbe の照合と同じ platform::same_dir_path にする。
// cordmux が最後に書いたフォルダーを除くのは、cordmux のフォルダーを移した後に残る古い場所に対しては、
// ダイアログの説明 (cordmux.dialog.vesktop_switch_current.text) が事実と違うためである。vencordDir のキーがないとき
// (Vesktop の既定の Vencord を使っているとき) にダイアログを開かないのも、同じ理由である。
bool other_vencord_dialog_needed(const std::optional<std::string> &vencord_dir, const std::string &bundled_dir,
				 const std::optional<std::string> &last_written);

// ---- [再インストール] ----

// 1 つのファイルをコピーする関数 (製品では platform::copy_file_replace)。コピーできたら true を返す。
using CopyFileFn = std::function<bool(const std::string &from_utf8, const std::string &to_utf8)>;

// install::plan_reinstall の順にコピーし、コピーできなかったファイルの名前を、その順で返す (空なら、
// すべてコピーできた)。1 つコピーできなくても、残りはコピーし続ける。コピーできるファイルまで古いまま
// 残さないためである。順の最後は cordmux-build.txt なので、途中で失敗しても cordmux-build.txt は古いまま残り、
// 次の照合で見つけ直せる (install/reinstall_plan.h の順の約束)。
// 計画が空なら、成功として扱わずに、nothing_to_copy_name を 1 つだけ返す。data/vencord-src には必ずファイルが
// あるので、空の計画は data/vencord-src のファイルを数えられなかったことを表し、コピーするものがないことでは
// ないためである。
std::vector<std::string> run_copy_steps(const std::vector<install::CopyStep> &steps, const CopyFileFn &copy,
					const std::string &nothing_to_copy_name);

// 同梱のファイルをコピーした後に、OsProbe の問い合わせの間隔 (30 秒か 5 分) を待たずに、1 回問い合わせるか
// (platform/os_probe.h の request_probe_now)。
//   copied  - すべてコピーできたか
//   pressed - 利用者が [再インストール] を押したか (false は、前のバージョンのファイルを見つけた問い合わせの
//             結果から、自動でコピーし直したとき)
// コピーできたときは、いつも問い合わせる。OS の側を変えたので、照合が一致したことを、次の問い合わせより先に画面に
// 表示するためである (一致すれば、自動のコピーし直しは止まる)。押してコピーできなかったときも問い合わせ、失敗の理由が
// ファイルの側にあれば、現在の状態を画面に反映する。
// 自動のコピーし直しでコピーできなかったときは、問い合わせない。自動のコピーし直しは問い合わせの結果から始まるので、
// 問い合わせると、また前のバージョンのファイルを見つけてコピーし直す。コピーできない間 (ファイルが開かれたままの
// ときや、フォルダーに書き込めないとき) は、問い合わせとコピーし直しが止まらずに繰り返される。次は定期的な
// 問い合わせで、1 回やり直す。
// 失敗した後に、次に [再インストール] が押されるまで自動のコピーを止めると、定期的な問い合わせの間に開かれていた
// ファイルが閉じられれば直るのに、利用者の操作まで待たせることになる。失敗の回数で待つ時間を延ばす必要もない。
// 問い合わせの間隔は既に 30 秒か 5 分で、止まらずに繰り返されるのは、ここで問い合わせを起こしたときだけである。
bool probe_after_reinstall(bool copied, bool pressed);

// 照合の結果を、install::classify_shown_bundle の hashes_match の値に変換する (照合できなかった Unknown は nullopt)。
std::optional<bool> hashes_match_of(platform::BundleState state);

// data/vencord-src と違ったファイルの名前 (data/vencord から読めなかったものと、中身が違ったもの)。順は照合の順にする
// (platform::kVencordBundleFiles。ledger/observation.h の Install::mismatched_files の約束)。
std::vector<std::string> mismatched_file_names(const platform::BundleComparison &comparison);

// ---- [Vesktopから削除] ----

// [Vesktopから削除] の結果 (install::remove_from_vesktop) を、ドックに残す失敗 (ledger::OperationFacts::
// vesktop_remove_failure) に変える。成功 (Restored と AlreadyRestored) と、書く直前に Vesktop が起動していた (か、
// 調べられなかった) とき (VesktopRunning と VesktopUnknown) は nullopt を返す。後者は失敗にせず、押せない理由をボタンの
// 下に出す (dock/build_observation.h の DockFacts::vesktop_process_seen)。ほかは、画面に出す理由の文ごとにまとめる。
//   StatePathUnknown、StateUnreadable          -> StateUnreadable (どちらも「設定ファイルを読み込めません」)
//   NoBackup、BackupInvalid、Refused           -> NotRestorable (cordmux を追加する前の値が分からない)
//   WritesDisabled                             -> WritesDisabled
//   StateWriteFailed                           -> StateWriteFailed
// Refused を NotRestorable にまとめるのは、現在の値が同梱の Vencord を指しているのに cordmux が最後に書いた値と違う
// (利用者が手で同じ場所を選んだ) か、state.json が壊れているときで、どちらも、戻す先の値が分からないためである。
std::optional<ledger::VesktopRemoveFailure> remove_failure_of(install::VesktopRemoveOutcome outcome);

// ---- アンインストール ----

// アンインストールに渡す入力を、DockPollConfig から作る。DockPollConfig からコピーするフィールド (backup_json_path、
// max_json_bytes、stop_listening、bundled_vencord_dir、allow_state_json_write) と、実行中にしか分からない 2 つ
// (module_config_dir は dock_bind.h の module_config_dir()、record_dir は Pipeline::record_base_dir()) を、
// ここでまとめて入れる。
// Qt を使う dock_poll_install.cpp の中でフィールドを 1 つずつ代入しないのは、渡し忘れを単体テストで見つけるためである。
// allow_state_json_write の既定値は false (install/uninstall.h) なので、渡し忘れると、アンインストールが毎回 Vesktop の
// 設定を元に戻せずに止まる。
install::UninstallContext uninstall_context_of(const DockPollConfig &cfg, const std::string &module_config_dir,
					       const std::string &record_dir);

// 削除の一覧 (install/removal_table.h) の行の種類を、画面に表示する、残っているものの種類に変換する。cordmux が消す
// 6 種類だけが値を持つ。利用者が消すものと、消さないものは、アンインストールが残したものではないので nullopt を返す。
std::optional<ledger::UninstallLeftoverKind> leftover_kind_of(install::RemovalItem item);

// アンインストールの結果を、ledger::UninstallFacts に変換する。OBS の状態 (配信中、録画中、仮想カメラ、
// リプレイバッファ) の 4 つのフィールドは、呼び出し元が入れる。
//   phase  - ドックが持つ進み具合 (まだ押していない、実行中、終わった、止まった)
//   report - run_uninstall の結果 (まだ押していなければ nullopt)
// 残っているものは、削除の一覧の行の残り、開けなかったシーンコレクション、同じ名前のシーンコレクションの順に並べる
// (install::UninstallReport のフィールドの順)。種類を持たない行 (利用者が消すものと、消さないもの) が混ざっていたら、
// 設定のファイルの行として表示する。アンインストールの側の誤りを、表示せずに捨てないためである。
ledger::UninstallFacts uninstall_facts_of(ledger::UninstallPhase phase,
					  const std::optional<install::UninstallReport> &report);

// アンインストールの確認のダイアログを開いてよいか。設定ウィンドウの「バージョン情報」の
// [cordmuxをアンインストール...] が押せるか (ledger/rules_dock.cpp の about_page が、同じ ActionId::Uninstall で理由を
// 決める) と同じ判断で、ledger::apply(obs, ActionId::Uninstall).can_run() を返す。dock_poll_install.cpp が確認の
// ダイアログの前と後にこれを呼び、[ツール] メニューの項目とページのボタンは、View の押せない理由を見る。
// 2 つが食い違わないことを、tests/test_dock_install_flow.cpp で確認する。
bool uninstall_can_start(const ledger::Observation &obs);

// run_uninstall の結果から、押した後の進み具合を決める。最後の手順まで終わったら Done、それ以外は Stopped を返す。
ledger::UninstallPhase phase_after(const install::UninstallReport &report);

// ---- 手順書と違う置き方で、利用者に削除してもらうファイル ----

// 展開したフォルダーが 1 つに決まらないインストール (DLL のフォルダーが plugins\<名前> でない置き方。例えば、
// cordmux.dll と同梱のファイルを OBS 自身の obs-plugins/64bit に、データを data/obs-plugins/cordmux に
// 移したもの) で、利用者に削除してもらうファイル。アンインストールが終わったときのダイアログが、場所の行と、ファイルの名前の箇条書きにする
// (install_dialog_plan.h の uninstall_result_plan)。
// 確認のダイアログで「最後にcordmuxのファイルとフォルダーの場所を表示します」
// (cordmux.dialog.uninstall_files_after.text) と伝えているので、終わったときのダイアログに表示する。同梱のファイルは
// avcodec-61.dll などで、別のプラグインと同じフォルダーにあることがあり、名前からは cordmux のファイルと分からない。
// 手順書と zip を見比べる作業を、利用者に任せないためである。
struct LooseInstallFiles {
	std::string files_dir;               // cordmux.dll と同梱のファイルがあるフォルダー (UTF-8)
	std::vector<std::string> file_names; // そこで削除するファイルの名前 (名前の順。loose_install_files_of)
	std::string data_dir;                // cordmux のデータのフォルダー (UTF-8。分からなければ空)
};

// 実行中に分かる値から LooseInstallFiles を作る (純粋関数)。
//   module_dir          - cordmux.dll のフォルダー (platform::module_dir()。空なら作らずに nullopt を返す)
//   module_file_name    - cordmux.dll のファイル名 (obs_get_module_file_name)
//   runtime_names       - 同梱の実行時のファイルの名前 (cmake/cordmux_ffmpeg_runtime.cmake の一覧)
//   bundled_vencord_dir - 配布用の zip の data/vencord (plugin-main.cpp がデータのフォルダーに "/vencord" を
//                         付けて作る、DockPollConfig のフィールド)。最後の vencord を除いたものを、データの
//                         フォルダーにする。最後が vencord でないか、親がないときは data_dir を空にする。違う
//                         フォルダーを、削除するフォルダーとして表示しないためである。
// 空の名前と、同じ名前 (Windows のファイル名なので、大文字と小文字を区別しない) の 2 つ目は入れない。名前が
// 1 つもなければ作らない。順は名前の順 (大文字と小文字を区別しない) にする。エクスプローラーの既定の順と、zip の
// cordmux フォルダーの順と同じなので、ダイアログの箇条書きとフォルダーの中を、上から見比べられる。CMake の一覧の順
// (cordmux.dll が先頭) にしないのは、一覧はリンクの都合の順で、開いたフォルダーの順と合わないためである。
// ファイルがあるかは見ない。同梱のファイルの一覧は zip と手順書と同じもので、あるかを見ても、同じ名前の別のプラグインの
// ファイルと見分けられない。あるファイルだけに絞ると、表示する一覧が利用者ごとに変わり、zip の中のファイルの
// 一覧と合わなくなる。データのフォルダーを obs_get_module_data_path で取得し直さないのは、plugin-main.cpp の
// module_data_dir が相対パスを絶対パスにしてから vencord をつないでおり、同じ処理を 2 か所に持つことになるためである。
std::optional<LooseInstallFiles> loose_install_files_of(const std::string &module_dir,
							const std::string &module_file_name,
							const std::vector<std::string> &runtime_names,
							const std::string &bundled_vencord_dir);

} // namespace cordmux::dock
