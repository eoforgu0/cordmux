# ワイヤー形式

送信側 (Vesktop の中で動く cordmux) と受信側 (OBS の中で動く cordmux.dll) は、1 本の WebSocket で接続する。この文書は、その WebSocket で送るメッセージの形式を決める。形式を変えるときは、この文書と両側の実装 (「実装の場所」) を同じ変更で修正する。

## 概要

- 受信側が 127.0.0.1 で待ち受け、送信側がクライアントとして接続する。受信側は 4470 番から 4485 番のポートを順に試し、最初に待ち受けを始められた番号を使う。
- ポート番号と、接続の認証に使うトークンは、受信側がトークンファイルに書く。送信側は、接続を試すたびにこのファイルを読む (「トークンファイル」)。
- 同時に受け付ける接続は 1 つだけである。
- メッセージは 2 種類ある。映像と音声はバイナリメッセージ (「バイナリメッセージ」) で、それ以外は制御用の JSON のテキストメッセージ (「制御用の JSON」) で送る。WebSocket の 1 メッセージに、映像の 1 アクセスユニット、音声の 1 チャンク、JSON の 1 オブジェクトのどれか 1 つを入れる。

接続から切断までの流れは次のとおりである。

```
送信側 (Vesktop)                              受信側 (cordmux.dll)
    |  HTTP の Upgrade (Host、Origin)             ->  |
    |  hello                                      ->  |
    |  <-  hello_ok (通らなければ hello_ng)           |
    |  <-  recording、media、limits                   |
    |  <-  main_account、receive_scope                |
    |  configure、log、facts、profiles、avatar  ->  |
    |  映像と音声のバイナリメッセージ             ->  |
    |  <-  ping                                       |
    |  pong                                       ->  |
    |  <-  keyframe_request                           |
    |  <-  stream_check (映像が届かないとき)          |
    |  stream_check_result                        ->  |
    |  <-  move_to_main_call、leave_call (押したとき) |
```

1. 受信側は、WebSocket のハンドシェイクで Host と Origin を確認する (「Origin と Host の検証」)。
2. 送信側は、最初のメッセージとして hello を送る。受信側は hello を、送信側はその返事を決まった時間だけ待ち、届かなければ接続を切る (「メッセージの種類」の hello の扱いと、hello_ok と hello_ng の扱い)。
3. 受信側は、hello が通れば hello_ok を返し、続けて recording、media、limits、main_account、receive_scope を 1 つずつ送る。hello が通らなければ hello_ng を返してから接続を切る。
4. 送信側は、hello_ok を受け取った直後に configure、log、facts、profiles、avatar を送る。log、profiles、avatar は、送るものがないときは送らない。その後に送るときは、メッセージごとに決めてある (「メッセージの種類」)。
5. 受信側は決まった間隔で ping を送り、送信側は pong を返す。pong が返らないまま続くと、受信側は接続を切る (「メッセージの種類」の ping と pong の扱い)。

接続が切れた後に試し直す間隔は、docs/architecture.md の「起動、再接続、異常時の動作」にある。

## バイナリメッセージ

バイナリメッセージは、14 バイトのヘッダーと、その後ろのペイロードからなる。ペイロードの長さはヘッダーに持たない。WebSocket のメッセージの長さから 14 を引いた値がペイロードの長さである。数値はリトルエンディアンで並べる。

受信側は、次のどれかに当てはまるメッセージを捨てて `ListenerStats::dropped_invalid_frames` に数える。接続は切らない。送信側のヘッダーのデコード (`src-vencord/core/wire.ts` の `decodeHeader`) も、同じ条件で不正とする。

- 14 バイトより短い
- type が 1 でも 2 でもない
- flags の未使用のビット (ビット 2 から 7) のどれかが 1 である

1 つのメッセージのサイズの上限は、受信側の `ListenerConfig::max_message_bytes` (既定 64 MiB) である。目安の値で、測って決めたものではない。上限を超えたメッセージを受け取ると、websocketpp 0.8.2 は close のコード 1009 (message too big) で接続を閉じる (websocketpp の `connection_impl.hpp` の `handle_read_frame`)。これはソースを読んで確認したことで、上限を超えるメッセージを送って確認してはいない。

### ヘッダー

| オフセット | バイト数 | 名前 | 型 | 中身 |
|---|---|---|---|---|
| 0 | 1 | type | 符号なし整数 | メッセージの種類 (「type」) |
| 1 | 4 | stream_id | 符号なし整数 | ストリームの番号 |
| 5 | 8 | timestamp_us | 符号なし整数 | タイムスタンプ (「timestamp_us」) |
| 13 | 1 | flags | ビットの並び | フラグ (「flags」) |
| 14 | 残り全部 | payload | type ごとに違う | 「映像のペイロード」と「音声のペイロード」 |

stream_id は送信側が振る。番号は送信側のプラグインが動き始めるたびに 1 から始め、新しいストリームができるたびに 1 ずつ増やす。画面共有と声は同じ通し番号から番号を取り、プラグインが動いている間は同じ番号を使い直さない。どの番号が誰のどの種類のストリームかは、configure (「メッセージの種類」) が伝える。

画面共有のストリームは、同じ stream_id で映像と音声の両方を送る。声のストリームは音声だけを送る。音声のメッセージには画面共有の音か声かを示すフィールドがないので、受信側は configure の kind で見分ける。受信側は、直近の configure にない stream_id のメッセージを表示に使わない。Vesktop を再起動すると、同じ番号が別の人に使われることがあるので、受信側は接続が切れたときに stream_id と人の対応を捨てる。

### type

| 値 | 意味 |
|---|---|
| 1 | 映像 (「映像のペイロード」) |
| 2 | 音声 (「音声のペイロード」) |
| 0 と 3 以上 | 使わない。受け取った側は捨てる |

### timestamp_us

送信側の時計の値 (マイクロ秒) である。送信側の時計は「ページの原点の値 + `performance.now()`」を us にした値で (`src-vencord/encode/sender_clock.ts`)、Windows の単調な時計 (QueryPerformanceCounter) で進み、システム時刻に合わせ直さない。ページの原点の値の決め方は 2 つあり、送信側は初めて接続を試す直前に決め、ページが続く間は変えない (決めるまでは接続しない)。送信側がページの原点の Chromium の TimeTicks の値を求められたとき (`src-vencord/encode/ticks_origin.ts`) は、その値を使う。このとき送信側の時計は QueryPerformanceCounter の us そのもので、OBS の `os_gettime_ns` / 1000 と同じ値になり、送信側は hello に clock_base の "qpc" を載せる。求められなかったときは `performance.timeOrigin` を使う。このときの値はエポック (1970 年 1 月 1 日 UTC) に近い大きさから始まるが、システム時刻ではなく、原点は Vesktop のレンダラーを起動したときに決まり、起動し直すと変わる。

- 映像: エンコーダに渡したフレームの `VideoFrame.timestamp` を、送信側の時計にした値。タイムスタンプが WebRTC の描く予定の時刻で、送信側の時計が clock_base の "qpc" のときは、`VideoFrame.timestamp` そのもの。
- 音声: チャンクの先頭のサンプルが入っていた `AudioData` の `timestamp` を送信側の時計にした値に、その `AudioData` の中のサンプルの位置の時間を足した値。それまでに送ったサンプルの数からは計算しない。
- 黒埋め用のフレーム (「flags」のビット 1): そのまとまりの最初のフレームを作ったときの送信側の時計に、フレームの周期の倍数を加えた値。

