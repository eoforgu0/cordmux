# 判定スクリプト (frames.py / sync.py / smooth.py / analyze.py) のセルフテスト用に、GPU も OBS も使わず
# ffmpeg (PATH の 8.0、libx264 ultrafast) と Python (numpy) だけでテスト用の mp4 を作る。
#
# 柄とマーカーは test/tools/harness の renderer.js の描き方に合わせる。
#   - 左上に 32 ビットのフレーム番号を升目で描く。最上位ビットが左、白 = 1、黒 = 0。
#     renderer.js の升目は等倍で 16 px。ただしこのテストデータは smooth.py 向けの升目幅 8 px を使う
#     (smooth.py はセル位置 (0,0)/(960,0)/(0,540)/(960,540) とセル幅 8 px を決め打ちしていて
#     引数で変えられないため。frames.py 側は --cell オプションで 8 に合わせて呼ぶ。
#     升目は 32 ビットのフレーム番号を 16 px で描き、縮小 0.5 のときに 8 px になる)。
#   - 5 秒ごとに、全面の白 3 フレームと同時刻に 60 ms だけ 1 kHz を 2 kHz に変えるマーカーを入れる。
#
# 壊し方 (欠落・凍結・重複) はフレーム番号の「値の並び」を変えることで作る。実際の録画に対して
# ffmpeg の select / freezeframes で壊した場合と同じ壊れ方になる。
#   --drop-every N   : N 番目ごとの位置の出力フレームを 1 つ丸ごと落とす (値が飛ぶ = 欠落)
#   --freeze-ms M --freeze-at S : S 秒の位置から、直前のフレーム番号を M ms 相当のフレーム数ぶん
#                       繰り返す (その位置の本来の値は失われ、後続は自分の本来の値に戻る。
#                       例えば 119 を 120-124 に複製する形で、凍結の後に欠落が付随して出る)
#   --dup-every N    : N 番目ごとの位置の出力フレームをもう 1 つ複製して差し込む (重複)
#   --audio-shift-ms D : マーカー音 (1 kHz/2 kHz) の鳴る実時刻を D ms ずらす (映像側の白は動かさない)
#   --competing-1k A : マーカー音とは別に、振幅 A の 1 kHz を最初から最後まで鳴らし続けて混ぜる
#                       (Chrome のテスト用のマイクの 1 kHz が録画に混ざった形。マーカーの 2 kHz と同じ
#                       振幅 0.5 を渡しても、sync.py はマーカーを見つけられなければならない)
#   --vfr            : 奇数番目のフレームを、13 フレームに 1 回を除いて落とし、残りのフレームは元の時刻のまま置く
#                       (cordmux の録画のように、平均は約 32 fps なのに r_frame_rate が 60 と出る可変フレームレートの
#                       テストデータ。フレーム番号を r_frame_rate で割ると、映像の時刻が約半分になる)
#
# 実際の画面共有の形 (--scene):
#   plain  (既定): 上のとおり。枠そのもの (--width x --height) の左上に旧形式の帯 (--cell の升 32 個) を等倍で描く。
#   window : ブラウザのウィンドウの共有で pattern.html を見せた形。1920x1080 のウィンドウ (上の 114 px に
#            タブの列・アドレスの欄・ブックマークの欄、ページの下地 32、余白 8 px の後に 1280x720 の canvas を中央寄せ)
#            を描き、ffmpeg で 1280x720 (共有) に縮めてから 0.5 倍 (枠の 640x360) にする。
#   tab    : Chrome のタブの共有で sync-video.html を見せた形。1920x905 の表示域 (ページの
#            下地 32、上の余白 12 px の後に 1280x720 の動画を中央寄せ) を 1280x720 に引き伸ばして (縦横の倍率が
#            違う) から 0.5 倍にする。5 秒ごとに動画の中が白になる 3 フレーム (sync-video.mp4 と同じ)。動画は枠の約 53%
#            しか占めず、白のフレームの枠の平均は 139 になる (平均 200 超えを白の条件にすると白が 0 になる形)。
#   tabfill: 動画が表示域いっぱいに引き伸ばされる sync-video.html (object-fit: fill) を Chrome のタブで共有した形。
#            1280x720 の動画の絵を ffmpeg で 1920x905 (表示域) に引き伸ばし、1280x720 (共有) に縮めてから 0.5 倍にする
#            (動画が枠の全部を占める)。
#   寸法は OBS の枠のスクリーンショットから測った値である。共有の上で canvas の上の縁が y = 81
#   (ウィンドウの 122 px x 2/3)、タブの列 0-28・アドレスの欄 32-52・ブックマークの欄 53-73
#   (ウィンドウの座標で 0-42 / 48-78 / 80-110)、Chrome のタブの動画は y = 9.5 から 583 (表示域 905 px x 0.796)。
#   帯は --strip clock (現在の pattern.html / sync-video.mp4 の時計つきの帯。bit_strip.py) か --strip legacy
#   (16 px の升 32 個の旧形式) で、canvas / 動画の左上に描く。升の一辺は pattern.html が 32 px、
#   sync-video.mp4 が 24 px。
#   --counter-start N : 帯に描く番号に N を足す (実際の pattern.html は 1085089 前後になり、上位のビットまで使う)
#   --pad-to WxH      : 枠 (640x360) を WxH の黒の左上に置く (録画の 1 区画。960x540 の象限の左上など)
#   --crf N           : libx264 の crf (既定 23)。共有と録画の 2 度の圧縮の代わりに、粗い圧縮で帯の縁を崩す
#   ブラウザで実物を開いて撮る方法は採らない。テストが実行中のブラウザを操作することになり、CI でも動かせない。
#
# 使い方: python make_material.py <出力.mp4> [--seconds 20] [--fps 60] [--width 640] [--height 360]
#         [--sync-every 5] [--cell 8]
#         [--drop-every N] [--freeze-ms M --freeze-at S] [--dup-every N] [--audio-shift-ms D]
#         [--competing-1k A] [--vfr]
#         [--scene plain|window|tab|tabfill] [--strip clock|legacy] [--counter-start N] [--pad-to WxH] [--crf N]
import argparse
import os
import subprocess
import sys
import wave

