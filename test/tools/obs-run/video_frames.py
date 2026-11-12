# 判定スクリプト (sync.py / frames.py / smooth.py) が共有する、録画からフレームと音を取り出すツール。
#
# cordmux の録画 (約 30 fps、Firefox の共有は約 14 fps の可変) は r_frame_rate が 60000/1001 と出るので、
# 「フレームの番号 / r_frame_rate」で時刻を出すと映像の時刻が半分になる (フレームごとの pts で出すと、
# 音の先行が 142 ms から 10 分後に 173-177 ms と読めた)。さらに ffmpeg の rawvideo の出力は既定で一定の fps へ
# 揃える (足りないフレームを複製し、多いフレームを捨てる) ので、可変の録画では番号そのものが実際のフレームと
# 一致しない (可変 194 フレームの mov が既定では 359 フレームで出た)。
# そこで次の 3 つをここに集める。
#   1. フレームは `-fps_mode passthrough` で 1 枚ずつそのまま出し、時刻は showinfo が書く pts_time を使う
#      (番号から時刻を作らない)。
#   2. 領域は ffmpeg の側で切り出す (crop)。1920x1080 の灰色を 1 フレーム 2 MB ずつ
#      パイプへ流すと、20 分の録画で 1 領域あたり約 4 分かかる (枠 3 つで frames.py 10.7 分、
#      sync.py 12.4 分)。切り出せば流す量は領域の大きさだけになる。
#   3. 音は `aresample=async=1:first_pts=0` で先頭の無音を埋める。素の f32le の出力は音の流れの先頭から
#      始まるので、音が映像より遅れて始まる録画では、音の時刻の原点が映像とずれる
#      (音を 0.5 秒遅らせた mov で、埋めなければ最初の音が 0.003 秒、埋めれば 0.481 秒に出た)。
#
# showinfo の checksum=0 (1 フレームごとのチェックサムを計算しない) は ffmpeg 5.1 以降の
# 指定。受け付けない ffmpeg では素の showinfo でやり直す (CI の Ubuntu は 6.1 系なので通る見込み)。
# ffprobe -show_frames で pts だけを別に読む方法は使わない (デコードが 2 回になり、判定スクリプトの
# 所要が倍になる)。packet の pts を並べ替えて使う方法も使わない (B フレームの並べ替えとデコードの失敗の扱いが
# 判定スクリプトごとに要り、フレームとの対応が保証されない)。
import json
import queue
import re
import subprocess
import threading

import numpy as np

# showinfo の 1 行 (ffmpeg 6.1 / 7.1 / 8.0 で同じ形): "... n:   0 pts:      0 pts_time:0       duration: ..."
SHOWINFO_LINE = re.compile(r"Parsed_showinfo.*?\bn:\s*(\d+)\s+pts:\s*(\S+)\s+pts_time:\s*(\S+)")

PTS_WAIT_TIMEOUT_S = 60.0


def probe_video(path, ffprobe="ffprobe"):
    """映像の 1 本目の幅・高さ・r_frame_rate (字のまま) を返す。"""
    probe = subprocess.run(
        [ffprobe, "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height,r_frame_rate",
         "-of", "json", path], capture_output=True, text=True, check=True)
    st = json.loads(probe.stdout)["streams"][0]
    return {"width": int(st["width"]), "height": int(st["height"]), "r_frame_rate": st.get("r_frame_rate")}


def rate_to_float(text):
    """"60000/1001" のような字を数に修正する (読めなければ None)。"""
    try:
        num, den = str(text).split("/")
        den_f = float(den)
        return float(num) / den_f if den_f else None
    except (ValueError, TypeError):
        return None


def clamp_region(x, y, w, h, frame_w, frame_h):
    """領域を映像の内側に収める。w や h が 0 以下なら映像全体。収まる部分が無ければ None。"""
    if w <= 0 or h <= 0:
        x, y, w, h = 0, 0, frame_w, frame_h
    x0 = max(0, min(int(x), frame_w))
    y0 = max(0, min(int(y), frame_h))
    x1 = max(x0, min(int(x) + int(w), frame_w))
    y1 = max(y0, min(int(y) + int(h), frame_h))
    if x1 <= x0 or y1 <= y0:
        return None
    return x0, y0, x1 - x0, y1 - y0


def _drain_stderr(stream, pts_queue, tail):
    # showinfo の行から pts_time を拾って順に積む。それ以外の行は失敗の理由に使うため末尾だけ残す。
    for raw in stream:
        line = raw.decode("utf-8", "replace")
        m = SHOWINFO_LINE.search(line)
        if m:
            text = m.group(3)
            try:
                pts_queue.put(float(text))
            except ValueError:
                pts_queue.put(None)  # NOPTS など。読む側が誤りにする
        else:
            tail.append(line.rstrip())
            del tail[:-20]
    pts_queue.put(StopIteration)


def _read_exact(stream, size):
    chunks = []
    remaining = size
    while remaining > 0:
        buf = stream.read(remaining)
        if not buf:
            break
        chunks.append(buf)
        remaining -= len(buf)
    return b"".join(chunks)