`AudioData.timestamp` はページの原点からの時間 (`performance.now()` と同じ基準) なので、送信側はページの原点の値を足して、見積もりなしに送信側の時計にする (`src-vencord/audio/chunk.ts`。基準はトラックの最初の断片で確かめる)。Discord の画面共有の `VideoFrame.timestamp` は、WebRTC が決めた描く予定の時刻の TimeTicks の値なので、送信側の時計が clock_base の "qpc" なら、そのまま使う。送信側は、映像のタイムスタンプが描く予定の時刻かを、読んだ時刻との差でストリームの途中も見分け続ける (`src-vencord/encode/video_time.ts`)。描く予定の時刻でないとき (受けた側が往復の時間を測れる接続での取り込みの時刻と、最初のフレームからの経過)、送信側の時計が "qpc" でないとき、音声の基準が分からないときは、トラックごとの定数を足す見積もりの換算を使う (`src-vencord/encode/clock_map.ts`)。定数は最初の「届いた時刻 - メディアのタイムスタンプ」で、その後は、2 秒の期間ごとに見たこの差の小さいほうから 3 番目が今の定数より小さければ、すぐその値まで下げる。直近 20 秒のどの期間でもこの値が今の定数より上 (2 ms 以上) なら、送信側の時計の 1 秒あたり 10 ms までの速さで少しずつ上げる (描く予定の時刻は WebRTC のデコードの時間の見積もりを含み、受信を始めた直後は届く時刻より先に出て、後で戻るため)。「届いた時刻 - 換算した時刻」が 200 ms 以上か -200 ms 以下の状態が 1 秒続いたときは、メディアの時計が替わったとみなして決め直す (docs/architecture.md の「送信側: 送信側の時計への換算」)。

timestamp_us は単調に増えるとは限らない。見積もりの換算で定数を下げたときと決め直したとき、換算の方法を替えたとき、WebRTC が描く予定の時刻を早めたときに、前の値より小さい値が来ることがある。受信側は、clock_base が "qpc" の接続では送信側の時計をそのまま OBS の時計として使い、そうでない接続では、送信側の時計を OBS の時計に換算する差を、hello と pong の clock_us から接続ごとに決める (「ping と pong の扱い」)。受信側がこれらを吸収する方法は、docs/architecture.md の「タイムスタンプの合わせ方」と「録画ファイルの作成」にある。

### flags

| ビット | 値 | 名前 | 意味 |
|---|---|---|---|
| 0 | 0x01 | キーフレーム | 映像のメッセージで、アクセスユニットがキーフレームのときに 1。音声は常に 0 |
| 1 | 0x02 | 録画専用 | 黒埋め用のフレーム。録画だけが使い、デコードも表示もしない |
| 2 から 7 | 0xFC | (未使用) | 0 にする。どれかが 1 のメッセージは捨てる |

録画専用のフラグを付けて送るのは、黒埋め用のフレームだけである。黒埋め用のフレームは、録画の切れ目を黒で埋めるための、真っ黒な 1 秒分の映像 (キーフレーム 1 つと、続く P フレーム) である。送信側は、cordmux の録画が ON の間だけ、エンコーダのインスタンスごとに 1 回、次のうち最初に起きたときに作る。

- エンコーダを作った直後 (最初のフレームより前)
- 録画が ON になったとき (動いているエンコーダとは別のインスタンスで作る)
- エンコーダを閉じるとき

フレームの数は、そのエンコーダの fps の 1 秒分である。fps を半分にしているストリームでは半分になる。どのタイミングで作ったかは、ワイヤー形式では運ばない。送信側は、接続していない間に作った黒埋め用のフレームを捨て、再接続しても作り直さない。その場合に録画ファイルをどう作成するかは、docs/architecture.md の「録画ファイルの作成」にある。

受信側は、録画専用のフラグを見て、そのメッセージをデコードにも表示にも渡さない。cordmux の録画が ON なら録画だけに渡し、OFF なら数えて捨てる。録画専用のフラグが付いた音声のメッセージは、録画も使わずに数えて捨てる。黒埋めに使うのは映像だけで、音声の切れ目は録画が無音で埋めるためである。

### 映像のペイロード

映像のペイロードは、H.264 の Annex B 形式 (開始コードで区切った NAL ユニットの並び) の 1 アクセスユニットである。1 つのメッセージに 1 つのアクセスユニットを入れ、分けたりまとめたりしない。

キーフレームのアクセスユニットには、SPS と PPS を含める。SPS と PPS を別のメッセージでは送らない。受信側はこれを前提にしている。

- デコーダー (`src-obs/decode/h264_decoder.h` の `H264Decoder`) は、SPS と PPS をキーフレームのアクセスユニットの中から読む。
- 録画は、セグメントの最初のキーフレームの SPS と PPS から、そのセグメントのファイルの avcC を作る。

送信側は、WebCodecs の `VideoEncoder` を `avc: { format: "annexb" }` で設定してエンコードする (`src-vencord/encode/session.ts`)。エンコーダが出したキーフレームに SPS か PPS がないときは、同じエンコーダのインスタンスが前に出した SPS と PPS を、アクセスユニットの前に付けてから送る (`src-vencord/encode/annexb.ts`)。付けた回数は facts の sps_pps_prepended で受信側に伝える (「facts」)。別のインスタンスの SPS と PPS は使わない。解像度の違う SPS で avcC を作ることになるためである。

送信側は 2 秒ごとにキーフレームを入れる (`src-vencord/index.ts` の `VIDEO_KEYINT_SEC`)。受信側は、それとは別にキーフレームが必要になると keyframe_request で求める (「メッセージの種類」)。

### 音声のペイロード

音声のペイロードは、32 ビット浮動小数点の PCM である。

| 項目 | 値 |
|---|---|
| サンプルの形式 | 32 ビット浮動小数点、リトルエンディアン |
| チャンネル | 2 (左右を交互に並べるインターリーブ) |
| サンプリングレート | 48 kHz |
| 1 メッセージのサンプル数 | 480 (10 ms) |
| 1 メッセージのバイト数 | 3840 (480 x 2 チャンネル x 4 バイト) |

送信側は常に 480 サンプルごとに送る。受信側は、長さが 0 でなく 8 バイト (2 チャンネル x 4 バイト) の倍数であれば受け取り、そうでないメッセージは数えて捨てる。画面共有の音と声は同じ形式で、stream_id で見分ける (「ヘッダー」)。

エンコードせずに PCM で送るのは、Discord から取り出せる音声がデコードした後のものだけで、Opus でエンコードし直すと録画の音質が落ちるためである (詳しくは `src-vencord/audio/README.md`)。1 ストリームは毎秒 384,000 バイトで、12 ストリームなら毎秒約 4.6 MB になる。同じ PC の中の WebSocket なので、この帯域は問題にならない。録画のセグメントファイルにも、この形式のまま書く。

## 制御用の JSON

制御用の JSON は、WebSocket のテキストメッセージで送る。1 つのメッセージは 1 つの JSON のオブジェクトで、文字列のキー `type` にメッセージの種類を入れる。JSON のキーと、決まった文字列の値 (type、kind、stage、reason の値) は snake_case にする。

受け取った側は、次のように扱う。

- 下の表にないキーは無視する。キーを追加しても、古い相手との接続は壊れない。
- 表で「任意」と書いていないキーは省けない。省いたメッセージと、値の型が違うメッセージは、形式が不正なメッセージとして扱う。
- type が分からないメッセージと、形式が不正なメッセージは、そのメッセージだけを捨てて数える。接続は切らない。受信側は `ListenerStats::dropped_unknown_control` に、送信側は `droppedInvalidControl` に数える。片方だけが新しい種類を知っている状態 (片方だけを新しいバージョンにした間) でも、接続を続けるためである。
- 例外として、接続して最初のメッセージは hello でなければならない (「メッセージの種類」の hello)。
- どちらの側も、壊れた入力に対して例外を投げずに捨てる。

