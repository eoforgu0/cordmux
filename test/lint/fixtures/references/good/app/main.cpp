// 架空の本体。コメントの中の存在するパス: app/main.h と app/sub/util.cpp。
#include "main.h"
#include <libfoo/foo.h>

static const char *kTable = "app/main.h";
static const char *kOut = "../escape.txt";

int tier_of() { return 1; }
