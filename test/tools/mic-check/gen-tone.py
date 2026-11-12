#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

# 標準ライブラリだけで、48 kHz、モノラル、16 bit の WAV (10 秒、1 kHz の正弦波、-6 dBFS) を書く。run.mjs が
# --out の下に毎回作り直すので、生成した WAV はリポジトリに置かない。
#
# 使い方: python gen-tone.py <出力先の .wav のパス>

import math
import struct
import sys
import wave

SAMPLE_RATE = 48000
DURATION_SEC = 10
FREQ_HZ = 1000.0
# -6 dBFS (16 bit の整数のフルスケール 32767 に対する比) = 32767 * 10^(-6/20)
AMPLITUDE = int(32767 * (10 ** (-6.0 / 20.0)))


def build_pcm():
    n = SAMPLE_RATE * DURATION_SEC
    frames = bytearray()
    for i in range(n):
        t = i / SAMPLE_RATE
        sample = int(round(AMPLITUDE * math.sin(2 * math.pi * FREQ_HZ * t)))
        frames += struct.pack("<h", sample)
    return bytes(frames)


def main(argv):
    if len(argv) != 2:
        print("使い方: python gen-tone.py <出力先の .wav のパス>", file=sys.stderr)
        return 2
    out_path = argv[1]
    with wave.open(out_path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)  # 16 bit
        w.setframerate(SAMPLE_RATE)
        w.writeframes(build_pcm())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
