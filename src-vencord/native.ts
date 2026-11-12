// SPDX-License-Identifier: Apache-2.0

// Vesktop のメインプロセスで動く部分。Vencord は userplugin の native.ts をメインプロセス用にバンドルし、export した
// 関数をレンダラーから VencordNative.pluginHelpers.cordmux 経由で呼び出せるようにする。プラグインを無効にしても
// このファイルは読み込まれたままで、ipcMain.handle の登録も残るので、状態も接続も持たせない。
// トークンは接続の認証に使う値なので、その値とファイルの中身を console にもエラーのメッセージにも出さない。

import { readFile } from "node:fs/promises";
import { join } from "node:path";
import type { IpcMainInvokeEvent } from "electron";

import { getHandshakeDir } from "./paths";

const SESSION_FILE_NAME = "session.json";

interface RawSessionFile {
    port?: unknown;
    secret?: unknown;
}

function isValidSession(value: RawSessionFile): value is { port: number; secret: string } {
    return (
        typeof value.port === "number" &&
        Number.isInteger(value.port) &&
        value.port > 0 &&
        typeof value.secret === "string" &&
        value.secret.length > 0
    );
}

/**
 * トークンファイル (session.json) を読み、port と secret を返す。ファイルの形は docs/wire_format.md の
 * 「トークンファイル」にある。cordmux.dll は起動のたびに secret を作り直し、port も変わりうるので、レンダラーの
 * transport/socket.ts は接続を試すたびにこの関数を呼び出す。起動のときに 1 回だけ読むと、OBS を起動し直した後に
 * 接続できなくなる。
 *
 * pid と started_at は cordmux.dll だけが使う値なので読まない。ファイルがない、読めない、JSON として読めない、
 * port か secret の型が違う、のどれでも null を返し、例外は投げない。呼び出し元は理由を区別せずに、接続できなかった
 * ものとして扱う。
 */
export async function readSession(_: IpcMainInvokeEvent): Promise<{ port: number; secret: string } | null> {
    const dir = getHandshakeDir();
    if (dir === undefined) return null; // Windows でないか、LOCALAPPDATA がない。

    let text: string;
    try {
        text = await readFile(join(dir, SESSION_FILE_NAME), "utf-8");
    } catch {
        return null; // ファイルがない (OBS がまだ起動していないときなど) か、権限がなくて読めない。
    }

    let parsed: unknown;
    try {
        parsed = JSON.parse(text);
    } catch {
        return null; // JSON として読めない (書き込みの途中に読んだときなど)。
    }

    if (typeof parsed !== "object" || parsed === null) return null;
    const raw = parsed as RawSessionFile;
    if (!isValidSession(raw)) return null;

    return { port: raw.port, secret: raw.secret };
}
