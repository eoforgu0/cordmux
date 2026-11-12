// SPDX-License-Identifier: Apache-2.0

// encode/ladder.ts のテスト。エンコードのフォールバックの 5 つの段階、no-preference を使わないこと、H.264 の Level を
// 解像度と fps の両方から選ぶことを確認する。Vencord を import しないので、Node で実行できる。

import { describe, expect, it } from "vitest";

import {
    advanceOnFailure,
    downscaleToLongSide,
    firstLadderOutcome,
    LadderStage,
    nextLadderStage,
    outcomeForStage,
    selectH264Level,
    stageWireName,
    type LadderOutcome,
    type SourceFormat,
} from "../encode/ladder";

const SOURCE_1080P60: SourceFormat = { width: 1920, height: 1080, fps: 60 };

function targetOf(outcome: LadderOutcome) {
    if (outcome.stage === LadderStage.Unavailable) {
        throw new Error("この段階は target を持たない (テストの誤り)");
    }
    return outcome.target;
}

describe("selectH264Level", () => {
    it("面積が同じでも fps が上がると Level が上がる (面積だけで選ぶと、fps を上げたときにエンコーダが閉じる)", () => {
        // 1920x1080@30 は fs=8160、mbps=244800 で、Level 4.0 (maxFs 8192、maxMbps 245760) に収まる。Level 4.0 と
        // 4.1 は maxFs と maxMbps が同じ値なので、表で先にある Level 4.0 を選ぶ。
        const level30 = selectH264Level(1920, 1080, 30);
        expect(level30.levelIdc).toBe(40);

        // 同じ解像度で 60 fps にすると mbps=489600 になり、Level 4.0 と 4.1 の maxMbps (245760) を超えて、Level 4.2
        // まで上がる。
        const level60 = selectH264Level(1920, 1080, 60);
        expect(level60.levelIdc).toBe(42);
    });

    it("とても小さい入力は Level 1.0 になる", () => {
        expect(selectH264Level(16, 16, 1).levelIdc).toBe(10);
    });

    it("1920x1080、60 fps は avc1.64002A になる", () => {
        const outcome = firstLadderOutcome(SOURCE_1080P60);
        expect(targetOf(outcome).codec).toBe("avc1.64002A");
    });
});

describe("no-preference を使わない", () => {
    it("1 から 4 段階目は、すべて prefer-hardware か prefer-software のどちらかである", () => {
        const stages: LadderStage[] = [
            LadderStage.Native,
            LadderStage.HalfFps,
            LadderStage.Downscaled,
            LadderStage.SoftwareFloor,
        ];
        for (const stage of stages) {
            const outcome = outcomeForStage(stage, SOURCE_1080P60);
            const target = targetOf(outcome);
            expect(["prefer-hardware", "prefer-software"]).toContain(target.hardwareAcceleration);
        }
    });

    it("4 段階目 (エンコードする最後の段階) は prefer-software を指定する (no-preference は使わない)", () => {
        const outcome = outcomeForStage(LadderStage.SoftwareFloor, SOURCE_1080P60);
        expect(targetOf(outcome).hardwareAcceleration).toBe("prefer-software");
    });
});

describe("outcomeForStage: 5 つの段階の設定", () => {
    it("1 段階目: 受信した解像度と fps のまま、prefer-hardware", () => {
        const outcome = outcomeForStage(LadderStage.Native, SOURCE_1080P60);
        expect(targetOf(outcome)).toMatchObject({
            hardwareAcceleration: "prefer-hardware",
            width: 1920,
            height: 1080,
            fps: 60,
        });
    });

    it("2 段階目: fps を半分にし (60 -> 30)、解像度は変えない", () => {
        const outcome = outcomeForStage(LadderStage.HalfFps, SOURCE_1080P60);
        expect(targetOf(outcome)).toMatchObject({
            hardwareAcceleration: "prefer-hardware",
            width: 1920,
            height: 1080,
            fps: 30,
        });
    });

    it("3 段階目: 長い辺を 1280 まで縮小し、fps は元に戻す (16:9 の 1920x1080 は 1280x720 になる)", () => {
        const outcome = outcomeForStage(LadderStage.Downscaled, SOURCE_1080P60);
        expect(targetOf(outcome)).toMatchObject({
            hardwareAcceleration: "prefer-hardware",
            width: 1280,
            height: 720,
            fps: 60,
        });
    });

    it("3 段階目: 長い辺が既に 1280 以下なら縮小しない", () => {
        const small: SourceFormat = { width: 640, height: 360, fps: 60 };
        const outcome = outcomeForStage(LadderStage.Downscaled, small);
        expect(targetOf(outcome)).toMatchObject({ width: 640, height: 360 });
    });

    it("4 段階目: 元の解像度にかかわらず 1280x720、30 fps に固定し、prefer-software にする", () => {
        const outcome = outcomeForStage(LadderStage.SoftwareFloor, SOURCE_1080P60);
        expect(targetOf(outcome)).toMatchObject({
            hardwareAcceleration: "prefer-software",
            width: 1280,
            height: 720,
            fps: 30,
        });
    });

    it("5 段階目: 映像を送らない (target を持たない)", () => {
        const outcome = outcomeForStage(LadderStage.Unavailable, SOURCE_1080P60);
        expect(outcome).toEqual({ stage: LadderStage.Unavailable });
    });
});

