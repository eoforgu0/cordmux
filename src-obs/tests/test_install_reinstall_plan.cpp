// SPDX-License-Identifier: Apache-2.0

// install/reinstall_plan.h ([再インストール] でコピーする手順) の単体テスト。
// コピーする関数 (platform::copy_file_replace) は platform/ のものなので、ここでは計画のとおりに
// std::filesystem::copy_file でコピーし、計画が正しい順と場所を返すことだけを確認する (一時ファイルに書いてから
// 置き換える書き方のテストは platform/ の側にある)。一時フォルダーは std::filesystem::temp_directory_path の下に
// 作り、RAII で消す (test_install_vencord_dir.cpp の TempStateFile と同じ)。

#include "doctest.h"

#include "install/build_mark.h"
#include "install/reinstall_plan.h"
#include "platform/bundle_check.h"
#include "platform/sha256.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

using cordmux::install::BuildMark;
using cordmux::install::build_mark_to_text;
using cordmux::install::classify_shown_bundle;
using cordmux::install::CopyStep;
using cordmux::install::kBuildMarkFileName;
using cordmux::install::parse_build_mark;
using cordmux::install::plan_reinstall;
using cordmux::install::ShownBundleVerdict;

namespace {

// data/vencord-src にある名前 (Vencord の 5 つのファイル、.LEGAL.txt、マーカーのファイル)。list_directory が返す
// 名前の順に並べてある (マーカーは名前の順では先頭に来る)。
const std::vector<std::string> kOriginalNames = {
	"cordmux-build.txt",
	"package.json",
	"vencordDesktopMain.js",
	"vencordDesktopPreload.js",
	"vencordDesktopRenderer.css",
	"vencordDesktopRenderer.js",
	"vencordDesktopRenderer.js.LEGAL.txt",
};

class TempBundle {
public:
	TempBundle()
	{
		static std::atomic<int> counter{0};
		root_ = std::filesystem::temp_directory_path() /
			("cordmux_reinstall_test_" + std::to_string(++counter) + "_" +
			 std::to_string(reinterpret_cast<std::uintptr_t>(this)));
		std::filesystem::create_directories(original());
		std::filesystem::create_directories(shown());
	}

	~TempBundle()
	{
		std::error_code ec;
		std::filesystem::remove_all(root_, ec);
	}

	TempBundle(const TempBundle &) = delete;
	TempBundle &operator=(const TempBundle &) = delete;

	std::filesystem::path original() const { return root_ / "vencord-src"; }
	std::filesystem::path shown() const { return root_ / "vencord"; }

	static void write(const std::filesystem::path &path, const std::string &text)
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << text;
	}

	static std::string read(const std::filesystem::path &path)
	{
		std::ifstream in(path, std::ios::binary);
		std::ostringstream buffer;
		buffer << in.rdbuf();
		return buffer.str();
	}

	// Vesktop の設定を調べる側 (platform/os_probe.h の probe_once) と同じ読み方で、4 つのファイルのハッシュを取る。
	static std::vector<cordmux::platform::FileDigest> digests(const std::filesystem::path &dir)
	{
		std::vector<cordmux::platform::FileDigest> out;
		for (const char *name : cordmux::platform::kVencordBundleFiles) {
			cordmux::platform::FileDigest d;
			d.name = name;
			const std::filesystem::path path = dir / name;
			if (std::filesystem::exists(path)) {
				d.found = true;
				d.sha256_hex = cordmux::platform::sha256_hex(read(path));
			}
			out.push_back(d);
		}
		return out;
	}

	cordmux::platform::BundleComparison compare() const
	{
		return cordmux::platform::compare_bundle(digests(original()), digests(shown()));
	}

	std::optional<BuildMark> mark_of(const std::filesystem::path &dir) const
	{
		const std::filesystem::path path = dir / kBuildMarkFileName;
		if (!std::filesystem::exists(path))
			return std::nullopt;
		return parse_build_mark(read(path));
	}

	// 計画のとおりにコピーする (count を渡すと、先頭から count 個だけコピーして、途中で止まったときを再現する)。
	static void run(const std::vector<CopyStep> &steps, std::size_t count)
	{
		for (std::size_t i = 0; i < steps.size() && i < count; ++i) {
			std::filesystem::copy_file(std::filesystem::u8path(steps[i].from), std::filesystem::u8path(steps[i].to),
						   std::filesystem::copy_options::overwrite_existing);
		}
	}