### メッセージの種類

送信側から受信側に送るメッセージ:

| type | キー | 送るとき |
|---|---|---|
| hello | wire (整数)、version (文字列)、secret (文字列)、vesktop (文字列、任意)、clock_us (0 以上の整数、任意)、clock_base (文字列、任意) | 接続して最初に 1 回 |
| pong | seq (0 以上の整数)、clock_us (0 以上の整数、任意) | ping を受け取るたび |
| configure | streams (配列) | 接続した直後と、ストリームの一覧か、ストリームの解像度、fps、音声の有無が変わったとき |
| log | lines (文字列の配列)、alerts (文字列の配列、任意)、stats (文字列の配列、任意) | 接続した直後と、その後 5 秒ごと。送る行がなければ送らない。alerts の行を書いたときは、すぐに送る |
| facts | facts (オブジェクト) | 接続した直後と、値が変わったとき。1 秒に 1 回まで (「facts」) |
| profiles | profiles (配列) | 接続した直後と、値が変わったとき (「profiles」) |
| avatar | user_id、avatar_key、avatar_png (すべて文字列) | アバターを取得できた人の avatar_key が、前に送った値と違うとき (「avatar」) |
| stream_check_result | stream_id (0 以上の整数)、outcome (文字列)、detail (文字列、任意) | stream_check を受け取って確認し終えたとき (「stream_check の扱い」) |

受信側から送信側に送るメッセージ:

| type | キー | 送るとき |
|---|---|---|
| hello_ok | wire (整数)、version (文字列)、media (真偽値、任意)、limits (オブジェクト、任意) | hello が通ったとき |
| hello_ng | reason (文字列) | hello が通らなかったとき。送った後に接続を切る |
| ping | seq (0 以上の整数) | 5 秒ごと |
| keyframe_request | stream_id (0 以上の整数) | キーフレームが必要になったとき |
| stream_check | stream_id (0 以上の整数) | 枠に載せている人の映像が届かないまま、待つ時間がたったとき (「stream_check の扱い」) |
| recording | on (真偽値) | 接続した直後と、cordmux の録画の状態が変わったとき |
| media | on (真偽値) | 接続した直後と、値が変わったとき |
| limits | max_long_edge (正の整数、任意)、fps_divisor (1 か 2、任意)、encoder_preference ("hardware" か "software"、任意)、latency_mode ("realtime" か "quality"、任意) | 接続した直後と、値が変わったとき |
| main_account | user_id (文字列。空の文字列は、メインアカウントを使わない) | 接続した直後と、値が変わったとき |
| receive_scope | scope ("everyone" か "slots")、user_ids (文字列の配列。scope が "slots" のときだけ) | 接続した直後と、範囲が変わったとき |
| move_to_main_call | なし | [移動] を押したとき |
| leave_call | なし | [退出] を押したとき |

受信側は、接続していない間に送ろうとした recording、media、limits、main_account、receive_scope、keyframe_request、stream_check、move_to_main_call、leave_call を、キューに入れずに捨てる。recording、media、limits、main_account、receive_scope は、接続した直後に現在の値を送り直す。move_to_main_call と leave_call は、接続していない間に押されても送り直さない (押したのに、あとから通話が動くことがないようにするため)。

#### hello の扱い

- wire は送信側が使うワイヤー形式のバージョンで、今は 1 である (「バージョンと拡張性」)。
- version は送信側のビルドの ID (`src-vencord/version.ts` の `CORDMUX_BUILD_ID`。`<バージョン>+<短いコミット>`) で、接続を受け付けるかどうかには使わない。受信側は、これを設定ウィンドウの [バージョン情報] ページの「バージョン」に表示し、同梱の Vencord の cordmux-build.txt の build_id と比べて、違えばドックにそのことを表示する。
- secret はトークンファイルから読んだトークンである。
- vesktop は Vesktop 自身のバージョンで、表示にだけ使う。送信側は、Vesktop から読めたときだけ載せる (`src-vencord/vesktop_version.ts`)。受信側は、値が文字列でなければこのキーだけをないものとし、hello は捨てない。
- clock_us は、送信側が hello を作る直前に読んだ送信側の時計 (「timestamp_us」と同じ時計) である。受信側は、これを送信側の時計と OBS の時計の差を決めるのに使う (「ping と pong の扱い」)。0 以上の整数でなければ、このキーだけをないものとし、hello は捨てない。
- clock_base は、送信側の時計の種類である。今の値は "qpc" だけで、送信側の時計が Windows の QueryPerformanceCounter の us (OBS の `os_gettime_ns` / 1000 と同じ値) であることを示す (「timestamp_us」)。送信側は、送信側の時計がページの原点の TimeTicks の上にあるときだけ載せる。受信側は、"qpc" なら送信側の時計と OBS の時計の差を 0 にし、hello と pong の clock_us は監視に使う (「ping と pong の扱い」)。監視で同じ時計という前提が崩れたと分かったら、差を決める扱いに戻る。キーがないとき、文字列でないとき、"qpc" でないときは、このキーだけをないものとし (差を決める扱い)、hello は捨てない。
- 受信側は、ハンドシェイクを受け付けてから 5 秒以内に hello が届かなければ接続を切る (`ListenerConfig::hello_timeout_ms`)。
- 受信側は、最初のメッセージがテキストでないとき、JSON でないとき、type が hello でないときは、hello_ng を送らずに接続を切る。hello のキーが足りないか型が違うときは reason が malformed の hello_ng を、wire が 1 でないときは bad_wire を、secret がトークンと一致しないときは bad_secret を返してから切る。secret とトークンは定数時間で比べる (`src-obs/ws/secret.h` の `constant_time_equals`)。
- hello が通った後に届いた hello は、type が分からないメッセージと同じく捨てて数える。

#### hello_ok と hello_ng の扱い

- hello_ok の wire は 1、version は cordmux.dll のビルドの ID である。送信側は、wire が 0 以上の整数で version が文字列であることだけを確認し、値は使わない。
- hello_ok の media と limits は、受信側が最後に決めた値で、送信側が接続を確立するときの初めの値になる。受信側は、まだ値を決めていなければキーを省く。送信側は、この 2 つのキーの値が不正でも hello_ok を捨てず、そのキーだけをないものとして扱う。hello_ok を捨てると接続できなくなるためである。同じ値は、hello_ok の直後の media と limits のメッセージでも届く。
- 送信側は、media を受け取っていなければ true として、limits を受け取っていなければ上限なしとして動く。この 2 つを送らない古い受信側と接続したときの動作である。
- hello_ng の reason は、今は bad_secret、bad_wire、malformed のどれかである。送信側は、知らない値でも文字列として受け取る。理由を追加しても壊れないようにするためである。受信側は、受け取った secret や version を reason にもログにも書かない。
- 送信側は、hello_ng を受け取ると接続を閉じ、間隔を空けて接続を試し直す。hello を送ってから 5 秒以内に hello_ok か hello_ng が届かないときも、接続を切り、接続を試し直す。

#### ping と pong の扱い

