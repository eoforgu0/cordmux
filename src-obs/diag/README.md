# src-obs/diag

cordmux.dll がログに書く行の出力先。ws/ を除く cordmux.dll のコードは、ログの行をすべて `diag::logf` で書き、plugin-main.cpp が登録した出力先に渡す。出力先は、行にトークンとポート番号が含まれていないかをチェックしてから OBS のログに書き、診断情報の plugin.log になるリングバッファーにも積む。`blog()` を直接呼ぶと、その行はどちらも通らない。ログの行の書き方は CODESTYLE.md にある。

## 主な関数

- `diag::logf(level, format, ...)` (`diag_log.h`): 1 行を組み立てて出力先に渡す。`level` は libobs のログレベル (`LOG_INFO` など)。先頭の `[cordmux] ` は出力先が付けるので、書式文字列に含めない。
- `diag::set_log_sink(sink)` (`diag_log.h`): 出力先を登録する。呼び出すのは plugin-main.cpp だけで、`obs_module_load` の最初に登録し、`obs_module_unload` の最後に解除する。登録する前と解除した後に渡した行は捨てる。
- `cordmux_add_diag(<target>)` (`diag.cmake`): target に diag/ のソースファイルを追加する。

## 依存

libobs にも Qt にも依存しない。plugin-main.cpp と、dock/、install/、pipeline/、sources/ が呼び出す。ws/ は diag/ に依存せず、`ws::ListenerConfig::log` で書く。

テストは src-obs/tests/test_diag_log.cpp にある。
