// log_lines.py の OK 例。正しい形のログの行。コメントの中 (diag::logf(LOG_INFO, "日本語")) は読まない。
void logf(int level, const char *format, ...);

void log_examples()
{
	diag::logf(LOG_INFO, "Version %s loaded", kPluginVersion);
	diag::logf(LOG_INFO, "Vesktop connected (cordmux in Vesktop %s)", version);
	diag::logf(LOG_WARNING, "Couldn't export diagnostics: the folder can't be written to");
	diag::logf(LOG_INFO, "Finalizing the cordmux recording...");
	blog(LOG_INFO, "[cordmux] Version 1.2.0 loaded");
	blog(level, "%s%.*s", prefix, int(line.size()), line.data());
	diag::logf(LOG_INFO, "Rearranged slots (%d people)", count);
	const char *not_a_log = "日本語の文字列はログの外ならチェックしない";
	dialog(LOG_INFO, "関数の名前が logf で終わらない呼び出しは見ない");
}
