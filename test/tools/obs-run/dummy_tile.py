# 判定スクリプト: ダミー表示が「cordmux 画面 N」に色タイルを描いているかを、撮った PNG
# (screenshot-source が obs-websocket GetSourceScreenshot で書いたもの) から判定する。
#
# 実行スクリプト (--dock-scenario dummy) が、人手でスクリーンショットを見ずに、色タイルが
# 出ているかを判定するために使う。frames.py と同じく ffmpeg で生のピクセルを読み取る。判定は
# 全ピクセルの平均を取るだけで行列演算が要らないので、numpy を使わず標準ライブラリだけで行う
# (frames.py / sync.py / smooth.py はフレーム単位のピクセルの切り出しが要るので numpy を使う)。
#
# 使い方: python dummy_tile.py <PNG のパス> [--threshold 8]
#   --threshold: 平均が黒 (0) からこの値を超えて離れていれば「色タイルが出ている」とする
#                (0-255 の RGB 値の平均に対するしきい値。既定の 8 は目安である。PNG 化と色空間変換の
#                丸めで真っ黒でも 1-2 程度は揺れうるが、8 あればそのノイズと実際の色タイルを見分けられる)。
import json
import subprocess
import sys

DEFAULT_THRESHOLD = 8.0


def probe_size(path):
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height", "-of", "csv=p=0:s=x", path],
        capture_output=True, text=True, check=True,
    )
    w_str, h_str = out.stdout.strip().split("x")
    return int(w_str), int(h_str)


def read_rgb24(path, width, height):
    proc = subprocess.run(
        ["ffmpeg", "-hide_banner", "-nostats", "-loglevel", "error", "-i", path, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        stdout=subprocess.PIPE, check=True,
    )
    return proc.stdout


def main():
    path = sys.argv[1]
    threshold = DEFAULT_THRESHOLD
    for i, a in enumerate(sys.argv):
        if a == "--threshold":
            threshold = float(sys.argv[i + 1])

    width, height = probe_size(path)
    buf = read_rgb24(path, width, height)
    expected_bytes = width * height * 3
    if len(buf) < expected_bytes:
        print(json.dumps({
            "file": path, "ok": False,
            "error": "read fewer bytes than expected (%d/%d)" % (len(buf), expected_bytes),
        }))
        sys.exit(1)

    # 標準ライブラリだけで平均を取る (numpy を使わない = 上の由来欄)。
    mean = sum(buf) / len(buf)
    not_black = mean > threshold

    print(json.dumps({
        "file": path,
        "width": width,
        "height": height,
        "mean": mean,
        "threshold": threshold,
        "notBlack": not_black,
    }))


if __name__ == "__main__":
    main()
