// SPDX-License-Identifier: Apache-2.0

// エンコードのフォールバックの 5 つの段階 (LadderStage) と、段階ごとの VideoEncoder.configure の設定、H.264 の
// Level の選び方を決める純粋関数。prefer-hardware は失敗してもソフトウェアのエンコーダに切り替わらずにエラーに
// なるので、段階を明示的に下げる。no-preference は使わない。no-preference では Chromium がソフトウェアに切り替え
// ても JavaScript に伝わらず、フォールバックしたことをドックに表示できないためである。WebCodecs の型は使わず、
// 文字列のリテラル型を自前で持つ (tsconfig.json の lib に DOM を追加しないため)。

/** VideoEncoder.configure に渡す hardwareAcceleration の値 (DOM の型を使わないための自前の定義)。 */
export type HardwareAccelerationPreference = "prefer-hardware" | "prefer-software";

/**
 * フォールバックの段階。2 と 3 は、画面共有の元の解像度と fps (SourceFormat) から 1 つの値だけを変え、前の段階の
 * 変更は引き継がない。2 は fps だけを半分にし、3 は fps を元のままにして長辺だけを 1280 まで縮小する。4 は
 * ソフトウェアのエンコーダで 1280x720 30 fps にする。元の長辺が 1280 以下なら 3 は 1 と同じ設定になり、1 と同じ
 * 理由で失敗することがあるが、4 はソフトウェアのエンコーダを使うので、1 から 3 が失敗しても 4 で成功する前提に
 * している。
 */
export const LadderStage = {
    /** 1. prefer-hardware。受信した解像度と fps のまま。 */
    Native: 1,
    /** 2. prefer-hardware。fps を半分にする (60 -> 30)。 */
    HalfFps: 2,
    /** 3. prefer-hardware。長辺を 1280 まで縮小する。 */
    Downscaled: 3,
    /** 4. prefer-software。1280x720 30 fps にする。 */
    SoftwareFloor: 4,
    /** 5. その人の映像は送らない。 */
    Unavailable: 5,
} as const;

export type LadderStage = (typeof LadderStage)[keyof typeof LadderStage];

/** エンコードの設定を持つ段階 (1 から 4) だけの型。 */
export type ActiveLadderStage = Exclude<LadderStage, typeof LadderStage.Unavailable>;

/** 画面共有の映像の解像度と fps (Discord から受信したまま)。画面共有ごとに違うので、決まった値にしない。 */
export interface SourceFormat {
    width: number;
    height: number;
    fps: number;
}

/**
 * ある段階で VideoEncoder.configure に渡す設定の元。encode/session.ts は width と height をそのまま使わず、
 * 長辺 (と maxLongEdge) だけを使い、実際のサイズは元の縦横比のまま downscaleToLongSide() で決める。そのため
 * 4 の 1280x720 は、縦横比が 16:9 でない画面共有では、長辺を 1280 にする意味になる (縦横比を変えて引き伸ばさない
 * ため)。fps は、この値をそのまま使う。
 */
export interface EncoderTarget {
    hardwareAcceleration: HardwareAccelerationPreference;
    width: number;
    height: number;
    fps: number;
    /**
     * avc1.PPCCLL の形式 (RFC 6381)。encode/session.ts はこの値を使わず、実際にエンコードするサイズから
     * codecStringForResolution() で作り直した値を configure に渡す。
     */
    codec: string;
}

/** 1 から 4 は設定 (target) を持ち、5 (Unavailable) は映像を送らないことを表す。 */
export type LadderOutcome = { stage: ActiveLadderStage; target: EncoderTarget } | { stage: typeof LadderStage.Unavailable };

// H.264 の Level ごとの上限。MaxFS は 1 フレームのマクロブロックの数、MaxMBPS は 1 秒あたりのマクロブロックの数で、
// 値は H.264 の規格 (ITU-T H.264 の Annex A の Level の表) のとおり。表の値は規格の定数で、測って確認する必要が
// あるのは、その Level を PC のハードウェアのエンコーダが受け付けるかのほうである。
export interface H264LevelLimit {
    /** avc1 のコーデックの文字列の level_idc に使う値 (Level x 10)。 */
    levelIdc: number;
    maxFs: number;
    maxMbps: number;
}

