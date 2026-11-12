# 送信モード (HARNESS_MODE=send) の結果 JSON を 1 ファイルずつ要約する。
# 使い方: python summarize-send.py <out フォルダ または JSON>
import glob
import json
import os
import sys

arg = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "..", "..", "_work", "out")
files = [arg] if arg.endswith(".json") else sorted(glob.glob(os.path.join(arg, "harness-send-*.json")))
for f in files:
    d = json.load(open(f, encoding="utf-8"))
    env = d.get("env", {})
    data = d.get("data", {})
    cfg = data.get("config", {})
    print(f"== {os.path.basename(f)} | {env.get('nvidiaSmiRaw', '')} | N={cfg.get('sustainN')} sec={cfg.get('sustainSec')} hw={cfg.get('sustainHw')}")
    print(f"   wsError={data.get('wsError')} aborted={data.get('aborted')} fatal={data.get('fatalError')} syncEvents={len(data.get('syncEvents', []))}")
    for e in data.get("perEncoderTotals", []):
        lat = e.get("encodeLatencyMs") or {}
        print(
            f"   stream {e.get('streamId')}: video sent/out/dropped={e.get('totalVideoSent')}/{e.get('totalVideoOutput')}/{e.get('droppedVideo')} "
            f"audio sent={e.get('totalAudioSent')} maxQueue={e.get('maxQueueSize')} err={e.get('error')} "
            + (f"encodeLatency mean/p95/max={lat.get('mean')}/{lat.get('p95')}/{lat.get('max')} ms" if lat else "")
        )
    ps = data.get("perSecond", [])
    if ps:
        # 各秒の outputsPerSec の最小 (どこかで詰まったか)
        mins = []
        for s in ps:
            for pe in s.get("perEncoder", []):
                mins.append(pe.get("outputsPerSec", 0))
        print(f"   perSecond: {len(ps)} 秒、outputsPerSec の最小={min(mins) if mins else None} (起動直後の 1 秒目を含む)")
