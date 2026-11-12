// SPDX-License-Identifier: Apache-2.0

// 録画の保存先の決め方 (choose_record_base_dir)。設定に表示する場所と、録画ごとのフォルダー (開始日時の名前) を作る場所は
// 同じである。利用者が選んだ保存先は、その場所そのものに録画ごとのフォルダーを作る (D:\hoge なら D:\hoge\<開始日時>)。
// 選んでいないときの既定は、録画を始める時点の OBS の録画の保存先の下の「cordmux」で、OBS の録画の保存先を読めない
// (空か、見つからない) ときだけ Windows のビデオフォルダーの下にする。既定で 1 つ下げるのは、OBS の録画ファイルも日時の名前で、
// 直接置くと混ざるためである。
// libobs にも Windows の API にも依存しない。OBS の録画の保存先とビデオフォルダーの場所は呼び出し元 (Pipeline) が読んで
// 渡し、場所が見つかるかも呼び出し元の関数に問い合わせる (テストがテスト用の値で確認できるようにするため)。

#pragma once

#include <functional>
#include <string>

#include "record/record_segment.h"
#include "record/record_types.h"

namespace cordmux::record {

// 保存先をどの候補から選んだか。
enum class RecordDirSource {
	Setting,        // 利用者が選んだ保存先 (設定の record_dir)
	ObsRecordPath,  // OBS の録画の保存先
	Videos,         // Windows のビデオフォルダー (OBS の保存先が読めないとき)
	None,           // どれもない (録画を始めると StartFailed になる)
};

struct RecordDirChoice {
	// 録画ごとのフォルダー (`<開始日時>`) を作るフォルダー (UTF-8 の絶対パス。空は書ける場所がないことを示す)。
	// 画面とログに表示する、実際に書く場所はこれである。
	std::string base_dir;
	RecordDirSource source = RecordDirSource::None;
};

// 保存先を決める。setting は設定の record_dir、obs_record_path は OBS の録画の保存先 (読めなければ空)、videos は
// ビデオフォルダー (解決できなければ空)。is_absolute は絶対パスかを、found はその場所が見つかるかを答える関数である。
// 呼び出し元は、届かないネットワークの場所で UI のスレッドを止めないように、時間の上限を付けて found に答える。
// 利用者が選んだ保存先は、見つからなくても書けなくてもそのまま返す。書けるかは録画を始めるとき (ensure_session_open) に
// 確認し、書けなければそのセッションを始めずに StartFailed にする。ここで別の場所に切り替えると、利用者の知らない場所に
// 録画を書いてしまうためである。ただし相対パスのときは空を返す (OBS の作業フォルダーの下には書かない)。
// OBS の録画の保存先も、見つかったが書けないときはそのまま返し、録画を始めるときに失敗させる (別の場所へは移さない)。
// AppData の下は既定にしない。1 時間で約 38 GB になる録画を置く場所ではないためである。
inline RecordDirChoice choose_record_base_dir(const std::string &setting, const std::string &obs_record_path,
					      const std::string &videos,
					      const std::function<bool(const std::string &)> &is_absolute,
					      const std::function<bool(const std::string &)> &found)
{
	RecordDirChoice choice;
	if (!setting.empty()) {
		// 選んだ場所はそのまま返す。相対パスだけは空にする (どこにも書かない)。
		choice.source = RecordDirSource::Setting;
		if (is_absolute && is_absolute(setting))
			choice.base_dir = setting;
		return choice;
	}
	if (!obs_record_path.empty() && is_absolute && is_absolute(obs_record_path) && found &&
	    found(obs_record_path)) {
		choice.source = RecordDirSource::ObsRecordPath;
		choice.base_dir = join_path(obs_record_path, kRecordRootDirName);
		return choice;
	}
	if (!videos.empty()) {
		choice.source = RecordDirSource::Videos;
		choice.base_dir = join_path(videos, kRecordRootDirName);
		return choice;
	}
	return choice;
}

} // namespace cordmux::record
