// SPDX-License-Identifier: Apache-2.0

// transport/control.ts のテスト。制御用の JSON のメッセージが往復することと、形が不正なメッセージ、知らない type、
// 省略できるキーの扱いを確認する。メッセージの種類とキーは docs/wire_format.md の「制御用の JSON」にある。

import { describe, expect, it } from "vitest";

import {
    decodeControlMessage,
    encodeControlMessage,
    readLimitsFields,
    validateControlMessage,
    type ConfigureStream,
    type ControlMessage,
} from "../transport/control";

function roundTrip(message: ControlMessage): ControlMessage | undefined {
    return decodeControlMessage(encodeControlMessage(message));
}

const SAMPLE_STREAM: ConfigureStream = {
    stream_id: 1,
    user_id: "123456789012345678",
    kind: "screen",
    width: 1920,
    height: 1080,
    fps: 60,
    audio: true,
};

describe("正しい形のメッセージが往復する", () => {
    it("hello が往復する", () => {
        const message: ControlMessage = { type: "hello", version: "0.1.0", secret: "deadbeef", wire: 0 };
        expect(roundTrip(message)).toEqual(message);
    });

    it("hello_ok が往復する (wire と version を送る)", () => {
        const message: ControlMessage = { type: "hello_ok", wire: 0, version: "0.1.0" };
        expect(roundTrip(message)).toEqual(message);
    });

    it("hello_ng が往復する", () => {
        const message: ControlMessage = { type: "hello_ng", reason: "secret mismatch" };
        expect(roundTrip(message)).toEqual(message);
    });

    it("ping と pong は seq を保つ", () => {
        expect(roundTrip({ type: "ping", seq: 42 })).toEqual({ type: "ping", seq: 42 });
        expect(roundTrip({ type: "pong", seq: 42 })).toEqual({ type: "pong", seq: 42 });
    });

    it("hello と pong の clock_us (送信側の時計) が往復し、0 以上の安全な整数でなければそのキーだけを捨てる", () => {
        const hello: ControlMessage = {
            type: "hello",
            version: "0.1.0",
            secret: "deadbeef",
            wire: 1,
            clock_us: 1_700_000_000_123_456,
        };
        expect(roundTrip(hello)).toEqual(hello);
        expect(roundTrip({ type: "pong", seq: 3, clock_us: 5 })).toEqual({ type: "pong", seq: 3, clock_us: 5 });
        expect(decodeControlMessage(JSON.stringify({ type: "pong", seq: 3, clock_us: -1 }))).toEqual({ type: "pong", seq: 3 });
        expect(decodeControlMessage(JSON.stringify({ type: "pong", seq: 3, clock_us: 1.5 }))).toEqual({ type: "pong", seq: 3 });
        expect(decodeControlMessage(JSON.stringify({ type: "pong", seq: 3, clock_us: 2 ** 60 }))).toEqual({ type: "pong", seq: 3 });
        expect(
            decodeControlMessage(
                JSON.stringify({ type: "hello", version: "0.1.0", secret: "s", wire: 1, clock_us: "1" }),
            ),
        ).toEqual({ type: "hello", version: "0.1.0", secret: "s", wire: 1 });
    });

    it("hello の clock_base は文字列なら往復し、文字列でなければそのキーだけを捨てる", () => {
        const hello: ControlMessage = { type: "hello", version: "0.1.0", secret: "s", wire: 1, clock_base: "qpc" };
        expect(roundTrip(hello)).toEqual(hello);
        expect(
            decodeControlMessage(JSON.stringify({ type: "hello", version: "0.1.0", secret: "s", wire: 1, clock_base: 1 })),
        ).toEqual({ type: "hello", version: "0.1.0", secret: "s", wire: 1 });
    });

    it("configure が往復する (voice のストリームにも width、height、fps を付けられる)", () => {
        const message: ControlMessage = {
            type: "configure",
            streams: [
                SAMPLE_STREAM,
                { ...SAMPLE_STREAM, stream_id: 2, kind: "voice", audio: true },
            ],
        };
        expect(roundTrip(message)).toEqual(message);
    });

    it("configure の width、height、fps、audio は省略できる", () => {
        const message: ControlMessage = {
            type: "configure",
            streams: [{ stream_id: 1, user_id: "123456789012345678", kind: "voice" }],
        };
        expect(roundTrip(message)).toEqual(message);
    });

    it("configure は streams が空でもよい", () => {
        const message: ControlMessage = { type: "configure", streams: [] };
        expect(roundTrip(message)).toEqual(message);
    });

    it("keyframe_request が往復する", () => {
        const message: ControlMessage = { type: "keyframe_request", stream_id: 3 };
        expect(roundTrip(message)).toEqual(message);
    });

    it("log が往復する", () => {
        const message: ControlMessage = { type: "log", lines: ["a", "b", "c"] };
        expect(roundTrip(message)).toEqual(message);
    });

    it("log は空の配列でもよい", () => {
        const message: ControlMessage = { type: "log", lines: [] };
        expect(roundTrip(message)).toEqual(message);
    });

    it("log の alerts と stats は任意で、往復する。文字列の配列でなければ捨てる", () => {
        const message: ControlMessage = { type: "log", lines: ["e"], alerts: ["a"], stats: ["s1", "s2"] };
        expect(roundTrip(message)).toEqual(message);
        expect(decodeControlMessage(JSON.stringify({ type: "log", lines: [], alerts: "a" }))).toBeUndefined();
        expect(decodeControlMessage(JSON.stringify({ type: "log", lines: [], stats: [1] }))).toBeUndefined();
    });

    it("stream_check と stream_check_result が往復する。知らない outcome と負の stream_id は捨てる", () => {
        expect(roundTrip({ type: "stream_check", stream_id: 4 })).toEqual({ type: "stream_check", stream_id: 4 });
        const result: ControlMessage = { type: "stream_check_result", stream_id: 4, outcome: "restarted", detail: "rtp=+12" };
        expect(roundTrip(result)).toEqual(result);
        expect(roundTrip({ type: "stream_check_result", stream_id: 4, outcome: "not_receiving" })).toEqual({
            type: "stream_check_result",
            stream_id: 4,
            outcome: "not_receiving",
        });
        expect(decodeControlMessage(JSON.stringify({ type: "stream_check", stream_id: -1 }))).toBeUndefined();
        expect(
            decodeControlMessage(JSON.stringify({ type: "stream_check_result", stream_id: 1, outcome: "maybe" })),
        ).toBeUndefined();
    });

    it("facts が往復し、facts の中身は unknown のまま受け取る (オブジェクト、配列、数値、null)", () => {
        expect(roundTrip({ type: "facts", facts: { a: 1 } })).toEqual({ type: "facts", facts: { a: 1 } });
        expect(roundTrip({ type: "facts", facts: [1, 2, 3] })).toEqual({ type: "facts", facts: [1, 2, 3] });
        expect(roundTrip({ type: "facts", facts: 42 })).toEqual({ type: "facts", facts: 42 });
        expect(roundTrip({ type: "facts", facts: null })).toEqual({ type: "facts", facts: null });
    });

    it("profiles が往復する", () => {
        const message: ControlMessage = {
            type: "profiles",
            profiles: [
                { user_id: "123456789012345678", display_name: "たろう", avatar_key: "a_1b2c", bot: false },
                { user_id: "223456789012345678", avatar_key: "", bot: true },
                { user_id: "323456789012345678" }, // display_name、avatar_key、bot は、どれも省略できる。
            ],
        };
        expect(roundTrip(message)).toEqual(message);
    });

    it("avatar が往復する (avatar_key は空の文字列でもよい)", () => {
        const message: ControlMessage = {
            type: "avatar",
            user_id: "123456789012345678",
            avatar_key: "",
            avatar_png: "aGVsbG8=",
        };
        expect(roundTrip(message)).toEqual(message);
    });

    it("avatar は 3 つのキーのどれが欠けても、型が違っても undefined を返す", () => {
        const ok = { type: "avatar", user_id: "u1", avatar_key: "k", avatar_png: "AAAA" };
        expect(decodeControlMessage(JSON.stringify(ok))).toEqual(ok);
        for (const key of ["user_id", "avatar_key", "avatar_png"]) {
            const missing: Record<string, unknown> = { ...ok };
            delete missing[key];
            expect(decodeControlMessage(JSON.stringify(missing))).toBeUndefined();
            expect(decodeControlMessage(JSON.stringify({ ...ok, [key]: 1 }))).toBeUndefined();
            expect(decodeControlMessage(JSON.stringify({ ...ok, [key]: null }))).toBeUndefined();
        }
    });

    it("profiles は、profiles が空の配列でもよい", () => {
        const message: ControlMessage = { type: "profiles", profiles: [] };
        expect(roundTrip(message)).toEqual(message);
    });
});

