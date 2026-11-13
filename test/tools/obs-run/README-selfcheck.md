# obs-run 判定スクリプトのセルフテスト

frames.py / sync.py / smooth.py / analyze.py に、わざと壊したテストデータを渡す。正常なデータで GREEN、壊した箇所に対応する判定スクリプトで RED になることを確認する。実行は `python selfcheck.py` である。既定で `build_x64/selfcheck/` にテストデータを作り、判定の後に消す。

## テストデータの作り方 (make_material.py)

ffmpeg (libx264 ultrafast) と numpy だけで mp4 を作る。640x360、60fps、約 20 秒。左上に 32 bit のフレーム番号をセル幅 8px で描く。smooth.py の既定 (`--regions` なし) がセル位置 (0,0)/(960,0)/... とセル幅 8px なので合わせてある。frames.py には `--cell 8` を渡す。5 秒ごとに、全面が白のフレームを 3 つと、同じ時刻に 60ms だけ 2kHz のマーカー音を入れる。

壊し方の引数:

- `--drop-every`: フレームを一定の間隔で抜く
- `--freeze-ms` と `--freeze-at`: 映像を凍らせる
- `--dup-every`: フレームを重複させる
- `--audio-shift-ms`: 音をずらす
- `--competing-1k A`: マーカーとは別に、振幅 A の 1 kHz を鳴らし続ける (テスト用のマイクの 1 kHz が録画に混ざった形)
- `--vfr`: 奇数番目のフレームを、13 フレームに 1 回を除いて落とし、残りは元の時刻のまま置く (可変フレームレート)
- `--audio-step A:B:D`: A 秒から B 秒の前までのマーカー音だけを、さらに D ms ずらす (一時的な段)
- `--audio-drift-ms-per-min R`: マーカー音のずれを、分あたり R ms ずつ増やす (時間とともに広がるずれ)
- `--scene window` / `tab` / `tabfill`: 実際の画面共有の形 (後述)

## 結果 (16 通り。すべて PASS、終了コード 0)

| テストデータ | frames.py | sync.py | smooth.py | analyze.py |
|---|---|---|---|---|
| 正常 | GREEN | GREEN | GREEN | GREEN |
| 1 フレーム抜け | RED (gap=1) | GREEN | RED (missing=1) | GREEN |
| 凍り 100ms | RED (dup=5, 100.0ms) | GREEN | RED ([5,5,0]) | GREEN |
| 凍り 300ms | RED (dup=17, 300.0ms) | GREEN | RED ([17,17,0]) | GREEN |
| 凍り 500ms | RED (dup=29, 500.0ms) | GREEN | RED ([29,29,0]) | GREEN |
| 音 100ms ずれ | GREEN | RED (mean=100.0ms) | GREEN | GREEN |
| 重複 | RED (dup=1, 33.3ms) | GREEN | RED ([0,1,0]) | GREEN |
| 1 kHz が鳴り続ける (competing1k、振幅 0.5 = マーカーと同じ) | GREEN | GREEN (3 対、0.0ms) | GREEN | GREEN |
| 可変フレームレート (vfr、平均 32.4 fps、r_frame_rate 60) | RED (gap=552) | GREEN (3 対、0.0ms) | RED ([544,0,0]) | GREEN |
| ウィンドウの共有 (window) | GREEN (clock、first=1085000、読めないフレーム 0) | - | GREEN (clock、[0,0,0]、読めないフレーム 0) | - |
| ウィンドウの共有で 1 フレーム抜け (window_drop1) | RED (clock、gap=1、missing=1) | - | RED (clock、[1,0,0]) | - |
| タブの共有 (tab。縦横の倍率が違う。動画が枠の約 53%) | GREEN (clock、first=31700、読めないフレーム 0) | GREEN (白 3・対 3、0.0ms) | GREEN (clock、[0,0,0]、読めないフレーム 0) | GREEN |
| タブの共有 (tabfill。動画が枠の全部) | GREEN (clock、first=31700、読めないフレーム 0) | GREEN (白 3・対 3、0.0ms) | GREEN (clock、[0,0,0]、読めないフレーム 0) | GREEN |
| 古い形の帯のウィンドウの共有 (window_legacy) | NOTFOUND (固定位置の値が 1 種類 = 0 のまま) | - | NOTFOUND (同じ) | - |
| 音が一時的に 30 ms 遅れる段 (audiostep。150 秒、30 fps) | - | GREEN (29 対、段 1 つ: 60-80 秒、30.0 ms、戻った。段を除いた傾き 0.0 ms/分) | - | - |
| 音のずれが分あたり 12 ms 広がる (audiodrift。150 秒、30 fps) | - | GREEN (29 対、段なし。傾き 12.18 ms/分、95% の幅 11.37-12.99) | - | - |

