// SPDX-License-Identifier: Apache-2.0

import { afterEach, describe, expect, it } from "vitest";

import { trackProcessorAvailable } from "../core/track_processor";

type Holder = { MediaStreamTrackProcessor?: unknown };

describe("trackProcessorAvailable", () => {
    const original = (globalThis as Holder).MediaStreamTrackProcessor;

    afterEach(() => {
        if (original === undefined) delete (globalThis as Holder).MediaStreamTrackProcessor;
        else (globalThis as Holder).MediaStreamTrackProcessor = original;
    });

    it("コンストラクターがあれば true", () => {
        (globalThis as Holder).MediaStreamTrackProcessor = class {};
        expect(trackProcessorAvailable()).toBe(true);
    });

    it("なければ false (例外を投げない)", () => {
        delete (globalThis as Holder).MediaStreamTrackProcessor;
        expect(trackProcessorAvailable()).toBe(false);
    });

    it("関数でない値は false", () => {
        (globalThis as Holder).MediaStreamTrackProcessor = {};
        expect(trackProcessorAvailable()).toBe(false);
    });
});