	std::vector<CopyStep> plan() const
	{
		return plan_reinstall(original().u8string(), shown().u8string(), kOriginalNames);
	}

	std::optional<bool> hashes_match() const
	{
		const auto c = compare();
		if (c.state == cordmux::platform::BundleState::Unknown)
			return std::nullopt;
		return c.state == cordmux::platform::BundleState::Matches;
	}

private:
	std::filesystem::path root_;
};

const BuildMark kNew{"0.2.0+abc1234", "0123456789abcdef0123456789abcdef01234567", "v1.15.9"};
const BuildMark kOld{"0.1.0+fedcba9", "0123456789abcdef0123456789abcdef01234567", "v1.15.9"};

// data/vencord-src に "new <名前>"、data/vencord に "<shown_version> <名前>" という中身のファイルを置く
// (data/vencord のマーカーは shown_mark で、nullopt なら置かない)。
void lay_out(const TempBundle &bundle, const std::optional<BuildMark> &shown_mark, const char *shown_version)
{
	for (const std::string &name : kOriginalNames) {
		if (name == kBuildMarkFileName)
			continue;
		TempBundle::write(bundle.original() / name, "new " + name);
		TempBundle::write(bundle.shown() / name, std::string(shown_version) + " " + name);
	}
	TempBundle::write(bundle.original() / kBuildMarkFileName, build_mark_to_text(kNew));
	if (shown_mark.has_value())
		TempBundle::write(bundle.shown() / kBuildMarkFileName, build_mark_to_text(*shown_mark));
}

} // namespace

// ---- 手順の並び (ヘッダーのコメント) ----

TEST_CASE("data/vencord-src のファイルを 1 つずつ並べ、マーカーは最後に、それ以外は渡された順に置く")
{
	const std::vector<CopyStep> steps = plan_reinstall("C:/x/data/vencord-src", "C:/x/data/vencord", kOriginalNames);
	REQUIRE(steps.size() == kOriginalNames.size());
	CHECK(steps.back().name == kBuildMarkFileName);
	for (std::size_t i = 0; i + 1 < steps.size(); ++i) {
		CAPTURE(i);
		CHECK(steps[i].name == kOriginalNames[i + 1]); // 先頭のマーカーを除いた残りの順
		CHECK(steps[i].name != kBuildMarkFileName);
	}
	for (const CopyStep &step : steps) {
		CAPTURE(step.name);
		CHECK(step.from == "C:/x/data/vencord-src/" + step.name);
		CHECK(step.to == "C:/x/data/vencord/" + step.name);
	}
}

TEST_CASE("場所のつなぎ方は Vesktop の設定を調べる側と同じで、末尾の区切りの有無と向きによらない")
{
	const std::vector<std::string> names = {"vencordDesktopMain.js"};
	for (const char *dir : {"C:\\x\\data\\vencord-src\\", "C:\\x\\data\\vencord-src", "C:/x/data/vencord-src/"}) {
		CAPTURE(dir);
		const std::vector<CopyStep> steps = plan_reinstall(dir, "D:/y/", names);
		REQUIRE(steps.size() == 1);
		CHECK(steps[0].from == cordmux::platform::join_path(dir, "vencordDesktopMain.js"));
		CHECK(steps[0].to == "D:/y/vencordDesktopMain.js");
	}
}

TEST_CASE("data/vencord-src にマーカーがなければマーカーはコピーせず、空の一覧なら空の並びを返す")
{
	const std::vector<std::string> names = {"b.js", "a.js"};
	const std::vector<CopyStep> steps = plan_reinstall("o", "s", names);
	REQUIRE(steps.size() == 2);
	CHECK(steps[0].name == "b.js");
	CHECK(steps[1].name == "a.js");
	CHECK(plan_reinstall("o", "s", {}).empty());
}

// ---- 一時フォルダーで通して確認する ----