- sync.py の判定の境目は 20ms である。正常の実測は 0.0ms、100ms ずらした実測は 100.0ms なので、その中間に置いた。
- smooth.py の帯は領域ごとに選ぶ。古い形の帯 (legacy) を読むデータと、時計つきの帯 (clock) を読む共有のデータがある。

audiostep と audiodrift の意味 (sync.py の「一時的な段と、時間とともに広がる傾きを分ける」):

- audiostep: 11-13 の実際の通話の段 (約 25 秒だけ 20-30 ms 上がって戻る) の形。段が 1 つ見つかって戻り、段を除いた基準の傾きが 1 ms/分より小さいことを確かめる (全部の組に 1 本の直線を当てる読み方では、段の分の傾きが立つ)。
- audiodrift: 時間とともに広がるずれ。段とは読まず、基準の傾きが 12 ms/分の近くで、95% の幅の下の端が 1 ms/分より上 (欠陥と読める) ことを確かめる。音の始まりは 5 ms の刻みで読むので、ずれは階段になる。
- 傾きを判定する長さ (組 10 以上、120 秒以上) にするため 150 秒にし、作る手間を減らすため 30 fps にして、sync.py だけを当てる。

competing1k と vfr の意味:

- competing1k: 「2 kHz が 1 kHz の 2 倍を超える範囲」でマーカーを探す方式は、同じデータで対が 0 になる (マーカーが 1 つも見つからない)。sync.py は 3 対を見つける。
- vfr: sync.py はフレームの pts で時刻を出すので GREEN になる。frames.py と smooth.py はセルの番号が 2 つずつ進むので、欠落として RED になる (正しい動作)。pts で読めば可変フレームレートでも合うところまでを確認するデータであり、出力のフレーム数と r_frame_rate 60000/1001 が食い違う形までは作れていない。

## 共有の形の 5 通り

frames.py は、領域の中で時計つきの帯を探して読む。次の 5 通りでその動作を確認する。

- `--scene window`: 1920x1080 の Firefox のウィンドウ。上の 114 px にタブの列・アドレスの欄・ブックマークの欄を置き、文字に似た明るい短い線を散らして紛らわしくする。ページの背景は 32、余白は 8 px。その後に `pattern.html` の canvas (背景 58 と格子 68、往復する円、大きな数字の代わりの白い枠、左上にセル 32 px の時計つきの帯) を描く。
- `--scene tab`: 1920x905 の Chrome のタブの表示域。余白 12 px の後に `sync-video.mp4` の映像 (勾配の背景、セル 24 px の帯、5 秒ごとの白の 3 フレーム) を描く。
- `--scene tabfill`: 1280x720 の `sync-video.mp4` の映像を、ffmpeg で 1920x905 (表示域。`object-fit: fill`) に引き伸ばす。帯は枠の左上の角に写る。
- `window_drop1`: window から 1 フレーム抜いたもの。
- `window_legacy`: 古い形の帯 (16 px のセル 32 個) を window と同じウィンドウに描いたもの。

