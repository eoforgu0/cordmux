// SPDX-License-Identifier: Apache-2.0

// Vesktop の state.json の vencordDir を書き換えるか、元に戻すかを決める純粋関数 (plan_write、plan_restore) と、
// 書き換える前の値を保存するバックアップ (Backup)。
// ファイルも時計も読まない。state.json の全文、書きたい場所、現在のバックアップ、現在の時刻、DLL の場所を
// 引数で受け取り、何をするかだけを返す。ファイルの読み書きは呼び出し元 (dock/ と obs_module_load) が行う。
// libobs にも Qt にも依存しない。nlohmann/json と、platform/bundle_check.h (純粋関数だけのヘッダー) にだけ
// 依存する。

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cordmux::install {

// Vencord の設定 (Vesktop の settings/settings.json) の plugins にある cordmux の項の、cordmux を入れる前の状態。
// 利用者が前から使っている別のバージョンの cordmux (自分で組み立てた Vencord に入れたプラグイン) も、同じ名前の項を持つ。
// アンインストールで項を無条件に消すと、そのバージョンの設定 (接続のトークンなど) まで失われるので、入れる前の状態を保存し、
// それに合わせて戻す。
enum class PluginEntryState {
	Unknown, // 分からない (設定が読めなかった、形が想定と違う、古いバックアップに項がない)。アンインストールは触らない
	Absent,  // 項がなかった。アンインストールは項を消す
	Present, // 項があった。アンインストールは value_json の値に戻す
};

struct PluginEntryBackup {
	PluginEntryState state = PluginEntryState::Unknown;

	// state == Present のときだけ使う。項の値 (JSON のオブジェクト) を、改行も字下げもない 1 行にした文字列。
	std::string value_json;
};

// vencordDir のバックアップ。呼び出し元が obs_module_config_path の下の vencord_dir_backup.json に JSON として
// 保存する (backup_to_json)。cordmux のフォルダーを移しても残るので、アンインストールで元の値に戻せる。
// JSON のキーは previous、written、written_at、dll_dir、plugin_entry。platform/bundle_check.h の
// parse_vencord_dir_backup は、plugin_entry 以外の 4 つのキーのうち written と written_at を読むので、その 2 つの
// 名前を変えるときは両方を同時に変える (片方だけ変えると、読む側が値を見つけられず、ledger::restart_needed() が
// true にならない)。tests/test_install_vencord_dir.cpp が 2 つを直接つないで確認する。
// plugin_entry は PluginEntryBackup の JSON で、項がなかったなら null、あったならその値のオブジェクトをそのまま
// 入れ、分からないならキーごと書かない。
struct Backup {
	// 書き換える前に state.json にあった vencordDir の値。キーがなかった (利用者が一度も Vencord Location を
	// 設定していない) ときは nullopt にして、空の文字列だったときと区別する。
	std::optional<std::string> previous;

	// cordmux が最後に書いた値。
	std::string written;

	// cordmux が最後に Vesktop の設定を書いた時刻 (エポックからのマイクロ秒)。
	// ledger::Observation::Install::discord_side_written_at_us と同じ意味で、ledger::restart_needed() が Vesktop の
	// 起動時刻と比べる。vencordDir を書いたときと、[再インストール] (cordmux.dock.reinstall.button) で
	// data/vencord にコピーしたとき (advance_written_at) に進む。
	std::int64_t written_at_us = 0;

	// 書いたときの cordmux.dll の場所。ログと診断のための値で、書くかどうかの判断には使わない (DLL を移したか
	// どうかは、written と書きたい場所を比べて判断する。書きたい場所は DLL の場所から作るので、同じことが分かる)。
	std::string dll_dir;

	// 書き換える前の Vencord の設定の cordmux の項 (PluginEntryBackup)。previous と同じ規則で、plan_write が決める。
	PluginEntryBackup plugin_entry;

	// 書いた state.json の場所 (UTF-8)。plan_write は決めない (場所は呼び出し元が platform::vesktop_state_json_path で
	// 決めて、plan の new_backup に入れる)。元に戻すとき ([Vesktopから削除]、アンインストール) は、現在の環境変数から
	// 決め直さず、この場所を使う。環境変数 VENCORD_USER_DATA_DIR が書いた後に変わっても、書いた場所を戻せる。
	// この項がないバックアップ (古い版が書いたもの) では空で、そのときは現在の場所を使う。
	std::string state_path;
};

// Backup を JSON にする。失敗しない。previous が nullopt なら JSON の null を書く (空の文字列と区別するため)。
std::string backup_to_json(const Backup &backup);

// JSON を Backup にする。4 つのキーがそろい、型が正しいときだけ true を返して *out に書く。
// 壊れた JSON、オブジェクトでない JSON、キーが欠けている、型が違う (previous は文字列か null、written と
// dll_dir は文字列、written_at は整数) ときは false を返し、*out は変えない。out が nullptr でもよい
// (形を確認するだけに使うとき)。plugin_entry は欠けていても型が違っていても失敗にせず、分からない
// (PluginEntryState::Unknown) として読む。ないバックアップのために vencordDir まで戻せなくならないようにするためである。
bool backup_from_json(std::string_view json_text, Backup *out);

// state.json を書かない理由。画面の文言にするのは呼び出し元 (dock/) である。
enum class RefuseReason {
	BrokenJson,          // state_json_text が JSON として読めない
	NotObject,           // JSON だがオブジェクトでない (配列、数値など)
	VencordDirNotString, // "vencordDir" キーはあるが、値が文字列でない
	ForeignChange,       // 現在の値が target_dir にも current.written にも current.previous にも
	                      // 一致しない (利用者かほかのツールが書き換えた)
	// その場面では書かない (WriteScene::Startup)。異常ではなく、起動のたびに普通に返る値で、画面に何かを
	// 表示する理由にはならない。返すのは、起動時の分岐 2 (バックアップがない) と分岐 4 (元の値に戻っている)
	// だけである (plan_write のコメントの表)。
	SceneNotAllowed,
};

// plan_write を呼び出す場面。場面ごとに、書いてよい条件が違う (plan_write のコメントの表)。
//   Pressed      Vesktop に追加されていないときに、[Vesktopに追加] (cordmux.dock.add_to_vesktop.button) を押した。
//   FixRequested Vesktop の設定がほかの Vencord を指しているときに [Vesktopに追加] を押したか、ほかの Vencord から
//                切り替える確認のダイアログ (cordmux.dialog.vesktop_switch.headline) で [切り替え]
//                (cordmux.dialog.switch.button) を選んだ。
//   Startup      OBS の起動時 (obs_module_load の中で 1 回)。cordmux のフォルダーを移した後に、vencordDir を
//                新しい場所に書き直すためだけに使う。
// 起動時の書き直しにも別の関数を作らず、plan_write(..., WriteScene::Startup) を使う。関数を分けると、書き込みの
// 規則が 2 か所に分かれる。
enum class WriteScene {
	Pressed,
	FixRequested,
	Startup,
};

enum class WriteAction {
	Write,       // state.json を書き換えてよい (new_text と new_backup を使う)
	AlreadyOurs, // もう target_dir と一致している。何もしない
	Refuse,      // 書かない (refuse_reason が理由)
};

struct WritePlan {
	WriteAction action = WriteAction::Refuse;

	// action == Write のときだけ使う。state.json にそのまま書き戻す全文 (ほかのキーは変えていない)。
	std::string new_text;

	// action == Write のときは新しいバックアップ。action == AlreadyOurs のときは current をそのまま返す
	// (変える理由がないので受け渡すだけ。current がなければ nullopt のまま)。action == Refuse のときは nullopt
	// (バックアップは変えない)。
	std::optional<Backup> new_backup;

	// action == Refuse のときだけ使う。
	RefuseReason refuse_reason = RefuseReason::BrokenJson;
};

// state.json の vencordDir を target_dir に書き換えるかを決める純粋関数。
//
//   state_json_text - 現在の state.json の全文
//   target_dir      - 書きたい場所 (配布した data\vencord。呼び出し元が DLL の場所から作る。起動時も、ドックと
//                      同じ作り方の値を渡す)
//   current         - 現在のバックアップ (なければ nullopt。壊れたバックアップのファイルをどう扱うかは呼び出し元が
//                      決め、ここは渡された nullopt を「ない」として扱う。起動時に nullopt を渡すと、下の分岐 2 に
//                      なるので書かない)
//   now_us          - 現在の時刻 (エポックからのマイクロ秒。呼び出し元が読む)
//   dll_dir         - 現在の cordmux.dll の場所 (Backup にそのまま入るだけで、判断には使わない)
//   scene           - 呼び出す場面。既定値を持たない (場面ごとに書いてよい条件が違うので、呼び出し元が
//                      必ず指定する)
//   plugin_entry_now - 現在の Vencord の設定の cordmux の項 (snapshot_plugin_entry の結果)。書くときの次の
//                      Backup::plugin_entry の元になる。分岐 3 と 4 では使わないので、起動時は渡さなくてよい。
//                      既定値は「分からない」で、渡し忘れても、アンインストールが Vencord の設定に触らない側になる
//
// state.json の現在の値 (vencordDir がなければ「ない」) を [E]、target_dir を [T]、現在のバックアップを [C] とする。
// まず次の 5 つの分岐のどれに当たるかを決め (場面によらない)、次に場面ごとに書くかを決める。
//   1. [E] と [T] が同じ場所                -> AlreadyOurs (何もしない。バックアップもそのまま)
//   2. [C] がない (一度も書いていない)       -> 書くなら previous = [E]
//   3. [E] と [C].written が同じ場所         -> 書くなら previous は [C].previous を引き継ぐ
//      (cordmux のフォルダーを移した後の書き直しもここ。ここで [E] を previous にすると、書き直すたびに前に
//      書いた値が元の値になり、アンインストールが利用者の元の値ではなく cordmux の値へ戻してしまう)
//   4. [E] と [C].previous が同じ (どちらもキーがないか、同じ場所。元の値に戻した後)
//      -> 書くなら previous は変えない (cordmux が知っている値への書き戻しで、ほかの人の変更ではない)
//   5. それ以外 (利用者かほかのツールが別の値に書き換えたか、キーを消した)
//
//                 分岐 1       分岐 2        分岐 3               分岐 4        分岐 5
//   Pressed       AlreadyOurs  Write         Write                Write         Refuse(ForeignChange) (*)
//   FixRequested  AlreadyOurs  Write         Write                Write         Write (previous = [E])
//   Startup       AlreadyOurs  Refuse(Scene) Write                Refuse(Scene) Refuse(ForeignChange)
//
// (*) [E] にキーがないときは Write (previous = [E]、つまりキーがない)。
// Refuse(Scene) は Refuse(SceneNotAllowed)。起動時の分岐 3 は「[E] == [C].written かつ [C].written が [T] と
// 違う」場合で、[C].written が [T] と同じ場所なら [E] も [T] と同じ場所なので、分岐 1 に先に当たる。
// 書くときは written = [T]、written_at = now_us、dll_dir = 現在の場所にし、previous は分岐の規則のとおりにする。
// plugin_entry は previous と同じ規則で決める。previous を [E] にする分岐 (2 と、書くときの 5) では plugin_entry_now に
// し、previous を引き継ぐ分岐 (3 と 4) では [C].plugin_entry を引き継ぐ (分岐 3 で現在の項にすると、書き直すたびに前の
// 書き直しの後の項が元の値になり、アンインストールが cordmux を入れる前の項ではなく、入れた後の項へ戻してしまう)。
//                 分岐 2        分岐 3        分岐 4        分岐 5 (書くとき)
//   plugin_entry  現在の項        引き継ぐ      引き継ぐ      現在の項
// state.json が JSON として読めない、オブジェクトでない、vencordDir が文字列でないときは、どの場面でも、その
// 理由で Refuse を返す (分岐を決められない。起動時にも返すので、呼び出し元がログに理由を書ける)。
//
// 表の決め方の理由:
//   - 分岐 1 はどの場面でも AlreadyOurs にする。もう書いてあることは場面によらない。起動時に SceneNotAllowed に
//     すると、「書く必要がなかった」と「この場面では書かない」を、呼び出し元が区別できない。
//   - 起動時に書くのは分岐 3 だけにする。利用者が操作していない起動時には、cordmux が最後に書いた値だけを
//     新しい場所に書き直し、cordmux が書いたものではない値は書き換えないためである。分岐 2 と 4 は起動時には
//     異常ではないので SceneNotAllowed を返す。分岐 5 は ForeignChange を返す (利用者かほかのツールが書き換えた
//     ことは、画面が [Vesktopに追加] を勧める理由そのものなので、ログに正しい理由を残す)。
//   - FixRequested が Pressed と違うのは分岐 5 だけで、previous を現在の値 (ほかの Vencord の場所) にする。
//     確認のダイアログは、アンインストールすると現在の Vencord に戻すと伝えている
//     (cordmux.dialog.vesktop_switch_restore.text) ので、最初の previous を残すと、その説明が事実と違ってしまう。
//     また、インストールした後に利用者が選んだ値のほうが新しい。分岐 3 と 4 では previous を引き継ぐ。分岐 3
//     (現在の値が cordmux の古い場所) で現在の値を previous にすると、アンインストールが cordmux の古い場所へ戻す。
//     Vesktop は vencordDir の場所がないと公式の Vencord をダウンロードするので、その場所を消した後に、利用者の
//     Vencord ではなく公式の Vencord が使われる。
//   - Pressed でも、分岐 5 で [E] にキーがないときは書く。Vesktop の [Reset Vesktop] は Vesktop のデータの
//     フォルダーを丸ごと消すので、vencordDir のキーも消える。前にほかの Vencord を使っていた人 ([C].previous が
//     場所) は分岐 5 になり、書かないと、ドックが勧める [Vesktopに追加] を押しても何も起きない。キーがないのは
//     Vesktop の既定 (公式の Vencord) を使う状態で、利用者のいちばん新しい選択なので、previous をキーがない状態に
//     し、アンインストールは Vesktop の既定に戻す。[E] が場所のとき (ほかの Vencord を選び直した) は、切り替えの
//     確認 (FixRequested) を経てから書く。
//
// 場所の比べ方はどれも platform/bundle_check.h の same_dir_path (区切りの向き、末尾の区切り、英字の大小を
// そろえて比べる) である。Vesktop の設定を調べる側 (platform/os_probe.h の points_at_bundle と
// points_at_last_written) と同じ関数なので、画面は追加されていると表示するのに書き込みは拒む、という食い違いが
// 起きない。キーがないことは、空の値とも、どの場所とも一致しない。
//
// 書くときは vencordDir だけを書き換え、ほかのキーは値も順番も変えない (nlohmann::ordered_json)。整形 (改行の
// 有無、字下げの幅、末尾の改行) は入力から推定し、それに近い形で書き戻す。Vesktop がどの形で書いていても、
// 利用者のファイルの見た目を大きく変えないためである。
WritePlan plan_write(const std::string &state_json_text, const std::string &target_dir,
		     const std::optional<Backup> &current, std::int64_t now_us, const std::string &dll_dir,
		     WriteScene scene, const PluginEntryBackup &plugin_entry_now = PluginEntryBackup{});

// バックアップの written_at だけを now_us に進めたものを返す。[再インストール] で data/vencord-src からコピーし
// 終えたときに使い、ledger::restart_needed() (Vesktop の再起動を勧めるメッセージ) が true になるようにする。
// previous、written、dll_dir は変えない。now_us がバックアップの時刻より前 (時計が戻った) でも、now_us をその
// まま入れる (plan_write と同じ。restart_needed() も、現在の時計で読んだ Vesktop の起動時刻と比べる)。
Backup advance_written_at(const Backup &current, std::int64_t now_us);

// アンインストールで vencordDir を元に戻すときの書き方。バックアップの previous に戻し、previous が nullopt
// (キーがなかった) ならキーごと消す (test/tools/obs-run/cleanup.ps1 と同じ)。ほかのキーは plan_write と同じく
// 変えない。
enum class RestoreAction {
	Write,           // new_text を state.json に書く
	AlreadyRestored, // もう previous になっている (キーがなかったなら、もうキーがない)。何もしない
	Refuse,          // 書かない (refuse_reason が理由)
};

struct RestorePlan {
	RestoreAction action = RestoreAction::Refuse;
	std::string new_text;                              // action == Write のときだけ使う
	RefuseReason refuse_reason = RefuseReason::BrokenJson; // action == Refuse のときだけ使う
};

// state.json の現在の値 [E] とバックアップ [B] から、戻し方を決める純粋関数 (比べ方は plan_write と同じ
// same_dir_path。キーがないことは、どの場所とも一致しない)。
//
//   state.json が読めないなど      -> Refuse (BrokenJson、NotObject、VencordDirNotString)
//   [E] と [B].previous が同じ      -> AlreadyRestored (どちらもキーがないか、同じ場所)
//   [E] と [B].written が同じ場所   -> Write ([B].previous の文字列をそのまま書く。nullopt ならキーごと消す)
//   それ以外                        -> Refuse(ForeignChange)
//
// 現在の値が written とも previous とも違う (インストールの後に、利用者かほかのツールが別の場所に変えた。キーを
// 消した場合も含む) ときは戻さない。理由は次のとおり。
//   (i)   利用者かほかのツールが書き換えた値を書き直さないのは、plan_write と同じである。
//   (ii)  インストールの後に利用者が選んだ値のほうが、新しい意思である。
//   (iii) cordmux が書いた値はもう残っていないので、戻すべき cordmux の変更がない。
//   (iv)  現在の値は cordmux のフォルダーを指していないので、フォルダーを消しても、Vesktop が公式の Vencord を
//         ダウンロードすることはない。
// Refuse(ForeignChange) を残っているものに数えるかは、アンインストールの段階 (install/uninstall.h の
// restore_vencord_dir) が決める。
RestorePlan plan_restore(const std::string &state_json_text, const Backup &backup);

// Vencord が保存するプラグインの項の名前 (Vesktop の settings/settings.json の plugins のキー)。src-vencord/index.ts の
// definePlugin の name と同じ文字列である。
inline constexpr const char *kVencordPluginName = "cordmux";

// Vencord の設定 (Vesktop の settings/settings.json) の plugins から、plugin_name の項だけを消した全文を返す。ほかの
// キーの値と順番は変えず、整形は plan_restore と同じ (render_like) 書き方で保つ。書き換えるものがないとき
// (JSON として読めない、オブジェクトでない、plugins がオブジェクトでない、項がない) は nullopt を返す。
// Vencord は、知っているプラグインの項を有効の値つきで設定に足して保存する。cordmux のプラグインは必須なので、項が
// 必ずでき、アンインストールの後も残る。
std::optional<std::string> plan_remove_plugin_entry(const std::string &settings_json_text,
						    const std::string &plugin_name);

// Vencord の設定の全文から、plugin_name の項の現在の状態をバックアップの形にする。plugins がないか、plugins に項がなければ
// Absent、項がオブジェクトなら Present (値をそのまま入れる)。JSON として読めない、オブジェクトでない、plugins か項が
// オブジェクトでないときは Unknown (形が想定と違うものを、項がないとは扱わない)。
PluginEntryBackup snapshot_plugin_entry(const std::string &settings_json_text, const std::string &plugin_name);

// アンインストールで、Vencord の設定の plugin_name の項を、バックアップ (Backup::plugin_entry) に合わせて戻した全文を返す。
//   Absent  -> 項を消す (plan_remove_plugin_entry と同じ)
//   Present -> 項をバックアップの値に置き換える。項がなければ plugins の最後に足し、plugins もなければ作る。
//              項が元の場所にあれば、そこで置き換える (ほかの項は動かさない)
//   Unknown -> 何も返さない (触らない)
// ほかのキーの値と順番は変えず、整形は plan_remove_plugin_entry と同じ書き方で保つ。書き換えるものがないとき (JSON として
// 読めない、オブジェクトでない、plugins がオブジェクトでない、バックアップの値が読めない、もう同じ値になっている) も nullopt を
// 返す。
std::optional<std::string> plan_restore_plugin_entry(const std::string &settings_json_text,
						     const std::string &plugin_name, const PluginEntryBackup &entry);

} // namespace cordmux::install
