// 架空の本体。コメントの中の存在するパス: app/main.h と app/sub/util.cpp。
#include "main.h"
#include <libfoo/foo.h>

// C0 と C1 の制御文字は画面に表示しない。
// 機械語の並び (B8 xx xx xx xx C3) をコピーする。
// 依存関係の deps の 2024-05-01 のバージョンで確かめた。
// 架空の日付 2000-01-01 をテストデータに使う。
static const char *kSdp = "o=- 1 2 IN IP4 127.0.0.1";
static const char *kTable = "app/main.h";
static const char *kOut = "../escape.txt";
static const char *kPattern = "[A-Z0-9]+";

int tier_of() { return Tier::L1; }