- 受信側は 5 秒ごとに ping を送る。pong が返っていない ping が 2 つある状態で、次の ping を送る時刻になると、受信側は接続を切る (`ListenerConfig::ping_interval_ms` と `max_missed_pongs`)。
- seq は 1 から始めて ping ごとに 1 ずつ増やす。送信側は、受け取った ping の seq をそのまま pong に入れる。
- 受信側は、pong の seq が直近の ping と一致するかを見ない。どの pong が届いても、pong が返っていない ping の数を 0 に戻す。
- pong の clock_us は、送信側が pong を作る直前に読んだ送信側の時計である (hello の clock_us と同じ扱い)。受信側は、hello と pong が運んだ clock_us と、それを受け取った時の OBS の時計 (`os_gettime_ns`) の差の、接続の間の最小値を、送信側の時計から OBS の時計への差とする (`src-obs/pipeline/sender_clock.h`)。送信側は時計を読んでから送るので、この差は本当の差に届くまでの遅れを足した値で、最小値には、最も早く届いたメッセージの遅れだけが残る。どちらの時計も Windows の単調な時計で進むので、差は接続の間は変わらない。clock_us を送らない送信側では差が決まらず、受信側は、届いたメッセージを、届いた時に生まれたものとして扱う。hello の clock_base が "qpc" の接続では、差は 0 で、この最小値は最も早く届いたメッセージの届くまでの遅れになる。受信側はこれを統計の行 (`Sender clock delay`) に書く。最小値が -1.101 ms (送信側の誤差の上限。ページの原点の TimeTicks の誤差の上限 1 ms と `performance.now()` の丸め 100 us と 1 us) より小さいか、組が 12 個そろった後に 1 秒より大きければ、同じ時計という前提が崩れているとして、差をそれまでの最小値 (clock_base がないときと同じ見積もり) に戻し、警告の行を書く。
- WebSocket のプロトコルの ping と pong (opcode 0x9 と 0xA) は使わない。プロトコルの pong はブラウザーのネットワークの処理が返すので、Vesktop のレンダラーが止まっていても返事が届き、止まったことに気付けないためである。

#### configure の扱い

- streams は、送信側が今送っているストリームの全部の一覧である。受信側は、届いた configure で前の一覧を置き換える。
- streams の各要素のキー:

  | キー | 型 | 中身 |
  |---|---|---|
  | stream_id | 0 以上の整数 | ストリームの番号 (「ヘッダー」) |
  | user_id | 文字列 | そのストリームの人の Discord のユーザー ID |
  | kind | "screen" か "voice" | 画面共有か、声か |
  | width、height | 正の整数、任意 | 送信側が今エンコードしている映像の幅と高さ |
  | fps | 正の数、任意 | 送信側が今エンコードしている映像の fps |
  | audio | 真偽値、任意 | 画面共有に音声があるか |

- width、height、fps は、Discord から受け取った映像の値ではなく、縮小と fps の間引きをした後にエンコードしている値である。受信側のドックは、これを現在の画質として表示する。送信側は、エンコードの段階がまだ決まっていない間と、段階が unavailable (その人の映像を送らない) の間は、3 つとも省く。分からない値を適当な値で埋めないためである。
- 声のストリームは stream_id、user_id、kind だけを持つ。
- 要素が 1 つでも不正か、同じ stream_id が 2 回以上あれば、受信側は configure の全体を捨てる。一部だけを使うと、どの番号が誰のものかが途中までしか決まらないためである。捨てたときは、ログに 1 行書く。

#### keyframe_request の扱い

- 送信側は、その stream_id のエンコーダに、次のフレームをキーフレームにするよう求める。声のストリームと、知らない stream_id は無視する。
- 受信側が求めるのは、次のときである (`src-obs/pipeline/pipeline.cpp`)。
  - デコードを待つアクセスユニットが上限を超えて捨て、次のキーフレームを待ち始めたとき (すぐに 1 回)
  - キーフレームを待っている間に P フレームが届いたときと、デコーダーがキーフレームを求めたとき (ストリームごとに 1 秒に 1 回まで)
  - cordmux の録画を ON にしたとき (直近の configure にある画面共有のストリームの全部。録画のセグメントはキーフレームから始まるため)

#### recording の扱い

- on は、cordmux の録画をしているかどうかを表す。真偽値でなければ (0、1、"true" も) そのメッセージを捨てる。
- 送信側は、接続が切れても値を OFF に戻さない。戻すと、再接続のたびに録画が ON になり直したように見え、黒埋め用のフレームを作り直すことになるためである。送信側は、持っている値を facts の recording で返す。
- 受信側は、facts の recording と自分の録画の状態を比べ、食い違いが続けばドックに表示する。

#### media の扱い

- on が false の間、送信側は Discord のトラックからの読み出しも、エンコードも、アバターの取得もしない。
- 受信側は、cordmux が有効で、さらに現在のシーンコレクションに cordmux のソースがあるか cordmux の録画をしているときに true にする (`src-obs/pipeline/pipeline.h` の `decide_media`)。シーンコレクションを読み終える前は、ソースがないものとして扱う。cordmux を無効にしても接続は切らず、false を送り、あわせて視聴する人の範囲を空にして送る (receive_scope の扱い)。
- 送信側は、接続している間だけ読み出しとエンコードをする。接続が切れたら、値にかかわらず全部止め、値を既定 (true) に戻す。
- on が真偽値でなければ、そのメッセージを捨てる。

#### limits の扱い

- max_long_edge は、映像の長い辺の上限 (ピクセル) である。1 以上 4294967295 以下の整数で、省くと上限なしになる。
- fps_divisor は fps を割る数で、1 (そのまま) か 2 (半分) である。省くと 1 と同じになる。
- 範囲の外の値と型の違う値があれば、受信側も送信側もそのメッセージの全体を捨てる。受信側は範囲の外の値を送らず、送ろうとしたときはログに 1 行書く。
- 受信側は、設定の `limit_max_long_edge` (0 か、16 から 7680) と `limit_fps_divisor` から値を作る。0 (制限なし) の max_long_edge と 1 の fps_divisor は、キーを省いて表す。省いた形は、何も届いていないときの送信側の既定 (`{}`) と同じになり、facts の limits とそのまま比べられる。
- 送信側は、新しく始める画面共有にこの値を使う。エンコード中の画面共有には、上限を下げたキーだけをすぐに使い、上げたキーは次の画面共有から使う。
- 上限は全体に 1 つで、参加者ごと、枠ごとの上限は持たない。
- encoder_preference は、優先するエンコーダの種類である。"hardware" ならハードウェアを先に試し、使えなければソフトウェアに切り替える (`encode/ladder.ts` のエンコードの段階)。"software" ならどの段階もソフトウェアで試す。省くと "hardware" と同じになる。
- latency_mode は WebCodecs の latencyMode で、"realtime" は遅延を優先し、"quality" は遅延より画質を優先する。省くと "realtime" と同じになる。ビットレートの係数 (bitrateFactor) はワイヤー形式に持たず、送信側は 1 を使う。
- encoder_preference と latency_mode の範囲の外の値と型の違う値も、メッセージの全体を捨てる。
- 受信側は、設定の `encoder_preference` ("hardware" か "software") と `encode_priority` ("speed" か "quality") から値を作る。既定の "hardware" と "speed" は、キーを省いて表す (`limits_from_settings`)。"software" は encoder_preference の "software"、"quality" は latency_mode の "quality" になる。
- 送信側は、この 2 つを新しく始める画面共有からだけ使う。エンコード中の画面共有は、確保済みのエンコーダの種類と latencyMode を変えない。上限と違い、下げたほうだけをすぐに使うということはない。
- 送信側のログの行 `video stream <N>: stage=... format=... preference=... latencyMode=...` に、そのストリームを作ったときの好み (prefer-hardware か prefer-software) と latencyMode が出る。受信側はこの行で、実際に使われたエンコーダの種類を確認できる。

#### main_account の扱い

- 送信側は、Vesktop のアカウントから見える通話の中で、メインアカウントがいる通話を読み、facts の main_call で返す。そのために、メインアカウントの userId を受信側から受け取る (受信側の設定 broadcaster_user_id の値)。メインアカウントの userId を送信側が自分で知る手段はない (Vesktop のアカウントがサブアカウントのとき、メインアカウントは別のアカウントである)。
- user_id が空の文字列なら、メインアカウントを使わない構成で、送信側は main_call を送らない。user_id が文字列でなければ、そのメッセージを捨てる。
- 送信側は、接続が切れても値を戻さない (再接続の直後に、受信側が現在の値を送り直す)。

