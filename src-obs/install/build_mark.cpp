// SPDX-License-Identifier: Apache-2.0

// install/build_mark.h の実装。

#include "install/build_mark.h"

#include <cstddef>
#include <utility>

namespace cordmux::install {

namespace {

// 半角の空白とタブだけを除く (ヘッダーの、書式のコメントにある読み方)。
std::string_view trim_blanks(std::string_view s)
{
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
		s.remove_prefix(1);
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
		s.remove_suffix(1);
	return s;
}

constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

} // namespace

std::optional<BuildMark> parse_build_mark(std::string_view text)
{
	if (text.substr(0, kUtf8Bom.size()) == kUtf8Bom)
		text.remove_prefix(kUtf8Bom.size());

	std::optional<std::string> build_id;
	std::optional<std::string> vencord_commit;
	std::optional<std::string> vencord_tag;

	while (!text.empty()) {
		const std::size_t end = text.find('\n');
		std::string_view line = text.substr(0, end);
		text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);

		if (!line.empty() && line.back() == '\r')
			line.remove_suffix(1);

		const std::size_t colon = line.find(':');
		if (colon == std::string_view::npos)
			continue; // キーの行ではない (空の行も含む)

		const std::string_view key = trim_blanks(line.substr(0, colon));
		const std::string_view value = trim_blanks(line.substr(colon + 1));

		std::optional<std::string> *slot = nullptr;
		if (key == kBuildMarkKeyBuildId)
			slot = &build_id;
		else if (key == kBuildMarkKeyVencordCommit)
			slot = &vencord_commit;
		else if (key == kBuildMarkKeyVencordTag)
			slot = &vencord_tag;
		if (slot == nullptr)
			continue; // 知らないキーは無視する

		if (slot->has_value())
			return std::nullopt; // 同じキーが 2 回ある (ヘッダーのコメント)
		*slot = std::string(value);
	}

	if (!build_id.has_value() || build_id->empty() || !vencord_commit.has_value() || vencord_commit->empty() ||
	    !vencord_tag.has_value() || vencord_tag->empty())
		return std::nullopt;

	BuildMark mark;
	mark.build_id = std::move(*build_id);
	mark.vencord_commit = std::move(*vencord_commit);
	mark.vencord_tag = std::move(*vencord_tag);
	return mark;
}

std::string build_mark_to_text(const BuildMark &mark)
{
	std::string out;
	out += kBuildMarkKeyBuildId;
	out += ": ";
	out += mark.build_id;
	out += '\n';
	out += kBuildMarkKeyVencordCommit;
	out += ": ";
	out += mark.vencord_commit;
	out += '\n';
	out += kBuildMarkKeyVencordTag;
	out += ": ";
	out += mark.vencord_tag;
	out += '\n';
	return out;
}

ShownBundleVerdict classify_shown_bundle(const std::optional<BuildMark> &original,
					 const std::optional<BuildMark> &shown, std::optional<bool> hashes_match)
{
	// data/vencord-src のマーカーが読めないか、比べられなかったので判定しない (ヘッダーの表の上の 2 行)。
	if (!original.has_value() || !hashes_match.has_value())
		return ShownBundleVerdict::Unknown;

	// data/vencord に cordmux の別のバージョンのマーカーがあるので、前のバージョンのファイルが残っている。
	if (shown.has_value() && *shown != *original)
		return ShownBundleVerdict::StaleOwnBundle;

	// マーカーが同じか、data/vencord にマーカーがないので、ハッシュで決める。
	return *hashes_match ? ShownBundleVerdict::Matches : ShownBundleVerdict::Overwritten;
}

} // namespace cordmux::install
