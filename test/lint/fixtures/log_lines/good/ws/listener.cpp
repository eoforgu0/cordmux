// log_lines.py の OK 例。ws の下の、正しい形のログの行 (ws::LogSink に書く行)。何も検出しない。
struct Impl {
	void log(const std::string &line);
	void on_close() { log("Disconnected from Vesktop"); }
	void on_hello() { impl_->log("Connected to Vesktop"); }
	void on_send() { this->log("Couldn't send a message to Vesktop"); }
	void others()
	{
		cfg.log(line);
		owner->on_log(message);
		const auto m = parse_log(j);
		const double v = std::log(2.0);
	}
};