def iter_gray_frames(path, frame_w, frame_h, region=None, ffmpeg="ffmpeg"):
    """(pts 秒, 灰色の 2 次元配列) を 1 フレームずつ返す。region = (x, y, w, h) なら ffmpeg の側で切り出す。
    (中身は iter_frames の pix_fmt gray。)"""
    return iter_frames(path, frame_w, frame_h, region=region, ffmpeg=ffmpeg, pix_fmt="gray")


# iter_frames が受けるピクセルの形と、1 ピクセルのバイト数。rgb24 は名前ラベルのプレートと話している人の光の色を読むために使う。
PIX_FMT_BYTES = {"gray": 1, "rgb24": 3}


def iter_frames(path, frame_w, frame_h, region=None, ffmpeg="ffmpeg", pix_fmt="gray"):
    """(pts 秒, ピクセルの配列) を 1 フレームずつ返す。gray は (高さ, 幅)、rgb24 は (高さ, 幅, 3)。region = (x, y, w, h) なら
    ffmpeg の側で切り出す。

    pts はファイルの先頭 (ffmpeg が全部の流れから引く start_time) からの秒で、音を read_mono_pcm で
    読んだときの原点と同じ。pts を読めないフレームがあれば RuntimeError (番号から時刻を作って黙って
    続けない)。
    """
    if region is None:
        x, y, w, h = 0, 0, frame_w, frame_h
    else:
        x, y, w, h = region
    if pix_fmt not in PIX_FMT_BYTES:
        raise ValueError("pix_fmt must be one of " + ", ".join(PIX_FMT_BYTES))
    channels = PIX_FMT_BYTES[pix_fmt]
    filters = ["format=" + pix_fmt]
    if (x, y, w, h) != (0, 0, frame_w, frame_h):
        filters.append("crop=%d:%d:%d:%d" % (w, h, x, y))
    frame_bytes = w * h * channels
    last_error = None
    for showinfo in ("showinfo=checksum=0", "showinfo"):
        cmd = [ffmpeg, "-hide_banner", "-nostats", "-loglevel", "info", "-i", path, "-map", "0:v:0",
               "-an", "-sn", "-dn", "-vf", ",".join(filters + [showinfo]), "-fps_mode", "passthrough",
               "-f", "rawvideo", "-pix_fmt", pix_fmt, "-"]
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        pts_queue = queue.Queue()
        tail = []
        drainer = threading.Thread(target=_drain_stderr, args=(proc.stderr, pts_queue, tail), daemon=True)
        drainer.start()
        produced = 0
        try:
            while True:
                buf = _read_exact(proc.stdout, frame_bytes)
                if len(buf) < frame_bytes:
                    break
                try:
                    pts = pts_queue.get(timeout=PTS_WAIT_TIMEOUT_S)
                except queue.Empty:
                    pts = StopIteration
                if pts is StopIteration or pts is None:
                    raise RuntimeError("no pts for frame %d of %s (showinfo did not report it)" % (produced, path))
                produced += 1
                arr = np.frombuffer(buf, dtype=np.uint8)
                yield pts, (arr.reshape(h, w) if channels == 1 else arr.reshape(h, w, channels))
            # 出力を読み切った (ffmpeg は終わるところ)。終わるのを待って本当の終了コードを取る (待たずに下で kill すると
            # 終了コードが kill のものになり、フレームが 0 の正しい録画まで「失敗」としてやり直し、最後に例外になる)。
            # 途中で読むのをやめた経路 (呼び出し元が閉じた・例外) だけが下の kill を通る。
            proc.wait()
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.wait()
            drainer.join(timeout=5)
        if produced > 0 or proc.returncode == 0:
            return
        # 1 フレームも出ずに失敗した (古い ffmpeg が checksum の指定を知らない等)。素の showinfo でやり直す。
        last_error = " | ".join(tail[-5:])
    raise RuntimeError("ffmpeg could not decode %s: %s" % (path, last_error))


def read_mono_pcm(path, sample_rate=48000, ffmpeg="ffmpeg"):
    """音を 48 kHz モノラルの float32 で返す。頭の無音を埋めて、サンプル 0 = ファイルの先頭 (映像の pts と同じ原点)。"""
    pcm = subprocess.run(
        [ffmpeg, "-hide_banner", "-nostats", "-loglevel", "error", "-i", path, "-vn",
         "-af", "aresample=%d:async=1:first_pts=0" % sample_rate, "-ac", "1", "-ar", str(sample_rate), "-f", "f32le", "-"],
        capture_output=True, check=True).stdout
    return np.frombuffer(pcm, dtype=np.float32)


def median_interval(times):
    """フレームの時刻の並びから、隣り合う差の中央値 (秒) を返す (2 フレーム未満なら None)。"""
    if len(times) < 2:
        return None
    return float(np.median(np.diff(np.asarray(times, dtype=np.float64))))


def fps_from_pts(times):
    """フレームの時刻の並びから平均の fps を返す (区間が 0 なら None)。"""
    if len(times) < 2:
        return None
    span = float(times[-1]) - float(times[0])
    return (len(times) - 1) / span if span > 0 else None