describe("decodeControlMessage: 不正な形は捨てて undefined を返す (例外は投げない)", () => {
    it("JSON として読めない文字列なら undefined を返す", () => {
        expect(decodeControlMessage("{not json")).toBeUndefined();
        expect(decodeControlMessage("")).toBeUndefined();
    });

    it("JSON だがオブジェクトでなければ (数値、文字列、配列、null)、undefined を返す", () => {
        expect(decodeControlMessage("42")).toBeUndefined();
        expect(decodeControlMessage('"hello"')).toBeUndefined();
        expect(decodeControlMessage("[]")).toBeUndefined();
        expect(decodeControlMessage("null")).toBeUndefined();
    });

    it("type が知らない文字列なら undefined を返す", () => {
        expect(decodeControlMessage(JSON.stringify({ type: "unknown_type" }))).toBeUndefined();
    });

    it("type がなければ undefined を返す", () => {
        expect(decodeControlMessage(JSON.stringify({ version: "0.1.0" }))).toBeUndefined();
    });

    it("hello に省略できないキーがなければ undefined を返す", () => {
        expect(decodeControlMessage(JSON.stringify({ type: "hello", version: "0.1.0", wire: 0 }))).toBeUndefined();
    });

    it("hello_ok に wire か version がなければ undefined を返す (どちらも省略できない)", () => {
        expect(decodeControlMessage(JSON.stringify({ type: "hello_ok" }))).toBeUndefined();
        expect(decodeControlMessage(JSON.stringify({ type: "hello_ok", wire: 0 }))).toBeUndefined();
    });

    it("hello_ng の reason が文字列でなければ undefined を返す", () => {
        expect(decodeControlMessage(JSON.stringify({ type: "hello_ng", reason: 1 }))).toBeUndefined();
    });

    it("ping の seq が整数でなければ undefined を返す", () => {
        expect(decodeControlMessage(JSON.stringify({ type: "ping", seq: 1.5 }))).toBeUndefined();
        expect(decodeControlMessage(JSON.stringify({ type: "ping", seq: "1" }))).toBeUndefined();
        expect(decodeControlMessage(JSON.stringify({ type: "ping", seq: -1 }))).toBeUndefined();
    });

    it("keyframe_request の stream_id が負の数なら undefined を返す", () => {
        expect(decodeControlMessage(JSON.stringify({ type: "keyframe_request", stream_id: -1 }))).toBeUndefined();
    });

    it("configure の streams が配列でなければ undefined を返す", () => {
        expect(decodeControlMessage(JSON.stringify({ type: "configure", streams: {} }))).toBeUndefined();
    });

    it("configure のストリームの kind が screen でも voice でもなければ undefined を返す", () => {
        const bad = { ...SAMPLE_STREAM, kind: "camera" };
        expect(decodeControlMessage(JSON.stringify({ type: "configure", streams: [bad] }))).toBeUndefined();
    });

    it("configure のストリームの width が 0 以下なら undefined を返す", () => {
        const bad = { ...SAMPLE_STREAM, width: 0 };
        expect(decodeControlMessage(JSON.stringify({ type: "configure", streams: [bad] }))).toBeUndefined();
    });

    it("configure のストリームの user_id が文字列でなければ undefined を返す", () => {
        const bad = { ...SAMPLE_STREAM, user_id: 123 };
        expect(decodeControlMessage(JSON.stringify({ type: "configure", streams: [bad] }))).toBeUndefined();
    });

    it("log の lines に文字列でない要素があれば undefined を返す", () => {
        expect(decodeControlMessage(JSON.stringify({ type: "log", lines: ["a", 1] }))).toBeUndefined();
    });

    it("facts に facts というキーがなければ undefined を返す", () => {
        expect(decodeControlMessage(JSON.stringify({ type: "facts" }))).toBeUndefined();
    });

    it("profiles の profiles が配列でなければ undefined を返す", () => {
        expect(decodeControlMessage(JSON.stringify({ type: "profiles", profiles: {} }))).toBeUndefined();
    });

    it("profiles の user_id が文字列でなければ undefined を返す (1 つでも壊れていれば、configure と同じく全体を不正とする)", () => {
        expect(
            decodeControlMessage(JSON.stringify({ type: "profiles", profiles: [{ user_id: 123 }] })),
        ).toBeUndefined();
    });

    it("profiles の display_name か avatar_key が文字列でなく、bot が真偽値でなければ undefined を返す", () => {
        expect(
            decodeControlMessage(
                JSON.stringify({ type: "profiles", profiles: [{ user_id: "u1", bot: "true" }] }),
            ),
        ).toBeUndefined();
        expect(
            decodeControlMessage(
                JSON.stringify({ type: "profiles", profiles: [{ user_id: "u1", bot: 1 }] }),
            ),
        ).toBeUndefined();
        expect(
            decodeControlMessage(
                JSON.stringify({ type: "profiles", profiles: [{ user_id: "u1", display_name: 1 }] }),
            ),
        ).toBeUndefined();
        expect(
            decodeControlMessage(
                JSON.stringify({ type: "profiles", profiles: [{ user_id: "u1", avatar_key: 1 }] }),
            ),
        ).toBeUndefined();
    });

    it("profiles の 2 つ目の要素が壊れていると (1 つ目は正しくても)、全体を不正とする", () => {
        expect(
            decodeControlMessage(
                JSON.stringify({ type: "profiles", profiles: [{ user_id: "u1" }, { user_id: 2 }] }),
            ),
        ).toBeUndefined();
    });
});