import numpy as np

import bit_strip

WHITE_RUN = 3          # renderer.js: マーカーは白 3 フレーム
BURST_MS = 60           # renderer.js: マーカー音は 60 ms
SAMPLE_RATE = 48000
BASE_HZ = 1000.0
BURST_HZ = 2000.0


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("output")
    p.add_argument("--seconds", type=float, default=20.0)
    p.add_argument("--fps", type=float, default=60.0)
    p.add_argument("--width", type=int, default=640)
    p.add_argument("--height", type=int, default=360)
    p.add_argument("--sync-every", type=float, default=5.0)
    p.add_argument("--cell", type=int, default=8)
    p.add_argument("--drop-every", type=int, default=0)
    p.add_argument("--freeze-ms", type=float, default=0.0)
    p.add_argument("--freeze-at", type=float, default=0.0)
    p.add_argument("--dup-every", type=int, default=0)
    p.add_argument("--audio-shift-ms", type=float, default=0.0)
    p.add_argument("--competing-1k", type=float, default=0.0)
    p.add_argument("--vfr", action="store_true")
    p.add_argument("--scene", choices=("plain", "window", "tab", "tabfill"), default="plain")
    p.add_argument("--strip", choices=("clock", "legacy"), default="clock")
    p.add_argument("--counter-start", type=int, default=0)
    p.add_argument("--pad-to", default=None)
    p.add_argument("--crf", type=int, default=None)
    return p.parse_args()


# 出力フレームの並び (counter 値, 白フレームかどうか) を組み立てる。壊し方は 1 テストデータにつき高々 1 つを
# 想定 (selfcheck.py は 1 テストデータ 1 壊し方で呼ぶ)。値は「本来のフレーム番号」を表し、frames.py /
# smooth.py が読む升目にそのまま描く。
def build_plan(nominal_frames, fps, sync_every, drop_every, freeze_ms, freeze_at, dup_every):
    boundaries = []
    b = sync_every
    while b < nominal_frames / fps:
        boundaries.append(round(b * fps))
        b += sync_every

    freeze_total = round(freeze_ms / 1000.0 * fps) if freeze_ms > 0 else 0
    replaced_count = max(0, freeze_total - 1)  # 直前のフレーム + これで凍結の合計フレーム数になる
    freeze_start = round(freeze_at * fps) if freeze_ms > 0 else None

    values = []
    whites = []
    freeze_remaining = 0
    frozen_value = 0
    for logical in range(nominal_frames):
        if freeze_start is not None and logical == freeze_start:
            freeze_remaining = replaced_count
            frozen_value = logical - 1
        if freeze_remaining > 0:
            v = frozen_value
            freeze_remaining -= 1
        else:
            v = logical
        is_white = any(bd <= logical < bd + WHITE_RUN for bd in boundaries)

        is_drop = drop_every > 0 and (logical + 1) % drop_every == 0
        if is_drop:
            continue  # このフレームは出力そのものをしない (欠落)

        values.append(v)
        whites.append(is_white)

        is_dup = dup_every > 0 and (logical + 1) % dup_every == 0
        if is_dup:
            values.append(v)  # 同じフレームをもう 1 枚差し込む (重複)
            whites.append(is_white)

    return values, whites, boundaries


