// SPDX-License-Identifier: Apache-2.0

// Vesktop のバージョンを読む。値は hello の vesktop に入れ、受信側は設定ウィンドウのバージョンの一覧と診断情報に
// 表示する。Discord の内部ではなく Vesktop が公開している API なので、依存箇所の一覧 (ledger.ts) には入れない。
// 読めなくても、バージョンの一覧で Vesktop の値が "-" になるだけで、ほかの動作は変わらない。
// Vencord を import しないので、リポジトリのルートの tsc と vitest でテストできる。グローバルオブジェクトは、
// 呼び出し元の index.ts が globalThis を渡す。

/**
 * バージョンとして受け取る文字列。semver 2.0.0 で使う文字 (英数字と "." "+" "-") の 64 字までにする。
 * バージョンは画面にそのまま表示するので、想定外の長さの文字列や制御文字を通さない。範囲の外の文字列は
 * 切り詰めずに捨てる。切り詰めると、別のバージョンに見えるためである。
 */
const VERSION_PATTERN = /^[0-9A-Za-z.+-]{1,64}$/;

/** オブジェクト (関数を含む) のプロパティを 1 つ読む。オブジェクトでなければ undefined を返す。 */
function fieldOf(value: unknown, key: string): unknown {
    if ((typeof value !== "object" && typeof value !== "function") || value === null) return undefined;
    return (value as Record<string, unknown>)[key];
}

/**
 * `scope.VesktopNative.app.getVersion()` を呼び出し、Vesktop のバージョンを返す。Vesktop の外で動いている
 * (VesktopNative がない)、呼び出しが例外を投げた、戻り値が文字列でない、VERSION_PATTERN に一致しない、のどれかの
 * ときは undefined を返す。推測の値で埋めずに、分からないままにする。例外は投げないので、バージョンが読めなくても
 * プラグインの起動は止まらない。
 *
 * VesktopNative は、Vesktop の preload が contextBridge.exposeInMainWorld でレンダラーに公開している。
 * getVersion() は同期の IPC で、メインプロセスの Electron の app.getVersion() (Vesktop の package.json の version)
 * を返す。Vencord の設定画面の「Vesktop v...」も、この呼び出しで表示している (Vencord の
 * src/plugins/_core/settings.tsx)。Vesktop v1.6.0 の src/preload/VesktopNative.ts と src/main/ipc.ts で確認した。
 * navigator.userAgent は使えない。Vesktop がレンダラーの UA を Chrome の UA に置き換えるので
 * (src/main/constants.ts の BrowserUserAgent)、Vesktop の名前もバージョンも入っていない。
 * native.ts で app.getVersion() を呼び出しても同じ値が取れるが、IPC の関数が 1 つ増え、start() で非同期に待つ
 * ことになる。
 *
 * 同期の IPC を 1 回使うので、呼び出し元はプラグインの起動ごとに 1 回だけ呼び出す (再接続のたびには呼び出さない)。
 */
export function readVesktopVersion(scope: unknown): string | undefined {
    try {
        const app = fieldOf(fieldOf(scope, "VesktopNative"), "app");
        const getVersion = fieldOf(app, "getVersion");
        if (typeof getVersion !== "function") return undefined;
        const value: unknown = getVersion.call(app);
        if (typeof value !== "string") return undefined;
        const trimmed = value.trim();
        return VERSION_PATTERN.test(trimmed) ? trimmed : undefined;
    } catch {
        return undefined;
    }
}