// 昇順に並べる。selectH264Level は、先頭から見て最初に条件を満たす行を選ぶ。
const H264_LEVEL_LIMITS: readonly H264LevelLimit[] = [
    { levelIdc: 10, maxFs: 99, maxMbps: 1_485 },
    { levelIdc: 11, maxFs: 396, maxMbps: 3_000 },
    { levelIdc: 12, maxFs: 396, maxMbps: 6_000 },
    { levelIdc: 13, maxFs: 396, maxMbps: 11_880 },
    { levelIdc: 20, maxFs: 396, maxMbps: 11_880 },
    { levelIdc: 21, maxFs: 792, maxMbps: 19_800 },
    { levelIdc: 22, maxFs: 1_620, maxMbps: 20_250 },
    { levelIdc: 30, maxFs: 1_620, maxMbps: 40_500 },
    { levelIdc: 31, maxFs: 3_600, maxMbps: 108_000 },
    { levelIdc: 32, maxFs: 5_120, maxMbps: 216_000 },
    { levelIdc: 40, maxFs: 8_192, maxMbps: 245_760 },
    { levelIdc: 41, maxFs: 8_192, maxMbps: 245_760 },
    { levelIdc: 42, maxFs: 8_704, maxMbps: 522_240 },
    { levelIdc: 50, maxFs: 22_080, maxMbps: 589_824 },
    { levelIdc: 51, maxFs: 36_864, maxMbps: 983_040 },
    { levelIdc: 52, maxFs: 36_864, maxMbps: 2_073_600 },
    { levelIdc: 60, maxFs: 139_264, maxMbps: 4_177_920 },
    { levelIdc: 61, maxFs: 139_264, maxMbps: 8_355_840 },
    { levelIdc: 62, maxFs: 139_264, maxMbps: 16_711_680 },
];

// 表のどの Level でも足りない入力のときに使う。4K 60 fps までは Level 5.2 に収まるので、ふつうは使わない。
const HIGHEST_LEVEL: H264LevelLimit = H264_LEVEL_LIMITS[H264_LEVEL_LIMITS.length - 1] ?? {
    levelIdc: 62,
    maxFs: 139_264,
    maxMbps: 16_711_680,
};

/** High Profile (profile_idc 0x64) で、制約のフラグは付けない (avc1.64002A などの 6400 の部分)。 */
const AVC_PROFILE_AND_CONSTRAINTS_HEX = "6400";

function macroblocksPerFrame(dimension: number): number {
    return Math.ceil(dimension / 16);
}

/**
 * width、height、fps から、MaxFS (面積) と MaxMBPS (処理の速さ) の両方を満たす最小の Level を選ぶ。面積だけで
 * 選ぶと、fps を上げたときに MaxMBPS を超え、エンコーダが閉じる。
 */
export function selectH264Level(width: number, height: number, fps: number): H264LevelLimit {
    const fs = macroblocksPerFrame(width) * macroblocksPerFrame(height);
    const mbps = fs * fps;
    for (const limit of H264_LEVEL_LIMITS) {
        if (fs <= limit.maxFs && mbps <= limit.maxMbps) return limit;
    }
    return HIGHEST_LEVEL;
}

function buildCodecString(level: H264LevelLimit): string {
    const levelHex = level.levelIdc.toString(16).toUpperCase().padStart(2, "0");
    return `avc1.${AVC_PROFILE_AND_CONSTRAINTS_HEX}${levelHex}`;
}

/**
 * width、height、fps から、VideoEncoder.configure に渡す avc1.PPCCLL の文字列を求める。encode/session.ts は、
 * 実際にエンコードする (縮小した後の) サイズから Level を選び直すために使う。Level の MaxFS が実際にエンコード
 * する面積より小さいと isConfigSupported が false を返し、4 の段階でもエンコーダを確保できなくなる。たとえば
 * 1920x1080 (8160 マクロブロック) に対して、3 の段階の 1280x720 から選ぶ Level 3.2 の MaxFS は 5120 しかない。
 */
export function codecStringForResolution(width: number, height: number, fps: number): string {
    return buildCodecString(selectH264Level(width, height, fps));
}