TEST_CASE("data/vencord の 1 つのファイルが上書きされると名前で分かり、計画のとおりにコピーすると一致に戻る")
{
	TempBundle bundle;
	lay_out(bundle, kNew, "new");
	// shown_version を "new" にしたので、data/vencord に data/vencord-src と同じ中身を置いた。
	REQUIRE(bundle.compare().state == cordmux::platform::BundleState::Matches);
	CHECK(classify_shown_bundle(bundle.mark_of(bundle.original()), bundle.mark_of(bundle.shown()),
				    bundle.hashes_match()) == ShownBundleVerdict::Matches);

	// Force Update の代わりに、1 つのファイルの末尾に 1 バイトを追加する。
	TempBundle::write(bundle.shown() / "vencordDesktopRenderer.js",
			  TempBundle::read(bundle.shown() / "vencordDesktopRenderer.js") + "x");

	const auto broken = bundle.compare();
	CHECK(broken.state == cordmux::platform::BundleState::Mismatch);
	CHECK(broken.differing == std::vector<std::string>{"vencordDesktopRenderer.js"}); // 違うファイルの名前
	CHECK(classify_shown_bundle(bundle.mark_of(bundle.original()), bundle.mark_of(bundle.shown()),
				    bundle.hashes_match()) == ShownBundleVerdict::Overwritten);

	const std::vector<CopyStep> steps = bundle.plan();
	TempBundle::run(steps, steps.size());

	CHECK(bundle.compare().state == cordmux::platform::BundleState::Matches);
	CHECK(classify_shown_bundle(bundle.mark_of(bundle.original()), bundle.mark_of(bundle.shown()),
				    bundle.hashes_match()) == ShownBundleVerdict::Matches);
	for (const std::string &name : kOriginalNames) {
		CAPTURE(name);
		CHECK(TempBundle::read(bundle.shown() / name) == TempBundle::read(bundle.original() / name));
	}
}

TEST_CASE("data/vencord が別の中身で、マーカーがなく 1 つのファイルが欠けていても、コピーすると一致に戻る")
{
	TempBundle bundle;
	lay_out(bundle, std::nullopt, "official");
	std::filesystem::remove(bundle.shown() / "vencordDesktopPreload.js");

	const auto broken = bundle.compare();
	CHECK(broken.missing == std::vector<std::string>{"vencordDesktopPreload.js"});
	CHECK(classify_shown_bundle(bundle.mark_of(bundle.original()), bundle.mark_of(bundle.shown()),
				    bundle.hashes_match()) == ShownBundleVerdict::Overwritten);

	const std::vector<CopyStep> steps = bundle.plan();
	TempBundle::run(steps, steps.size());
	CHECK(classify_shown_bundle(bundle.mark_of(bundle.original()), bundle.mark_of(bundle.shown()),
				    bundle.hashes_match()) == ShownBundleVerdict::Matches);
	CHECK(bundle.mark_of(bundle.shown()) == std::optional<BuildMark>(kNew));
}

TEST_CASE("前のバージョンのファイルは StaleOwnBundle になり、途中で止まってもマーカーが古いまま残るので、もう一度見つかる")
{
	TempBundle bundle;
	lay_out(bundle, kOld, "old"); // 展開が data/vencord の手前で止まった更新
	CHECK(classify_shown_bundle(bundle.mark_of(bundle.original()), bundle.mark_of(bundle.shown()),
				    bundle.hashes_match()) == ShownBundleVerdict::StaleOwnBundle);

	const std::vector<CopyStep> steps = bundle.plan();
	REQUIRE(steps.size() >= 2);

	// マーカーの手前で止まった (最後の 1 つだけコピーできなかった)。4 つのファイルはそろったが、マーカーは
	// 古いままなので、次に調べたときも前のバージョンのファイルと判定され、コピーし直す (マーカーを最後に置く理由)。
	TempBundle::run(steps, steps.size() - 1);
	CHECK(bundle.compare().state == cordmux::platform::BundleState::Matches);
	CHECK(bundle.mark_of(bundle.shown()) == std::optional<BuildMark>(kOld));
	CHECK(classify_shown_bundle(bundle.mark_of(bundle.original()), bundle.mark_of(bundle.shown()),
				    bundle.hashes_match()) == ShownBundleVerdict::StaleOwnBundle);

	// コピーし直すとマーカーまでそろい、一致になる。
	TempBundle::run(steps, steps.size());
	CHECK(classify_shown_bundle(bundle.mark_of(bundle.original()), bundle.mark_of(bundle.shown()),
				    bundle.hashes_match()) == ShownBundleVerdict::Matches);
}
