# ハーネスの結果 JSON をまとめて 1 行ずつにする。
# 使い方: python summarize.py <out フォルダ>
import glob
import json
import os
import sys

out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "..", "..", "_work", "out")
for f in sorted(glob.glob(os.path.join(out, "harness-*.json"))):
    d = json.load(open(f, encoding="utf-8"))
    env = d.get("env", {})
    data = d.get("data", {})
    b = data.get("b", {})
    c = data.get("c", {})
    dd = data.get("d", {})
    e = data.get("e", {})
    b_ok = sum(1 for a in b.get("attempts", []) if a.get("ok"))
    b_err = [f"{a['n']}:{a['error'].get('name') if isinstance(a.get('error'), dict) else a.get('error')}" for a in b.get("attempts", []) if not a.get("ok")]
    c_ok = sum(1 for r in c.get("results", []) if r.get("ok"))
    c_err = [f"{r['index']}:{(r['error'] or {}).get('name') if isinstance(r.get('error'), dict) else r.get('error')}" for r in c.get("results", []) if not r.get("ok")]
    fifth = dd.get("fifth", {})
    after = dd.get("afterFour", [])
    after_ok = sum(1 for r in after if r.get("ok"))
    print(
        f"{os.path.basename(f)[5:24]} | enc-util-before={env.get('nvidiaSmiRaw', '').split(',')[-1].strip()} "
        f"| B sequential ok={b_ok}/{len(b.get('attempts', []))} err={b_err} "
        f"| C simultaneous ok={c_ok}/{len(c.get('results', []))} err={c_err} "
        f"| D fifth ok={fifth.get('ok')} after4 ok={after_ok}/{len(after)} "
        f"| E software ok={e.get('ok')} | aborted={data.get('aborted')} fatal={data.get('fatalError')}"
        + (" | " + env["nvidiaSmiRaw"].split("|", 1)[1].strip() if "|" in env.get("nvidiaSmiRaw", "") else "")
    )
