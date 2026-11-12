"""架空のツール。まとめを出力する。"""
import os


def write_summary(out_dir):
    # 出力するファイルの名前を、コードの中にその名前だけの文字列として書く (文書からファイル名だけで指せる)。
    with open(os.path.join(out_dir, "summary.md"), "w", encoding="utf-8") as f:
        f.write("ok\n")