window と tab は ffmpeg で 1280x720 (共有。tab は縦横の倍率が違う) に縮めてから 640x360 (枠の 0.5 倍) にする。tabfill は 1280x720 に縮めてから 640x360 にする。どれも 960x540 の黒の左上に置き、crf 30 (既定の 23 より粗い) でエンコードする。共有と録画で 2 度圧縮されたように、帯の縁を崩すためである。

判定スクリプトへの引数と、確認する項目:

- frames.py には実行スクリプトと同じ領域の引数 (`--x 0 --y 0 --w 640 --h 360 --cell 8`) を渡す。色に加えて、出力のキー(`layout`・`first`・`unreadable_frames`・`gap_count` など) が期待どおりかを見る (`selfcheck.py` の `checks`)。
- smooth.py は 5 通り全部に `1 --regions 0,0,640,360 --cell 8` で渡す。`checks` で `cells.0.layout` (`clock`)・`cells.0.total_missing_dup_back`・`cells.0.unreadable_frames` を見る。`window_legacy` は frames.py と同じく `barcode_found: false` (`NOTFOUND`) になる。
- sync.py と analyze.py は tab と tabfill に渡す (`pattern.html` の形には白のマーカーも音もない)。sync.py には `--x 0 --y 0 --w 640 --h 360` を渡す。
  - tab では、白 3・対 3 に加えて `white_detector.mean_over_200_events` が 0 であることを見る。これは同じフレームに「領域の平均が 200 超え」の規則を当てた白の数である。白のフレームの領域の平均は 139 なので、この規則では白が見つからない。sync.py の規則は「直前 4 フレームより明るくなった点の割合」で、白のフレームで 0.45 (境目 0.2) になる。
  - tabfill の白のフレームの割合は 0.84 である。
  - `trend` (全部の対で、ずれの分あたりの傾きと、先頭と末尾の 5 対の平均) も渡す。ずれのないデータなので、対 3・傾き 0.0・先頭の平均 0.0 になる。傾きの標準誤差 `trend.slope_se_ms_per_min` (「0 と分けられるか」の判断に使う) も、残差がないので 0.0 になる。
- smooth.py は、古い形の帯の番号が 1 つも動かない領域を「見つからない」とする (`frames.py` の `legacy_probe` を使う)。ずっと凍った領域は「重複 n-1」ではなく `NOTFOUND` になる。
- 失敗だけでは「帯がない」と「凍った」を区別できない。frames.py は `barcode_found: false` を返し、selfcheck.py はそれを `NOTFOUND` として期待と比べる。

`bit_strip.py --selftest` は、合成の画像 (ffmpeg を使わない) で次を確認する。

- 倍率 1/3・1/6・1.0・0.5 x 0.6・1/3 x 0.398 とぼかしの後も、位置とセルの大きさを測って読める。
- 古い形の帯、暗い画面、右半分しかない帯では見つからない。
- CRC が合わないフレームを分ける。

## smooth.py の判定の境目

しきい値はない。凍り 100/300/500ms でも、単発の重複 (33ms 相当、dup-every 1 回) でも、`total_missing_dup_back` は最小の壊し方からすでに 0 でなくなる。selfcheck.py は frames.py と同じく「1 件でも RED」の 2 値で判定する。境目は実質 0ms である。

## 判定スクリプトの引数と制約

- smooth.py は `--regions x,y,w,h;...` と `--cell N` を受け取る。実行スクリプトは、OBS のシーンアイテムから求めた枠の位置と大きさを渡す。
- smooth.py と frames.py は、領域の中で帯を探す。共有ではタブの列や余白の下に帯が写るため、領域の左上だけを読むと番号が読めない。見つからない領域は件数を出さず、`barcode_found: false` を返す。
- smooth.py は生の件数を返すだけで、GREEN と RED の判定を持たない。しきい値は呼び出し側が決める。
- sync.py の note は「正の値 = 音が映像より遅れ」である。音を 100ms 遅らせたデータで `audio_minus_video_ms` が +100 になる。
