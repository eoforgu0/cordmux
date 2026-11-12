// SPDX-License-Identifier: Apache-2.0

// OS に依存する処理の宣言。Windows の実装は platform/platform_win.cpp にあり、このヘッダーには Windows の名前を
// 書かない。ほかの OS に広げるときは、同じ関数を実装した platform_<OS>.cpp を追加する。
// 場所 (パス) の文字列は、どの関数も UTF-8 で受け取り、UTF-8 で返す。

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cordmux::platform {

// OS に依存する初期化。Windows では WinSock を起動する (WSAStartup)。obs_module_load から呼ぶ。失敗したら false を
// 返す。
bool startup();

// startup() が確保した資源の後始末。obs_module_unload から呼ぶ。
void shutdown();

// ---- トークンファイル (session.json) ----

// %LOCALAPPDATA%\cordmux\session.json に書き、そこから読む内容 (形式は docs/wire_format.md の「トークンファイル」)。
// secret_hex は 32 バイトの乱数の 16 進表記 (64 文字)。writer_started_at は is_process_alive() に渡すための OS に
// 依存する値で、そのままでは時刻として読めない (時刻にするときは os_process_time_to_epoch_us を使う)。
struct SessionInfo {
	uint16_t port = 0;
	std::string secret_hex;
	uint32_t writer_pid = 0;
	int64_t writer_started_at = 0;
};

// このプロセスの識別子 (session.json の pid と started_at に書く値)。
struct ProcessIdentity {
	uint32_t pid = 0;
	int64_t started_at = 0;
};

// session.json を、この利用者だけが読める権限 (ACL) で書く。書けたら true を返す。ファイルがあれば置き換える。
// 同じフォルダーの一時ファイルに書き、置き換える前に一時ファイルに権限を付けるので、置き換わった時点から、この
// 利用者だけが読める。前に別のプロセスが広い権限で作ったファイルも、置き換えで入れ替わる。
bool write_session_file(const SessionInfo &info);

// session.json を読む。2 つ目の OBS が、先に session.json を書いた OBS が動いているかを調べるのに使う。
// ファイルがないか壊れていれば false を返し、out は変えない。
bool read_session_file(SessionInfo &out);

// session.json を削除する。もうなければ true を返す。削除できなければ false を返す。フォルダーは作らず、削除も
// しない (アンインストールが削除した後に呼んでも、フォルダーが作り直されない)。中身が自分の物かどうかは見ないので、
// 呼び出し元が read_session_file で確認してから呼ぶこと。
bool delete_session_file();

// 呼び出したプロセスの識別子。
ProcessIdentity current_process_identity();

// pid と起動の時刻 (started_at) の組が指すプロセスが、いまも動いているかを返す。pid が別のプロセスに再利用されて
// いても、起動の時刻が違えば false を返す。
bool is_process_alive(uint32_t pid, int64_t started_at);

// ProcessIdentity::started_at (session.json の started_at と同じ値) を、エポックからのマイクロ秒にする。0 以下には
// 0 (分からない) を返す。
//
// ProcessIdentity::started_at は pid の再利用を見分けるための OS に依存する値で、そのままでは時刻として表示できない。
// dock/build_observation.cpp は、別の OBS が cordmux を使っているとき (ledger::PrimaryRuleId::OtherObsHolding) の
// その OBS の起動の時刻を、この関数で変換して ledger::ListenerFacts に入れる。OS の時刻の形を知っているのは
// platform/ だけなので、変換もここに置く。VesktopProcessInfo::started_at_us と同じ変換である。
int64_t os_process_time_to_epoch_us(int64_t os_process_time);

// ---- Vesktop とファイルの状態を調べる関数 ----
//
// どれも、現在の OS の状態を 1 回調べて返すだけで、資源を持たない。一定の間隔で呼ぶのは platform/os_probe.h の
// ワーカーである (OS への問い合わせで UI のスレッドを止めないため)。

// 動いている Vesktop のプロセス 1 つの情報。started_at_us はエポックからのマイクロ秒で、ProcessIdentity::started_at
// の OS に依存する値とは違う (0 は分からないことを示す)。表示の規則は、cordmux が Vesktop の設定を書いた時刻と
// Vesktop の起動の時刻を比べて、Vesktop の再起動が必要かを決める (ledger::PrimaryRuleId::RestartVesktop) ので、
// 同じ基準の時刻にする。
struct VesktopProcessInfo {
	uint32_t pid = 0;
	int64_t started_at_us = 0;
	// 動いている Vesktop が、ポータブル版か (vesktop_exe_is_portable)。実行ファイルの場所を読めなかったときは false。
	bool portable = false;
};

// 相手のプロセスを、このプロセスと同じ Windows の利用者のものとして数えるか。OS にはアクセスしない。
//   own_sid      - このプロセスの利用者の SID (SID そのもののバイト列)。読めなければ空
//   other_sid    - 相手のプロセスの利用者の SID。プロセスを開けないか、トークンを読めなければ空
//   same_session - 相手が同じ Windows のセッションで動いているか。読めなければ false
// 2 つの SID を読めれば、それで比べる。相手の SID を読めないときは、同じセッションなら数える。管理者として起動した
// 同じ利用者のプロセスは、トークンを読めないことがあるが、同じセッションで動き、session.json も読めて接続できる。
// 別の利用者のプロセスは、ふつう別のセッションで動く。自分の SID を読めないときは、比べられないので数える
// (利用者で分けない今までの数え方に戻す。数えないと、動いている Vesktop を見落として、アンインストールの前の
// 確認などが Vesktop の終了を待たなくなる)。
bool counts_as_same_user(const std::vector<uint8_t> &own_sid, const std::vector<uint8_t> &other_sid, bool same_session);

// pid のプロセスを、このプロセスと同じ利用者のものとして数えるか (counts_as_same_user と同じ決め方)。
bool is_process_of_current_user(uint32_t pid);

// 動いている Vesktop のプロセスを探す。OBS (このプロセス) と同じ Windows の利用者のプロセスだけを数える
// (counts_as_same_user)。別の利用者の Vesktop は、session.json を読めず接続できないので、動いていないのと同じに
// 扱う。1 つでも動いていれば true を返し、いちばん早く起動したプロセスの pid と起動の時刻を out に書く。動いて
// いなければ false を返し、out は変えない。
//
// いちばん早く起動したものを選ぶのは、Vesktop が同じ名前のプロセスを複数持つ (描画のプロセスなど) ためである。
// 知りたいのは、いま動いている Vesktop が、cordmux が Vesktop の設定を書き換える前から起動していたかである。
// 後から増えたプロセスの起動の時刻を使うと、書き換えた後に増えたプロセスのせいで、再起動を勧めるメッセージが消える。
// 起動の時刻が 1 つも読めなかったとき (権限がないとき) は、started_at_us を 0 にして true を返す (動いていることは
// 分かり、起動の時刻だけが分からない)。
bool find_vesktop_process(VesktopProcessInfo &out);

// Vesktop が今動いているかを、1 回調べて返す (数え方は find_vesktop_process と同じ)。プロセスの一覧を読めなかった
// ときは nullopt を返す (find_vesktop_process は、そのときも false を返すので、動いていないと区別できない)。
// Vesktop の設定を書く直前の確認に使う (dock/dock_poll_install.cpp、install/uninstall.h の remove_from_vesktop、
// plugin-main.cpp の起動時の書き直し)。書いてよいのは false のときだけで、分からないときは書かない側にする。
std::optional<bool> vesktop_running_now();

// 実行ファイルの名前 (UTF-8。フォルダーを含まない) が、Vesktop か Discord のデスクトップクライアント (安定版、PTB、
// Canary) のものか。大文字と小文字を区別しない。OS にはアクセスしない。OBS のアプリケーション音声キャプチャが
// 取り込む先を、この名前で見分ける (sources/double_audio.h)。実行ファイルの名前は OS によって違うので、ここに置く。
bool is_discord_client_exe_name(const std::string &name_utf8);

// このプロセスの起動の時刻を、エポックからのマイクロ秒で返す (0 は分からないことを示す)。find_vesktop_process と
// 同じ変換を通るので、単体テストはこの関数で変換が正しいことを確認する (動いているプロセスの起動の時刻は、いまより
// 前で、遠い過去でもないことが分かっているため)。
int64_t current_process_started_at_us();

// Vesktop が設定を置くフォルダー (Vesktop v1.6.9 の src/main/constants.ts の DATA_DIR) を out に書く。決められなければ
// false を返す。Vesktop と同じ順で決める。
//   (1) 環境変数 VENCORD_USER_DATA_DIR があればそこ (利用者が設定した場合と、実機テストの差し替え)。
//   (2) ポータブル版の Vesktop (exe の隣に "Uninstall Vesktop.exe" がない) は exe の隣の Data だが、OBS の側からは、
//       Vesktop が動いていない間は exe の場所が分からないので扱わない。動いている Vesktop を見つけたときに
//       ポータブル版と分かれば、書かずに伝える (vesktop_portable_seen)。黙って別の場所を書かないための安全策で、
//       ポータブル版への対応ではない。
//   (3) それ以外は、Windows の RoamingAppData の既知のフォルダーの下の Vesktop。環境変数の APPDATA は読まない。
// フォルダーがあるかどうかは確認しない。
bool vesktop_data_dir(std::string &out);

// Vesktop の state.json の場所 (vesktop_data_dir の下) を out に書く。決められなければ false を返す。
// ファイルがあるかどうかは確認しない。
bool vesktop_state_json_path(std::string &out);

// Vesktop の Vencord の設定の場所 (vesktop_data_dir の下の settings/settings.json) を out に書く。決められなければ
// false を返す。ファイルの有無の扱いは vesktop_state_json_path と同じ。
bool vesktop_settings_json_path(std::string &out);

// state.json の場所 (UTF-8) から、同じフォルダーの Vencord の設定 (settings/settings.json) の場所を求める。
// 求められなければ空。バックアップに残した state.json の場所から、書いたときと同じ Vesktop の設定を指すために使う。
std::string vesktop_settings_json_beside(const std::string &state_json_path);

// 実行ファイルの場所 (UTF-8) が、ポータブル版の Vesktop のものか。Vesktop と同じ規則 (v1.6.9 の PORTABLE) で、
// 実行ファイルの隣に "Uninstall Vesktop.exe" がなく (ないと分かったときだけ。調べられなければ false)、
// electron.exe でなければ true。
bool vesktop_exe_is_portable(const std::string &exe_path_utf8);

// 動いているポータブル版の Vesktop を、この OBS が動いている間に 1 度でも見たか。find_vesktop_process と
// vesktop_running_now が見つけたときに残す。[Vesktopに追加] は、true なら書かずにその理由を伝える。
bool vesktop_portable_seen();

// ファイル全体を読む。読めたら out に入れて true を返す。ないとき、開けないとき、max_bytes より大きいときは false を
// 返し、out は変えない。読むファイルは cordmux が書いたものとは限らない (利用者やほかのツールが書き換えた
// state.json も読む) ので、大きさの上限を必須の引数にしている。
bool read_file_bytes(const std::string &path_utf8, std::string &out, std::size_t max_bytes);

// ファイル全体を text にする (なければ作り、あれば置き換える)。書けたら true を返す。フォルダーがないとき、開けない
// とき、書けなかったときは false を返す。write_file_atomic と同じ処理で、失敗したらファイルは元のままで、一時
// ファイルも残さない。新しく書くコードは write_file_atomic を呼ぶ。
//
// ファイルを読み書きするコードは、自分で std::fstream を開かずに、read_file_bytes とこの関数を使う。場所の文字列は
// libobs も cordmux も UTF-8 で持つが、Windows で char の場所を取る関数 (fopen や std::ofstream) は、それを
// アクティブコードページの文字列として読む。そのため、ASCII でない文字を含む利用者のフォルダーでは、フォルダーは
// 作れるのにファイルを開けない。場所の文字列を OS の形に変えるのは platform/ の中だけにする。
bool write_file_bytes(const std::string &path_utf8, const std::string &text);

} // namespace cordmux::platform

