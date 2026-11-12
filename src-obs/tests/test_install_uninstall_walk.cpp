// SPDX-License-Identifier: Apache-2.0

// install/uninstall.h の段階 (2) (ほかのシーンコレクションも順に開いて消し、最後に元に戻る) の単体テスト。
// テスト用の OBS の関数 (現在のシーンコレクションの名前と、開く、消す、保存の記録) で、順序と結果を確認する。
// 表示名の規則は、OBS 31.1.1 の frontend/widgets/OBSBasic_SceneCollections.cpp の RefreshSceneCollectionCache と
// 同じ。

#include "doctest.h"

#include "install/uninstall.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace cordmux::install;

namespace {

// 巡回の処理は種類の中身を見ないので、2 つ目の種類は表の別の行を使う。
const std::vector<RemovalItem> kItems = {RemovalItem::CordmuxSources, RemovalItem::ModuleConfigFiles};

std::vector<RemovalCount> zero_counts(const std::vector<RemovalItem> &items)
{
	std::vector<RemovalCount> out;
	for (RemovalItem item : items)
		out.push_back(RemovalCount{item, 0});
	return out;
}

// テスト用の OBS。open_collection は現在の名前を変え、remove_here は現在の名前で消したことを記録する。
struct FakeObs {
	std::string current = "B";
	std::vector<std::string> listed = {"A", "B", "C"};
	std::set<std::string> unopenable;
	std::map<std::string, std::vector<RemovalCount>> remaining; // シーンコレクションごとの残り
	std::vector<CollectionFile> files;
	bool folder_unreadable = false; // シーンコレクションのフォルダーを読めない
	std::vector<std::string> events;

	CollectionWalkOps ops()
	{
		CollectionWalkOps o;
		o.current_collection = [this] { return current; };
		o.list_collections = [this] { return listed; };
		o.open_collection = [this](const std::string &name) {
			events.push_back("open " + name);
			if (unopenable.count(name) != 0)
				return false;
			current = name;
			return true;
		};
		o.remove_here = [this](const std::vector<RemovalItem> &items) {
			events.push_back("remove@" + current);
			const auto it = remaining.find(current);
			return it != remaining.end() ? it->second : zero_counts(items);
		};
		o.list_collection_files = [this]() -> std::optional<std::vector<CollectionFile>> {
			if (folder_unreadable)
				return std::nullopt;
			return files;
		};
		o.save = [this] { events.push_back("save"); };
		return o;
	}
};

bool has_leftover(const UninstallStepResult &r, RemovalItem item, const std::string &where)
{
	for (const UninstallLeftover &l : r.leftovers)
		if (l.item == item && l.where == where)
			return true;
	return false;
}

} // namespace

TEST_CASE("ほかのシーンコレクションを 1 回ずつ開き、現在のシーンコレクションに戻る順になる")
{
	const CollectionVisitPlan plan = plan_collection_visits({"A", "B", "C", "A"}, "B");
	CHECK(plan.original == "B");
	CHECK(plan.others == std::vector<std::string>{"A", "C"});
	CHECK(plan.not_visited.empty());
}

TEST_CASE("現在の名前が分からなければ、元に戻れなくなるので、ほかのシーンコレクションを開かない")
{
	const CollectionVisitPlan plan = plan_collection_visits({"A", "B"}, "");
	CHECK(plan.original.empty());
	CHECK(plan.others.empty());
	CHECK(plan.not_visited == std::vector<std::string>{"A", "B"});
}

TEST_CASE("表示名は OBS と同じく name のキーか、なければ拡張子を除いたファイル名で、.json のファイルだけが対象になる")
{
	CHECK(collection_display_name(CollectionFile{"Untitled.json", ""}) == std::optional<std::string>("Untitled"));
	CHECK(collection_display_name(CollectionFile{"a.json", "Stream"}) == std::optional<std::string>("Stream"));
	CHECK_FALSE(collection_display_name(CollectionFile{"a.json.bak", "Stream"}).has_value());
	CHECK_FALSE(collection_display_name(CollectionFile{"a.JSON", ""}).has_value());
	CHECK_FALSE(collection_display_name(CollectionFile{".json", ""}).has_value());
	CHECK_FALSE(collection_display_name(CollectionFile{"notes.txt", ""}).has_value());
}