#### receive_scope の扱い

- 送信側が視聴する人の範囲である。"everyone" は画面共有している全員 (Vesktop のアカウントを除く)。"slots" は user_ids の人だけで、空の配列なら誰も視聴しない。画面共有している人のうち、範囲に入っている人の視聴を始め、範囲から外れた人の視聴をやめる。範囲に入っていても、画面共有していない人は何もしない。
- 受信側は、設定の receive_scope が "slots" (既定) のとき、画面の枠に載っている人の userId を昇順に重複なく user_ids に入れる。枠に載っている人が変わるたびに送る (枠の付け外し、自動の割り当て、配置の読み込み)。設定が "everyone" のときは、"everyone" を送る。cordmux を無効にしている間は、設定にかかわらず空の "slots" を送り、有効に戻したときに設定どおりに送り直す (無効の間は読み出しも送信もしないので、視聴もしない)。
- 範囲が届いていない間は、誰も視聴しない (空の "slots" と同じ)。OBS を起動していない間に、共有している人の画面へ視聴者として出ないためである。
- 送信側は、接続が切れても 30 秒は届いた範囲のまま視聴を続け、切れたままなら空の範囲にする。30 秒続けるのは、短い切断で映像を止めないためである。空の範囲にするのは、OBS を終了した後も視聴者として出続けないためである。
- "slots" のとき、枠に載っていない人の画面共有は configure に載らない。受信側は、枠への自動の割り当てを、configure ではなく facts の sharing から行う。
- scope が "everyone" か "slots" でなければ、"slots" で user_ids が文字列の配列でなければ、そのメッセージを捨てる。

#### move_to_main_call と leave_call の扱い

- どちらも、利用者が受信側のドックで [移動] か [退出] を押したときだけ、受信側が送る。接続、再接続、通話の変化では送らない。送信側も、受け取ったときだけ実行し、自分では実行しない。
- move_to_main_call は移動先を運ばない。送信側は、受け取った時点のメインアカウントがいる通話 (facts の main_call と同じ読み方) を読み直し、Vesktop のアカウントがその通話に参加できるときだけ、その通話に移る。受信側から任意の通話を指定させず、押した瞬間に状況が変わっていても、押した時点の通話に移る。
- leave_call は、Vesktop のアカウントを、現在の通話から退出させる。
- 実行できなかったとき (メインアカウントが見える通話にいない、参加できない、通話を移る関数が見つからない) は、移動も退出もせず、理由を送信側のログに 1 行書く (`move_to_main_call: failed (...)`)。やり直さない。受信側には結果を返さない。通話の名前が変わらないことが、受信側の画面での結果になる。
- キーを持たないので、知らないキーは無視する (形の確認はない)。

#### log の扱い

- 送信側は、ログの行を 3 種類に分けて保ち (`src-vencord/core/logbook.ts`)、まとめて取り出して送る。lines は出来事の行 (状態が変わったときの行)、alerts は異常に関わる出来事の行 (映像が届かない、受信の確認、接続の failed など)、stats は数秒から 60 秒ごとの統計の行である。送信側は、alerts の行を書いたときはすぐに送るので、1 つのメッセージの中では、alerts の行は lines のどの行よりも後に書かれている (受信側は lines、alerts の順に積む)。
- トラックの mute と unmute は、続いた時間にかかわらず lines に書く。ブラウザーが RTP の途切れで受信のトラックを mute にするかは確認していないので、mute にする実装なら、静止した画面の共有のたびに OBS のログに警告が出てしまうためである。
- alerts と stats は任意で、古い送信側は lines だけを送る (受信側は、そのときは全部を出来事の行として扱う)。どの配列も、1 つでも文字列でない要素があれば、受信側はメッセージの全体を捨てる。
- 受信側は、トークンとポート番号を含む行を捨ててから、残りを自分のリングバッファーに入れる。lines と alerts は出来事の行のリングバッファー (設定の `js_event_log_lines` 行まで。診断情報の discord.log)、stats は統計の行のリングバッファー(設定の `js_log_ring_lines` 行まで。discord-stats.log) である。
- 受信側は、lines の行を OBS のログに LOG_DEBUG で、alerts の行を LOG_WARNING で書き、stats の行は OBS のログに書かない。送信側は、alerts の行に Discord のユーザー ID を書かない (OBS のログは、診断情報と違って ID を置き換えずに人に渡される)。

#### stream_check の扱い

- 受信側は、枠に載せている人の映像が、最後のフレームから設定の `receive_restart_wait_ms` (既定 5 秒) の間届かず、configure にその人の画面共有のストリームがあるときに、そのストリームの stream_check を送る。届かないままなら、間隔を広げて送り直す (10 秒後、その 30 秒後、以後 30 秒ごと)。映像が届いたら、間隔を最初に戻す。
- 送信側は、そのストリームの映像を送れているか、トラックのレシーバーの getStats() の inbound-rtp の packetsReceived と framesReceived が増えているか (1 秒あけて 2 回読む)、トラックの状態、同じ人の新しい接続の映像があるかを見て、読み出しとエンコードを始め直すかを決め、stream_check_result で返す (`src-vencord/core/stream_check.ts`)。
- outcome の値:

| outcome | 意味 | 送信側がすること |
|---|---|---|
| restarted | RTP が届いているのに、送れたフレームがなかった | 同じトラックから、読み出しとエンコードを始め直す。stream_id は替えない |
| superseded | 同じ人の、より新しい接続の映像が動いている | 古いストリームを止める (configure から消える) |
| not_receiving | RTP が届いていない (Discord が送っていない。静止した画面の共有など) | 何もしない |
| sending | 確認の間もフレームを送れていた | 何もしない |
| ended | トラックが終わっていた | ストリームを止める |
| unknown_stream | そのストリームがない | 何もしない |

- 受信側は、知らない outcome も文字列のまま受け取る。detail は、確認に使った数の短い説明 (`sent=+0 rtp=+412 muted=false` など) で、ログにだけ書く。
- 受信側は、stream_check を送るときも、restarted を受け取ったときも、デコードの経路を作り直さない。stream_check はフレームが受信側に届いていないときだけに送るので、デコーダーには何も来ていない。送信側が始め直した後の最初のフレームは SPS と PPS を前に付けたキーフレームで、受信側のデコーダーはそのまま読む (エンコーダを確保し直したときと同じ)。sending の答えは、ログに書くだけである。枠の割り当て、経路、合成のソースの子の大きさと最後のフレームは変えない。

### Origin と Host の検証

受信側は、WebSocket のハンドシェイク (HTTP の Upgrade の要求) で、次の順に確認する (`src-obs/ws/listener.cpp`)。

1. すでに接続があれば、503 を返して断る (`ListenerStats::rejected_clients` に数える)。
2. Host ヘッダーが `127.0.0.1:<待ち受けているポート番号>` と一致しなければ (大文字と小文字は区別しない)、403 を返して断る。`localhost` は受け付けない。
3. Origin ヘッダーがあり、`https://discord.com`、`https://canary.discord.com`、`https://ptb.discord.com` のどれとも完全に一致しなければ、403 を返して断る。Origin ヘッダーがない接続は受け付ける。

403 で断った回数は `ListenerStats::handshake_rejections` に数える。断ったときはログに 1 行書くが、トークンもポート番号も書かない。

Host の検証は、ブラウザーで開いたページが DNS リバインディングで 127.0.0.1 に接続するのを防ぐ。Origin の検証は、ブラウザーで開いたほかのサイトのページからの接続を断る。どちらも、ブラウザーでないプログラムは好きな値を送れるので、認証の本体はトークン (hello の secret) である。トークンで守れる範囲は、docs/architecture.md の「接続の認証と制限」にある。

