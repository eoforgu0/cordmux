// SPDX-License-Identifier: Apache-2.0

// bundle/export_bundle.h の leave_out_revealing_entries の単体テスト (Qt を使わない)。トークンを検出したファイルだけを
// 1 行の説明に置き換え、ほかのファイルはそのまま残すことを確認する。

#include "doctest.h"

#include "bundle/export_bundle.h"
#include "ws/log_sink.h"

#include <string>
#include <vector>

using cordmux::bundle::build_bundle_entries;
using cordmux::bundle::BundleMaterials;
using cordmux::bundle::bundle_reveals_secret;
using cordmux::bundle::kDiscordLogName;
using cordmux::bundle::kPluginLogName;
using cordmux::bundle::leave_out_revealing_entries;
using cordmux::bundle::left_out_note;
using cordmux::bundle::RedactFn;
using cordmux::bundle::RevealFn;
using cordmux::bundle::ZipEntry;

namespace {

// テストだけで使うトークン (実際のトークンと同じ 16 進の 64 文字の形)。
const std::string kSecret = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

RevealFn make_reveal()
{
	return [](std::string_view text) { return cordmux::ws::text_reveals_secret_or_port(text, kSecret, 0); };
}

const ZipEntry &entry_named(const std::vector<ZipEntry> &entries, const std::string &name)
{
	for (const ZipEntry &entry : entries) {
		if (entry.name == name)
			return entry;
	}
	REQUIRE_MESSAGE(false, "その名前の中身が zip にない");
	return entries.front();
}

BundleMaterials materials_with_secret_in_plugin_log()
{
	BundleMaterials materials;
	materials.plugin_log = {"listener opened", "hello secret=" + kSecret};
	materials.discord_log = {"connected"};
	materials.settings_json = "{}";
	return materials;
}

} // namespace

TEST_CASE("トークンが残ったファイルだけを 1 行の説明に置き換え、ほかのファイルはそのまま残す")
{
	// 伏せる関数を何もしない関数にして、トークンが plugin.log に残った状態を作る (tests/test_bundle_files.cpp と同じ作り方)。
	const RedactFn identity = [](const std::string &text) { return text; };
	std::vector<ZipEntry> entries = build_bundle_entries(materials_with_secret_in_plugin_log(), identity);
	const std::vector<ZipEntry> before = entries;
	REQUIRE(bundle_reveals_secret(entries, make_reveal()));

	const std::vector<std::string> left_out = leave_out_revealing_entries(entries, make_reveal());

	CHECK(left_out == std::vector<std::string>{kPluginLogName});
	CHECK(entry_named(entries, kPluginLogName).content == left_out_note(kPluginLogName));
	CHECK(entry_named(entries, kPluginLogName).content.find(kSecret) == std::string::npos);
	// ほかのファイルの中身は 1 バイトも変えない。
	CHECK(entry_named(entries, kDiscordLogName).content == entry_named(before, kDiscordLogName).content);
	CHECK(entries.size() == before.size());
	// 置き換えた後の全文では検出されない (prepare_bundle は、この状態なら書き出してよい)。
	CHECK_FALSE(bundle_reveals_secret(entries, make_reveal()));
}

TEST_CASE("トークンを検出しなければ、leave_out_revealing_entries は何も置き換えない")
{
	BundleMaterials materials = materials_with_secret_in_plugin_log();
	materials.plugin_log = {"listener opened"};
	std::vector<ZipEntry> entries = build_bundle_entries(materials, RedactFn{});
	const std::vector<ZipEntry> before = entries;

	CHECK(leave_out_revealing_entries(entries, make_reveal()).empty());
	for (std::size_t i = 0; i < entries.size(); ++i)
		CHECK(entries[i].content == before[i].content);
}

TEST_CASE("確認する関数がなければ、leave_out_revealing_entries は何も置き換えない")
{
	const RedactFn identity = [](const std::string &text) { return text; };
	std::vector<ZipEntry> entries = build_bundle_entries(materials_with_secret_in_plugin_log(), identity);
	CHECK(leave_out_revealing_entries(entries, RevealFn{}).empty());
}

TEST_CASE("left_out_note は、ファイルの名前で始まる英語の 1 行で、トークンを含まない")
{
	const std::string note = left_out_note(kDiscordLogName);
	CHECK(note.rfind(kDiscordLogName, 0) == 0);
	CHECK(note.back() == '\n');
	CHECK(note.find('\n') == note.size() - 1);
	CHECK_FALSE(make_reveal()(note));
}
