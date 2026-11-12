// log_lines.py の NG 例。ws の下のログの行 (ws::LogSink に書く行。log_lines.py の WS_CALL_RE)。plugin-main.cpp の
// cordmux_log_sink を通って「[cordmux] 」を付けて OBS のログに出力されるので、diag::logf と同じようにチェックする。
// 行の末尾のコメントの expect: の後ろは、その行で検出されるはずのチェック項目である。
struct Impl {
	void log(const std::string &line); // 宣言は文字列リテラルがないので、何も検出しない
	void on_close() { log("close: 接続が切れた"); } // expect: ascii | lowercase-start
	void on_hello() { impl_->log("Connected to Vesktop (AC-66)"); } // expect: design-number
	void on_send() { this->log("cordmux couldn't send a message"); } // expect: cordmux-start
	void on_limits() { log("Quality limits not sent."); } // expect: period-end
	void others()
	{
		cfg.log(line);               // 文字列リテラルがない
		owner->on_log(message);      // on_log は別の名前
		const auto m = parse_log(j); // parse_log も別の名前
		const double v = std::log(2.0);
	}
};