許可する Origin の一覧は `ListenerConfig::allowed_origins` で変えられる。Vesktop 1.6.7 と 1.6.9 は discord.com を開き、Vesktop の設定で canary か ptb を選ぶと、それぞれのホストを開く。そのため、一覧はこの 3 つにしてある。Vesktop が接続のときに Origin ヘッダーを送るか、送るならどの値かは、確認していない。実際の Vesktop からの接続は、この一覧で受け付けられている。テスト用の送信側は `https://discord.com` を送る。

### トークンファイル

受信側は、ポートを開いた後に、ポート番号とトークンを `%LOCALAPPDATA%\cordmux\session.json` に書く。中身は次の 4 つのキーを持つ 1 行の JSON である。

```
{"port":4470,"secret":"<64 文字の 16 進数>","pid":<OBS のプロセス ID>,"started_at":<OBS のプロセスの作成時刻>}
```

| キー | 中身 |
|---|---|
| port | 受信側が待ち受けているポート番号 |
| secret | トークン。32 バイトの乱数を 16 進数の小文字 64 文字にしたもの |
| pid | このファイルを書いた OBS のプロセス ID |
| started_at | そのプロセスの作成時刻 |

- トークンは、受信側が待ち受けを始めるたびに作り直す (`src-obs/ws/secret.h` の `generate_secret_hex`)。
- started_at は、Windows の GetProcessTimes が返すプロセスの作成時刻を整数にした値 (1601 年 1 月 1 日 UTC からの 100 ナノ秒単位の数) で、表示する時刻ではない。pid と started_at は、別の OBS がこのファイルを書いたかを見分けるためだけに使う。受信側は、ファイルを書いたプロセスが動いていて、作成時刻も同じなら、別の OBS が待ち受けているとして、自分では待ち受けを始めない。pid だけで比べると、Windows が同じ番号を別のプロセスに使い直したときに見誤る。
- 受信側は、同じフォルダーの一時ファイルに書いてから置き換える。置き換える前に、一時ファイルのアクセス許可を、現在のユーザーだけが読み書きできるようにする (継承しない ACL)。読む側が書きかけの中身を読まないようにし、広いアクセス許可のファイルが見える時間を作らないためである。アクセス許可を変えられなくても、ファイルは書く。
- 送信側は、接続を試すたびにこのファイルを読み、port (正の整数) と secret (空でない文字列) だけを使う (`src-vencord/native.ts` の `readSession`)。ファイルがない、読めない、形式が違うときは、理由を区別せずに、接続を試さずに次の機会を待つ。pid と started_at は読まない。
- どちらの側も、トークンをログに書かない。

### facts

facts は、送信側が知っている Discord の状態などをまとめたメッセージである。キー `facts` のオブジェクトに、次のキーを入れる (`src-vencord/core/facts.ts` の `buildFacts`)。`{count, user_ids}` は、人数 `count` (0 以上の整数) と、Discord のユーザー ID の配列 `user_ids` を持つオブジェクトで、count は user_ids の要素の数と同じにする。

| キー | 型 | 省くか | 中身 |
|---|---|---|---|
| warnings | 文字列の配列 | 省かない | Discord の内部への依存箇所のうち、今読めないもの (状態が missing か lost) の id (`src-vencord/ledger.ts`) |
| sharing | `{count, user_ids}` | 省かない | 今、画面共有している人 |
| roster | `{count, user_ids}` | 任意 | 通話に参加している人。送信側が一度も読めていない間は省く |
| self_user_id | 文字列 | 任意 | Vesktop のアカウントのユーザー ID。分からない間は省く |
| recording | 真偽値 | 省かない | 送信側が持っている cordmux の録画の状態 (recording のメッセージで届いた値) |
| media | 真偽値 | 省かない | 送信側が今使っている media の値。何も届いていなければ true |
| limits | オブジェクト | 省かない | 送信側が今使っている limits の値。キーは limits のメッセージと同じ (encoder_preference と latency_mode も含む) で、何も届いていなければ `{}` |
| sps_pps_prepended | 0 以上の整数 | 省かない | キーフレームに SPS と PPS を前に付けた回数 (「映像のペイロード」) の、プラグインが動き始めてからの累計 |
| stages | `{stream_id, stage}` の配列 | 任意 | 映像をエンコードしているストリームごとの、エンコードの段階。報告する段階が 1 つもなければ省く |
| call_name | 文字列 | 任意 | Vesktop のアカウントがいる通話の名前。通話にいないとき、名前を読めないときは省く |
| main_call | `{user_id, name, joinable}` | 任意 | メインアカウントがいる通話。Vesktop のアカウントから見える通話に参加しているときだけ載せる (「call_name と main_call」) |

送る頻度:

- 送信側は、値が変わったときだけ送り、1 秒に 1 回までにする。前に送ってから 1 秒たっていなければ、1 秒たったときに、その時点の値で 1 回だけ送る。
- 接続した直後には、値が変わっていなくても送る。受信側は、接続が切れると facts を捨てるためである。
- 1 つの facts の roster と sharing は、同じ時点の値である。送信側は Flux の 1 つの処理の中で roster と sharing を続けて読み直すので、その処理が終わった後に 1 回だけ facts を作る (マイクロタスク。`src-vencord/core/coalesce.ts`)。受信側が同じ facts の roster と sharing で枠を空けるとき、新しい roster と古い sharing が組み合わさらないようにするためである。
- そのため受信側は、送信側に何かを伝えてから facts で返ってくるまでに、最大で 1 秒ほど待つことになる。たとえば、cordmux の録画を始めた直後は、facts の recording がまだ古い値である。

省くキーと省かないキー:

- roster と self_user_id は、分からない間はキーごと省く。roster を省くのは、「通話に誰もいない」(空の配列) と「まだ分からない」を区別するためである。self_user_id は、空の文字列のときも省く。
- stages は、報告する段階が 1 つもないときに省く。roster と違って、空のときと分からないときを区別する必要がないためである。
- recording、media、limits、sps_pps_prepended は、値にかかわらず必ず載せる。省くと、送信側の値が既定の値 (recording は false、media は true、limits は `{}`、sps_pps_prepended は 0) なのか、このキーを知らない古い送信側なのかを、受信側が区別できない。

キーごとの説明:

- sharing: 受信側は、このキーがないか形式が違う facts の間は、誰が画面共有しているかを「分からない」として扱う。画面共有している人の映像が届かないことを伝えるかは、このキーで決める。
- roster: 受信側は、メインアカウントが通話に参加しているかを、このキーで判断する。
- self_user_id: Vesktop のアカウントの声と画面共有は、受信側に届かない (WebRTC は自分が送った音声を返さず、送信側は自分の画面共有を視聴しない)。受信側は、この人を「使う声」の候補にも、声を置き換える相手にもしない。roster の中に入れずに別のキーにしたのは、roster が読めない間にも分かる値で、roster は 1 か所でも形式が違えばキーごと捨てるためである。
- sps_pps_prepended: 受信側からは見えない値なので、受信側はこれを録画ログに書く。ストリームが終わっても減らない。
- stages: stream_id は 0 以上の整数、stage は文字列で、stream_id の昇順に並べる。現在の送信側が送る stage は次の 5 つである (`src-vencord/encode/ladder.ts`)。

  | stage | 意味 |
  |---|---|
  | native | ハードウェアのエンコーダで、受け取った解像度と fps のまま |
  | halffps | ハードウェアのエンコーダで、fps を半分にする |
  | downscaled | ハードウェアのエンコーダで、長い辺を 1280 まで縮小する |
  | software | ソフトウェアのエンコーダで、長い辺 1280、30 fps |
  | unavailable | エンコードできないので、その人の映像を送らない |

  stage の値は、この 5 つに限らない。受信側は、知らない値が来ても文字列のまま持ち、画面にそのまま表示する。整数で送らないのは、受信側が送信側の段階の並びに依存しないようにするためである。声のストリームはエンコーダを持たないので、stages に入らない。送信側は、エンコーダの最初の段階が決まるまで、そのストリームを stages に入れない。