// wait_child だけは、record/record_types.h の型 (ChildWaitResult) をそのまま返す。このヘッダーを読み込むだけの
// ファイル (ws/ や dock/) に record/ のヘッダーを読み込ませないように、ここでは前方宣言だけにする。wait_child を
// 呼ぶファイルは record/record_types.h を読み込む。
namespace cordmux::record {
struct ChildWaitResult;
}

namespace cordmux::platform {

// ---- 録画の書き込みと録画ファイルの作成 ----
//
// ここの関数は、record/record_types.h の RecordPlatformOps に入れて使う。入れるのは record/recorder.cpp の
// make_real_record_platform_ops である (ws::PlatformOps を ws/listener.cpp の make_real_platform_ops() が作るのと
// 同じ形)。
// どれも例外を投げず、失敗は戻り値 (false、0、空、nullopt) で返す。

// ファイルのハンドル。0 は開けなかったことを示す (record::FileHandle と同じ型と意味)。閉じた後のハンドルを append
// と close に渡してはいけない (別のファイルに書くおそれがある)。ハンドルを使うのは、開いたスレッドだけである
// (録画の書き込みのスレッド)。
using FileHandle = std::uint64_t;

// 子プロセスを指す値。0 は起動できなかったことを示す (record::ChildHandle と同じ型と意味)。OS のハンドルではなく
// 通し番号なので、終わった子プロセスや止めた子プロセスの値を後から渡されても、OS がハンドルの値を使い回した
// 別のプロセスを指さない (kill_child が別のプロセスを止めないため)。
using ChildHandle = std::uint64_t;

// path までのフォルダーを、途中の階層も含めて作る (既にあれば true)。
bool make_directories(const std::string &path_utf8);

// path のフォルダーを 1 階層だけ作る (親のフォルダーはあること)。既にあれば作らずに false を返し、*already_exists を
// true にする (ほかの理由で作れないときは false を返し、*already_exists は false)。録画のセッションのフォルダーを、
// ほかのプロセスと取り違えないために使う。2 つのプロセスが同じ名前のフォルダーを同時に作っても、true を返すのは
// 片方だけである (record/record_segment.h の ensure_session_open)。
bool make_new_directory(const std::string &path_utf8, bool *already_exists);

// 書き込み用に開く (既にあれば中身を捨てる)。開けなければ 0 を返す。
FileHandle open_file(const std::string &path_utf8);

// 開いたファイルの末尾に書く。len バイトをすべて書けたときだけ true を返す。len が 0 なら何もせずに true を返す
// (書くものがないのは失敗ではない)。
bool append(FileHandle file, const void *data, std::size_t len);

// 閉じる。0 を渡してもよい (何もしない)。
void close(FileHandle file);

// ロックファイルを、共有を許さずに開いて保持する (なければ作る)。録画のセッションが使用中であることを示すのに使う
// (record/recorder.cpp の take_session_lock と、record/recorder.h の resume_pending のコメント)。閉じると (close に
// 渡すか、保持しているプロセスが終わって OS が閉じると)、ファイルも消える。開けなければ 0 を返し、ほかのハンドル
// (同じプロセスのものも含む) が保持しているためなら *held_elsewhere を true にする (ほかの理由の失敗では false)。
// 閉じたら削除する開き方が、共有違反以外の理由で拒否される保存先 (削除を許さない権限、読み取り専用の古い
// ロックファイル) では、削除の指定を外して、共有を許さずに開き直す。このときは閉じてもファイルが残るが、残った
// ファイルは、だれも保持していないと判定される。
FileHandle hold_lock_file(const std::string &path_utf8, bool *held_elsewhere);

// path のロックファイルを、いまだれかが保持しているかを返す。作らず、消さず、書かない (読み取りで開いてすぐに
// 閉じる)。ファイルがないとき、だれも保持していない古いファイル (電源が落ちたときの残りなど) のとき、確認でき
// なかったときは false を返す。
bool lock_file_held(const std::string &path_utf8);

// path のあるボリュームで、この利用者が使える空き容量 (バイト)。読めなければ nullopt を返す (読めないことを、
// 空き容量 0 と区別するため)。まだないフォルダーを渡してもよく、そのときは 1 つずつ上のフォルダーで調べる
// (録画を始める前に、まだ作っていない保存先の空き容量を調べるため)。
std::optional<std::uint64_t> free_space_bytes(const std::string &path_utf8);

// 子プロセスを起動する。program は実行ファイルの絶対パス、args は引数 (program を含まない。引用符はこの関数が
// 付ける = build_child_command_line)。起動できなければ 0 を返す。コンソールのウィンドウは表示しない。
//
// output_path_utf8 が空でなければ、子プロセスの標準出力と標準エラー出力を、その 1 つのファイルに書く (ファイルが
// あれば中身を捨てる)。RecordPlatformOps::start_child にはない引数で、record/recorder.cpp の
// make_real_record_platform_ops が渡す。録画ファイルの作成が失敗したときに原因の種類を分けるための情報が、子プロセスの出力に
// しかないためである。出力先を開けなくても、出力先なしで子プロセスを起動する (platform/platform_win.cpp の
// start_child のコメント)。
ChildHandle start_child(const std::string &program_utf8, const std::vector<std::string> &args,
                        const std::string &output_path_utf8 = std::string());

// 子プロセスが終わるまで待つ (timeout_ms が 0 以下なら、待たずに現在の状態だけを見る)。終わって片付けた子プロセスと
// 知らない値には Failed を返す (終わった子プロセスに 2 回目の wait_child を呼ぶと Failed になる)。
cordmux::record::ChildWaitResult wait_child(ChildHandle child, int timeout_ms);

// 子プロセスを止める。止まるまで待ってから戻る (待つ時間には上限がある)。呼び出し元は止めた直後に作りかけの
// 録画ファイルを削除するので、そのときに子プロセスがファイルを開いたままにならないようにする。0 と知らない値には
// 何もしない。
void kill_child(ChildHandle child);

// steady_clock の時刻 (マイクロ秒。起点に意味はない)。
std::int64_t now_steady_us();

// システム時刻 (エポックからのマイクロ秒)。録画のセッションのフォルダーの名前と録画ログに使う。
std::int64_t now_wall_us();

// 現在の時差 (秒。現地の時刻 - UTC。夏時間を含む)。録画のセッションのフォルダーの名前を、OBS の録画の既定の名前と
// 同じく現地の時刻で付けるために使う。読めなければ 0 を返す。
int utc_offset_sec_now();

// 同梱の FFmpeg の実行ファイル (ffmpeg.exe) の絶対パス。cordmux.dll と同じフォルダーにあるときだけ返し、なければ
// 空を返す。実行ファイルの名前は platform/ の中にだけ書く。
std::string ffmpeg_path();

// Windows の「ビデオ」フォルダーの絶対パス。OBS の録画の保存先を読めないときに、録画の保存先として使う (OBS の
// 既定の保存先と同じ場所)。決められなければ空を返す。ビデオのフォルダーには差し替えに使える環境変数がないので、
// 環境変数は見ない。実機テストで録画するときは、先に OBS の録画の保存先をテスト用の場所にしておく。
std::string videos_dir();

// 場所が絶対パスかを返す。`C:\...` のようにルートを持つ場所と `\\server\share\...` には true を返し、相対の場所と、
// ドライブだけが付いた `C:rec` と `C:` には false を返す。録画の保存先は、この関数で確認する。record/ は base_dir が
// 絶対パスであることを前提にしていて、相対の場所を渡すと、OBS のプロセスの作業フォルダーの下に録画を書いてしまう。
bool is_absolute_path(const std::string &path_utf8);

// 相対の場所を、OBS のプロセスの作業フォルダーを基準にした絶対パスにする (`.` と `..` も解く)。絶対パスと空の文字列は
// そのまま返し、変換できなければ元の文字列を返す。ポータブル版の OBS では、libobs が cordmux の場所を作業フォルダー
// (OBS の bin\64bit) からの相対パスで返し (obs_module_config_path、obs_get_module_data_path)、libobs 自身もその作業
// フォルダーを基準に開くので、同じ基準で解く。相対パスのまま画面に出すと、利用者はどこのことか分からず、state.json に
// 書くと、Vesktop が自分の作業フォルダーからの場所として読む。
std::string absolute_path(const std::string &path_utf8);

// 子プロセスに渡すコマンドラインの文字列を作る (start_child が使う)。
//
// 外から呼べるようにしているのは、引用符の付け方を単体テストで確認するためである。子プロセスを実際に起動する
// テストは Windows でしか実行できないが、引用符の付け方は、この関数だけで全部の場合を確認できる。空白を含む
// 場所が別の引数に分かれる不具合は、実機でしか起きず、ファイルが見つからないという遠い形で表れるので、ここは
// テストで押さえる。規則は、子プロセスが引数を読む規則 (CommandLineToArgvW と同じ) に合わせる。空白も引用符も
// 含まない引数はそのままにし、含むものは前後を引用符で囲み、引用符の前のバックスラッシュを 2 倍にする。program は
// 常に囲む。
std::string build_child_command_line(const std::string &program_utf8, const std::vector<std::string> &args);

// ---- インストール、更新、アンインストールで使うファイルの操作 ----
//
// ファイルの削除、コピー、置き換え、一時ファイルを使った書き込みは、platform/ の外で書かない (libobs の os_* を
// dock/ から呼ぶと、同じ操作が 2 通りの実装になる)。どれも例外を投げない。
//
// state.json を、書き込みの途中を読まないように読む関数は置かない。cordmux は write_file_atomic で一時ファイルと
// 置き換えて書くので、cordmux の書き込みの途中を読むことはない。Vesktop が書いている途中を read_file_bytes で
// 読んだときは JSON として壊れて見え、install::plan_write は Refuse (理由は BrokenJson) を返して書かず、ワーカーは次の
// 周期で読み直す (壊れた中身を書き戻すことはない)。読むための関数を別に追加すると、同じファイルを読む関数が 2 つになる。
//
// 一時ファイルの名前は `<置き換える先の名前>.cordmux-tmp-<pid>-<通し番号>` で、置き換える先と同じフォルダーに作る
// (copy_file_replace、write_file_atomic、write_file_bytes、write_session_file で共通)。失敗したときは削除してから
// 戻るので残らないが、書いている途中に OBS がクラッシュしたときだけは残ることがある。アンインストールとテストが、
// 残った一時ファイルを cordmux のものとして見分けられるように、名前の形をここで決めておく。

// ファイルを 1 つ削除する。もうなければ true を返す (アンインストールをやり直しても同じ結果になるように、
// 削除したいファイルがないことを失敗にしない)。フォルダーを渡したら false を返す。読み取り専用のファイルは
// 削除せずに false を返す (利用者かほかのツールが付けた属性を、メッセージもログも出さずに外さない)。
bool delete_file(const std::string &path_utf8);

// フォルダーが空なら削除する。もうなければ true を返す。中身が残っていれば削除せずに false を返す (中身を、メッセージも
// ログも出さずに削除しない。中身を削除するのは、呼び出し元が list_directory で 1 つずつ行う)。
bool remove_directory_if_empty(const std::string &path_utf8);

// フォルダーの中身の一覧 (1 階層だけ。`.` と `..` は含めない)。読めたら out を入れ替えて true を返す。ないか読めない
// ときは false を返し、out は変えない。並びは名前の UTF-8 のバイト順にする。OS が返す順はファイルシステムによって
// 違い、大文字と小文字を区別しない並べ方は地域の設定で変わるので、呼び出し元とテストが当てにできる 1 つの順に
// 決めている。同梱の Vencord のフォルダー (data/vencord-src) のファイルを数えることと、アンインストールで設定の
// フォルダーの中身を削除することに使う。
//
// シンボリックリンクとジャンクションは、フォルダーを指すものも is_directory が true になる。中身を 1 つずつ削除する
// 呼び出し元がその中に入ると、cordmux のフォルダーの外にある利用者のファイルを削除してしまう。is_link が true の
// ものには入らずに、リンクそのものを remove_directory_if_empty (フォルダーを指すリンク) か delete_file (ファイルを
// 指すリンク) で削除する (どちらもリンクが指す先には触らない)。
struct DirEntry {
	std::string name;         // 名前だけ (場所は含めない)
	bool is_directory = false;
	bool is_link = false;     // シンボリックリンクかジャンクション。true なら中に入らない
};
bool list_directory(const std::string &path_utf8, std::vector<DirEntry> &out);

// from を to にコピーする。to と同じフォルダーの一時ファイルに書き終えてから to と置き換えるので、途中で失敗しても
// to が途中までの中身にならない。to が既にあれば置き換え、なければ作る。to のフォルダーはあること。コピーできたら
// true を返す。失敗したら to は元のままで、一時ファイルも残さない。コピーするのは中身だけで、属性 (読み取り専用
// など) はコピーしない。
bool copy_file_replace(const std::string &from_utf8, const std::string &to_utf8);

// text を path に書く。同じフォルダーの一時ファイルに書き終えてから置き換える。state.json はいつもこの関数で書く
// (バックアップと設定のファイルにも使う)。書けたら true を返す。失敗したら path は元のままで、一時ファイルも
// 残さない。
bool write_file_atomic(const std::string &path_utf8, const std::string &text);

// cordmux.dll があるフォルダー (インストールしたときは plugins/cordmux。末尾の区切りは付けない)。読めなければ空を返す。バックアップの dll_dir、
// 起動時に Vesktop の設定を書き直すかの判断、アンインストールで表示するフォルダーに使う。ffmpeg_path() と同じ
// 方法で調べる (platform/platform_win.cpp の 1 か所を共有する)。
std::string module_dir();

// session.json を置くフォルダー (%LOCALAPPDATA% の下の cordmux。末尾の区切りは付けない)。決められなければ空を返す。
// アンインストールで削除する。write_session_file と同じ場所を返す (場所の組み立てを 2 か所に書かない)。場所を返す
// だけでフォルダーは作らない (アンインストールで削除した後に場所を調べ直しても、フォルダーが作り直されない)。
std::string local_data_dir();

// Windows の利用者のプロファイルのフォルダー。決められなければ空を返す。環境変数の USERPROFILE を先に読む
// (理由は local_data_dir と同じで、実機テストが環境変数を差し替えるため)。診断情報の zip から利用者の名前を除くために、
// 実際の場所を読む。
std::string user_profile_dir();

// 診断情報の zip の中で、user_profile_dir の代わりに入れる文字列。
inline constexpr const char *kUserProfileMark = "%USERPROFILE%";

// OS のバージョンを、メジャー、マイナー、ビルド、更新の番号の 4 つの数をピリオドでつないだ形で返す。読めなければ空を
// 返す。更新の番号だけが読めなかったときは 0 にする (3 つ目までは正しいので、全体を捨てない)。設定ウィンドウに
// 表示する (dock/dock_poll.cpp の read_platform_versions)。OS のバージョンを読む処理を platform/ の外に置かない
// ために、ここにある。
std::string os_version_text();

// ---- ファイルの場所をエクスプローラーで開く ----
//
// ダイアログのファイルの場所の行に付ける [ファイルの場所を開く] で使う。押すと、エクスプローラーでそのファイルを
// 選んだ状態で開く (Windows のエクスプローラーの「ファイルの場所を開く」と同じ動作)。呼び出し元は
// dock/ui の MessageSpec::reveal_file に、open_file_location を呼ぶ関数を入れる。
//
// 処理を 2 つに分けている。どこを開いて何を選ぶかを決める resolve_file_location はウィンドウを開かないので、
// 単体テストで全部の場合を確認できる。それをエクスプローラーに渡す open_file_location はウィンドウを開くので、
// 単体テストでは呼ばない。
//
// 何も選ばずにフォルダーを開く処理 ([フォルダーを開く]) は、ここではなく dock/ui/open_location.h の open_folder に
// ある。Qt の QDesktopServices だけで開け、Windows の API を使わないうえ、platform/ をビルドに含めないツール
// (tests/ui_preview と tests/dock_drive) からも呼ぶためである。

// resolve_file_location の結果。どちらも区切りを `\` にそろえ、末尾の区切りは付けない (ドライブのルートだけは
// `C:\` の形にする。`C:` はそのドライブの、現在の作業フォルダーを指すため)。
struct FileLocationTarget {
	std::string folder; // 開くフォルダー
	std::string item;   // そのフォルダーの中で選ぶもの (場所そのもの)。空なら選ばずにフォルダーだけを開く
};

// path_utf8 (絶対パス) を開くときの、開くフォルダーと選ぶものを決める。決められたら true を返す。
//   - ファイルがある: folder は親のフォルダー、item はそのファイル。
//   - フォルダーがある: folder は親のフォルダー、item はそのフォルダー (エクスプローラーの「ファイルの場所を開く」と
//     同じく、親のフォルダーの中で選ぶ)。ドライブのルートは親がないので、folder はルートで、item は空。
//   - もうない (利用者が先に削除したか移動した): 存在するフォルダーのうち、いちばん近い親を folder にし、item は空。
//     ダイアログはファイルより長く開いていることがある (ダイアログを開いたまま、利用者がファイルを片付ける) ので、
//     押しても何も起きないことにはしない。[フォルダーを開く] (dock/ui/open_location.h の folder_to_open) と同じ規則。
//   - 空か相対の場所 (OBS の作業フォルダーの下を開いてしまう) と、ルートまでさかのぼっても存在するフォルダーがないとき:
//     false を返し、out は変えない。
bool resolve_file_location(const std::string &path_utf8, FileLocationTarget &out);

// path_utf8 をエクスプローラーで開き、あればそれを選ぶ (resolve_file_location の結果のとおり)。エクスプローラーに
// 渡せたら true を返す。例外を投げず、利用者にも何も表示しない (開けなかったときに、ダイアログの上に別のメッセージを
// 重ねない。[ファイルの場所を開く] は戻り値を見なくてよい)。場所の行は文字列として残っているので、利用者は
// 場所をコピーできる。UI のスレッドから呼ぶ (ボタンを押したときの処理の中。COM はこの関数の中で初期化する)。
bool open_file_location(const std::string &path_utf8);

} // namespace cordmux::platform