describe("知らないフィールドは無視する (キーが増えても読めるようにするため)", () => {
    it("hello_ok に知らないキーが付いていてもデコードできる", () => {
        const decoded = decodeControlMessage(
            JSON.stringify({ type: "hello_ok", wire: 0, version: "0.1.0", extra: "future field" }),
        );
        expect(decoded).toEqual({ type: "hello_ok", wire: 0, version: "0.1.0" });
    });
});

describe("知らない type は、例外を投げずに捨てる", () => {
    // 受信側 (src-obs/ws/listener.cpp) と送信側の両方が、知らない type のメッセージを捨てる。ここでは、捨てる動作を
    // 確認する (数えるのは transport/socket.ts である)。
    it("cordmux.dll が後で増やす種類が届いても、捨てるだけで例外を投げない", () => {
        const futureTypes = [
            { type: "recording_limits", max_bitrate: 1 },
            { type: "finalize_progress", percent: 50 },
            { type: "hello_ok_v2", wire: 1 },
        ];
        for (const message of futureTypes) {
            expect(decodeControlMessage(JSON.stringify(message))).toBeUndefined();
            expect(validateControlMessage(message)).toBeUndefined();
        }
    });

    it("type が文字列でなくても捨てる (数値、null、オブジェクト、配列)", () => {
        expect(validateControlMessage({ type: 1 })).toBeUndefined();
        expect(validateControlMessage({ type: null })).toBeUndefined();
        expect(validateControlMessage({ type: { name: "ping" } })).toBeUndefined();
        expect(validateControlMessage({ type: ["ping"] })).toBeUndefined();
    });

    it("知っている type と同じ綴りでも、大文字と小文字が違えば知らない type として捨てる", () => {
        expect(validateControlMessage({ type: "Recording", on: true })).toBeUndefined();
        expect(validateControlMessage({ type: "PING", seq: 1 })).toBeUndefined();
    });
});

