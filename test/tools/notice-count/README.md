# test/tools/notice-count

cordmux のログから、メッセージの表示と消去 (`notices`) と、映像、接続、画面共有の終わり、最初のフレームにかかる時間 (`timing`) を数える `notice_count.py`。ログを読むだけで、OBS も Vesktop も起動しない。

## 読むログ

- OBS のログ (`HH:MM:SS.mmm: ` で始まる行)。ファイル名が `YYYY-MM-DD HH-MM-SS.txt` なら、日付をファイル名から取る。ほかの名前なら `--date` で渡す。
- 診断情報の zip (中の `plugin.log`) と、`plugin.log`。行は OBS のログと同じ `HH:MM:SS.mmm: ` で始まり、日付は zip の `manifest.json` の作成時刻 (`taken_at_us` と `utc_offset_sec`) から取る (`plugin.log` だけなら `--date`)。古いバージョンの `plugin.log` の行には時刻がないので、件数だけを数え、時刻が要る数は「読めない」と書く。
- メッセージの行は、`src-obs/dock/dock_poll.cpp` の `report_notice_changes` が書く次の 2 つ。`tier` はメッセージのレベル (`row` は人の行の状態)、`subject` は枠の番号である。

  ```
  [cordmux] Notice shown (rule=<規則の名前> tier=<1|2|3|row> subject=<slot n か ->)
  [cordmux] Notice cleared (rule=<規則の名前> tier=<1|2|3|row> subject=<slot n か ->)
  ```

`--selftest` は、この形と、`timing` が読む行 (`src-obs/ws/listener.cpp` の接続の行、`src-obs/pipeline/pipeline.cpp` の stats と facts、`src-vencord` の ws classified、screen entry、stage、ws closed) の文字列が、製品のソースに今もあるかを確認する。製品のログの文字列を変えたら、notice_count.py の `CONNECT_UP_MARKS` などを合わせる。

## notices

```
python test/tools/notice-count/notice_count.py notices <ログ>... [--normal] [--build-writes-notices]
    [--fault <規則>@<時刻>/<秒>]... [--faults <TSV>]
    [--chatter-count 3] [--chatter-window-s 60] [--chatter-event shown|both] [--markdown]
```

- 数えるもの:
  - メッセージのレベルごとの表示の件数 (メッセージのレベル 1、2、3 と人の行の状態)
  - 規則ごとの表示と消去の回数
  - 表示から消去までの長さ (消えないまま終わったものを含む)
  - チャタリング (同じ規則と対象の表示を、任意の 60 秒の中で 3 回以上)。数え方の理由は notice_count.py の `find_chatter` のコメントにある。
- `--normal`: メッセージのレベル 1 と 2 が 0 件であることと、チャタリングが 0 件であることを判定する。メッセージのレベル 1 の誤検知がないかは、人が判断する項目として書く。
- `--fault` と `--faults`: 故障を起こした時刻と、メッセージが表示されるまでの上限の秒数を、故障ごとに渡す (ツールは上限の既定値を持たない)。TSV の列は `rule`、`at` (`HH:MM:SS[.mmm]` か `YYYY-MM-DD HH:MM:SS`)、`within_s`、`note`。`#` で始まる行と空行は飛ばす。
- メッセージの行が 0 行のログは、合否を「読めない」にする。メッセージの行を書かない古いバージョンのログと、メッセージが表示されなかったログを区別できないためである。行を書くバージョンのログなら `--build-writes-notices` を付ける。
- 終了コード: 成功は 0、失敗は 1、判定できない項目があれば 2。

## timing

```
python test/tools/notice-count/notice_count.py timing <OBS のログ>... [--from HH:MM:SS] [--to HH:MM:SS] [--utc-offset +09:00] [--markdown]
```

映像、画面共有の終わり、通話を抜けた回数、最初のフレームを数えるには、`--verbose` で起動した OBS のログを渡す (実機テストのツール `test/tools/obs-run` は `--verbose` で起動する)。`--verbose` を付けずに起動した OBS のログでは、これらは 0 件になる。stats、facts と送信側 (`[cordmux/js]`) の行は LOG_DEBUG で、OBS は `--verbose` で起動したときだけ、LOG_DEBUG の行をログのファイルに書くためである。

- 映像: ストリームごとの 10 秒ごとのフレーム数 (毎秒の数の最小、1%、5%、中央値)、途中でフレームが 0 になった周期、不連続 (推定した周期の 1.5 倍を超えた間隔) の回数。1 フレームずつの間隔はログにないので読めない。
- 接続: Vesktop との切断 (close、ハートビート、ハンドシェイクの失敗) の回数と、再接続までの秒数。
- 画面共有の終わり: 送信側の `ws closed ... role=golive` から、OBS 側で facts の sharing が減るまでの秒数。画面共有をやめる操作の時刻はログにない。
- 通話を抜けた回数: facts の roster が減った回数だけを数える (時刻の付いた行がない)。
- 最初のフレーム: 視聴の開始 (golive の接続を分類した時刻) から最初のエンコードの段階までと、再接続の screen entry から最初の段階まで (どちらも送信側の時刻)。
- `--utc-offset` は、送信側の UTC の時刻を OBS のログの時刻に修正する差。既定は、このツールを実行する PC の時間帯 (ログを取った PC で実行する前提)。
- `--from` と `--to` は、準備の手順とメインの手順が 1 つのログに入るときに、メインの手順だけを数えるために使う。