TEST_CASE("重複した表示名を、見つけた順に 1 回ずつ返す")
{
	const std::vector<CollectionFile> files = {
		{"a.json", "Stream"}, {"b.json", "Stream"}, {"c.json", ""},        {"d.json", "c"},
		{"e.json", "Solo"},   {"f.json", "Stream"}, {"a.json.bak", "Solo"},
	};
	CHECK(duplicate_collection_names(files) == std::vector<std::string>{"Stream", "c"});
	CHECK(duplicate_collection_names({}).empty());
}

TEST_CASE("現在のシーンコレクション、ほかのシーンコレクション、元のシーンコレクション (もう一度消す)、保存の順に実行する")
{
	FakeObs obs;
	const UninstallStepResult r = walk_collections(obs.ops(), kItems);
	CHECK(obs.events == std::vector<std::string>{"remove@B", "open A", "remove@A", "open C", "remove@C", "open B",
						     "remove@B", "save"});
	CHECK(r.leftovers.empty());
	CHECK(r.collections_not_opened.empty());
	CHECK(r.collections_duplicate_names.empty());
	CHECK(obs.current == "B"); // 利用者は元のシーンコレクションに戻っている
}

TEST_CASE("シーンコレクションが 1 つだけなら、切り替えずに 1 回消して保存する")
{
	FakeObs obs;
	obs.listed = {"B"};
	walk_collections(obs.ops(), kItems);
	CHECK(obs.events == std::vector<std::string>{"remove@B", "save"});
}

TEST_CASE("開けないシーンコレクションは結果に入れ、ほかを続けて開く")
{
	FakeObs obs;
	obs.unopenable = {"A"};
	const UninstallStepResult r = walk_collections(obs.ops(), kItems);
	CHECK(obs.events == std::vector<std::string>{"remove@B", "open A", "open C", "remove@C", "open B", "remove@B", "save"});
	CHECK(r.collections_not_opened == std::vector<std::string>{"A"});
	CHECK(r.leftovers.empty());
}

TEST_CASE("元のシーンコレクションに戻れなければ結果に入れ、最初に消したときの数を使う")
{
	FakeObs obs;
	obs.unopenable = {"B"};
	obs.remaining["B"] = {{RemovalItem::CordmuxSources, 1}, {RemovalItem::ModuleConfigFiles, 0}};
	const UninstallStepResult r = walk_collections(obs.ops(), kItems);
	CHECK(r.collections_not_opened == std::vector<std::string>{"B"});
	CHECK(has_leftover(r, RemovalItem::CordmuxSources, "B"));
	CHECK(obs.events.back() == "save");
}

TEST_CASE("消した後に残ったものを、シーンコレクションと種類ごとに結果に入れる")
{
	FakeObs obs;
	obs.remaining["A"] = {{RemovalItem::CordmuxSources, 2}, {RemovalItem::ModuleConfigFiles, 0}};
	obs.remaining["C"] = {{RemovalItem::CordmuxSources, 0}, {RemovalItem::ModuleConfigFiles, 1}};
	const UninstallStepResult r = walk_collections(obs.ops(), kItems);
	REQUIRE(r.leftovers.size() == 2);
	CHECK(has_leftover(r, RemovalItem::CordmuxSources, "A"));
	CHECK(has_leftover(r, RemovalItem::ModuleConfigFiles, "C"));
}