describe("validateControlMessage: JSON として読んだ後の値も、直接確認できる", () => {
    it("正しい形なら ControlMessage を返す", () => {
        expect(validateControlMessage({ type: "ping", seq: 1 })).toEqual({ type: "ping", seq: 1 });
    });

    it("オブジェクトでない値には undefined を返す", () => {
        expect(validateControlMessage("ping")).toBeUndefined();
        expect(validateControlMessage(null)).toBeUndefined();
        expect(validateControlMessage([1, 2])).toBeUndefined();
    });
});

describe("recording (受信側から送信側へ送る)", () => {
    it("on が true でも false でも往復する", () => {
        expect(roundTrip({ type: "recording", on: true })).toEqual({ type: "recording", on: true });
        expect(roundTrip({ type: "recording", on: false })).toEqual({ type: "recording", on: false });
    });

    it("on がなければ、不正な形として捨てる", () => {
        expect(decodeControlMessage(JSON.stringify({ type: "recording" }))).toBeUndefined();
    });

    it("on が真偽値でなければ捨てる (0 と 1 も文字列も受け入れない)", () => {
        expect(decodeControlMessage(JSON.stringify({ type: "recording", on: 1 }))).toBeUndefined();
        expect(decodeControlMessage(JSON.stringify({ type: "recording", on: 0 }))).toBeUndefined();
        expect(decodeControlMessage(JSON.stringify({ type: "recording", on: "true" }))).toBeUndefined();
        expect(decodeControlMessage(JSON.stringify({ type: "recording", on: null }))).toBeUndefined();
    });

    it("知らないキーが付いていてもデコードできる", () => {
        expect(
            decodeControlMessage(JSON.stringify({ type: "recording", on: true, reason: "future field" })),
        ).toEqual({ type: "recording", on: true });
    });
});

