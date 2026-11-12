// 親のフォルダーの ../main.h を読む。
#include "../main.h"

// テストデータの架空のパスは文字列なので、./ と ../ をツリーと照合しない。
static const char *kRelative = "./rec";
