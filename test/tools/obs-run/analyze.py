# 録画 (または配信の受け口が書いた flv) を検分する。
# 使い方: python analyze.py <ファイル> [期待する周波数 Hz (既定 1000)]
# 出すもの (JSON): ストリームの一覧、長さ、映像のフレーム数、音の平均と最大の音量 (dBFS)、
#                  期待する周波数の成分が全体に占める割合 (Goertzel)、無音かどうかの判定
import json
import math
import struct
import subprocess
import sys

path = sys.argv[1]
target_hz = float(sys.argv[2]) if len(sys.argv) > 2 else 1000.0
out = {"file": path}

probe = subprocess.run(
    ["ffprobe", "-v", "error", "-show_entries",
     "stream=index,codec_type,codec_name,width,height,r_frame_rate,sample_rate,channels:format=duration",
     "-of", "json", path], capture_output=True, text=True, check=True)
info = json.loads(probe.stdout)
out["streams"] = info.get("streams", [])
out["duration"] = float(info.get("format", {}).get("duration", 0))

has_video = any(s["codec_type"] == "video" for s in out["streams"])
has_audio = any(s["codec_type"] == "audio" for s in out["streams"])

if has_video:
    cnt = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-count_frames", "-show_entries",
         "stream=nb_read_frames", "-of", "csv=p=0", path], capture_output=True, text=True, check=True)
    out["video_frames"] = int(cnt.stdout.strip() or 0)

if has_audio:
    # 音量: ffmpeg の volumedetect
    vd = subprocess.run(
        ["ffmpeg", "-hide_banner", "-nostats", "-i", path, "-vn", "-af", "volumedetect", "-f", "null", "-"],
        capture_output=True, text=True)
    for line in vd.stderr.splitlines():
        if "mean_volume" in line:
            out["audio_mean_db"] = float(line.split("mean_volume:")[1].split("dB")[0])
        if "max_volume" in line:
            out["audio_max_db"] = float(line.split("max_volume:")[1].split("dB")[0])
    # 期待する周波数の成分: 最初の 3 秒を 48kHz モノラル f32 で取り出して Goertzel
    sr = 48000
    pcm = subprocess.run(
        ["ffmpeg", "-hide_banner", "-nostats", "-i", path, "-vn", "-t", "3", "-ac", "1", "-ar", str(sr),
         "-f", "f32le", "-"], capture_output=True, check=True).stdout
    n = len(pcm) // 4
    samples = struct.unpack("<%df" % n, pcm[: n * 4])
    total = sum(s * s for s in samples) or 1e-12

    def goertzel(freq):
        k = int(0.5 + n * freq / sr)
        w = 2 * math.pi * k / n
        c = 2 * math.cos(w)
        s0 = s1 = s2 = 0.0
        for x in samples:
            s0 = x + c * s1 - s2
            s2 = s1
            s1 = s0
        power = s1 * s1 + s2 * s2 - c * s1 * s2
        return power * 2 / n  # 全体のエネルギーと同じ尺度に寄せる

    out["target_hz"] = target_hz
    out["target_ratio"] = goertzel(target_hz) / total
    out["other_ratio_440"] = goertzel(440.0) / total
    out["audio_samples_checked"] = n
    out["audio_is_silent"] = out.get("audio_max_db", -99) < -60
    out["audio_has_target_tone"] = out["target_ratio"] > 0.5

print(json.dumps(out, indent=1, ensure_ascii=False))