describe("media、limits と、hello_ok の省略できるキー", () => {
    it("media: 往復し、on がないか真偽値でないものは捨てる", () => {
        for (const on of [true, false]) {
            const message: ControlMessage = { type: "media", on };
            expect(roundTrip(message)).toEqual(message);
        }
        expect(validateControlMessage({ type: "media" })).toBeUndefined();
        expect(validateControlMessage({ type: "media", on: 1 })).toBeUndefined();
        expect(validateControlMessage({ type: "media", on: "true" })).toBeUndefined();
    });

    it("media: 知らないキーは無視し、戻り値には入れない", () => {
        expect(validateControlMessage({ type: "media", on: false, reason: "no-sources" })).toEqual({
            type: "media",
            on: false,
        });
    });

    it("limits: 2 つのキーはどちらも省略でき、付いているキーだけが往復する", () => {
        const cases: ControlMessage[] = [
            { type: "limits", max_long_edge: 1920, fps_divisor: 2 },
            { type: "limits", max_long_edge: 640 },
            { type: "limits", fps_divisor: 1 },
            { type: "limits" },
            { type: "limits", encoder_preference: "software" },
            { type: "limits", latency_mode: "quality" },
            { type: "limits", max_long_edge: 1280, encoder_preference: "hardware", latency_mode: "realtime" },
        ];
        for (const message of cases) expect(roundTrip(message)).toEqual(message);
    });

    it("limits: エンコードの設定が範囲の外か型が違えば、メッセージごと捨てる", () => {
        const bad: unknown[] = [
            { type: "limits", encoder_preference: "gpu" },
            { type: "limits", encoder_preference: true },
            { type: "limits", latency_mode: "fast" },
            { type: "limits", latency_mode: 1 },
            { type: "limits", fps_divisor: 2, latency_mode: "slow" },
        ];
        for (const value of bad) expect(validateControlMessage(value)).toBeUndefined();
    });

    it("limits: 知らない値を足した新しい受信側のキーを、古い送信側は無視する (キーを知らないだけで、メッセージは通る)", () => {
        expect(validateControlMessage({ type: "limits", fps_divisor: 2, future_key: "x" })).toEqual({
            type: "limits",
            fps_divisor: 2,
        });
    });

    it("limits: 範囲の外か型が違うキーがあれば、メッセージごと捨てる (max_long_edge は正の整数、fps_divisor は 1 か 2)", () => {
        const bad: unknown[] = [
            { type: "limits", max_long_edge: 0 },
            { type: "limits", max_long_edge: -1 },
            { type: "limits", max_long_edge: 1280.5 },
            { type: "limits", max_long_edge: "1280" },
            { type: "limits", max_long_edge: 4294967296 },
            { type: "limits", fps_divisor: 0 },
            { type: "limits", fps_divisor: 3 },
            { type: "limits", fps_divisor: true },
            { type: "limits", max_long_edge: 1280, fps_divisor: 4 },
        ];
        for (const value of bad) expect(validateControlMessage(value)).toBeUndefined();
    });

    it("limits: 知らないキーは無視し、戻り値には入れない", () => {
        expect(validateControlMessage({ type: "limits", fps_divisor: 2, prefer_software: true })).toEqual({
            type: "limits",
            fps_divisor: 2,
        });
    });

    it("readLimitsFields: hello_ok の limits と limits のメッセージを、同じ基準で読む", () => {
        expect(readLimitsFields({ max_long_edge: 1280 })).toEqual({ max_long_edge: 1280 });
        expect(readLimitsFields({})).toEqual({});
        expect(readLimitsFields({ fps_divisor: 3 })).toBeUndefined();
        expect(readLimitsFields([1280])).toBeUndefined();
        expect(readLimitsFields(null)).toBeUndefined();
    });

    it("hello_ok: media と limits の初期値を送る", () => {
        const message: ControlMessage = {
            type: "hello_ok",
            wire: 0,
            version: "0.1.0+abc1234",
            media: false,
            limits: { max_long_edge: 960, fps_divisor: 2 },
        };
        expect(roundTrip(message)).toEqual(message);
    });

    it("hello_ok: 省略できるキーの形が壊れていても hello_ok は捨てず、そのキーだけをないものとする", () => {
        expect(validateControlMessage({ type: "hello_ok", wire: 0, version: "v", media: 1 })).toEqual({
            type: "hello_ok",
            wire: 0,
            version: "v",
        });
        expect(
            validateControlMessage({ type: "hello_ok", wire: 0, version: "v", media: true, limits: { fps_divisor: 3 } }),
        ).toEqual({ type: "hello_ok", wire: 0, version: "v", media: true });
        // 省略できないキーがなければ、hello_ok が不正になる。
        expect(validateControlMessage({ type: "hello_ok", wire: 0, media: true })).toBeUndefined();
    });
});