describe("nextLadderStage と advanceOnFailure: 段階の進み方", () => {
    it("1 から 4 段階目は、1 つずつ進む", () => {
        expect(nextLadderStage(LadderStage.Native)).toBe(LadderStage.HalfFps);
        expect(nextLadderStage(LadderStage.HalfFps)).toBe(LadderStage.Downscaled);
        expect(nextLadderStage(LadderStage.Downscaled)).toBe(LadderStage.SoftwareFloor);
        expect(nextLadderStage(LadderStage.SoftwareFloor)).toBe(LadderStage.Unavailable);
    });

    it("5 段階目からは、それ以上進まない", () => {
        expect(nextLadderStage(LadderStage.Unavailable)).toBe(LadderStage.Unavailable);
    });

    it("advanceOnFailure は、次の段階の結果を返す", () => {
        const outcome = advanceOnFailure(LadderStage.Native, SOURCE_1080P60);
        expect(outcome.stage).toBe(LadderStage.HalfFps);
    });

    it("5 段階目で失敗しても、Unavailable のままにする", () => {
        const outcome = advanceOnFailure(LadderStage.Unavailable, SOURCE_1080P60);
        expect(outcome).toEqual({ stage: LadderStage.Unavailable });
    });
});

describe("firstLadderOutcome", () => {
    it("1 段階目 (Native) と同じ結果を返す", () => {
        expect(firstLadderOutcome(SOURCE_1080P60)).toEqual(outcomeForStage(LadderStage.Native, SOURCE_1080P60));
    });
});

describe("stageWireName (facts の stages に入れる名前)", () => {
    it("1 から 4 段階目を、決まった 4 つの文字列にする", () => {
        expect(stageWireName(LadderStage.Native)).toBe("native");
        expect(stageWireName(LadderStage.HalfFps)).toBe("halffps");
        expect(stageWireName(LadderStage.Downscaled)).toBe("downscaled");
        expect(stageWireName(LadderStage.SoftwareFloor)).toBe("software");
    });

    it("5 段階目 (Unavailable) は unavailable にする", () => {
        expect(stageWireName(LadderStage.Unavailable)).toBe("unavailable");
    });
});

describe("downscaleToLongSide (縮小するときの丸め方)", () => {
    it("長い辺が上限以下なら、1 ピクセルも変えない", () => {
        expect(downscaleToLongSide(1280, 720, 1280)).toEqual({ width: 1280, height: 720 });
        expect(downscaleToLongSide(640, 480, 1280)).toEqual({ width: 640, height: 480 });
    });

    it("縦横比を保ったまま長い辺を上限まで縮め、H.264 のために偶数に切り捨てる", () => {
        expect(downscaleToLongSide(1920, 1080, 1280)).toEqual({ width: 1280, height: 720 });
        expect(downscaleToLongSide(1920, 1080, 1000)).toEqual({ width: 1000, height: 562 }); // 562.5 -> 562
        expect(downscaleToLongSide(1920, 1080, 1600)).toEqual({ width: 1600, height: 900 });
    });

    it("縦長の画面共有では、高さを上限に合わせる", () => {
        expect(downscaleToLongSide(1080, 1920, 1280)).toEqual({ width: 720, height: 1280 });
    });

    it("とても小さい上限でも 0 にはしない (最小で 2 ピクセル)", () => {
        expect(downscaleToLongSide(1920, 1080, 1)).toEqual({ width: 2, height: 2 });
    });
});
