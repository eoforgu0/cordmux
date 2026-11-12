// removal_table.py の NG 例: 種類の列挙子に値を書いた。
// expect: 列挙子に値を書かない
// 表の始まりと終わりの目印は行頭にだけ置く。

namespace cordmux::install {

// 種類。1 行に 1 つ、値は書かない。
enum class RemovalItem {
	CordmuxSources = 0, // (a)
	ModuleConfigFiles, // (c)
	Recordings, // (g)
};

// 誰が消すか (3 つ)。
enum class Remover {
	Product,
	User,
	Nobody,
};

// 探し方。
enum class Locate {
	SourceTypeId,
	ModuleConfigDir,
	RecordDir,
};

// 段階 (値を書いてよい)。
enum class UninstallStep {
	MarkUninstalling = 0,
	SettingsAndSecret = 1,
	SourcesAndScenes = 2,
	NotRemoved = 9,
};

struct RemovalRow {
	RemovalItem item;
	Remover remover;
	Locate locate;
	UninstallStep step;
};

inline constexpr int kRemovalItemCount = 3;

// clang-format off
// removal-table-begin (表の始まり)
inline constexpr RemovalRow kRemovalTable[] = {
	{RemovalItem::CordmuxSources, Remover::Product, Locate::SourceTypeId, UninstallStep::SourcesAndScenes},
	// 行と行の間にコメントを書いてよい。
	{RemovalItem::ModuleConfigFiles, Remover::Product, Locate::ModuleConfigDir, UninstallStep::SettingsAndSecret},

	{RemovalItem::Recordings, Remover::Nobody, Locate::RecordDir, UninstallStep::NotRemoved}, // 行末にもコメントを書いてよい
};
// removal-table-end (表の終わり)
// clang-format on

} // namespace cordmux::install