def draw_legacy_strip(img, x, y, cell, value):
    # 旧形式の帯 (sender.ts の drawFrameNumberBits と同じ並び): 升 cell の 32 個、左が最上位、白 = 1。
    for bit in range(32):
        bitval = (value >> (31 - bit)) & 1
        img[y:y + cell, x + bit * cell:x + (bit + 1) * cell] = 255 if bitval else 0


def make_frame(w, h, cell, value, white):
    img = np.full((h, w), 255 if white else 0, dtype=np.uint8)
    draw_legacy_strip(img, 0, 0, cell, value)
    return img


# ---- 実際の共有の形 (--scene window / tab) ----
SHARE_W, SHARE_H = 1280, 720      # Discord が届ける共有の大きさ (実測)
CELL_W, CELL_H = 640, 360         # 枠の大きさ (共有を 0.5 倍で置く)
PAGE_BG = 32                      # ページの下地 #202020 (スクリーンショットで 31)
WINDOW_W, WINDOW_H = 1920, 1080
WINDOW_CANVAS_X, WINDOW_CANVAS_Y = 320, 122   # (1920 - 1280) / 2、ブラウザの上の帯 114 + ページの余白 8
TAB_W, TAB_H = 1920, 905
TAB_VIDEO_X, TAB_VIDEO_Y = 320, 12            # sync-video.html の #wrap の余白 12 px (再生ボタンは押した後に隠れる)
VIDEO_W, VIDEO_H = 1280, 720                  # sync-video.mp4 の大きさ (tabfill は動画の絵だけを描き、引き伸ばしは ffmpeg)
PATTERN_STRIP_U = 32              # share/pattern.html の STRIP_CELL
SYNC_VIDEO_STRIP_U = 24           # make_sync_video.py の STRIP_CELL
LEGACY_CELL = 16                  # 旧形式の pattern.html / sync-video.mp4 の升


def _text_like(img, x0, y0, x1, y1, rng, density):
    # 文字に似た明るい短い棒を散らす (タブのタイトル・アドレス・ページの説明の行)。帯を探すときの紛らわしいもの。
    for _ in range(density):
        x = int(rng.integers(x0, x1))
        y = int(rng.integers(y0, y1))
        img[y:y + int(rng.integers(2, 6)), x:x + int(rng.integers(2, 9))] = int(rng.integers(150, 235))