function buildTarget(
    hardwareAcceleration: HardwareAccelerationPreference,
    width: number,
    height: number,
    fps: number,
): EncoderTarget {
    const level = selectH264Level(width, height, fps);
    return { hardwareAcceleration, width, height, fps, codec: buildCodecString(level) };
}

/**
 * 長辺が maxLongSide を超えないように、縦横比を保って縮小する。H.264 のエンコーダには偶数のサイズを渡す必要が
 * あるので、偶数に切り捨てる (最小は 2)。もとから maxLongSide 以下なら変えない。encode/session.ts も、この関数で
 * 実際にエンコードするサイズを決める。
 */
export function downscaleToLongSide(width: number, height: number, maxLongSide: number): { width: number; height: number } {
    const longSide = Math.max(width, height);
    if (longSide <= maxLongSide) return { width, height };
    const scale = maxLongSide / longSide;
    const evenFloor = (value: number) => Math.max(2, Math.floor((value * scale) / 2) * 2);
    return { width: evenFloor(width), height: evenFloor(height) };
}

const DOWNSCALE_LONG_SIDE = 1280; // 段階 3 の長辺
const SOFTWARE_FLOOR_WIDTH = 1280; // 段階 4 のサイズと fps
const SOFTWARE_FLOOR_HEIGHT = 720;
const SOFTWARE_FLOOR_FPS = 30;

/** 段階と、画面共有の元の解像度と fps (source) から、その段階で使うエンコーダの設定を求める。 */
export function outcomeForStage(stage: LadderStage, source: SourceFormat): LadderOutcome {
    switch (stage) {
        case LadderStage.Native:
            return { stage, target: buildTarget("prefer-hardware", source.width, source.height, source.fps) };
        case LadderStage.HalfFps: {
            const fps = Math.max(1, Math.round(source.fps / 2));
            return { stage, target: buildTarget("prefer-hardware", source.width, source.height, fps) };
        }
        case LadderStage.Downscaled: {
            const { width, height } = downscaleToLongSide(source.width, source.height, DOWNSCALE_LONG_SIDE);
            return { stage, target: buildTarget("prefer-hardware", width, height, source.fps) };
        }
        case LadderStage.SoftwareFloor:
            return {
                stage,
                target: buildTarget(
                    "prefer-software",
                    SOFTWARE_FLOOR_WIDTH,
                    SOFTWARE_FLOOR_HEIGHT,
                    SOFTWARE_FLOOR_FPS,
                ),
            };
        case LadderStage.Unavailable:
            return { stage: LadderStage.Unavailable };
    }
}

/** 次の段階を返す (設定は求めない)。5 からは進まない。 */
export function nextLadderStage(stage: LadderStage): LadderStage {
    return stage >= LadderStage.Unavailable ? LadderStage.Unavailable : ((stage + 1) as LadderStage);
}

/** 最初の段階 (1) の設定を返す。 */
export function firstLadderOutcome(source: SourceFormat): LadderOutcome {
    return outcomeForStage(LadderStage.Native, source);
}

/**
 * 現在の段階でエンコードに失敗した (isConfigSupported が false だった、configure かエンコードが失敗した) ときに、
 * 次の段階の設定を返す。エンコーダを作り直すのは呼び出し元である。5 (Unavailable) からは下がらない。
 */
export function advanceOnFailure(currentStage: LadderStage, source: SourceFormat): LadderOutcome {
    return outcomeForStage(nextLadderStage(currentStage), source);
}

/**
 * 段階を、ワイヤー形式の facts の stages に入れる文字列にする (docs/wire_format.md の「facts」)。番号のまま
 * 送らないのは、受信側をこのファイルの LadderStage の番号に依存させないためで、受信側は知らない文字列が来ても、
 * そのまま持つ。5 (Unavailable) も "unavailable" として送る。送らないと、受信側は、その人の映像を送れなくなった
 * ことをドックに表示できない。
 */
export function stageWireName(stage: LadderStage): string {
    switch (stage) {
        case LadderStage.Native:
            return "native";
        case LadderStage.HalfFps:
            return "halffps";
        case LadderStage.Downscaled:
            return "downscaled";
        case LadderStage.SoftwareFloor:
            return "software";
        case LadderStage.Unavailable:
            return "unavailable";
    }
}