call_name と main_call:

- call_name と main_call は、読めない間はキーごと省く。Discord の内部を読む部分は、依存箇所の一覧の call-info (`src-vencord/ledger.ts`) である。
- call_name はチャンネルの名前で、サーバーのチャンネルは「チャンネルの名前 / サーバーの名前」にする (Discord の通話のパネルと同じ順)。名前のない DM とグループ DM は、相手の表示名を ", " でつなぐ。空の文字列は載せない。
- main_call の user_id は、送信側が読んだときのメインアカウントの userId である (main_account で受信側から届いた値)。受信側は、現在の設定のメインアカウントと違えば、設定を変えた直後の古い値として使わない。name は call_name と同じ作り方で、読めなければ省く。joinable は、Vesktop のアカウントがその通話に参加できるかで、読めなければ省く。サーバーのボイスチャンネルとステージは、接続の権限があり、人数の上限に達していない (上限を超えて入れる権限があれば無視する) ときに true になる。DM とグループ DM は true になる。ステージには人数の上限を見ない。
- main_call は、Vesktop のアカウントから見える通話にメインアカウントがいるときだけ載せる。「どの通話にもいない」を表すキーや値は持たない。Vesktop のアカウントが参加していないサーバーの通話にメインアカウントがいるとき、その状態は Discord の情報に現れず、どの通話にもいないときと区別できないためである。受信側は、キーがないことを「分からない」として扱い、「通話なし」と表示しない。

受信側の読み方 (`src-obs/pipeline/facts_state.h`):

- 受信側の `src-obs/ws` は facts のキーがあることだけを確認し、中身を読まずに渡す。中身を読むのは `src-obs/pipeline/facts_state.h` だけである。
- 型が違って読めないキーは、そのキーだけを捨てて数え (`FactsSnapshot::unreadable_keys`)、ほかのキーは使う。数はドックと診断情報で、送信側と受信側のバージョンが合っていないことを疑う手がかりになる。
- 一覧を持つキー (warnings、sharing、roster、stages) は、要素が 1 つでも読めなければキーごと捨てる。途中までの一覧を作らない。
- count が負の数か 1000000 より大きいときは、そのキーを捨てる。count が user_ids の数と違うときは、捨てずに持ち、画面には user_ids の数を使う。
- 省かれた roster、self_user_id、stages、call_name、main_call と、古い送信側が送らないキーは、数えない。call_name は文字列でないか空のとき、main_call は user_id が空でない文字列でないか、name が文字列でないか、joinable が真偽値でないとき (一部だけを使わない) に、そのキーだけを捨てて数える。
- 受信側は、facts の media と limits を読まない。

### profiles

profiles は、参加者ごとの表示名、ユーザー名、アバターのバージョン、Bot の印を運ぶメッセージである。キー `profiles` の配列に、送信側が読めた通話の参加者 (facts の roster の人) を 1 人 1 要素で入れる (`src-vencord/core/facts.ts` の `buildProfiles`)。アバターの画像は載せず、avatar で参加者ごとに送る (「avatar」)。

| キー | 型 | 中身 |
|---|---|---|
| user_id | 文字列 | Discord のユーザー ID |
| display_name | 文字列、任意 | Discord で表示される名前。読めない人は省く |
| username | 文字列、任意 | Discord のユーザー名。読めない人は省く |
| avatar_key | 文字列、任意 | Discord の user.avatar の値。アバターを設定していない人は空の文字列。まだ読めていない人は省く |
| bot | 真偽値、任意 | Bot のアカウントなら true。送信側は、読めなければ false にして必ず入れる。省かれたときは false として扱う |

- Vesktop のアカウント (facts の self_user_id) は、通話に参加していなくても、表示名を読めていれば 1 要素入れる。受信側が通話の外でもサブアカウントの名前を表示するためである。その要素は display_name と bot だけを持つ。
- username は、Discord の中で 1 人に 1 つの形にする。discriminator が "0" の人 (現在のユーザー名の仕組みの人) はユーザー名のまま、4 桁の discriminator を持つ人は `ユーザー名#1234` の形にする (`src-vencord/adapter/username.ts`)。受信側は、表示名が同じ人を見分けるために使う。空の文字列は載せない。
- avatar_key は、アバターの画像が変わると変わる。受信側は、avatar_key が前に受け取った avatar の avatar_key と違う人のアバターを、古いものとして扱える。
- bot は Discord の user.bot である。
- 表示名とユーザー名は、画面共有していない人の分もドックと OBS のソース名に使うので、参加者の全員に載せる。
- 要素の user_id、display_name、username、avatar_key のどれかの型が違うか、bot が真偽値でなければ、受信側は profiles の全体を捨て、ログに 1 行書く。

送る頻度:

- 送信側は、値が変わったときに送る。facts と違い、1 秒に 1 回までにはしない。
- 接続した直後には、値が変わっていなくても送る。
- 送信側が通話の参加者をまだ一度も読めていない間 (facts の roster を省く間) は、Vesktop のアカウントの 1 要素だけを送る。その表示名も読めていなければ送らない。

受信側は、届いた profiles で前の内容を全部置き換える (`src-obs/pipeline/profiles_state.h`)。1 つのメッセージが参加者の全員の分なので、前の内容を残すと、通話から抜けた人の名前が画面と OBS のソース名に残るためである。接続が切れたときも捨てる。

### avatar

avatar は、1 人のアバターの画像を運ぶメッセージである。画像は profiles に載せず、参加者ごとに分けて送る。profiles に載せると、1 人 32 KiB までの画像が参加者の全員の分になり、名前が変わるたびに全員の画像を送り直すことになるためである。

| キー | 型 | 中身 |
|---|---|---|
| user_id | 文字列 | Discord のユーザー ID |
| avatar_key | 文字列 | この画像を取ったときの Discord の user.avatar の値。アバターを設定していない人は空の文字列 |
| avatar_png | 文字列 | 64 x 64 の PNG を base64 にした文字列。32768 文字まで |

3 つのキーはどれも省略できない。

- 対象は、通話の参加者 (facts の roster の人) の全員である。画面共有しているかどうかは関係しない。
- 送信側は、参加者ごとに、前に送った avatar_key と違うときだけ送る。
- 接続した直後は、受信側が前の接続のアバターを覚えていないものとして、取得できている全員分を送り直す。
- 通話から抜けた人の記録は捨てる。戻ってきたら、もう一度送る。
- 1 秒に送る人数に上限を付ける (今は 4 人)。人数の多い通話に参加した直後に、まとめて送らない。上限で残った人は、次に送れるようになったときに送る。
- avatar_png が 32768 文字を超えたものは、受信側がそのメッセージを捨てる。

アバターの取得 (送信側。`src-vencord/core/avatar.ts`):

- アバターは送信側のレンダラーが取得する。cordmux.dll は外部と通信しない。
- 取得するのは、https で既定のポートを使う、ホストが `cdn.discordapp.com` の URL だけである。ユーザー名やパスワードを含む URL は使わない。クエリ文字列は `size=64` に置き換える。
- Content-Type が `image/png`、`image/webp`、`image/jpeg` のどれかで、256 KiB までのものだけを読む。
- 読んだ画像を OffscreenCanvas で 64 x 64 の PNG にし直す。base64 にして 32768 文字を超えたものは送らない。
- どれかの条件を満たさない人は、avatar を送らない。その人の表示名と、ほかの人の要素は送る。
- media が false の間は取得しない。取得済みのアバターを送るのは、media に関わらない。

