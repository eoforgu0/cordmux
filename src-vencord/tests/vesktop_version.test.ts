// SPDX-License-Identifier: Apache-2.0

// vesktop_version.ts のテスト。VesktopNative.app.getVersion() からの読み方と、transport/client.ts が hello に vesktop の
// キーを入れるか省くかと、transport/control.ts のデコードを確認する。index.ts は Vencord を import するので vitest
// から読み込めず、つなぎ方はソースの文字列で確認する (tests/version.test.ts と同じ)。

import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";

import { type ClientConfig, type ClientState, transition } from "../transport/client";
import { decodeControlMessage } from "../transport/control";
import { readVesktopVersion } from "../vesktop_version";

const here = dirname(fileURLToPath(import.meta.url));

/** Vesktop のレンダラーから見える形 (preload が contextBridge で公開する VesktopNative) を作る。 */
function vesktopScope(getVersion: unknown): unknown {
    return { VesktopNative: { app: { getVersion } } };
}

describe("readVesktopVersion: Vesktop の API からバージョンを読む", () => {
    it("VesktopNative.app.getVersion() が返すバージョンをそのまま返す", () => {
        expect(readVesktopVersion(vesktopScope(() => "1.6.7"))).toBe("1.6.7");
    });

    it("semver のプレリリースとビルドメタデータ (- と +) を含むバージョンも受け取る", () => {
        expect(readVesktopVersion(vesktopScope(() => "1.6.0-beta.2+build.5"))).toBe("1.6.0-beta.2+build.5");
    });

    it("前後の空白は取り除く", () => {
        expect(readVesktopVersion(vesktopScope(() => "  1.6.7 \n"))).toBe("1.6.7");
    });

    it("getVersion を app から呼ぶ (this が app になる)", () => {
        const app = {
            version: "1.5.8",
            getVersion(this: { version: string }) {
                return this.version;
            },
        };
        expect(readVesktopVersion({ VesktopNative: { app } })).toBe("1.5.8");
    });
});

describe("readVesktopVersion: 読めないときは undefined を返す (推測の値で埋めない)", () => {
    it("Vesktop の外 (VesktopNative がない) なら undefined を返す", () => {
        expect(readVesktopVersion({})).toBeUndefined();
        expect(readVesktopVersion({ VesktopNative: {} })).toBeUndefined();
        expect(readVesktopVersion({ VesktopNative: { app: {} } })).toBeUndefined();
    });

    it("渡された scope がオブジェクトでなければ undefined を返す", () => {
        expect(readVesktopVersion(undefined)).toBeUndefined();
        expect(readVesktopVersion(null)).toBeUndefined();
        expect(readVesktopVersion("1.6.7")).toBeUndefined();
        expect(readVesktopVersion({ VesktopNative: null })).toBeUndefined();
    });

    it("getVersion が関数でなければ undefined を返す", () => {
        expect(readVesktopVersion(vesktopScope("1.6.7"))).toBeUndefined();
    });

    it("getVersion が例外を投げても、外には投げない", () => {
        expect(
            readVesktopVersion(
                vesktopScope(() => {
                    throw new Error("ipc failed");
                }),
            ),
        ).toBeUndefined();
    });

    it("VesktopNative を読んだだけで例外が起きても、外には投げない", () => {
        const scope = {
            get VesktopNative(): unknown {
                throw new Error("getter failed");
            },
        };
        expect(readVesktopVersion(scope)).toBeUndefined();
    });

    it("文字列でない値 (Promise や数値) は受け取らない", () => {
        expect(readVesktopVersion(vesktopScope(() => Promise.resolve("1.6.7")))).toBeUndefined();
        expect(readVesktopVersion(vesktopScope(() => 167))).toBeUndefined();
        expect(readVesktopVersion(vesktopScope(() => undefined))).toBeUndefined();
    });

    it("空の文字列、空白だけの文字列、使える文字の範囲の外 (空白や制御文字を含む) は受け取らない", () => {
        expect(readVesktopVersion(vesktopScope(() => ""))).toBeUndefined();
        expect(readVesktopVersion(vesktopScope(() => "   "))).toBeUndefined();
        expect(readVesktopVersion(vesktopScope(() => "1.6.7 beta"))).toBeUndefined();
        expect(readVesktopVersion(vesktopScope(() => `1.6.7${String.fromCharCode(7)}`))).toBeUndefined();
        expect(readVesktopVersion(vesktopScope(() => "1.6.7/../x"))).toBeUndefined();
    });

    it("64 字までは受け取り、65 字からは切り詰めずに捨てる", () => {
        const at64 = `1.${"0".repeat(62)}`;
        const at65 = `1.${"0".repeat(63)}`;
        expect(at64).toHaveLength(64);
        expect(readVesktopVersion(vesktopScope(() => at64))).toBe(at64);
        expect(readVesktopVersion(vesktopScope(() => at65))).toBeUndefined();
    });
});

describe("hello の省略できるキー vesktop", () => {
    const CONNECTING: ClientState = { phase: "connecting", failureCount: 0 };
    const SESSION = { secret: "test-secret-not-real" };

    function sentHelloText(config: ClientConfig): string {
        const result = transition(config, CONNECTING, { kind: "connected", session: SESSION });
        const send = result.effects.find((effect) => effect.kind === "send");
        if (!send || send.kind !== "send") throw new Error("send の効果がない (テストの誤り)");
        return send.text;
    }

    it("バージョンがあれば hello に vesktop として入り、デコードしても残る", () => {
        const text = sentHelloText({ version: "0.1.0+dev", wireVersion: 0, vesktopVersion: "1.6.7" });
        expect(JSON.parse(text)).toEqual({
            type: "hello",
            version: "0.1.0+dev",
            secret: SESSION.secret,
            wire: 0,
            vesktop: "1.6.7",
        });
        expect(decodeControlMessage(text)).toEqual({
            type: "hello",
            version: "0.1.0+dev",
            secret: SESSION.secret,
            wire: 0,
            vesktop: "1.6.7",
        });
    });

    it("バージョンがなければキーを入れない (空の文字列も null も入れず、vesktop のない hello と同じ形になる)", () => {
        const text = sentHelloText({ version: "0.1.0+dev", wireVersion: 0 });
        const parsed = JSON.parse(text) as Record<string, unknown>;
        expect(Object.keys(parsed).sort()).toEqual(["secret", "type", "version", "wire"]);

        const explicitUndefined = sentHelloText({ version: "0.1.0+dev", wireVersion: 0, vesktopVersion: undefined });
        expect(Object.keys(JSON.parse(explicitUndefined) as Record<string, unknown>)).not.toContain("vesktop");
    });

    it("vesktop が文字列でない hello は、そのキーだけを捨てて、hello は受け取る", () => {
        const decoded = decodeControlMessage(
            JSON.stringify({ type: "hello", version: "0.1.0", secret: "s", wire: 0, vesktop: 167 }),
        );
        expect(decoded).toEqual({ type: "hello", version: "0.1.0", secret: "s", wire: 0 });
        expect(decoded && "vesktop" in decoded).toBe(false);
    });
});

describe("index.ts のつなぎ方", () => {
    it("socket の hello に readVesktopVersion(globalThis) の結果を渡している", () => {
        // index.ts は Vencord を import するので、vitest から読み込めない。ソースの文字列で確認する。
        const source = readFileSync(join(here, "../index.ts"), "utf8");
        expect(source).toContain('import { readVesktopVersion } from "./vesktop_version";');
        expect(source).toContain("vesktopVersion: readVesktopVersion(globalThis),");
    });
});
