// SPDX-License-Identifier: Apache-2.0

// トラックから映像と音声を読む MediaStreamTrackProcessor が、この実行環境にあるか。Vesktop のレンダラー (Chromium) には
// あるが、Chromium が変えたり外したりすると、encode/track_source.ts と audio/track_source.ts が `new` するところで
// ReferenceError になる。その例外は adapter が Discord の外へ出さずに捨てるので、画面共有と声が来ないまま、何も伝えない
// ことになる。そこで、読み出しを始める前にここで確認し、なければ読み出さずに、依存箇所の pc-hook の確認
// (adapter/vencord.ts の PROBE_CHECKS) が見つからないものとして、ドックに伝える。

/** MediaStreamTrackProcessor を使えるか。 */
export function trackProcessorAvailable(): boolean {
    return typeof (globalThis as { MediaStreamTrackProcessor?: unknown }).MediaStreamTrackProcessor === "function";
}
