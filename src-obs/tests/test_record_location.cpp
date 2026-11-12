// SPDX-License-Identifier: Apache-2.0

// record/record_location.h の単体テスト。録画の保存先を決める 3 つの段階と、書けないときに別の場所へ移さないことを
// 確認する。

#include "doctest.h"

#include "record/record_location.h"

#include <set>
#include <string>

using cordmux::record::choose_record_base_dir;
using cordmux::record::RecordDirChoice;
using cordmux::record::RecordDirSource;

namespace {

bool absolute(const std::string &path)
{
	return path.size() >= 3 && path[1] == ':' && (path[2] == '/' || path[2] == '\\');
}

// 見つかる場所の一覧で答える、テスト用の問い合わせ。
struct Found {
	std::set<std::string> places;
	int asked = 0;
	bool operator()(const std::string &path)
	{
		++asked;
		return places.count(path) != 0;
	}
};

} // namespace

TEST_CASE("利用者が選んだ保存先を最初に使い、見つからなくてもほかの場所へ移さない")
{
	Found found;
	found.places = {"D:/obs", "C:/Users/u/Videos"};
	auto ask = [&found](const std::string &p) { return found(p); };

	const RecordDirChoice chosen = choose_record_base_dir("E:/mine", "D:/obs", "C:/Users/u/Videos", absolute, ask);
	CHECK(chosen.source == RecordDirSource::Setting);
	CHECK(chosen.base_dir == "E:/mine"); // 見つからない場所でもそのまま返す (録画を始めるときに失敗させる)。cordmux のフォルダーは足さない
	CHECK(found.asked == 0);             // 選んだ場所は、見つかるかを問い合わせもしない

	// 相対パスは OBS の作業フォルダーの下に作らない (空は、書く場所がないことを示す)。
	const RecordDirChoice relative = choose_record_base_dir("rec", "D:/obs", "C:/Users/u/Videos", absolute, ask);
	CHECK(relative.source == RecordDirSource::Setting);
	CHECK(relative.base_dir.empty());
}

TEST_CASE("利用者が選んでいなければ、OBS の録画の保存先を使う")
{
	Found found;
	found.places = {"D:/obs"};
	auto ask = [&found](const std::string &p) { return found(p); };
	const RecordDirChoice choice = choose_record_base_dir("", "D:/obs", "C:/Users/u/Videos", absolute, ask);
	CHECK(choice.source == RecordDirSource::ObsRecordPath);
	CHECK(choice.base_dir == "D:/obs/cordmux"); // 選んでいないときは OBS の保存先の下の cordmux
}

TEST_CASE("OBS の録画の保存先が空か見つからないときだけビデオフォルダーを使い、AppData は使わない")
{
	Found found;
	auto ask = [&found](const std::string &p) { return found(p); };
	// 空。
	CHECK(choose_record_base_dir("", "", "C:/Users/u/Videos", absolute, ask).source == RecordDirSource::Videos);
	// 見つからない。
	const RecordDirChoice missing = choose_record_base_dir("", "D:/gone", "C:/Users/u/Videos", absolute, ask);
	CHECK(missing.source == RecordDirSource::Videos);
	CHECK(missing.base_dir == "C:/Users/u/Videos/cordmux");
	// 相対パスの OBS の保存先も、読めないものとして扱う。
	CHECK(choose_record_base_dir("", "rel", "C:/Users/u/Videos", absolute, ask).source == RecordDirSource::Videos);
	// どれもなければ書く場所はない (AppData は使わない。録画を始めると StartFailed になる)。
	const RecordDirChoice none = choose_record_base_dir("", "", "", absolute, ask);
	CHECK(none.source == RecordDirSource::None);
	CHECK(none.base_dir.empty());
}

TEST_CASE("選んだ保存先は、その場所そのものが録画ごとのフォルダーを作る場所になる")
{
	Found found;
	auto ask = [&found](const std::string &p) { return found(p); };
	CHECK(choose_record_base_dir("D:\\hoge", "", "", absolute, ask).base_dir == "D:\\hoge");
	CHECK(choose_record_base_dir("D:\\", "", "", absolute, ask).base_dir == "D:\\");
}
