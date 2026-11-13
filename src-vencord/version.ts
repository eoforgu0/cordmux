// SPDX-License-Identifier: Apache-2.0

// 送信側の cordmux のバージョン (CORDMUX_BUILD_ID) を置く。CI は配布用の zip を作るとき、Vencord にコピーした
// このファイルの CORDMUX_BUILD_ID の行だけを <version>+<短いコミット> に書き換える
// (test/tools/dist-zip/build_info.py の version-ts)。リポジトリのこのファイルには開発用の値 <version>+dev だけを
// 置き、バージョンは src-obs/buildspec.json で変える。2 つの version が同じことは tests/version.test.ts が
// チェックする。

/**
 * 送信側のビルドの ID。hello の version に入れる。配布用の zip では、cordmux.dll と、同梱の Vencord の
 * cordmux-build.txt の build_id と同じ文字列になる。受信側はこれを比べて、Vesktop が古い cordmux のまま
 * 動いていないかを確認する。テスト用の送信側 (test/tools/harness/sender.ts) も、hello の version にこの値を入れる。
 * build_info.py は export const CORDMUX_BUILD_ID = "..."; の形の行を探して書き換えるので、この行の形を変えない。
 */
export const CORDMUX_BUILD_ID = "1.0.3+dev";
