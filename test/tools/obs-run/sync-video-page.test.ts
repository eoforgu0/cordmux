/*
 * share/sync-video.html は、再生すると動画をタブの表示域いっぱいに引き伸ばし、ボタンと説明を隠す (理由はページの冒頭)。
 * 動画が共有の一部にしか写らないと、sync.py が白のマーカーを 1 つも見つけられない。この約束が崩れると実機の実行で
 * 同じことが起きるので、ページの約束を 2 つの形で確認する。
 *   1. CSS: 再生中の動画は表示域いっぱい (position: fixed、100vw x 100vh、object-fit: fill)、ボタンと説明は隠れ、
 *      スクロールバーも出ない。動画には最初から標準の操作バー (controls) を付けない。
 *   2. スクリプト: 小さな偽の document の上で動かし、再生ボタンで音を出して再生中の印が付くこと、C キー
 *      (e.code。日本語入力が有効で e.key が "Process" でも) で操作バーが出入りすること、再生の前と Ctrl つきでは
 *      変わらないこと。
 * 実際のブラウザーで開いて撮る方式にしないのは、テストでブラウザーを起動したくないことと、CI で実行できないことによる。
 */
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";

const HERE = dirname(fileURLToPath(import.meta.url));
const HTML = readFileSync(join(HERE, "share", "sync-video.html"), "utf8");

type Listener = (event: Record<string, unknown>) => void;

class FakeClassList {
    private readonly names = new Set<string>();
    add(name: string): void {
        this.names.add(name);
    }
    contains(name: string): boolean {
        return this.names.has(name);
    }
    toggle(name: string, force?: boolean): boolean {
        const on = force === undefined ? !this.names.has(name) : force;
        if (on) this.names.add(name);
        else this.names.delete(name);
        return on;
    }
}

// ページのスクリプトが触る分だけを持つ作り物の要素 (video / ボタン / 説明の行 / html を兼ねる)。
class FakeElement {
    readonly classList = new FakeClassList();
    readonly listeners = new Map<string, Listener[]>();
    textContent = "";
    controls = false;
    muted = true;
    volume = 0;
    playCalls = 0;
    playError: Error | null = null;
    addEventListener(type: string, listener: Listener): void {
        const list = this.listeners.get(type) ?? [];
        list.push(listener);
        this.listeners.set(type, list);
    }
    dispatch(type: string, event: Record<string, unknown> = {}): void {
        for (const listener of this.listeners.get(type) ?? []) listener(event);
    }
    play(): Promise<void> {
        this.playCalls += 1;
        return this.playError ? Promise.reject(this.playError) : Promise.resolve();
    }
}

function loadPage() {
    const script = /<script>([\s\S]*?)<\/script>/.exec(HTML)?.[1];
    if (script === undefined) throw new Error("sync-video.html has no script");
    const elements: Record<string, FakeElement> = { video: new FakeElement(), play: new FakeElement(), hint: new FakeElement() };
    const root = new FakeElement();
    const doc = {
        documentElement: root,
        getElementById: (id: string) => elements[id] ?? null,
        addEventListener: (type: string, listener: Listener) => root.addEventListener(type, listener),
    };
    new Function("document", script)(doc);
    const video = elements.video as FakeElement;
    const play = elements.play as FakeElement;
    const hint = elements.hint as FakeElement;
    return { root, video, play, hint };
}

const settle = () => new Promise<void>((resolve) => setTimeout(resolve, 0));

function cssRule(selector: string): string {
    const start = HTML.indexOf(selector + " {");
    if (start < 0) throw new Error("no css rule: " + selector);
    return HTML.slice(start, HTML.indexOf("}", start));
}

describe("sync-video.html は再生すると動画をタブの表示域いっぱいに広げる", () => {
    it("再生中の動画は表示域いっぱいに引き伸ばす (縦横の比は保たない)", () => {
        const rule = cssRule("html.playing video");
        for (const decl of ["position: fixed", "left: 0", "top: 0", "width: 100vw", "height: 100vh", "object-fit: fill"]) {
            expect(rule).toContain(decl);
        }
    });

    it("再生中はボタンと説明を隠し、スクロールバーも出さない", () => {
        expect(cssRule("html.playing #play, html.playing #hint")).toContain("display: none");
        expect(cssRule("html.playing, html.playing body")).toContain("overflow: hidden");
    });

    it("動画には最初から標準の操作バーを付けず、繰り返して再生する", () => {
        const tag = /<video[^>]*>/.exec(HTML)?.[0] ?? "";
        expect(tag).toContain('src="sync-video.mp4"');
        expect(tag).toContain(" loop");
        expect(tag).not.toContain("controls");
    });

    it("再生ボタンで音を出して再生し、再生中の印を付ける", async () => {
        const page = loadPage();
        expect(page.root.classList.contains("playing")).toBe(false);
        page.play.dispatch("click");
        await settle();
        expect(page.video.playCalls).toBe(1);
        expect(page.video.muted).toBe(false);
        expect(page.video.volume).toBe(1);
        expect(page.root.classList.contains("playing")).toBe(true);
    });

    it("再生できなければ説明の行に理由を出し、再生中の印は付けない", async () => {
        const page = loadPage();
        page.video.playError = new Error("blocked");
        page.play.dispatch("click");
        await settle();
        expect(page.hint.textContent).toBe("再生できませんでした: blocked");
        expect(page.root.classList.contains("playing")).toBe(false);
    });

    it("再生中は C キーで操作バーを出し入れする (日本語入力で e.key が Process でも)", async () => {
        const page = loadPage();
        page.play.dispatch("click");
        await settle();
        page.root.dispatch("keydown", { code: "KeyC", key: "Process" });
        expect(page.video.controls).toBe(true);
        expect(page.video.classList.contains("show-controls")).toBe(true);
        page.root.dispatch("keydown", { code: "KeyC", key: "c" });
        expect(page.video.controls).toBe(false);
        expect(page.video.classList.contains("show-controls")).toBe(false);
    });

    it("再生の前、Ctrl つきの C、ほかのキーでは操作バーは変わらない", async () => {
        const page = loadPage();
        page.root.dispatch("keydown", { code: "KeyC", key: "c" });
        expect(page.video.controls).toBe(false);
        page.play.dispatch("click");
        await settle();
        page.root.dispatch("keydown", { code: "KeyC", key: "c", ctrlKey: true });
        page.root.dispatch("keydown", { code: "KeyV", key: "v" });
        expect(page.video.controls).toBe(false);
    });
});