TEST_CASE("元のシーンコレクションの結果は、2 回目に消したときの数で決まる (切り替えたときに作り直されたものも拾う)")
{
	FakeObs obs;
	int removes_at_b = 0;
	CollectionWalkOps ops = obs.ops();
	ops.remove_here = [&obs, &removes_at_b](const std::vector<RemovalItem> &items) {
		obs.events.push_back("remove@" + obs.current);
		if (obs.current != "B")
			return zero_counts(items);
		// 1 回目は残りがあり、戻った後の 2 回目で消えた。
		++removes_at_b;
		std::vector<RemovalCount> counts = zero_counts(items);
		if (removes_at_b == 1)
			counts[0].remaining = 3;
		return counts;
	};
	const UninstallStepResult r = walk_collections(ops, kItems);
	CHECK(removes_at_b == 2);
	CHECK(r.leftovers.empty());
}

TEST_CASE("シーンコレクションのフォルダーで見つけた重複した表示名を、結果に入れる")
{
	FakeObs obs;
	obs.files = {{"x.json", "Twin"}, {"y.json", "Twin"}};
	const UninstallStepResult r = walk_collections(obs.ops(), kItems);
	CHECK(r.collections_duplicate_names == std::vector<std::string>{"Twin"});
	CHECK(r.leftovers.empty());
}

TEST_CASE("現在のシーンコレクションの名前が分からなければ、そこでだけ消し、ほかは開かずに結果に入れる")
{
	FakeObs obs;
	obs.current = "";
	const UninstallStepResult r = walk_collections(obs.ops(), kItems);
	CHECK(obs.events == std::vector<std::string>{"remove@", "save"});
	CHECK(r.collections_not_opened == std::vector<std::string>{"A", "B", "C"});
}

TEST_CASE("数が返らなかった種類は、残っているものとして結果に入れる")
{
	const std::vector<CollectionVisit> visits = {{"A", {{RemovalItem::CordmuxSources, 0}}}};
	const UninstallStepResult r = summarize_collection_walk(visits, kItems, {}, {});
	CHECK_FALSE(has_leftover(r, RemovalItem::CordmuxSources, "A"));
	CHECK(has_leftover(r, RemovalItem::ModuleConfigFiles, "A"));
}

TEST_CASE("消す関数がなければ、現在のシーンコレクションのすべての種類を残りにする")
{
	FakeObs obs;
	obs.listed = {"B"};
	CollectionWalkOps ops = obs.ops();
	ops.remove_here = nullptr;
	const UninstallStepResult r = walk_collections(ops, kItems);
	CHECK(r.leftovers.size() == 2);
}

TEST_CASE("シーンコレクションのフォルダーを読めなければ、確認できなかったものとして結果に入れ、シーンコレクションは開き続ける")
{
	FakeObs obs;
	obs.folder_unreadable = true;
	const std::vector<RemovalItem> items = {RemovalItem::CordmuxSources};
	const UninstallStepResult r = walk_collections(obs.ops(), items);
	CHECK(r.collection_folder_unreadable);
	CHECK(r.collections_duplicate_names.empty());
	CHECK(r.collections_not_opened.empty());
	// 止める理由にはならない (残りはなく、ほかのシーンコレクションも開いて消す)。
	CHECK(r.leftovers.empty());
	CHECK(obs.events.size() > 1);
}

TEST_CASE("シーンコレクションのフォルダーを読めたときは、確認できなかったものに入れない。ファイルが 1 つもなくても同じ")
{
	FakeObs obs;
	const std::vector<RemovalItem> items = {RemovalItem::CordmuxSources};
	CHECK_FALSE(walk_collections(obs.ops(), items).collection_folder_unreadable);
	obs.files = {{"a.json", "A"}};
	CHECK_FALSE(walk_collections(obs.ops(), items).collection_folder_unreadable);
}

TEST_CASE("シーンコレクションのフォルダーを読む関数がなければ、確認できなかったものとして結果に入れる")
{
	FakeObs obs;
	CollectionWalkOps ops = obs.ops();
	ops.list_collection_files = nullptr;
	const std::vector<RemovalItem> items = {RemovalItem::CordmuxSources};
	CHECK(walk_collections(ops, items).collection_folder_unreadable);
}
