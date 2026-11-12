// log_lines.py の NG 例。OBS のログに書く行の形の誤り。呼び出しの行の末尾のコメントの expect: の後ろは、その行で
// 検出されるはずのチェック項目である (--selftest が読む)。
void log_examples()
{
	diag::logf(LOG_WARNING, "保存先が見つからない"); // expect: ascii
	diag::logf(LOG_INFO, "Connected (AC-66)"); // expect: design-number
	diag::logf(LOG_INFO, "cordmux is disabled"); // expect: cordmux-start
	blog(LOG_INFO, "[cordmux] cordmux loaded"); // expect: cordmux-start
	diag::logf(LOG_INFO, "vesktop connected"); // expect: lowercase-start
	diag::logf(LOG_INFO, "Vesktop connected."); // expect: period-end
	diag::logf(LOG_INFO, "Vesktop connected.\n"); // expect: period-end
	diag::logf(LOG_WARNING, // expect: ascii | design-number
		   "Couldn't open %s "
		   "(R-12)", name, u8"名前");
	blog(LOG_WARNING, "[cordmux] ログにトークンを含む行があった"); // expect: ascii
}
