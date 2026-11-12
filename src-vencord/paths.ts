// SPDX-License-Identifier: Apache-2.0

// 送信側で Windows に依存する処理を、このファイルにだけ置く。ほかの TypeScript のファイルには、Windows の名前
// (process.platform、"win32"、LOCALAPPDATA など) を書かない。test/lint/windows_api_confinement.py がチェックする。

import { join } from "node:path";

const WIN32 = "win32";

/** Windows で動いているときに true を返す。cordmux は Windows だけに対応する。 */
export function isWindows(): boolean {
    return process.platform === WIN32;
}

/**
 * トークンファイル (session.json) を置くフォルダー (%LOCALAPPDATA%\cordmux) を返す。
 * Windows でないときと、環境変数 LOCALAPPDATA がないか空のときは undefined を返す。
 */
export function getHandshakeDir(): string | undefined {
    if (!isWindows()) return undefined;

    const localAppData = process.env.LOCALAPPDATA;
    if (!localAppData) return undefined;

    return join(localAppData, "cordmux");
}