describe("speaking (受け取らない type)", () => {
    it("speaking は知らない type として捨てる", () => {
        expect(validateControlMessage({ type: "speaking", speaking: { count: 1, user_ids: ["1"] } })).toBeUndefined();
    });
});

describe("profiles の username", () => {
    it("username を送り、なくてもよい (省略できる)", () => {
        const message: ControlMessage = {
            type: "profiles",
            profiles: [
                { user_id: "1", display_name: "taro", username: "taro_1" },
                { user_id: "2", display_name: "taro" },
            ],
        };
        expect(roundTrip(message)).toEqual(message);
    });

    it("username が文字列でなければ、display_name と同じく profiles の全体を捨てる", () => {
        expect(validateControlMessage({ type: "profiles", profiles: [{ user_id: "1", username: 5 }] })).toBeUndefined();
        expect(validateControlMessage({ type: "profiles", profiles: [{ user_id: "1", username: null }] })).toBeUndefined();
    });
});

describe("main_account、receive_scope、move_to_main_call、leave_call", () => {
    it("往復する (main_account の user_id は空の文字列でもよい)", () => {
        expect(roundTrip({ type: "main_account", user_id: "u1" })).toEqual({ type: "main_account", user_id: "u1" });
        expect(roundTrip({ type: "main_account", user_id: "" })).toEqual({ type: "main_account", user_id: "" });
        expect(roundTrip({ type: "receive_scope", scope: "everyone" })).toEqual({
            type: "receive_scope",
            scope: "everyone",
        });
        expect(roundTrip({ type: "receive_scope", scope: "slots", user_ids: [] })).toEqual({
            type: "receive_scope",
            scope: "slots",
            user_ids: [],
        });
        expect(roundTrip({ type: "move_to_main_call" })).toEqual({ type: "move_to_main_call" });
        expect(roundTrip({ type: "leave_call" })).toEqual({ type: "leave_call" });
    });

    it("main_account は user_id が文字列でなければ捨てる", () => {
        expect(validateControlMessage({ type: "main_account" })).toBeUndefined();
        expect(validateControlMessage({ type: "main_account", user_id: 1 })).toBeUndefined();
    });

    it("receive_scope は、slots の user_ids を省くか文字列以外を含むと捨て、everyone の user_ids は無視する", () => {
        expect(validateControlMessage({ type: "receive_scope", scope: "slots" })).toBeUndefined();
        expect(validateControlMessage({ type: "receive_scope", scope: "slots", user_ids: ["a", 1] })).toBeUndefined();
        expect(validateControlMessage({ type: "receive_scope", scope: "all" })).toBeUndefined();
        expect(validateControlMessage({ type: "receive_scope", scope: "everyone", user_ids: ["a"] })).toEqual({
            type: "receive_scope",
            scope: "everyone",
        });
    });

    it("move_to_main_call は、移動先を示すキーがあっても無視する", () => {
        expect(validateControlMessage({ type: "move_to_main_call", channel_id: "123" })).toEqual({
            type: "move_to_main_call",
        });
    });
});