def window_background():
    # Firefox のウィンドウ (濃い色の外観) と pattern.html の動かない部分。
    rng = np.random.default_rng(1001)
    img = np.full((WINDOW_H, WINDOW_W), PAGE_BG, dtype=np.uint8)
    img[0:42, :] = 31                       # タブの列
    img[0:42, 230:450] = 60                 # 選んでいるタブ
    _text_like(img, 10, 14, 440, 30, rng, 120)
    img[44:47, :] = 43
    img[48:78, :] = 31                      # アドレスの欄
    _text_like(img, 160, 56, 1100, 70, rng, 260)
    _text_like(img, 1500, 52, 1900, 74, rng, 80)   # ツールバーの絵
    img[80:111, :] = 43                     # ブックマークの欄
    _text_like(img, 10, 88, 480, 102, rng, 120)
    cx, cy = WINDOW_CANVAS_X, WINDOW_CANVAS_Y
    canvas = np.full((720, 1280), 58, dtype=np.uint8)      # pattern.html の BACKGROUND #3a3a3a
    for y in range(0, 720, 80):                             # 薄い格子 #444444
        for x in range((y // 80) % 2 * 40, 1280, 80):
            canvas[y:y + 80, x:x + 40] = 68
    img[cy:cy + 720, cx:cx + 1280] = canvas
    _text_like(img, 1040, 856, 1580, 872, rng, 90)          # 「1280x720 / 60 fps / ...」の行
    img[882:927, 880:1040] = 225                           # 「全画面にする」のボタン
    _text_like(img, 900, 896, 1020, 912, rng, 30)
    return img


def tab_background():
    # Chrome のタブの表示域 (sync-video.html) の動かない部分。動画の中は毎フレーム描く。
    rng = np.random.default_rng(1002)
    img = np.full((TAB_H, TAB_W), PAGE_BG, dtype=np.uint8)
    _text_like(img, 640, 750, 1280, 768, rng, 200)          # 説明の行
    img[790:830, 900:1020] = 225                           # 「全画面にする」のボタン
    return img


def video_background():
    # make_sync_video.py の build_background と同じ形 (上から下へ 48 -> 112 の勾配に、40 px の格子 +10)。
    rows = np.linspace(48, 112, 720, dtype=np.float32)
    bg = np.repeat(rows[:, None], 1280, axis=1)
    yy, xx = np.mgrid[0:720, 0:1280]
    grid = (((yy // 40) + (xx // 40)) % 2).astype(np.float32) * 10
    return np.clip(bg + grid, 0, 255).astype(np.uint8)


def draw_number_blocks(img, x, y, value, digit_w, digit_h):
    # 人が読む大きなフレーム番号の代わり (文字の形は要らない。桁ごとに白い四角の枠を描き、桁数で幅が変わる)。
    for k, ch in enumerate(str(value)):
        x0 = x + k * (digit_w + digit_w // 4)
        img[y:y + digit_h, x0:x0 + digit_w] = 255
        inner = 4 + (int(ch) % 3) * 2
        img[y + inner * 3:y + digit_h - inner * 3, x0 + inner * 2:x0 + digit_w - inner * 2] = 58


def draw_circle(img, cx, cy, r, value):
    yy, xx = np.ogrid[-r:r + 1, -r:r + 1]
    mask = xx * xx + yy * yy <= r * r
    region = img[cy - r:cy + r + 1, cx - r:cx + r + 1]
    region[mask[:region.shape[0], :region.shape[1]]] = value


def render_scene(scene, static, value, white, strip):
    img = static.copy()
    phase = (value % 180) / 180.0
    sweep = phase * 2 if phase < 0.5 else 2 - phase * 2
    if scene == "window":
        ox, oy, u = WINDOW_CANVAS_X, WINDOW_CANVAS_Y, PATTERN_STRIP_U
        draw_circle(img, ox + int(60 + sweep * 1160), oy + 360, 36, 120)
        draw_number_blocks(img, ox + 40, oy + 432, value, 80, 144)
    else:
        ox, oy = (TAB_VIDEO_X, TAB_VIDEO_Y) if scene == "tab" else (0, 0)
        u = SYNC_VIDEO_STRIP_U
        if white:
            img[oy:oy + 720, ox:ox + 1280] = 255   # 白のマーカーの 3 フレーム (帯だけは白の上にも描く)
        else:
            draw_circle(img, ox + int(60 + sweep * 1160), oy + 360, 36, 140)
            draw_number_blocks(img, ox + 40, oy + 432, value, 100, 140)
    if strip == "legacy":
        draw_legacy_strip(img, ox, oy, LEGACY_CELL, value)
    else:
        bit_strip.draw_strip(img, ox, oy, u, value)
    return img


def scene_static(scene):
    if scene == "window":
        return window_background()
    if scene == "tabfill":
        return video_background()
    img = tab_background()
    img[TAB_VIDEO_Y:TAB_VIDEO_Y + 720, TAB_VIDEO_X:TAB_VIDEO_X + 1280] = video_background()
    return img


def build_audio(nominal_frames, fps, boundaries, audio_shift_ms, competing_1k=0.0):
    duration_s = nominal_frames / fps
    n = int(round(duration_s * SAMPLE_RATE))
    shift_s = audio_shift_ms / 1000.0
    t = np.arange(n, dtype=np.float64) / SAMPLE_RATE
    burst_starts_s = [bd / fps + shift_s for bd in boundaries]
    freq = np.full(n, BASE_HZ)
    for bs in burst_starts_s:
        in_burst = (t >= bs) & (t < bs + BURST_MS / 1000.0)
        freq[in_burst] = BURST_HZ
    # 周波数が瞬時に切り替わる位置で位相が飛ぶが、sync.py / analyze.py は 5 ms の区間の
    # エネルギー比較 (Goertzel) で見るだけなので検出には影響しない。
    phase = 2 * np.pi * freq * t
    wave_f = 0.5 * np.sin(phase)
    if competing_1k > 0:
        # 鳴り続ける 1 kHz (マーカー音とは別の音源)。マーカー音の位相とは無関係に始まる形にするため 1/4 周期ずらす。
        wave_f = wave_f + competing_1k * np.sin(2 * np.pi * BASE_HZ * t + np.pi / 2)
        wave_f = wave_f / max(1.0, float(np.max(np.abs(wave_f))))
    pcm = np.clip(wave_f * 32767, -32768, 32767).astype(np.int16)
    return pcm


def write_wav(path, pcm):
    with wave.open(path, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(SAMPLE_RATE)
        wf.writeframes(pcm.tobytes())


def main():
    args = parse_args()
    nominal_frames = round(args.seconds * args.fps)

    values, whites, boundaries = build_plan(
        nominal_frames, args.fps, args.sync_every,
        args.drop_every, args.freeze_ms, args.freeze_at, args.dup_every)

    pcm = build_audio(nominal_frames, args.fps, boundaries, args.audio_shift_ms, args.competing_1k)
    wav_path = args.output + ".tmp.wav"
    write_wav(wav_path, pcm)

    filters = []
    if args.vfr:
        # 可変: 奇数番目のフレームを 13 フレームに 1 回だけ残し、残りのフレームの時刻は入力のまま (passthrough)。
        filters.append(r"select='not(mod(n\,2))+eq(mod(n\,13)\,1)'")
    if args.scene == "plain":
        in_w, in_h = args.width, args.height
    else:
        in_w, in_h = {"window": (WINDOW_W, WINDOW_H), "tab": (TAB_W, TAB_H), "tabfill": (VIDEO_W, VIDEO_H)}[args.scene]
        if args.scene == "tabfill":
            # ページが動画を表示域いっぱいに引き伸ばす (sync-video.html の object-fit: fill)。
            filters.append(f"scale={TAB_W}:{TAB_H}:flags=bilinear")
        # 共有の縮小 (Discord。タブは縦横の倍率が違う) と、枠の 0.5 倍 (OBS) の 2 段。
        filters.append(f"scale={SHARE_W}:{SHARE_H}:flags=bilinear,scale={CELL_W}:{CELL_H}:flags=bilinear")
        if args.pad_to:
            pw, ph = (int(v) for v in args.pad_to.lower().split("x"))
            filters.append(f"pad={pw}:{ph}:0:0:color=black")
    cmd = [
        "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
        "-f", "rawvideo", "-pixel_format", "gray",
        "-video_size", f"{in_w}x{in_h}", "-framerate", str(args.fps), "-i", "-",
        "-i", wav_path,
        *(["-vf", ",".join(filters)] if filters else []),
        *(["-fps_mode", "passthrough"] if args.vfr else ["-r", str(args.fps)]),
        "-c:v", "libx264", "-preset", "ultrafast", *(["-crf", str(args.crf)] if args.crf is not None else []),
        "-pix_fmt", "yuv420p",
        "-c:a", "aac", "-b:a", "192k",
        "-shortest",
        args.output,
    ]
    static = scene_static(args.scene) if args.scene != "plain" else None
    try:
        proc = subprocess.Popen(cmd, stdin=subprocess.PIPE)
        try:
            for v, white in zip(values, whites):
                if static is None:
                    img = make_frame(args.width, args.height, args.cell, v + args.counter_start, white)
                else:
                    # pattern.html には白のマーカーがない (ウィンドウの形では白を描かない)。
                    img = render_scene(args.scene, static, v + args.counter_start,
                                       white and args.scene in ("tab", "tabfill"), args.strip)
                proc.stdin.write(img.tobytes())
            proc.stdin.close()
        finally:
            ret = proc.wait()
    finally:
        os.remove(wav_path)  # ffmpeg が失敗しても一時 wav を残さない
    if ret != 0:
        print("ffmpeg failed", ret, file=sys.stderr)
        sys.exit(1)

    print(f"{args.output}: {len(values)} frames output (nominal {nominal_frames}), "
          f"{len(boundaries)} sync boundaries at {boundaries}")


if __name__ == "__main__":
    main()
