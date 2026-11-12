// SPDX-License-Identifier: Apache-2.0

// vitest の設定 (npm test)。送信側の単体テスト (src-vencord/tests) と、テストツールの単体テスト (test/tools/obs-run、
// test/tools/harness、test/tools/composite-check の直下の *.test.ts) を実行する。テストは Node.js で実行するので、Vencord のモジュール
// (@utils/types など) を import するファイルは読み込めない。ブラウザーにしかない API は、テストの中で用意する
// (vi.stubGlobal など)。
import { defineConfig } from "vitest/config";

export default defineConfig({
    test: {
        include: [
            "src-vencord/tests/**/*.test.ts",
            "test/tools/obs-run/*.test.ts",
            "test/tools/harness/*.test.ts",
            "test/tools/composite-check/*.test.ts",
        ],
    },
});