### 話している人

話している人を運ぶメッセージも facts のキーも、ない。名前ラベルの光は、受信側が混ぜた声と、自分の枠の音声ソースから決める。送信側が Discord から読める発言のシグナルは、どの接続から来たかを示さないので、声と画面共有の音を区別できないためである。

## バージョンと拡張性

hello と hello_ok の wire が、ワイヤー形式のバージョンである。今は 1 である。

追加するだけの変更では wire を上げない。次の変更がこれに当たる。

- 新しいメッセージの種類を追加する
- メッセージと facts に、任意のキーか新しいキーを追加する
- 決まった文字列の値 (stages の stage、hello_ng の reason) を追加する

受け取った側は知らないキーを無視し、知らない種類のメッセージを接続を切らずに捨てるので、古い相手とも接続が続く。古い相手が送らないキーは、次のように扱う。

| ないもの | 受け取った側の扱い |
|---|---|
| hello_ok の media と limits | 送信側は、映像と声を送り、画質の上限をかけない |
| facts の recording | 受信側は、送信側の録画の状態を分からないものとして扱う。OFF とはみなさない |
| facts の sps_pps_prepended | 受信側は、録画ログに 0 と書く |
| hello の vesktop | 受信側は、Vesktop のバージョンを分からないとして表示する |
| hello の clock_base | 受信側は、送信側の時計と OBS の時計の差を hello と pong の clock_us から決める。古い受信側は clock_base を読まずに差を決めるが、"qpc" の送信側の clock_us とタイムスタンプは同じ時計の上にあるので、差は最も早く届いたメッセージの遅れに決まり、そのまま動く |

wire を上げるのは、古い相手が別の意味に読む変更 (ヘッダーの並び、今あるキーの意味) をするときだけである。wire を上げると、新しい側と古い側は hello_ng (bad_wire) で接続できなくなる。追加するだけの変更で wire を上げると、上の表のように、キーがないことから古い相手だと見分ける扱いも使えなくなる。

送信側と受信側は同じ配布用の zip に入っていて、一緒に更新する。新しい側と古い側が混ざるのは、片方だけを更新した間 (Vesktop が古い cordmux のまま動いている間など) だけである。このとき受信側は、hello の version と、同梱の Vencord の cordmux-build.txt の build_id の違いからこれに気付き、ドックに表示する。

## 実装の場所

| 内容 | 受信側 (src-obs) | 送信側 (src-vencord) |
|---|---|---|
| バイナリメッセージのヘッダー | `src-obs/ws/wire.h` | `src-vencord/core/wire.ts` |
| 制御用の JSON の型と、作成と解析 | `src-obs/ws/control_messages.h` | `src-vencord/transport/control.ts` |
| 接続、ハンドシェイクの検証、hello、ping | `src-obs/ws/listener.h`、`src-obs/ws/listener.cpp` | `src-vencord/transport/client.ts`、`src-vencord/transport/socket.ts` |
| トークン | `src-obs/ws/secret.h` | |
| トークンファイル | `src-obs/platform/platform_win.cpp` の `write_session_file` と `read_session_file` | `src-vencord/native.ts`、`src-vencord/paths.ts` |
| 映像のペイロード | `src-obs/decode/h264_decoder.h` | `src-vencord/encode/session.ts`、`src-vencord/encode/annexb.ts` |
| 音声のペイロード | `src-obs/pipeline/pipeline.cpp` の `handle_audio_frame` | `src-vencord/audio/chunk.ts` |
| timestamp_us | `src-obs/align/`、`src-obs/pipeline/sender_clock.h` | `src-vencord/encode/sender_clock.ts`、`src-vencord/encode/ticks_origin.ts`、`src-vencord/encode/video_time.ts`、`src-vencord/encode/clock_map.ts`、`src-vencord/audio/chunk.ts` |
| hello と pong の clock_us、hello の clock_base | `src-obs/ws/control_messages.h`、`src-obs/ws/listener.cpp`、`src-obs/pipeline/sender_clock.h`、`src-obs/pipeline/pipeline.cpp` の `handle_sender_clock_base` | `src-vencord/transport/client.ts`、`src-vencord/transport/control.ts`、`src-vencord/index.ts`、`src-vencord/native.ts` の `hrtimeNs` |
| facts | `src-obs/pipeline/facts_state.h` | `src-vencord/core/facts.ts`、`src-vencord/index.ts` (送る頻度) |
| profiles | `src-obs/pipeline/profiles_state.h` | `src-vencord/core/facts.ts` |
| avatar | `src-obs/ws/control_messages.h` の `parse_avatar`、`src-obs/pipeline/profiles_state.h` | `src-vencord/core/avatar.ts`、`src-vencord/core/facts.ts` の `AvatarOutbox`、`src-vencord/index.ts` |
| media と limits の値 | `src-obs/pipeline/pipeline.h` | `src-vencord/core/gate.ts`、`src-vencord/encode/limits.ts` |
| main_account、move_to_main_call、leave_call | `src-obs/ws/control_messages.h`、`src-obs/ws/listener.cpp`、`src-obs/pipeline/pipeline.cpp`、`src-obs/dock/dock_poll.cpp` | `src-vencord/transport/control.ts`、`src-vencord/index.ts`、`src-vencord/adapter/vencord.ts` |
| receive_scope | `src-obs/pipeline/receive_scope.h`、`src-obs/pipeline/stream_router.h` の `admit_sharers`、`src-obs/pipeline/pipeline.cpp` | `src-vencord/core/state.ts` の `watchScope`、`src-vencord/index.ts` |
| call_name と main_call | `src-obs/pipeline/facts_state.h`、`src-obs/dock/build_observation.cpp` の `call_of` | `src-vencord/core/facts.ts`、`src-vencord/adapter/call_info.ts`、`src-vencord/adapter/vencord.ts` |
| log の種類 | `src-obs/pipeline/pipeline.cpp` の `handle_log`、`src-obs/plugin-main.cpp` の `cordmux_js_log_sink` | `src-vencord/core/logbook.ts`、`src-vencord/index.ts` |
| stream_check と stream_check_result | `src-obs/pipeline/receive_restart.h`、`src-obs/pipeline/pipeline.cpp` | `src-vencord/core/stream_check.ts`、`src-vencord/index.ts` |

テストは次のファイルにある。

- 受信側: `src-obs/tests/test_ws_wire.cpp`、`test_ws_control_messages.cpp`、`test_ws_listener.cpp`、`test_ws_secret.cpp`、`test_pipeline_facts_state.cpp`、`test_pipeline_profiles_state.cpp`、`test_decode.cpp`、`test_pipeline_receive_scope.cpp`、`test_pipeline_stream_router.cpp`、`test_dock_build_observation.cpp`、`test_pipeline_receive_restart.cpp`、`test_pipeline_sender_clock.cpp`
- 送信側: `src-vencord/tests/core_wire.test.ts`、`transport_control.test.ts`、`transport_client.test.ts`、`transport_socket.test.ts`、`core_facts.test.ts`、`core_avatar.test.ts`、`encode_annexb.test.ts`、`encode_clock_map.test.ts`、`encode_sender_clock.test.ts`、`encode_ticks_origin.test.ts`、`encode_video_time.test.ts`、`audio_chunk.test.ts`、`adapter_call_info.test.ts`、`core_state_decisions.test.ts`、`core_logbook.test.ts`、`core_stream_check.test.ts`
