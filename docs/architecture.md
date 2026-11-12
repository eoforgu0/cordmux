# cordmux の仕組み

この文書は、cordmux の構造を、開発に初めて参加する人が該当するコードへ進めるように説明する。現在の動作と、その動作にした理由を書く。利用者から見た動作は docs/spec.md に、WebSocket のメッセージの形は docs/wire_format.md に、画面の決まりは docs/ui_guidelines.md に、テストは docs/testing.md に、公開の手順は docs/release.md にある。

「WebSocket のメッセージ」は送信側と受信側の間で送る 1 つの単位、「ドックのメッセージ」はドックに表示する警告と状態の行を指す。

## 全体図

cordmux は 2 つのプログラムからなる。

- 送信側 (src-vencord): Vesktop の中で動く Vencord の userplugin である。Discord の通話の画面共有と声を WebRTC のトラックから読み出し、映像は WebCodecs で H.264 にエンコードし、音声は PCM のまま、WebSocket で送る。
- 受信側 (src-obs): OBS の中で動くプラグイン cordmux.dll である (C++ と Qt)。127.0.0.1 で待ち受け、受け取った映像と音声をデコードとタイムスタンプの調整を通して、OBS の合成のソースに渡す。ドック、設定、録画も受け持つ。

2 つは同じ PC の上で動く。受信側が待ち受け、送信側が接続する。通話への参加と画面共有の視聴は Vesktop のアカウントで行い、cordmux が Discord に対して行う操作は、視聴の開始と停止と、[移動] と [退出] の実行だけである。

```
 Discord の通話 (WebRTC)
        |
        v
+------------------ Vesktop (Electron) -------------------+
|  Vencord + 送信側 (src-vencord)                          |
|   adapter -> core -> encode / audio -> transport         |
|   native.ts: session.json を読む                         |
+--------------------------|-------------------------------+
                           |  WebSocket (127.0.0.1)
                           |  映像 (H.264) / 音声 (PCM) / 制御用の JSON
                           v
+------------------------ OBS ----------------------------+
|  cordmux.dll (src-obs)                                   |
|   ws -> pipeline -> decode -> align -> 表示のキュー (D)  |
|              |                              |            |
|              |                              v            |
|              |                  sources: 合成のソース    |
|              |                  (映像、混ぜた音、名前ラベル)|
|              +-> record: セグメントファイルと録画ログ    |
|   dock / ledger / config / install / bundle / diag       |
|   platform: Windows に依存する処理                       |
+----------------------------------------------------------+
   ^
   |  %LOCALAPPDATA%\cordmux\session.json
   |  (受信側が書き、送信側が接続のたびに読む。ポート番号とトークン)
```

図の D は、表示のバッファーの長さである (設定の `display_buffer_ms`)。

通話の映像と声が配信に出るまでは「映像と音声の流れ」に、状態の出し方は「ドック」に、Vesktop への結び付けは「配布、インストール、更新、アンインストール」にある。

## コンポーネントと状態の管理

### コンポーネント

受信側のフォルダーの役割は、`src-obs/README.md` の表にある。

- 判断を持つ部分は、libobs にも Qt にも依存しないコード (`ledger/`、`config/`、`align/`、`record/` の大半など) にして、OBS を起動せずに単体テストする。libobs や Qt を呼ぶのは、`pipeline/pipeline.cpp`、`sources/`、`dock/`、`plugin-main.cpp`、`install/uninstall_real.cpp`、`bundle/export_bundle.cpp` に限る。
- `ledger/` と `bundle/` は状態を持たない。

送信側のフォルダーの役割は `src-vencord/README.md` にある。`adapter/` は Discord と Vencord の内部から事実を読み、視聴を始めて止めることと、[移動] と [退出] の実行だけをする。誰を視聴するか、依存箇所の警告、facts、読み出しのゲートの判断は、Vencord を import しない純粋関数の `core/` が持つ。Discord の内部が変わったときに壊れる部分を `adapter/` に閉じ込め、判断を Vencord なしでテストするためである。

### 状態の所有者

状態は、所有者を 1 つに決める。ほかの部分は値のコピーを読む。

| 状態 | 所有者 | 書く所 | 保存先 |
|---|---|---|---|
| 設定 `Settings` | ドックが持つコピー | ドックだけ (起動のときに `plugin-main.cpp` が読む)。キーごとの書き換える所は `SettingsOwner` で決める (「ドック」の「設定の書き換え先」) | settings.json |
| 枠の割り当て表 `AllocationTable` | `plugin-main.cpp` (`Pipeline` は参照を借りる) | `Pipeline` が変える | 保存しない |
| これまでに見た人の一覧 | ドック | ドック | seen_people.json |
| 経路、デコード、キュー、混ぜた音、段階ごとの件数 | `Pipeline` | `Pipeline` | 保存しない |
| 録画のセッション | `Recorder` (`Pipeline` が作る) | 書き込むスレッド | 録画ログ |
| ドックのメッセージの表示状態 | `DockPoll` | `ledger::stabilize` の結果 | 保存しない |
| 既読のメッセージ `NoticeReadState`、状態タブの累計と履歴 `StatusState` | `DockPoll` | `ledger::settle_first_line`、`ledger::settle_status` の結果 | 保存しない |
| 全画面で表示している人 (`focus_user_id_`) | `DockPoll` | `SetFocus` の `Intent` と、その人が描かれなくなったときと無効にしたときの後始末 (`settle_focus`) | 保存しない |
| 全画面のホットキーに割り当てたキー | OBS | OBS の [設定] (`plugin-main.cpp` は読むだけ) | OBS のプロファイルの basic.ini の [Hotkeys] |
| OS の状態 (Vesktop が動いているか、Vesktop の設定、空き容量) | `OsProbe` | ワーカーのスレッド | 保存しない |

ドックは 200 ms ごとに `Pipeline::snapshot()` と `OsProbe::latest()` で値のコピーを読む。`Pipeline` への操作 (`assign_slot()`、`release_slot()`、`set_recording()` など) は関数で渡し、結果は次のスナップショットで読む。

割り当て表で枠を持つのは、いま画面共有をしている人 (声の枠は、いま通話に参加している人) だけで、画面共有をやめるか通話から抜けると、`pipeline/slot_release.h` の判断で枠が空く。Vesktop のアカウントが通話を抜けて roster が空になったときも、全員の枠が空く (roster が届いていないか読めない間と、Vesktop との接続が切れている間は空けない)。画面の枠は facts の sharing だけで判断し、`handle_facts` だけが空ける。声の枠は facts の roster で判断するが、その枠で声を出している人の声のストリームが直前の configure に載っている間は空けない (声の経路は configure でしか戻らないので、roster が一瞬欠けただけで、配信の音が次の configure まで消えるのを防ぐ)。声の枠は `handle_facts` と `handle_configure` の両方が判断するので、facts と configure のどちらが先に届いても、ストリームが消えて roster にもいなければ、最後に空く。画面の枠を `handle_configure` で空けないのは、configure が facts より先に届いたときに、画面共有を始めたばかりの人を外してしまうためである。

### スレッドとロック

スレッドは次のとおりである。

- OBS の UI のスレッド (ドック、`DockPoll`、`obs_module_load`)
- io_thread (asio。`ws::Listener` のコールバックから `Pipeline` を呼ぶ)
- ストリームごとのデコードのスレッド
- `frame_pacer_loop` と `mixer_loop` (OBS に渡す)
- `stats_loop` と `presence_loop`
- `Recorder` の書き込みと録画ファイルの作成のスレッド
- `OsProbe`
- 名前ラベルの描き直しのスレッド
- ダミー表示の間だけ動く、ダミーのフレームを渡すスレッド (`sources/dummy_feed.cpp`。OBS の描画のスレッドの tick のコールバックが、渡す時機を知らせる。始めたときとキャンバスの大きさが変わったときは、番号の入った背景の画像もこのスレッドで描画する)

`Pipeline` のロックは `mu` を最初に取る。`mu` を持ったまま `pacer_mu`、`mixer_mu`、`Recorder` のキューのロック、ログの出力先のロックを取ってよいが、逆の向きには取らない。順の全体は `src-obs/pipeline/pipeline.cpp` の `Pipeline::Impl` の `mu` の宣言にある。デコードのような重い処理は `mu` の外で行う。

受信のコールバックは `safe_call` で例外を捕まえて、その 1 件を捨てる。io_thread から例外が漏れると `std::terminate` になり、OBS ごとクラッシュするためである。

## 映像と音声の流れ

```
送信側                                      受信側
 track -> 読み出し -> エンコード/チャンク化 -> WebSocket --+--> 録画 (record)
                                                          |
   映像: 経路 -> デコードのキュー -> デコード -> 整列 -> 表示のキュー (D) -> 合成のソースの子
   音声: 経路 -> 時刻の変換 -> AudioMixer -> 合成のソース (obs_source_output_audio)
```

### 送信側

`src-vencord/index.ts` が adapter、core、encode、audio、transport をつなぐ。

1. 視聴。adapter は、画面共有している人 (Vesktop のアカウントを除く) の全員を `selection` として core に渡す。core は、`selection` と `sharing` の両方に入り、さらに受信側が送った視聴する人の範囲 (`watchScope`) に入る人について `start-watching` を決める。範囲が届くまでは空 (誰も視聴しない) で、受信側が everyone を送ると画面共有している全員になる。誰を枠に載せるかは受信側が決める。core が出す判断は `start-watching`、`stop-watching`、`mark-source-switched` の 3 つで、送っている映像や音声を止める判断は型としてない。`index.ts` が adapter の `startWatching()` を呼ぶ。adapter は Discord の関数に `forceMultiple: true` と `noFocus: true` を渡す。渡さないと、Discord がほかの視聴を閉じる。
2. トラックの特定。`ontrack` で届いたトラックを、`ConnectionTracker` が決めた接続の番号 (`pcId`) で分ける (「Discord の内部への依存箇所」)。`pcId` が 1 のボイス接続のトラックは声、Go Live の接続のトラックは画面共有の映像と音声である。Go Live の接続のトラックの人は、その接続で画面共有している人、ボイス接続のトラックの人は、SSRC から core が特定する。特定できないトラックは送らない。
3. ゲート。読み出しとエンコードは、受信側と接続していて、受信側の `media` が真の間だけ行う (`core/gate.ts`)。
4. 映像のエンコード (`encode/session.ts`)。エンコーダの確保は 5 つの段階を上から試す (`encode/ladder.ts`): `native` (受け取った解像度と fps)、`halffps`、`downscaled` (長辺 1280)、`software` (ソフトウェアで 1280x720 の 30 fps)、`unavailable` (その人の映像を送らない)。`prefer-hardware` は失敗してもソフトウェアに切り替わらないので、段階を明示的に下げる。`no-preference` は、切り替わったことが JavaScript に伝わらず、ドックに表示できないので使わない。段階は facts の `stages` で受信側に伝える。確保の後、黒いフレームを 60 フレームエンコードして捨てるウォームアップをする。キーフレームは 2 秒ごとと、受信側からの `keyframe_request` のときに入れる。画質の上限 (`limits`) は、エンコード中は下げる向きだけをすぐ適用する。cordmux の録画が ON の間は、黒埋め用のフレームを作る (「録画ファイルの作成」)。
5. 音声。`audio/chunk.ts` が `AudioData` の断片を 480 サンプルのチャンクに分け、タイムスタンプを付ける (「音声」)。
6. 送信 (`transport/socket.ts`)。`bufferedAmount` が 1 MiB を超えたら映像のフレームを捨て、次のキーフレームまで捨て続ける。音声は 16 MiB を超えるまで捨てない (`transport/backpressure.ts`)。

読むファイル: `src-vencord/index.ts`、`src-vencord/encode/session.ts`、`src-vencord/core/gate.ts`。

### 受信側

io_thread が WebSocket のメッセージを `Pipeline` のコールバック (`handle_video_frame` など) に渡す。映像は次の順に進む。

1. 録画。cordmux の録画が ON なら、経路の有無にかかわらず、アクセスユニットを `Recorder` にコピーして渡す。録画専用のフラグが付いた WebSocket のメッセージ (黒埋め用のフレーム) は、録画だけに渡し、表示の処理に入れない。
2. 経路。`StreamRouter` (`pipeline/stream_router.h`) が、configure の `streams` を割り当て表の枠に結び付け、`stream_id` から枠と人を引く。枠に載せない人には経路を作らず、デコードもしない。
3. デコードのキュー。ストリームごとのキューが 8 個か 512 MiB を超えたら、積んだ分も含めて全部捨て、次のキーフレームまで P フレームを捨てて `keyframe_request` を送る (`pipeline/queue_admission.h`)。デコードが追い付くのを待つより、次のキーフレームから出し直すほうが早い。
4. デコード。ストリームごとのスレッドが `H264Decoder` で I420 か NV12 のフレームにする。設定の `decoder` が `D3D11VA` ならハードウェアデコードを試し、使えなければソフトウェアに切り替える。切り替えは画面に出さず、ログと診断情報に残す。
5. 整列。`TimestampAligner` で OBS に渡すタイムスタンプを求め、映像のオフセットを `AvOffsetTranscriber` に保存する。
6. 表示のキュー。フレームを `FramePacer` に積む。送信側のタイムスタンプに表示のバッファー D (設定の `display_buffer_ms`、既定 100 ms) を足した時刻の後で最初の OBS の tick (OBS が映像を描く周期) に表示されるよう、tick の半分前に `frame_pacer_loop` が OBS に渡す。D が 0 でも表示のキューを通す。
7. 合成のソース。`sources::push_screen_video()` が、枠に対応する非公開の子に、OBS の緩衝を使わない非同期の映像として渡す。

表示の時刻を OBS の緩衝ではなく cordmux が決めるのは、画面の音を映像と同じ D で出して、そろえるためである。D は、送信側でフレームができてから表示に選ばれるまでの長さなので、受信までの遅れより長くしないと、到着のジッターを吸えない。統計の行の読み方は `src-obs/pipeline/README.md` にある。

音声は、経路の種類で画面共有の音か声かを決める (音声の WebSocket のメッセージのヘッダーには種類がない)。画面共有の音はタイムスタンプを調整してから、声は届いた時刻から置く時刻を決め、`AudioMixer` に置く (「音声」)。

読むファイル: `src-obs/pipeline/pipeline.cpp` (`handle_video_frame`、`screen_worker_main`、`push_decoded_frame`、`frame_pacer_loop`)、`src-obs/pipeline/stream_router.h`。

### タイムスタンプの合わせ方

送信側のタイムスタンプには数 ms のジッターがある。そのまま OBS に渡すと、60 Hz の描画でフレームの 15% から 25% が重複か欠落になる。そこで、送信側で映像と音声に同じ時計と同じ規則でタイムスタンプを付け、受信側で映像を一定の間隔のグリッドに合わせ、音声は置く位置をタイムスタンプに連続的に追従させる。

時計は 3 つを区別する。

| 時計 | 中身 | 使う所 |
|---|---|---|
| Windows の単調な時計 (QueryPerformanceCounter) | Chromium の `base::TimeTicks`、`performance.now()`、OBS の `os_gettime_ns`、`std::chrono::steady_clock` | タイムスタンプ、到着の時刻、OBS に渡す時刻のすべて |
| システム時刻 | NTP や手動の変更で合わせ直される。Chromium の `Date.now()` は単調な時計で進めながら 60 秒ごとに合わせ直し、ms に丸める | 画面に表示する時刻、録画のフォルダーの名前、録画ログに残す受け取った時刻 (`first_pts_wall_us` など。後から調べるためだけに持つ)。タイムスタンプの経路には入れない |
| 音を取り出す時計 | Vesktop の PC で、受信した音を再生のために取り出す音の装置の時計 | 音声のサンプル数がこれで進む。単調な時計との速さの差は数十 ppm になりうる |

システム時刻をタイムスタンプに使わないのは、合わせ直しの飛びと、単調な時計との速さの違い (測った例で約 10 ppm) が、時間に比例するずれとして映像と音声の関係に入るためである。音を取り出す時計の速さの差は送信側では直せない (サンプル数で時刻を数えると、その差が時間に比例してたまる) ので、受信側で音声を扱う処理 (`AudioMixer` と録画) が吸収する (「音声の時計の差の吸収」)。

#### 送信側: 送信側の時計への換算

送信側の時計は `performance.timeOrigin + performance.now()` を us にした値である (`encode/sender_clock.ts`)。単調な時計で進み、システム時刻に合わせ直さない。ワイヤー形式の `timestamp_us` は、この時計の値である。

`encode/clock_map.ts` の `mapMediaTime()` が、映像と音声を同じ規則で「メディアのタイムスタンプ + トラックごとの定数」に換算する。メディアのタイムスタンプ (`VideoFrame.timestamp` と `AudioData.timestamp`) は、Chromium のソースでは、どちらも単調な時計の値か、その差である (音声は受信した音を再生のために取り出した時刻、映像は WebRTC が決めた描画の時刻と最初のフレームの描画の時刻の差)。どちらの時計も単調な時計で進むので、本当の定数は時間で変わらない。定数は「到着の時刻 - メディアのタイムスタンプ」から決める。この差は、本当の定数に取り出しの遅れ (JavaScript が受け取るまでの時間) を足した値で、遅れは詰まると増える向きにだけ動く。

- 最初の到着の差を定数にし、その後は下げる向きにだけ動かす。2 秒の期間 (`CLOCK_MAP_WINDOW_US`) ごとに、その期間に見た差の小さいほうから 3 番目 (`CLOCK_MAP_RANK`) が今の定数より小さければ、その値まで下げる。小さい差ほど本当の定数に近いので、起動の直後やトラックの途中に詰まっていた分は、後で詰まりのない到着が同じ期間に 3 つ来れば定数から消える。最初に決めた値で固めると、そのときの詰まりがトラックの間ずっと残り、映像と音声は別々に決めるので、残った分が映像と音声のずれになる。最小にしないのは、同じ期間の外れ値 2 つまでで定数が下がらないためである。
- 上げる向きには、次の不連続のときにしか動かさない。差が上がって見えるのは詰まりで、詰まりで定数を上げると、その分が急なずれになって映像と音声の関係に入る。
- 到着の時刻 - 換算した時刻 が 200 ms 以上 (遅い向き) か -200 ms 以下 (早い向き) の状態が、到着の時刻で 1 秒途切れずに続いたら、メディアの時計が替わったとみなして定数を決め直す (`CLOCK_MAP_JUMP_US`、`CLOCK_MAP_JUMP_CONFIRM_US`)。1 秒より短い詰まりでは決め直さない。映像では、決め直すと送信側のログに異常に関わる行 (`video clock: ... origin moved by ...`) を書き、決め直したフレームをキーフレームにする (受信側の録画は、大きな進みか戻りでセグメントを閉じ、次のセグメントをキーフレームから始めるため)。
- 音声のチャンク (480 サンプル) の時刻は、その先頭サンプルが入っていた断片の時刻 + 断片の中の位置である (`audio/chunk.ts`)。累積のサンプル数からは時刻を作らない。

映像は、取り出した時刻が換算した時刻より 200 ms 以上遅れたフレームを捨てる (`encode/session.ts` の `STALE_FRAME_SKIP_US`)。定数が下がると、返す値が前の値より小さくなることがある (戻る量は、それまで定数に残っていた詰まりの分である)。受信側はこれを吸収する。

#### 送信側の時計と OBS の時計

受信側は、送信側の時計を OBS の時計 (`os_gettime_ns`) に、接続ごとに決めた差を足して換算する (`pipeline/sender_clock.h` の `SenderClockOffset`)。送信側は hello と pong に、送る直前に読んだ送信側の時計 (`clock_us`) を載せる。受信側は、メッセージを受け取ったときに読んだ OBS の時計と `clock_us` の差の最小値を、2 つの時計の差にする。読むのは、解釈と接続の処理の前である (`ws::Listener` の `receive_clock_us`)。送信側は時計を読んでから送るので、この値は「本当の差 + 届くまでの遅れ」で、本当の差より小さくならない。最小値には、届くまでの遅れの最も小さかったメッセージの遅れ (同じ PC の中で 1 ms 未満) だけが残る。ping と pong の往復の真ん中を取る方法は使わない。送信側の JavaScript が忙しいと、ping が届いてから読むまでの遅れが返りの遅れより長くなり、真ん中が本当の差から往復の半分までずれるためである。

映像を渡す時刻 (表示のキュー)、画面の音を置く時刻、遅れの統計は、どれもこの 1 つの差で換算する。差が変わらない間は、差に残る誤差 (最小値に残った届くまでの遅れ) は映像と音声に同じだけ入り、映像と音声の関係には入らない。

差が変わった直後は、映像と音声で効き方が違う。差が変わるのは、それまでの最小値より遅れの小さい hello か pong が届いたときで、変わる量はそれまでの最小値に含まれていた遅れの分である。映像は、次に渡すフレームから、渡す時刻がその量だけ動く。画面の音は、置く位置を目標に追従させる処理 (「音声の時計の差の吸収」) が、差の変化を目標の急な変化として受け取る。変化の量が置き直しのしきい値より小さければ置き直さずに速さで戻すので、追いつくまで (時定数 8 秒) の間、音は映像に対してその量だけずれ、その後はそろう。しきい値を超えれば置き直して、すぐにそろう。

#### 受信側: グリッドへの整列

`TimestampAligner` (`src-obs/align/timestamp_aligner.h`) は、ストリームごとに 1 つ作る。送信側のタイムスタンプを 1 フレームずつ受け取り、推定した周期の間隔で並ぶグリッドの上のタイムスタンプを返す。返す値は前の値より必ず大きい。定数と理由は `AlignerConfig` のコメントにある。

1. 周期の推定。(フレーム番号, タイムスタンプ) の指数重み付き最小二乗の傾きを周期とする。忘却の時定数は 600 フレーム (60 fps で 10 秒) である。隣り合うフレームの差の移動平均にしないのは、2 フレームぶんのジッター (10 ms) がそのまま推定に乗るためである。重みが足りない立ち上がりは、周期の初期値か差の平均を使う。
2. 誤差と補正。出力と送信側の差を時定数 120 フレームの指数移動平均でならし、不感帯 (500 us) を超えた分を時定数 64 フレームで割った量を 1 フレームで補正する。上限は `max(500 us, 推定周期の 3%)` である。補正はフレームの間隔の乱れとして見えるので、上限を間隔に対する割合でも決める。
3. 不連続。グリッドの予想位置 (前の出力 + 推定周期) より送信側が 0.5 周期を超えて先にあれば、送信側がフレームを落としたとみなして、グリッドを余分に進める。前のフレームとの差ではなく、予想位置からの残差で判定するのは、差には 2 フレームぶんのジッターが乗り、60 fps の 1.5 倍 (25 ms) を 1.4% のフレームが偶然に超えるためである (`jump_ratio`)。残差が余白を超えたフレームは、次のフレームでも超えたときに確定する (`jump_confirm_frames`)。遅れて届いたフレームの次が早く届くジッターを、欠落と読み違えないためである。疑ったフレームはその場で返すので、出力の遅延は増えない。
4. 再ロックと再整列。直近 8 フレームの差の平均が推定周期から 20% 以上ずれ、同じ向きにずれた差が過半数なら、送信側の fps が変わったとみなして、周期を差の中央値に置き直す (再ロック)。誤差が `max(3 周期, 100 ms)` を超えたら、送信側の時計が不連続に変わったとみなして、推定をやり直す (再整列)。送信側が先なら出力を送信側に合わせ、出力が先なら、戻せないので誤差を補正で戻す。
5. 送信側のタイムスタンプが進んでいないフレームは、推定に入れずにグリッドを 1 フレーム進める。

不連続の判定で差し引く誤差の量に、上限 W (`jump_excuse_max_us`、30 ms) を設ける。画面が静止して送信側がフレームを間引くような、間隔が不規則な入力で、出力が送信側より先に出たままになるのを防ぐためである。

画面共有の音声には、同じストリームの映像で付いたオフセット (出力 - 送信側) を足す (`AvOffsetTranscriber::map_audio()`)。映像だけを整列すると、同じ時計で付いた映像と音声の関係が崩れるためである。最初の映像が届く前の音声は、そのまま返す。

録画は、この整列を通さない。録画の時計には送信側のタイムスタンプだけを使い、映像の戻りを `record/record_clock.h` の `classify_pts()` で 3 段階に分ける。

- 1 フレームの間隔より小さい戻りは、前の値 + 1 フレームの間隔へずらす (`Pushed`)。
- 1 フレームの間隔以上 1 秒未満の戻りは、ずらして原点の置き直しとして数える (`Reanchored`)。
- 1 秒以上の戻りか前への大きな進みは、セグメントを閉じる (`Jumped`)。

送信側は換算の定数を下げたときに 1 秒未満だけ戻ることがあるので、しきい値 1 つでは分けられない。録画の音声は「音声の時計の差の吸収」の規則で書く。

#### 音声の時計の差の吸収

音声のサンプルは音を取り出す時計で進み、タイムスタンプは単調な時計で付く。2 つの速さの差 (数十 ppm) は、受信側で音声を扱う処理が、ごくわずかなサンプルレートの変換で連続的に、両方の向きで吸収する。配信の `AudioMixer` と録画は、同じ `align::AudioDriftCorrector` (`src-obs/align/audio_drift.h`) を、組 (配信) かストリーム (録画) ごとに 1 つ持つ。

- 目標。チャンクごとに、その先頭サンプルを置きたい位置 (サンプルの番号) を受け取る。配信では OBS の時計に換算した at_us、録画ではタイムスタンプ - セグメントの原点である。at_us は、画面の音では送信側のタイムスタンプ + 映像のオフセット + D + tick の半分、声では届いた時刻 + `kVoiceMixJitterUs` + 声の遅延である (「音声」)。
- 追従。前のチャンクの続きに書き、続きの位置と目標の差を、時定数 8 秒の比例と積分で変換の比に戻す (積分が時計どうしの速さの差を持つので、一定の速さの差では位置の差が 0 に戻る)。差は時定数 1 秒の指数移動平均でならしてから使い、半サンプル以内の差では比を動かさない。比を変える量の上限は 1000 ppm (音の高さで 1.7 セント) である。
- 変換。比が 1 で端数がない間は、受け取ったサンプルをそのまま書く。それ以外は、Kaiser 窓の sinc (32 タップ、位相 256 と直線の補間) で、書く位置の端数の値を計算する。変換の計算に後ろの 16 サンプルが要るので、書き出しはその分だけ入力より遅れる。
- 置き直し。目標と続きの位置の差が 45 ms を超えたら、本当の不連続 (共有の始め直し、受信の途切れ、時刻の飛び) とみなして目標の位置に置き直す。配信では、続けて書くと出し終えた位置より前になるときも置き直す。録画では、後ろへの置き直しは 0 のサンプルで埋め、前への置き直しはセグメントを閉じて次のセグメントを始める (書いたサンプルは消せないため)。45 ms より小さい差 (上流の段、音の装置が数チャンクずつ取り出すときの時刻の揺れ) は置き直さずに速さで戻す。45 ms は、音と映像のずれに人が気付き始める音の先行の量である (ITU-R BT.1359-1)。
- 配信の声の組だけは、置き直しのしきい値を 70 ms にする。声は届いた時刻から置くので、送信側の JavaScript が止まった後にまとめて届くと、目標が続きの位置から離れる。置き直すと、前へ戻るときは置いたのにまだ出していない声を上書きして失い、後ろへ進むときはその分の無音が入る。置き直さずに速さで戻すと、声の遅れがしばらく増えるが、声は失わない。声には対になる映像がないので、画面の音 (音と映像のずれに気付く 45 ms) より広くしてよい。70 ms は、OBS がソースの音の時刻の揺れを続きとして扱う幅 (`obs-source.c` の `TS_SMOOTHING_THRESHOLD`) と同じ値で、失う声と遅れの増え方の釣り合いを OBS のふつうのソースと同じにする。録画の声はタイムスタンプで置くので 45 ms を使う。
- 値の 4 つ (置き直しのしきい値、比を変える量の上限、時定数、配信の声の置き直しのしきい値) は、設定の `audio_resync_ms`、`audio_drift_max_ppm`、`audio_drift_time_constant_ms`、`voice_audio_resync_ms` で変えられる。時刻の揺れの大きさは音の装置と PC の負荷で変わり、上限は音の高さの変化と戻す速さの得失を決めるためである。

読むファイル: `src-obs/align/timestamp_aligner.h`、`src-obs/align/av_offset.h`、`src-obs/align/audio_drift.h`、`src-obs/pipeline/sender_clock.h`、`src-vencord/encode/clock_map.ts`、`src-vencord/encode/sender_clock.ts`。

### 音声

音声は、送信側で Opus にエンコードせず、32 ビット浮動小数点の PCM (48 kHz、2 チャンネル、480 サンプル = 10 ms) のまま送る。Discord のエンドツーエンドの暗号化 (DAVE) が WebRTC のレシーバーの変換を使うので、エンコードされた Opus を取り出せず、デコードした後の音声しか読めない。Opus で送るにはエンコードし直すことになり、録画の音質が落ちる。同じ PC の中の WebSocket なので、1 ストリームで毎秒 384 KB の帯域は問題にならない。

受信側は、画面共有の音と声を `AudioMixer` (`pipeline/audio_mixer.h`) で混ぜ、1 つの合成のソースから出す。

- 自分で混ぜる理由。OBS は、1 つのソースに時刻が重なるように渡された音を、後の音で前の音を消す形で置く。参加者ごとの音を同じソースに渡すと、互いに消し合う。
- 人と種類の組。人 (`user_id`) と種類 (声か画面共有の音) の組 (`MixerKey`) ごとにバッファーを持ち、指定した時刻に追従させて音を書く (「音声の時計の差の吸収」)。組ごとの利得は、バッファーから取り出すときに掛けて足し、10 ms の区間にして返す。利得を書くときに掛けると、D (表示のバッファー。設定の `display_buffer_ms`) だけ先に書いた画面の音の音量が、操作から D だけ遅れて聞こえる。音量とミュートの表 (`MixerLevels`) は `Pipeline` が持ち、組が `MixerRoster` に入るたびに適用し直す。
- 音を書く時刻。時刻はすべて OBS の時計の上にある。送信側のタイムスタンプは、接続ごとに決めた差を足して OBS の時計に換算する (「送信側の時計と OBS の時計」)。画面共有の音は、換算した時刻に D と tick (OBS が映像を描く周期) の半分を足す。映像は tick の上に来るまで平均で tick の半分遅れるためである。声は D で遅らせず、届いた時刻に `kVoiceMixJitterUs` (65 ms) と設定の `voice_audio_delay_ms` を足す。画面の音と映像のずれは、設定の `screen_audio_offset_ms` で調整する。
- 取り出し。`mixer_loop` が 10 ms ごとに取り出し、`obs_source_output_audio` で渡す。区間の終わりが現在 + 55 ms (`kMixerLookaheadUs`) 以内になったら返す。OBS は 1024 フレームの区間を混ぜ、先に渡しておかないと音の時刻を付け直して、音が遅れたまま戻らない。数値の根拠は `audio_mixer.h` のコメントにある。
- 自分の声。サブアカウントを使う構成では、メインアカウントの声も Vesktop に届く。既定ではミュートして配信に混ぜない (OBS がマイクから直接取った声と二重になるため)。受け取った声は、自分の枠の光を決めるのには使う。
- 話している人の光。`mixer_loop` が OBS に出した区間にその人の声の音があったか (利得を掛けた後で -60 dBFS を超えるか) で決め、100 ms 途切れたら話し終わりとする (`pipeline/voice_activity.h`)。送信側の発言のシグナルは、どの接続から来たかを示さず、声と画面共有の音を区別できないので使わない。

録画は、この混合も表示用の補正 (`screen_audio_offset_ms` と `voice_audio_delay_ms`) も通らない。libobs を通らないためである。

読むファイル: `src-obs/pipeline/audio_mixer.h`、`src-obs/pipeline/composite_audio_plan.h`、`src-obs/pipeline/README.md`。

## OBS のソースとシーン

cordmux はシーンも、利用者が見るソースも作らない。利用者が OBS の [+] から、型の ID が `cordmux_composite` の合成のソース `cordmux` を置く (`src-obs/sources`)。

- 描画。参加者ごとの画面共有を、キャンバスを 4 等分した位置に描く (`plan_composite_layout`)。画面共有の枠は 4 つで、声の枠に上限はない。シーンアイテムの変形による自動配置はしない。
- 子。枠ごとに非公開の子のソース `cordmux_tile` を持つ。最初の合成のソースを作るときに作り、最後の合成のソースを削除するとき (と `obs_module_unload`) に解放する。「ソースを追加」の一覧に出さず、シーンコレクションにも保存しない。
- 音。ソースの型は `OBS_SOURCE_AUDIO` を持ち、全員の音を 1 つの行として出す。トラックやフィルターを OBS の普通の操作で掛けられる。
- 名前ラベルと光。映像の後に重ねて描く。画像は CPU の `QPainter` で専用のスレッドが描き、そのスレッドでテクスチャにする。描画のスレッドの中でテクスチャを作ると、作る時間がそのままフレームの描画に足されるためである。
- ダミー表示。通話がなくても見た目を確認できるように、映像を色のタイルに替える。その間は実際の音も出さない。タイルは OBS のキャンバスの大きさの 60 fps で作り、大きさとフレームレートの設定は持たない (普通の映像が滑らかに動き、潰れずに映るかを確かめられればよいため)。本物の画面共有の映像と同じく、NV12 で、描画のスレッドの外 (専用のスレッド) から渡す。本物の映像と同じ経路を通ることも確かめるためで、負荷もキャンバスと同じ大きさの画面共有と同じになる (計算は `sources/dummy_feed.h` の `set_dummy_mode` にある)。
- 送信側に映像を読み出させるのは、現在のシーンコレクションに合成のソースがあるか、cordmux の録画が ON のときだけである。有無は `Pipeline` が 1 秒ごとに確認し、`media` に反映する (`decide_media`)。

### 自分の枠

自分の枠は、合成のソースの左上に、自分の映像を映す枠である。ドックの [表示] タブの [自分の枠に映す映像] で選んだ OBS のソースかシーンを映す。メインアカウントだけの構成では、Vesktop のアカウントの画面共有は受信側に届かないためである (WebRTC は自分が送った映像を返さず、送信側も自分の画面共有を視聴しない)。サブアカウントを使う構成では、「Discordの画面共有」を選ぶこともできる。OBS のソースかシーンを選んでいる間は、メインアカウントの Discord の画面共有を画面共有の枠に載せない (同じ画面が 2 枚出ないようにするため)。声と録画は変えない。

自分の枠を出している間、描ける Discord の人は 3 人までである。手動では、4 人目のチェックを押せない。自動では 4 人目も枠に載るが、表示名の順で後ろの人を描かず、枠に載っていない人の並びに置く (`plan_composite_layout`)。

- 探し方。ドックの周期ごとに、設定の `self_frame_source_uuid` で探し、見つからなければ `self_frame_source_name` で探す。名前で見つけたときと、名前が変わったときは、設定の UUID と名前を書き直す。候補は、映像を持つソースと、cordmux の種類のソースを含まないシーンだけである。合成のソースが自分自身を描く循環を作らないためで、選んだ後に cordmux を足されたら、周期の確認で結び付けを外す。
- 出し入れ。自分の枠を出すかの状態 (`on`、`excluded`、直前の共有の値 `prev`) を、`plan_self_frame()` が、ほかの人の枠と同じ規則で決める。ドックのチェックを付けると `on` を真に、外すと `on` を偽、`excluded` を真にする。OBS のソースかシーンを選んでいて `excluded` が偽のとき、載せる人の選び方が自動なら自分の Discord の共有が続いている間、「自動で枠に載せる」が付いていれば共有の始まりに、`on` を真にする。選んでいない間は `on` を変えない。Discord の人が枠を埋めていれば見送る。共有をやめると `excluded` を偽に戻す。共有の状態が分からない間は、状態を変えない。`on` だけが settings.json に残る。
- 話している判定。自分の枠の人が話しているかは、既定では Discord から受信した声で決める。設定で OBS の音声ソースを選ぶと、その音で決める (`sources/self_speaking_judge.h`)。

利用者から見た動作は docs/spec.md の「自分の枠」にある。読むファイル: `src-obs/sources/self_frame_plan.h`、`src-obs/sources/self_frame_source.h`、`src-obs/sources/composite_source.h`。

### 全画面

全画面は、cordmux のソースに映っている人か画面共有している人のうち 1 人の映像だけを、一時的にキャンバス全体に描くことである。どの人をどの枠に載せているか (割り当て表と並び順) は変えず、描くタイルだけを替える。

```
行のメニューの [全画面で表示]、1 行目の [元に戻す]、OBS のホットキー (UI のスレッドに渡してから)
  -> ledger::apply(SetFocus) -> DockPoll::set_focus() -> focus_user_id_
  -> push_composite_input(): composite_focus_tiles() で全画面にできるタイルを決め、
                             focus_after_layout() で描かなくなった人と無効のときの全画面をやめる
  -> CompositeInput::focus_user_id -> plan_composite_layout(): その人のタイルだけを全面に描く
  -> DockFacts -> Observation の focus_* -> 行のメニューの押せる/押せない、1 行目
```

- 全画面の人は `DockPoll` がメモリにだけ持つ (`focus_user_id_`)。値は人 (userId) で、自分の枠はメインアカウントの userId である。
- 全画面にできるのは、全画面でないときの配置で今描いているタイルの人と、描いていないが画面共有している人である。今描いているタイルの人は、`sources/composite_input.h` の `composite_focus_tiles` が `plan_composite_layout` と同じ `plan_slot_cells` で決めるので、描かない人 (dropped) は入らない。描いていないが画面共有している人は、`composite_focus_others` が facts の sharing から、Vesktop のアカウントと自分の枠で出す人を除いて決める。合成のソースに配置の入力を渡すたびに決め直し、全画面の人がいなくなったか cordmux を無効にしていれば、全画面をやめる (`focus_after_layout`)。合成のソースがないシーンコレクションでは、誰も描かれないのでやめる。シーンコレクションを切り替えている途中は、そのまま続ける。
- 合成のソースは、全画面の人のタイルが全画面でないときの配置にあれば、そのタイルだけを全面に描く (`plan_composite_layout` の `focus_user_id`)。名前ラベルと話している人の光は、描いたタイルに対して描くので、1 人だけを全体に置くときと同じになる。全画面で描かれなくなったタイルの人は、名前ラベル付きで描くタイルがないので、画面共有していない参加者の並びに出る (`sources/composite_label_plan.h` の `composite_roster_shows`)。声と画面共有の音声は、全画面に触れない。cordmux の録画は、描いていない人を全画面にしたときだけ、その人の画面を対象に加える (下の項目)。
- 描いていない人を全画面にしたとき (`DockPoll::focus_off_slot_`) は、その人を `Pipeline::set_focus_target` に渡す。経路の表は、その人が画面の枠を持たなければ、割り当て表に触らずに `config::kFocusOnlySlot` (合成のソースの 5 つ目の子) への経路を作り (`AssignPolicy::focus_user_id`)、受け取る範囲が Slots でも視聴の範囲と録画の対象に入れる。その子は映像だけを受け、画面共有の音の組は作らない。合成のソースは、その子に映像が届いてから全面に描き (`CompositeLayoutRequest::focus_extra_slot` と `focus_extra_ready`)、届くまでは前の配置のまま描く。描けずに dropped になった人は、その人の枠の子を全面に描く。全画面をやめると、経路と視聴の範囲と録画の対象から外す。
- ダミー表示の間は、ダミーのタイルの userId を、その枠に載っている人 (いなければダミーの人の ID) にする (`sources/dummy_feed.h` の `dummy_tile_user_id`)。ダミーに切り替えても、全画面の人の枠のダミーを全面に描き続ける。
- ホットキーは `plugin-main.cpp` が `obs_hotkey_register_frontend` で登録する。コールバックはホットキーのスレッドから来ることがあるので、`QMetaObject::invokeMethod` で UI のスレッドに渡し、`DockPoll::press_focus_hotkey` (位置から人を選ぶのは `focus_hotkey_target`) と `press_focus_restore_hotkey` が、行のメニューと同じ `ActionId::SetFocus` を通す。割り当てたキーは、OBS が [設定] で変えたときにプロファイルの basic.ini の [Hotkeys] に登録の名前で書く。OBS が読み込むのは自分のホットキーだけなので、cordmux が登録の直後と `OBS_FRONTEND_EVENT_PROFILE_CHANGED` で読み込む。

利用者から見た動作は docs/spec.md の「枠と配置」にある。読むファイル: `src-obs/sources/composite_input.h`、`src-obs/sources/composite_layout.h`、`src-obs/dock/dock_poll_layout.cpp`、`src-obs/plugin-main.cpp`。

### シーンコレクションを切り替えたとき

OBS は、シーンコレクションを切り替えると、すべてのソースを破棄してから作り直す。その間の cordmux の動作は、次のとおりである (`plugin-main.cpp` の `on_frontend_event`)。

| OBS のイベント | cordmux の処理 |
|---|---|
| `OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING` | 割り当て表を凍結する (`begin_scene_collection_change`)。凍結の間は、映像も音声も OBS に渡さない。割り当てと受信は変えない。調べてあった合成のソースの有無は、現在の値ではないことにする |
| `OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP` | 自分の枠の音声ソースに付けたコールバックと、自分の枠に映すソースへの結び付けを外す。参照を残して OBS の破棄を待たせると、OBS がエラーを書いて設定を保存しなくなる。終了のときも通る |
| `OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED` | 凍結を解く。合成のソースの有無を調べ直し、`media` を決め直して送信側に送る。ドックの周期が、自分の枠のソースを探し直す |

割り当て表、参加者ごとの音量とミュート、録画は、切り替えの影響を受けない。OBS を起動した直後も同じ理由で、シーンコレクションを読み終える (`OBS_FRONTEND_EVENT_FINISHED_LOADING`) まで configure の適用を保留し、最新の 1 通だけを覚えて、読み終えたときに適用する。

読むファイル: `src-obs/config/allocation.h`、`src-obs/pipeline/pipeline.h` (`on_scene_collection_changing` など)。

## ドック

ドックは、OBS の UI のスレッドで動く `DockPoll` (`src-obs/dock/dock_poll.h`) が、200 ms ごとに更新する。何を出すか、操作を押せるかは `ledger/` の純粋関数が決め、`dock/` は判断しない。

```
Pipeline::snapshot() --+
OsProbe::latest() -----+--> build_observation() --> Observation
設定、見た人の一覧 ----+                                |
                                 ledger::decide_dock()  (待ちを通す前の View)
                                                        |
                                 ledger::stabilize()    (表示と消去の待ち、並び)
                                                        |
                                 ledger::settle_first_line()  (1 行目、既読、状態タブの件数)
                                 ledger::settle_status()      (状態タブの表と履歴)
                                                        |
                                              DockWidget::render()
押された操作 -> ledger::apply(Observation, ActionId, ActionArgs) -> Intent -> Pipeline、ファイル、ダイアログ
```

- `Observation` (`ledger/observation.h`) は、ポーリングのたびに作り直す。時刻、時差、しきい値も値として持つので、`decide_dock()` は時計もファイルも OS も読まず、同じ入力には同じ結果を返す。
- `DockPoll` は、前の周期の `stabilize()` の結果を持つ。表示するまでの待ちと消すまでの待ちは、前の状態がないと決められないためである。既読のメッセージ (`NoticeReadState`) と、状態タブの累計と履歴 (`StatusState`) も、前の周期の値を `DockPoll` が持って純粋関数に渡す。
- 押された操作は `ledger::apply()` を通す。返った `Intent` を実行するのは `DockPoll` の各部分 (`dock_poll_settings`、`dock_poll_install`、`dock_poll_export` など) である。
- ほかのスレッドから UI のスレッドに処理を渡すのは、診断情報のエクスポートと、`Pipeline` の受信のスレッドからの枠の並べ直しの依頼と、全画面のホットキー (「OBS のソースとシーン」の「全画面」) の 3 か所に限る。
- 合成のソースがあれば、ドックは配置の入力 (`CompositeInput`) も作って渡す。

テキストは ini のキーで持つ (`src-obs/data/locale/ja-JP.ini` と `en-US.ini`)。コードの中のキーと ini のキーは `test/lint/locale_ids.py` が突き合わせる。画面の構成と文言の決まりは docs/ui_guidelines.md にある。

更新の間隔の 200 ms は既定の値である (設定ファイルの `dock_poll_ms`。設定ウィンドウには置かない)。更新 1 回の時間は、`dock_drive` の positions のシナリオが出す「(g) 更新 1 回の時間」で測る (95 パーセンタイル。`Pipeline::snapshot()` は含まない)。4 人と 8 人は、3 人の共有の有無が毎回変わる更新でも 2.6 ms 以下である。30 人では、何も変わらない更新が 3.7 から 5.5 ms、毎回変わる更新が 6.8 から 11.5 ms で、実行ごとに揺れる。200 ms ごとなら、8 人で UI のスレッドの 1.3% 以下、30 人で毎回変わる場合でも 6% 以下で、OBS の配信の描画は別のスレッドなので影響しない。100 ms は、4 人でも変わる更新が 2 ms を超えるので選んでいない。

### ドックの構成

`DockWidget` (`dock/dock_widget.h`) が、上から次の要素を並べる。それぞれの高さと位置は、メッセージや外部の出来事で動かさない。

| 要素 | ファイル | 役割 |
|---|---|---|
| 1 行目 | `dock/first_line.*` | 押すと状態タブを開くボタン。中身は `settle_first_line()` が決める |
| メインとサブの行 | `dock/accounts_bar.*` | 名前、通話の名前、[変更]、[移動]、[退出]。サブの行はサブアカウントを使う構成だけにある |
| タブ | `dock/dock_tabs.*` | [参加者] [音声] [表示] [状態] の 4 つのタブのバーと、ページの切り替え。開いているタブは設定の `dock_tab` に残す |
| 参加者タブ | `dock/people_list.*`、`person_row.*`、`row_menu.*`、`name_check.*` | 枠の数、2 組に分けた一覧、行のメニュー、入れ替える相手のメニュー |
| 音声タブ | `dock/audio_tab.*`、`ledger/audio_tab.*` | 1 人 2 行の音量のつまみとメーター。行の並びと状態、メーターが出す値と色は `ledger/audio_tab.*` が決める |
| 表示タブ | `dock/look_tab.*`、`ledger/look_tab.*`、`dock/deferred_save.*` | 枠、名前ラベル、話している人の強調。変えた値はすぐ配信画面に渡す。間引きの規則は `ledger/look_tab.*` が持つ |
| 状態タブ | `dock/status_tab.*`、`dock/notice_list.*`、`ledger/status_tab.*` | 現在のメッセージ、録画、映像の受信の表と [映像のバッファ]、履歴。累計と履歴の規則は `ledger/status_tab.*` が持つ |
| 下のバー | `dock/bottom_bar.*` | 録画のボタンとドックのメニューのボタン |
| 押した場所の隣の確認 | `dock/inline_confirm.*`、`ledger/inline_confirm.h` | 配信中の [退出] だけを確認する (録画の終了、[cordmuxを無効にする]、ダミー映像の表示は、確認なしで実行する)。確認するかの判断は `confirm_needed()`。ダイアログにしない (フォーカスを奪うため) |

メーターは音声タブを開いている間だけ動かし、状態タブの直近 1 分の列は、状態タブが見えている間だけ `Pipeline` から受け取る。

メインアカウントとサブアカウントの行は、参加者の一覧とは別に、ドックの上部に固定する。サブアカウントは参加者の一覧に出さない。[移動] と [退出] は `ActionId` を通り、`Intent` (`MoveToMainCall`、`LeaveCall`) を受けた `DockPoll` が `Pipeline` を通して、押したときだけ送信側へ送る (`move_to_main_call`、`leave_call`。形式は docs/wire_format.md)。送信側は失敗しても何も返さないことがあるので、`DockPoll` は押した種類と時刻を `OperationFacts::call_action` に持つ。押してから `call_action_timeout_ms` (settings.json の値。既定 5 秒) たっても結果が出ていなければ、レベル 2 のメッセージ `CallActionFailed` を出す (`ledger::call_action_failed()`)。結果は、`[移動]` ではメインアカウントが参加者の一覧に入ること、`[退出]` では通話からいなくなることである。もう一度押すか、結果が出るか、接続が切れると記録を捨てる。

### 設定の書き換え先

`settings.json` のキーごとに、値を書き換える所を 1 つに決める。表は `dock/settings_fields.h` の `SettingsOwner` で持つ。

| 区分 | 書き換える所 | 反映 |
|---|---|---|
| `SettingsWindow` | 設定ウィンドウの入力欄 | [OK] と [適用] |
| `DockTab` | ドックの表示タブと状態タブの入力欄 | すぐ反映する。保存は変更の少し後にまとめる |
| `Action` | `ActionId` を通る操作 (有効と無効の切り替え、メインアカウントの変更) | 押せるかは `ledger::apply` が決める |
| `FileOnly` | 画面に表示しない値 (settings.json を手で書いたときだけ変わる値と、ウィンドウの大きさと位置) | |

1 つの値を、反映のしかたが違う 2 つの場所で変えられるようにしない。設定ウィンドウの [適用] は `SettingsWindow` の行だけをコピーするので、ウィンドウを開いた時点の古い値で、表示タブの値とメインアカウントを上書きしない。キーを足したときは表にも行を足す。足し忘れると `test_dock_settings_fields.cpp` が失敗する。

### 表示の規則

`src-obs/ledger` は、2 つの表で状態からドックの表示を決める。

- `primary`: 表を上から見て、最初に条件が成り立った 1 行。ドックの 1 行目の状態の要約を作る。
- `notices`: 条件が成り立った行を、すべてドックのメッセージにする。表の並びは優先順位ではない。

表が持つのは行ごとの条件で、テキスト、ボタン、メッセージのレベルは `rules_dock.cpp` の `decide_dock()` が決める。操作の失敗と、すでに起きた損失 (cordmux の録画の途切れなど) のドックのメッセージは、現在の状態だけでは決まらないので、`Observation` の操作の結果を見る別の規則で作り、`notices` の後ろに並べる。

メッセージのレベルは、メッセージの種類では決めず、次の順に問うて決める。

1. 配信者がほかの場所で気付けるか。気付けるなら行だけにする。
2. 現在の配信か cordmux の録画が損なわれていて、今できることがあるか。あればレベル 1。
3. そうでなければレベル 2、何かをさせないものはレベル 3。

同じ状態でも、損なうものでレベルが変わる。たとえば、映像が届かなくなった人が枠に載っているか録画の対象でも、その人が 1 人だけなら行に出すだけで、メッセージにしない。規則ごとのメッセージのレベルとログレベルの対応は `rule_log_level()` にあり、`src-obs/tests/test_ledger_tiers.cpp` が確認する。決まりの全体は docs/ui_guidelines.md の「メッセージのレベルと表示方法」にある。

### 1 行目と既読

1 行目に出すのは、レベル 1 の見出し、「cordmuxは無効です」、未読のレベル 2 の見出し、「全画面: (名前)」、「ダミー映像を表示中」、状態の要約の順に 1 つである。「cordmuxは無効です」と、「全画面: (名前)」から下は、`decide_dock()` が `fallback_line` と `read_line` に入れる。`ledger::settle_first_line()` (`ledger/notice_tracker.h`) が、前の周期の `NoticeReadState`、`stabilize()` を通した View、配信中か、状態タブを開いているかを受け取り、1 行目、状態タブの件数 (レベル 1 と 2 の合計。0 なら出さない)、次の `NoticeReadState` を返す。

- 状態タブを開いている周期に出ているレベル 2 は、既読にする。ただし無効の間は既読にしない (有効に戻すと 1 行目に出る)。
- レベル 1 は既読にならず、解決するまで 1 行目に残る。ただし、cordmux の録画だけが止まった失敗 (空き容量が尽きて録画が止まった、録画の開始に失敗した) は、レベル 2 と同じに既読にする (`Notice::read_on_status_tab` が真のレベル 1)。
- 既読にしたレベル 2 は、読んだときの段階 (`Notice::read_stage`) を覚える。同じキーのまま段階が進んだら (空き容量の警告が残り 5 分以下になったとき)、未読に戻す。
- 既読のメッセージが消えて、別の配信で再び出たら未読に戻す。同じ配信の中で再び出たときは既読のままにする。配信の外では、消えて 60 秒 (`read_forget_us`) 以上たってから出たら未読に戻す。
- レベル 3 は 1 行目に出さず、件数にも数えない。

### メッセージ

ドックのメッセージは、状態から毎回作り直す。閉じる操作はなく、状態が直れば消える。メッセージの本文と操作のボタンは状態タブにだけ置き、一覧の上には挟まない。

- 待ち。推測で決まる状態 (ハートビートの応答が届かない、映像が届かなくなった) は、`stabilize()` が表示の前と消す前に待つ。規則ごとに `Settle` で待ち方を持つ。`Immediate` は待たない。`Debounced` は表示の前も消す前も待つ。`ClearOnly` は、表示の前の待ちが規則のしきい値そのもの (映像が止まったとみなす `video_stall_ms` など) で、消すときだけ待つ。Discord から画面共有の終わりが届いたような、確かに分かることは待たない。既定は表示 2 秒、消去 1 秒 (設定の `notice_show_delay_ms` と `notice_clear_delay_ms`) である。
- 並び。メッセージのレベルの順に並べ、同じレベルの中では表示した順に並べる。読んでいる途中のメッセージを動かさないためである。
- ログ。表示したときと消したときに、OBS のログに 1 行ずつ書く (規則の英語の名前を `rule=` に入れる)。1 分の間に 3 回以上表示されるドックのメッセージは、表示の規則の不具合として修正する。数えるツールは `test/tools/notice-count/notice_count.py` である。
- 位置の保留。ポインターが一覧の上で動いている間などは、行やメッセージの位置が動く変化を保留し、ポインターが止まって `layout_hold_idle_ms` (既定 2 秒) たったら反映する (`dock/layout_hold.h`)。ボタンを押している間とメニューを開いている間は保留を続ける。数字と文字の更新は保留しない。

人に関わる事実 (映像が届いていない、など) は、その人の行の状態にも出す。人数を添えた 1 件のドックのメッセージにするのは、出力か録画を損なうものである。

### 状態タブ

状態タブの判断は `ledger/status_tab.*` の純粋関数にある。`settle_status()` が、前の周期の `StatusState`、View、時刻を受け取り、状態タブの View と次の `StatusState` を返す。

- 映像途切れの累計。参加者の行が「映像が届いていません」を出す判定 (`ledger::stalled_people()`) の、出ていない状態から出ている状態への変わり目を数え、出ている間の時間を足す (`step_stall_tally`)。配信の開始と [途切れの累計をリセット] でゼロに戻る。
- 履歴。メッセージの発生と解消、Vesktop との接続と切断、枠の変更、録画の開始と終了を、時刻つきで最大 60 行持つ (`history_events`)。人の出入りと映像途切れの出入りは書かない。
- 最後の録画。録画を始めた時刻と長さを、この起動の中だけ覚える (`step_recording_memory`)。
- 映像のバッファ。表を見ながら変える。入力が 0.3 秒止まってから配信の処理に渡し、保存は渡した 1 秒後にまとめる。

### 音声タブと表示タブ

音声タブの行の並び、状態、行がないときの 1 文は `ledger::audio_tab_of()`、メーターが出す値と色の使い分けは `ledger::meter_reading()` が決める。操作は `SetVoiceVolume` と `SetScreenAudioVolume` を通り、すぐ反映する。つまみはホイールを受け取らず、スクロールに渡す。

表示タブは、つまみとスピンボックスを動かしている間に配信画面へ渡す回数を、`ledger::kLookReflectIntervalMs` (100 ms) ごとに 1 回へ間引き、最後の値は必ず渡す (`ledger/look_tab.*`)。保存は `DeferredSave` が変更の 1 秒後に 1 回にまとめ、OBS を閉じるときは `flush()` で待っている保存を行う。外から値が変わったとき (設定ファイルの書き換え、メインアカウントの変更) は、操作していない入力欄にだけ反映する。

読むファイル: `src-obs/ledger/README.md`、`src-obs/ledger/rules.h`、`src-obs/ledger/notice_tracker.h`、`src-obs/dock/README.md`。

## Discord の内部への依存箇所

送信側は、公開されていない Discord のクライアントの内部を読む。Discord の更新で壊れたときに、壊れた箇所を見つけられるように、読む部分を 9 つの依存箇所に分けて一覧にしている (`src-vencord/ledger.ts` の `CONTACT_LEDGER`)。

| id | 何を読むか |
|---|---|
| `pc-hook` | `RTCPeerConnection` を差し替えて、画面共有と音声のトラックを受け取る。Discord が現在も `RTCPeerConnection` で受け取っている前提。実行時の確認は、トラックを読む `MediaStreamTrackProcessor` があるかも含める (なければ映像と音声を読めないので、同じ「画面共有と声を受け取れない」として伝える) |
| `ssrc-map` | voice gateway の op12 (`video_ssrc`) と op5 (speaking) の WebSocket のメッセージから、SSRC とユーザーの対応を読む |
| `sharing` | 画面共有している人。`ApplicationStreamingStore` の `getAnyStreamForUser` |
| `watch-stop` | 視聴の開始と停止。`STREAM_WATCH` の action を dispatch する |
| `roster` | 通話に参加している人。`RTCConnectionStore.getChannelId()` から、現在のボイスチャンネルを読む |
| `profile` | 表示名とアバター。`UserStore.getUser` |
| `self` | Vesktop のアカウント。`UserStore.getCurrentUser`。自分が誰か分からないのは、名前が読めないより根本的な問題なので、`profile` と別の行にする |
| `call-info` | 通話の名前と、メインアカウントがいる通話。`ChannelStore`、`GuildStore`、`VoiceStateStore.getVoiceStateForUser`、`PermissionStore` で、チャンネルとサーバーの名前、メインアカウントのいるチャンネル、接続の権限を読む |
| `voice-select` | [移動] と [退出] の実行。`selectVoiceChannel(channelId)` で通話に参加し、`selectVoiceChannel(null)` で退出する |

- 探し方。一覧は、名前ではなく、何をするものかを表す述語で持つ。モジュールが指定した名前をすべて持つ (`props`) か、関数の本文が指定した文字列をすべて含む (`code`) かの 2 種類で、Vencord の `findByProps` と `findByCode` に当たる。実行時の `adapter/vencord.ts` は述語を使わず、使う参照が今使えるかを直接確認する。述語を使うのは、保存した Discord のバンドルを調べるチェック (`src-vencord/tests/discord_contacts.test.ts` と `test/tools/discord-bundle/scan.ts`) で、一致するモジュールがちょうど 1 つであることを確認する (`pc-hook` の行は 1 つ以上でよい)。
- 状態。依存箇所ごとに `unseen` (まだ確認していない。起動直後は異常ではない)、`found`、`missing` (1 度も使えていない)、`lost` (使えていたのに使えなくなった) を `core/state.ts` が持つ。`missing` と `lost` の id は facts の `warnings` で受信側に伝わり、ドックのメッセージになる。見出しは、受信側の `src-obs/ledger/rules_dock.cpp` の `kContactHeadlines` が id から決める。id を変えるときは、`kContactHeadlines` と診断情報 (「診断情報」) も同時に変える。
- 止めない。どの依存箇所が見つからなくても、すでに送っている映像と音声は止めない。`ContactEntry.stopsExisting` は型で `false` に固定する。影響は、新しい画面共有と、新しく参加した人の声を受け取れなくなる程度に収める。

### 接続と人の対応付け

画面共有の映像を別の人の名前で配信に出すのが、いちばん避けるべき誤りである。`adapter/connections.ts` の `ConnectionTracker` は、voice gateway の WebSocket と `RTCPeerConnection` を、時刻の近さではなく、接続そのものが持つ値で対応付ける。決まらないときは、推測せずにログに書いて捨てる。

1. 分類。WebSocket が最初に送る op0 の `server_id` を、現在の通話のサーバー ID (ギルド ID、なければチャンネル ID) と比べる。同じならボイス接続 (`pcId` が 1)、違えば Go Live の接続 (2 以上の新しい番号。使い回さない) にする。通話のサーバー ID が読めない間は、分類を待つ。
2. 再接続。op7 (RESUME) を送った WebSocket は、同じ `server_id` を送っていた前の WebSocket の続きとして、分類と `pcId` を引き継ぐ。
3. 対応付け。WebSocket ごとに、op4 と op1 の SDP から ICE の ufrag を取り出して持つ。トラックが届いたとき、`RTCPeerConnection` の remote と local の ufrag が一致する WebSocket を探し、1 つに決まるときだけ対応付ける。どちらも一致しないときだけ、`RTCPeerConnection` ができる前の 50 ms 以内に op2 を受けた、まだ対応付けていない WebSocket がちょうど 1 つなら対応付ける。
4. 画面共有している人。Go Live の接続では、その WebSocket が受けた op12 (なければ op5) の `user_id` にする。トラックの msid (`<userId>-<ssrc>`) の `userId` がその人と違うトラックは送らない。ボイス接続のトラックは、何人ものマイクが乗るので、`userId` を core が SSRC で特定する。
5. 待ち。行き先が決まらないトラックは保持し、250 ms ごとに見直し、5 秒たっても決まらなければ、ログに書いて捨てる。
6. ボイス接続のセッションが替わったら、古い SSRC の値を消す。SSRC の番号はサーバーをまたいで重なるので、古い事実で新しいトラックの `userId` を特定しない。

読むファイル: `src-vencord/ledger.ts`、`src-vencord/adapter/connections.ts`、`src-vencord/adapter/vencord.ts`、`src-vencord/core/state.ts`。

## 録画

cordmux の録画は、受信した画面共有と声を再エンコードせずに、参加者ごとのセグメントファイルに書く。録画を止めると、同梱の FFmpeg を子プロセスで起動して、参加者ごとに画面共有と声の録画ファイルを作る。受信が途切れていた間は、黒い画面と無音で埋める。OBS の録画とは別のもので、ドックの下のバーのボタンで ON と OFF を切り替える。

- OFF の間は、セグメントも録画ログも書かず、書き込むスレッドも起動しない。受け取ったデータもコピーしない (`test_record_gate.cpp` が確認する)。
- OBS を起動し直したときに録画が始まらないように、ON と OFF の状態は設定に保存せず、メモリにだけ持つ。
- ON にしたとき、送信側に recording を送って黒埋め用のフレームを作らせ、configure にある画面共有のストリームの全部に `keyframe_request` を送る。セグメントはキーフレームから始まるので、録画の開始にキーフレームが要る。録画は、枠に載っているかどうかにかかわらず、configure のすべてのストリームを受け取る。
- 保存先 (録画ごとのフォルダーを作る場所) は、設定の `record_dir` に入っている場所そのものである (`D:\hoge` なら `D:\hoge\<日時>`)。設定が空のときだけ、録画を始める時点の OBS の録画の保存先、それも読めなければ Windows のビデオフォルダーの下の `cordmux` フォルダーにする (`record/record_location.h`。OBS の録画ファイルも日時の名前なので、直接置くと混ざるため 1 つ下げる)。設定ウィンドウは、選んでいないときもこの場所を入力欄に表示する (表示している場所 = 録画が入る場所)。画面とログに出す場所、[フォルダーを開く]、空き容量の確認、作成待ちの録画を探す場所は、どれも `choose_record_base_dir` が返す同じ場所を読む。
- 利用者が選んだ保存先に書けないときは、別の場所へ移さず、そのセッションを始めない (`StartFailed`)。利用者の知らない場所に、1 時間で数十 GB になる録画を書かないためである。
- 録画のコードが自分で録画を止めるのは、空き容量が尽きたときだけである。
- ファイルの操作と子プロセスの起動は `RecordPlatformOps` の関数で受け取る。テストで差し替えられ、`record/` に Windows の名前を書かずに済む。

### セグメントファイルと録画ログ

#### フォルダーの構成

```
<保存先>/<セッションの日時>/
  <人の名前> 画面共有 <セッションの日時>.mov      録画ファイル (録画ファイルの作成で作る)
  <人の名前> 声 <セッションの日時>.mov
  data/                                          録画の中間ファイル (kMaterialsDirName)
    ledger.jsonl                                 録画ログ
    in_use.lock                                  使用中のマーカー
    segments/<user_id>_<種類>_<連番6桁>.mov        セグメントファイル
    black/<user_id>_<encoder_instance_id>.mov    黒埋め用のフレームのファイル
    concat_*.txt、*.pcm、FFmpeg のログなど        録画ファイルの作成の一時ファイル
```

- セッションのフォルダーの名前は、OBS の録画の既定のファイル名と同じ形 (`2000-01-01 00-00-00`、現地の時刻) である。同じ名前があるときは `2000-01-01 00-00-00 (2)` のように番号を付ける。すでにあれば作らない関数 (`make_new_directory`) で 1 階層ずつ作り、同じ秒に始めた 2 つの OBS が同じフォルダーを使わないようにする。
- 種類は `screen_video`、`screen_audio`、`voice` の 3 つで、別のセグメントファイルに書く。画面共有の映像と音声を同じファイルに入れると、片方だけが途切れたときにもセグメントを分けることになる。セグメントファイルは fMP4 の mov で、映像は H.264 (timescale 60000)、音声は 32 ビット浮動小数点の PCM (timescale 48000) のまま書く。
- セグメントファイルの名前に表示名と `stream_id` を使わない。表示名はまだ読めていない間にセグメントが始まることがあり、`stream_id` は再接続で別の人に割り当てられる。
- 録画ファイルの名前は、人の名前、種類の語、日時を、ini の書式 (`cordmux.source.file_name.label` など) で並べる。ファイル名に使えない文字は `_` に置き換え、人の名前は 64 バイトで切り、名前が重なる人には `_` と `user_id` の先頭 6 文字を付ける。
- 録画ファイルの作成は、成功しても失敗しても、セグメントファイルを削除しない。

#### 録画ログ

`data/ledger.jsonl` は、1 行が 1 つの JSON のオブジェクトで、`type` で種類を示す。録画ファイルの作成は、メディアのファイルを調べずに録画ログだけから FFmpeg の引数を作るので、作成に要る値はすべて録画ログに書く。追記だけをするので、同じものの行が何度か出る。読む側は、後の行が前の行を上書きし、並びは最初に出た位置を保つ。読めなかった行と、書き込みの途中で止まった末尾の行は、飛ばして `skipped_lines` に数え、読めた範囲で録画ファイルを作る。形式のバージョンは `session` の行の `schema` (現在は 1) である。

| type | 上書きのキー | 書くとき | 主なフィールド |
|---|---|---|---|
| `session` | 1 つだけ | セッションを始めたとき、録画ファイルの作成の状態が変わったとき | `session_id`、`started_at_wall_us`、`started_at_steady_us`、`ended_at_steady_us` (0 はまだ止めていない)、`stopped_reason`、`finalize_state`、`plugin_version`、`ffmpeg_version`、`dummy_intervals` |
| `person` | `user_id` | セグメントを開いたとき、表示名が読めたとき | `user_id`、`display_name`、`kinds` (実際にセグメントを開いた種類) |
| `segment` | `file` | セグメントを開いたとき、閉じたとき | 下の表 |
| `black` | `file` | 黒埋め用のフレームを受け取ったとき | `user_id`、`encoder_instance_id`、`file`、`avcc_hex`、`width`、`height`、`fps_num`、`fps_den`、`frame_count`、`duration_ticks`、`origin`、`first_frame_delay_us` |
| `finalize` | `output_file` | 録画ファイル 1 つの作成を終えたとき | `user_id`、`kind`、`branch`、`output_file`、`args`、`exit_code`、`output_duration_ticks`、`output_timescale`、`check`、`gaps` |

`segment` の行のフィールド:

| フィールド | 中身 |
|---|---|
| `user_id`、`kind` (`screen` か `voice`)、`media` (`video` か `audio`)、`stream_id` | 誰の、どの種類のセグメントか。`stream_id` は原因の調査のために残し、見分けるキーにしない |
| `encoder_instance_id` | 受信側が、ストリームを受け取り始めたときと avcC が変わったときに割り当てる ID。WebSocket のメッセージに、エンコーダのインスタンスを示す値はない |
| `file`、`avcc_hex` | 中間ファイルのフォルダーからの相対パス (`segments/...`)。受信側が SPS と PPS から組み立てた avcC のバイト列 (音声は空) |
| `width`、`height`、`fps_num`、`fps_den`、`timescale` | fps は有理数で持つ。59.94 と書くと、長さを整数で比べられない |
| `first_pts_us`、`first_pts_steady_us`、`first_pts_wall_us`、`last_pts_us` | 送信側のタイムスタンプ、受信側の steady_clock の時刻、システム時刻 |
| `sample_count`、`duration_ticks`、`bytes`、`starts_with_idr`、`au_hash` | 映像はフレーム数、音声はサンプル数。`bytes` は実際に書いた量。`au_hash` はセグメントごとの framemd5 |
| `pts_pushed_count`、`pts_reanchor`、`pts_periodic_backward` | `classify_pts()` の結果。後戻りがフレームの間隔の整数倍で 3 回以上繰り返すと、B フレームの疑いとして `pts_periodic_backward` が真になる |
| `sps_pps_prepended_count` | 送信側が SPS と PPS を前に付けた回数の累計 (facts の `sps_pps_prepended`)。セグメントごとの数は前の行との差 |
| `truncated`、`close_reason`、`dropped_tail_bytes` | 閉じ方 (下) |

列挙の値の文字列は、test/tools/obs-run の判定スクリプト (`ledger_check.py` など) も読むので、`src-obs/record/record_ledger.h` の `*_name()` 関数と同時に変える。

- `close_reason`: `disconnected`、`stream_gone`、`format_changed`、`clock_jump`、`recording_toggled`、`queue_overflow`、`write_failed`、`not_closed`。`not_closed` は、セグメントを開いたときに書く行の値である。録画の途中で OBS か PC が止まっても、開いていたセグメントを録画ログから見つけて作成できるようにするためで、正常に閉じれば、同じ `file` の行で上書きされる。残るのは閉じる前に止まったセグメントだけで、その行は `truncated` も真になる。
- 閉じる理由が重なったときは、外側の事情から先に見る: 接続、configure、録画の OFF、キューのあふれ、解像度と fps、タイムスタンプの順 (`record_segment.h` の `segment_cut_reason`)。枠が変わっただけでは閉じない。枠を並べ替えるたびに録画が分かれないようにするためである。
- `stopped_reason`: `still_running`、`turned_off`、`obs_exit`、`space_exhausted`、`interrupted`。`interrupted` は、止めた行がないまま終わったセッションを、後から作成したときの値である。
- `finalize_state`: `not_started`、`running`、`done`、`pending`、`failed`。
- `check`: `not_checked`、`matched`、`duration_mismatch`、`process_failed`、`space_exhausted`。
- `origin` (黒埋め用のフレームを作ったタイミング): `encoder_created`、`recording_turned_on`、`encoder_teardown`。最初に届いたものを使う。
- `branch`: `remux` か `prores`。
- `gaps`: 欠落を埋めた 1 件ずつの記録で、`start_us`、`duration_us`、`repeat_count`、`remainder_ticks`、`black_encoder_instance_id` を持つ。

フィールドの詳しい意味は `src-obs/record/record_types.h` のコメントにある。

### 録画ファイルの作成

録画を OFF にすると、`Recorder` の別のスレッドが `run_finalize()` で、参加者ごとに画面共有と声の録画ファイルを 1 つずつ作る。並列には作らない。一時ファイルに要る空き容量が重なり、ProRes への再エンコードが CPU を取り合うためである。

#### 計画

`build_finalize_plan()` (`record/record_args.h`) が、録画ログだけから、ファイルごとの計画を作る。

1. 時刻の基準。録画ログ全体で 1 回だけ、セグメントを PtsDomain (`first_pts_steady_us - first_pts_us` が 1 秒以内でそろうセグメントのまとまり) に分け、セッションの原点を決める。同じ PtsDomain の中では、送信側のタイムスタンプの差がそのまま経過時間を表す。PtsDomain をまたぐときだけ、受信側の steady_clock の時刻で位置を決める。送信側のタイムスタンプも steady_clock も同じ単調な時計で進むので、差は送信側の時計の原点が変わる (Vesktop を起動し直す) まで変わらない。PtsDomain が変わるのはそのときである。送信側のタイムスタンプのほうが、到着の時刻よりストリームごとのずれが小さい。到着の時刻で置くと、録画を ON にした直後の最初のキーフレームが音声より遅れて届いた分 (0.14 秒から 0.2 秒) が、ファイル全体で音声が先に鳴るずれとして残る。
2. 欠落。セグメントの間 (再接続、解像度の変更など) と末尾の欠落を求め、映像は黒で、音声は無音で埋める。黒で埋める長さの上限は、セッションの長さである。
3. 作成方法 (`decide_branch()`)。次のすべてが成り立つときだけ、`remux` (再エンコードしない) で作る。
   - その人のその種類の、すべてのセグメントと黒埋め用のフレームの avcC が一致する。
   - 規則的な後戻り (`pts_periodic_backward`) がない。
   - 修復していない途中で止まったセグメント (`truncated`) がない。再開のときに `repair_truncated_segments()` が修復したセグメントは、閉じたセグメントと同じに扱う。
   - 映像があるとき、その人の黒埋め用のフレームが 1 つ以上ある。

   1 つでも欠ければ、その人のその種類のファイルだけを `prores` で作る。声のファイルには映像がないので、作成方法によらず、音声を `copy` する。avcC が食い違うセグメントを concat で並べても、FFmpeg は警告を出さず、終了コード 0 で、コンテナーは 640x360 を示すのに途中から 640x480 になるファイルを作る。問題を見つける手段は avcC の比較だけなので、作成の前に avcC を比べる。

#### FFmpeg の起動

FFmpeg は、1 人の 1 つのファイルにつき 1 回だけ起動する (`-xerror` を付け、`-progress` で進み具合をファイルに書かせる)。

- `remux`: 映像のセグメントファイルと黒埋め用のフレームのファイルを、concat demuxer で並べる。concat の `duration` は書かない (タイムスタンプが重なり、muxer が 1 単位ずつずらすため)。欠落は、黒埋め用のフレームのファイルを N 回並べ、最後の 1 つを `outpoint` で切って埋める。音声は、全体の長さを 0 のサンプルで埋めた生の PCM のファイルを作り、各セグメントの音声をその位置に書き (`build_finalize_pcm()`)、`-f` で読ませる。映像と音声を `-c copy` で 1 つの mov にする。
- `prores`: 映像を `-filter_complex_script` で、セグメントごとに fps をそろえ、その人のその種類のセグメントのうち、長さの合計が最も長い解像度 (同じ長さならピクセル数の多いほう) にそろえて並べる。黒は lavfi の `color` で作るので、黒埋め用のフレームがない人も作れる。`prores_ks` (ProRes 422 HQ、`yuv422p10le`) で映像だけを再エンコードし、音声は `copy` する。
- 録画ファイルの PCM の形は設定の `record_output_pcm` で決める (既定は 32 ビット浮動小数点)。セグメントファイルは変えない。
- `-shortest` は付けない。長さがずれたときに、ドックのメッセージもログも出さずに短いほうへ切り詰め、ずれを隠すためである。

#### 黒埋め用のフレーム

黒埋め用のフレームは、録画の切れ目を黒で埋めるための、真っ黒な 1 秒分の映像である。送信側が、録画が ON の間に、エンコーダのインスタンスごとに 1 回、録画専用のフラグを付けて送る (docs/wire_format.md の「flags」)。受信側は、録画ログの `black` の行と `black/` のファイルにする。

送信側は、接続していない間に作った黒埋め用のフレームを捨て、再接続しても作り直さない。その人の `black` の行が 1 つもないと、`any_material_missing` が真になり、その人は `prores` で作る。行が 1 つでもあれば、その人の最初の行のファイルで、すべての欠落を埋める。`black` の行は種類を持たず `user_id` ごとにあるので、画面共有の映像があるときだけ照合する。

#### 確認と失敗

作成した後、FFmpeg の終了コードと、録画ファイルの長さとフレーム数を録画ログと比べる (`FinalizeCheck`)。終了コードが 0 のまま録画ファイルが短くなることがあるためである。`remux` では、フレーム数がちょうど一致しなければならず、長さは、コンテナーの丸めで 1 フレーム分まで許す。`prores` は fps のフィルターで間隔が一定になり、出るフレーム数がセグメントのサンプル数と同じとは限らないので、フィルターの規則で数えた値と比べる。フィルターに入る並びの数までのフレームのずれと、それに 1 フレームを加えた長さのずれを許す。

- 1 つのファイルの作成に失敗しても、次のファイルに進む。1 人の失敗で、全員の録画ファイルが作られなくなるのを避ける。
- 全体の結果は `done`、`pending`、`failed` のどれかである。打ち切れば `pending`、打ち切らずに 1 つでも失敗すれば `failed`、それ以外は `done` になる。
- OBS の終了で録画ファイルの作成を待つ時間には上限がある (設定の `record_finalize_wait_on_exit_s`、既定 30 秒)。超えたら子プロセスを止め、作りかけの録画ファイルを削除して、`pending` にする。
- 途中で切れたセグメント (`truncated`) は、再開したときに `repair_truncated_segments()` が、読めるサンプルだけのファイル (`_repair.mov`) に書き直す。読めずに捨てたバイト数は `dropped_tail_bytes` に残す。
- `pending`、`failed`、録画の途中で OBS か PC が止まったセッションは、次の起動の後に、ドックのメッセージの [作成を再開] で再開できる。使用中のマーカー(`in_use.lock`) は、ほかの OBS が録画中か作成中のセッションを、途中で止まったセッションと取り違えないために使う。持っているプロセスが終了すると (クラッシュしても)、OS が消す。
- 進み具合は `FinalizeProgress` に、ファイルの数と、FFmpeg が報告した `out_time_us` を録画ログの長さで割った割合で持つ。どちらも設定の `record_finalize_stall_s` (既定 120 秒) 進まなければ、止まっているとみなす。

録画ファイルの作成が済んでいないセッションが残る保存先は、設定のフォルダーの `record_pending_dirs.json` に覚える (`record/record_search_dirs.h`)。settings.json には現在の保存先しかないので、保存先を変えて OBS を起動し直すと、前の保存先に残るセッションを探せないためである。起動のたびにこの保存先も探すが、到達できない保存先で起動を止めないよう、場所ごとに別のスレッドで問い合わせる。

読むファイル: `src-obs/record/README.md`、`src-obs/record/record_types.h`、`src-obs/record/record_ledger.h`、`src-obs/record/record_args.h`、`src-obs/record/recorder.cpp`。

## 設定の保存場所

| ファイル | 場所 | 内容 | 書く所 |
|---|---|---|---|
| settings.json | OBS の設定のフォルダーの `plugin_config/cordmux` (`obs_module_config_path`) | 設定 | ドック |
| seen_people.json | 同上 | これまでに見た人の一覧と、参加者ごとの値 | ドック |
| vencord_dir_backup.json | 同上 | Vesktop の `vencordDir` を書き換える前の値 | ドックと起動時の書き直し |
| record_pending_dirs.json | 同上 | 録画ファイルの作成が済んでいないセッションがある保存先 | `Recorder` |
| session.json | `%LOCALAPPDATA%\cordmux` | ポート番号とトークン (「接続の認証と制限」) | 受信側 |

- settings.json があるのに読めない (開けない、または形が違う) ときは、その起動の間、ドックは settings.json に書かず、Vesktop の設定 (`state.json`) も書かない。既定値で、利用者の値を上書きしないためである。
- settings.json と seen_people.json は、形が違うファイルを丸ごと拒む。1 行だけ落として残りを使うと、書き戻したときに、読めなかった行が本当に消える。
- settings.json の値が範囲の外のときは、形が違う値と同じに扱う (`parse_settings`)。`clamp_settings` は、設定ウィンドウの入力を範囲に収めるだけで、ファイルを読むときには使わない。キーごとの既定値と範囲と理由は `src-obs/config/settings.h` のコメントにある。
- 割り当て表は保存しない。起動し直した後に、前の起動の選択を戻す理由がないためである。枠が空く条件は「状態の所有者」にある。
- 参加者ごとの音量とミュートは、人に付く。枠を別の人に割り当て直しても、前の人の値は新しい人に移らない。音量は 100% でないときだけ seen_people.json に書く。メインアカウントの声のミュートは settings.json の `self_voice_muted` に保存する。ほかの人の声のミュートと、画面共有の音声のミュートは保存しない。起動すると、ほかの人の声は音を出す状態に戻り、画面共有の音声は共有が始まるたびにミュートから始まる。
- 診断情報の zip は、利用者が保存先を選んで保存するファイルで、設定のフォルダーに置かない。
- 全画面のホットキーに割り当てたキーは、cordmux のファイルに書かない。OBS がプロファイルの basic.ini に書く (「OBS のソースとシーン」の「全画面」)。cordmux のファイルではないので、アンインストールでは消さず、診断情報にも入れない。

アンインストールは、設定のフォルダーを中身ごと削除する。ファイルを追加しても、削除の対象の表は変えなくてよい。

## 配布、インストール、更新、アンインストール

zip の中身、バージョンの付け方、リリースの手順は docs/release.md に、利用者向けの手順は `guide/ja.md` にある。

### 配布

配布用の zip の `cordmux/` は OBS のプラグインのフォルダーで、cordmux.dll、FFmpeg、ini、同梱の Vencord を含む。Vencord は 2 つのフォルダーに同じ中身で入る。`data/vencord-src` は元のファイルで、`data/vencord` は Vesktop が読むフォルダーである。どちらにも `cordmux-build.txt` (`build_id`、`vencord_commit`、`vencord_tag` の 3 行) がある。Vencord は送信側を組み込んでビルドしたもので、Vesktop は設定の `vencordDir` が指すフォルダーの 4 つのファイル (`vencordDesktopMain.js`、`vencordDesktopPreload.js`、`vencordDesktopRenderer.js`、`vencordDesktopRenderer.css`) を読む。

### インストール

利用者が zip の `cordmux` フォルダーを OBS のプラグインのフォルダーに移動すると、OBS が cordmux.dll を読み込む。Vesktop に送信側を読み込ませるには、Vesktop の `state.json` の `vencordDir` を `data/vencord` の絶対パスにする。ドックの [Vesktopに追加] が押されたときに、`plan_write()` (`src-obs/install/vencord_dir.h`) が書くかどうかを決める。書く前に、元の値を `vencord_dir_backup.json` (`previous`、`written`、`written_at`、`dll_dir`、`plugin_entry`、`state_path`) に保存する。`plugin_entry` は、Vencord の設定の `plugins` にある cordmux の項の、cordmux を入れる前の状態である。

`plan_write()` は、`state.json` の現在の値 [E]、書きたい値 [T]、バックアップ [C] から、5 つの分岐のどれかを決める。

| 分岐 | 条件 | [Vesktopに追加] | 切り替えの確認で [切り替え] | 起動時 |
|---|---|---|---|---|
| 1 | [E] と [T] が同じ | 何もしない | 何もしない | 何もしない |
| 2 | [C] がない | 書く (`previous` = [E]) | 書く | 書かない |
| 3 | [E] が [C].`written` と同じ (cordmux が書いた場所) | 書く (`previous` を引き継ぐ) | 書く | 書く |
| 4 | [E] が [C].`previous` と同じ (元の値に戻した後) | 書く (`previous` を変えない) | 書く | 書かない |
| 5 | それ以外 (利用者かほかのツールが書き換えた) | 断る (キーがないときは書く) | 書く (`previous` = [E]) | 断る |

- 起動時に書くのは分岐 3 だけである。cordmux のフォルダーを移した後に、cordmux が最後に書いた値だけを新しい場所へ書き直し、利用者が選んだ値は書き換えない。`obs_module_load` が、`OsProbe` を起動する前に 1 回だけ行う。そのとき Vesktop が起動していれば (起動しているかを調べられなくても) 書かず、OBS の起動中に `OsProbe` の結果で Vesktop の終了を見たときに、ドックが同じ入力でもう一度試す (書けたら、もう試さない)。
- `state.json` を書くのは、Vesktop が起動していない間だけである。[Vesktopに追加] は Vesktop の起動中と、起動しているかを読めていない間は押せず (`ledger/actions.cpp` の `vesktop_busy_reason`)、状態タブの「Vesktop」の組 ([Vesktopから削除]) はその間は表示しない。[Vesktopに追加]、[Vesktopから削除]、起動時の書き直し、アンインストールの段階 (3) のどの書き込みも、書く直前に Vesktop のプロセスを調べ直す (`platform::vesktop_running_now`)。Vesktop (v1.6.9 の `src/main/settings.ts` と `src/main/mainWindow.ts`) は `state.json` を起動したときに 1 回だけ読んでメモリに持ち、ウィンドウを動かす、大きさを変える、最小化するなどで値が変わるたびに、メモリの値の全体で書き直す。起動中に書いた `vencordDir` は、再起動の前にウィンドウを動かしただけで元の値に戻る。ほかのバージョンの Vesktop がこの動きを変えても、起動していない間に書けば Vesktop は起動したときに必ず読むので、安全な側になる。
- Vesktop が起動していて接続していない間は、ドックが 1 秒ごとにプロセスの一覧だけを調べ、終了を見たら `OsProbe` にすぐ問い合わせ直させる。終了した後も 30 秒押せないままにしないためである。調べるのは、cordmux が有効で、Vesktop の設定を読めているか、起動時の書き直しが待っているときだけである。
- 分岐 3 で `previous` を引き継ぐのは、書き直すたびに前に書いた値が元の値になり、アンインストールが利用者の値ではなく cordmux の古い場所へ戻すのを避けるためである。Vesktop は `vencordDir` の場所がないと公式の Vencord をダウンロードする。
- 書くのは `vencordDir` だけで、ほかのキーの値と順番は変えない。整形は入力から推定して合わせる。設定の `allow_state_json_write` が偽なら、書かない。
- 状態タブの [Vesktopから削除] は、アンインストールの段階 (3) と同じ処理で Vesktop の設定を戻す (`remove_from_vesktop()`。`plan_restore()` と、Vencord の設定の cordmux の項の戻し)。段階 (3) と違い、バックアップと設定のフォルダーは削除しない。そのため、後で [Vesktopに追加] を押すと分岐 4 になり、`previous` と `plugin_entry` を引き継ぐ (バックアップは cordmux の場所で上書きされない)。Discord の更新に同梱の Vencord が合わず、Vesktop の画面が真っ黒のままになったときに、利用者が cordmux を外して Vesktop を使えるようにするための操作である。

### 更新

cordmux に更新の機能はない。利用者が新しい zip の `cordmux` フォルダーを、前と同じ場所に置き換える。受信側は、置き換えで起きる食い違いを次のように検出する。

- 送信側と受信側のバージョンの食い違い。送信側は hello の `version` にビルドの ID を入れ、受信側は同梱の `data/vencord` の `cordmux-build.txt` の `build_id` と比べて、違えばドックのメッセージにする (docs/wire_format.md の「バージョンと拡張性」)。
- `data/vencord` の食い違い。`OsProbe` が 30 秒ごと (Vesktop と接続していれば 5 分ごと) に、`data/vencord` の 4 つのファイルを `data/vencord-src` と SHA-256 で比べる (`platform/bundle_check.h`)。`classify_shown_bundle()` (`install/build_mark.h`) が、マーカーとハッシュから判定する。

  | 判定 | 条件 | 意味 |
  |---|---|---|
  | `Matches` | ハッシュが一致し、マーカーが同じかない | 問題なし |
  | `StaleOwnBundle` | `data/vencord` のマーカーが `data/vencord-src` と違う | 別のバージョンのファイルが残っている |
  | `Overwritten` | マーカーが同じかないのに、ハッシュが違う | ほかのツールに上書きされた |
  | `Unknown` | 比べられなかった | 判定しない |

  マーカーが違えば、ハッシュによらず `StaleOwnBundle` にする。Vesktop の [Repair Vencord] と "Force Update Vencord" (公式の Vencord を `vencordDir` に書く) はマーカーを知らずに上書きするので、マーカーが別のバージョンのものであることは、上書きではなく、別のバージョンのファイルが残っていることを示す。`StaleOwnBundle` は自動で、`Overwritten` は [再インストール] で、`data/vencord-src` から `data/vencord` にコピーする (`plan_reinstall()`)。
- コピーの順。`cordmux-build.txt` を必ず最後にコピーする。途中で止まっても `data/vencord` のマーカーが古いまま残るので、次の問い合わせで見つけ直せる。先にコピーすると、一部だけコピーしたファイルを一致と判定するおそれがある。1 つコピーできなくても、残りはコピーを続ける。`data/vencord` にだけあるファイルは消さない。
- Vesktop の再起動。コピーし終えたら、バックアップの `written_at` を現在の時刻に進める (`advance_written_at()`)。`OsProbe` が読む Vesktop の起動の時刻がこれより前なら、表示の規則が Vesktop の再起動を勧めるドックのメッセージを出す。`state.json` は Vesktop が起動していない間だけ書くので、このメッセージが出るのは、Vesktop の起動中に [再インストール] でファイルをコピーし直したときである (Vesktop はファイルを起動したときに読む)。

### アンインストール

OBS の [ツール] メニューの「cordmuxをアンインストール...」は、押しただけでは何も削除しない。設定ウィンドウを [バージョン情報] のページで開き、ページのボタンから出る確認のダイアログで [アンインストール] を選んだときだけ、`run_uninstall()` (`src-obs/install/uninstall.h`) が実行される。配信、OBS の録画、リプレイバッファ、仮想カメラ、cordmux の録画、Vesktop のどれかが動いている間は、押せない。

`run_uninstall()` は、次の段階を順に実行する。段階が残ったもの (`leftovers`) を返したら、そこで止めて、残りを結果にまとめる。

| 段階 | 処理 |
|---|---|
| (0) | アンインストール中のフラグを立て、接続の待ち受けを止める |
| (1) | 設定のフォルダーと `%LOCALAPPDATA%\cordmux` を、中身ごと削除する。バックアップだけは残す |
| (2) | 現在のシーンコレクションと、ほかのすべてのシーンコレクションを順に開いて、cordmux の型のソースを削除し、元のシーンコレクションに戻って数え直す |
| (3) | `state.json` の `vencordDir` を、バックアップの `previous` に戻す (キーがなかったなら、キーごと削除する)。続けて Vencord の設定の `plugins` の cordmux の項を `plugin_entry` に合わせて戻し、バックアップと設定のフォルダーを削除する |
| (4) | 利用者に削除してほしいフォルダー (展開したプラグインのフォルダー) の場所と、録画の保存先を表示する |

(3) の前に (4) のフォルダーを削除すると Vesktop が公式の Vencord をダウンロードするので、順に意味がある。

- フラグはメモリにだけ置き、立てたら OBS を終了するまで下ろさない。立つと、枠の自動作成、settings.json、seen_people.json の書き込み、起動時の書き直し、接続の待ち受けが止まる。ファイルに保存しないのは、アンインストールの後に cordmux のファイルを残さないためである。ほかのスレッドから設定のフォルダーに書く処理は `write_unless_uninstalling()` を通し、フラグの確認と書き込みを 1 つのロックの下で行って、削除したファイルを作り直さない。
- 段階 (1) でバックアップを残すのは、段階 (3) が元の値を読むのがバックアップだけだからである。段階 (3) で止まったときにバックアップが先に消えていると、やり直しても元に戻せない。
- 段階 (3) で、`state.json` にインストールの後に利用者が選んだ値があるときは、その値を残す (`plan_restore()`)。ただし、現在の値が `data/vencord` を指しているときは、フォルダーを削除すると公式の Vencord がダウンロードされるので、結果に入れて止める。
- 段階 (3) は、`state.json` を書く直前に Vesktop が起動していないことを確認する (`platform::vesktop_running_now`。確認のダイアログの前と後に見た後で、起動されることがある)。起動していれば (調べられなくても) 書かず、書けなかったときと同じく、元に戻せなかった設定として結果に入れて止める (バックアップは残す)。`state.json` を書かなかったとき (もう戻っていた) は、Vencord の設定を書く前に同じ確認をし、起動していれば書かない (Vesktop の動作に関わらないので止めない)。
- 削除の対象は `src-obs/install/removal_table.h` の `kRemovalTable` が持つ。`Product` の行は cordmux が削除し、`User` の行 (展開したフォルダー。読み込まれている DLL は自分では削除できない) は利用者に削除してもらい、`Nobody` の行 (録画、OBS のドックの配置、OBS のログ、OBS 32 のプラグインマネージャーの記録) は削除しない。録画は利用者のファイルで、残りは OBS 自身の状態である。表は `test/lint/removal_table.py` と `test/tools/obs-run/install_snapshot.py` も読むので、ヘッダーのコメントの形を守る。

## 接続の認証と制限

受信側が受け付けるのは、同じ PC の中の送信側からの接続だけである。

- 待ち受け。127.0.0.1 だけで待ち受け、4470 番から 16 個のポートを順に試す (docs/wire_format.md の「概要」)。同時に受け付ける接続は 1 つで、2 つ目は 503 で断る。
- トークン。認証の本体は、hello の `secret` である。受信側は待ち受けを始めるたびに、32 バイトの乱数を 16 進数にしたトークンを作り、ポート番号と一緒に `%LOCALAPPDATA%\cordmux\session.json` に書く。送信側は接続を試すたびにこのファイルを読む。`secret` は定数時間で比べる。トークンはログにも診断情報にも書かない。
- ファイルの権限。session.json は、現在の Windows の利用者だけがフルアクセスできる ACL (親から継承しない) で書く。同じフォルダーの一時ファイルに権限を付けてから置き換えるので、書きかけの中身を読まれず、広い権限のファイルが見える時間もできない。
- Host と Origin。Host が `127.0.0.1:<ポート番号>` と一致しない接続と、Origin ヘッダーがあって Discord の 3 つのホストのどれとも一致しない接続は、403 で断る (docs/wire_format.md の「Origin と Host の検証」)。Origin ヘッダーがない接続は許す。ブラウザーで開いたページが DNS リバインディングで 127.0.0.1 に接続するのと、ほかのサイトのページからの接続を防ぐ。ブラウザーでないプログラムは好きな値を送れるので、認証の代わりにならない。
- 入力の制限。hello は接続から 5 秒以内に届かなければ切る。最初のメッセージが hello でない接続も切る。WebSocket のメッセージの大きさの上限は 64 MiB である。壊れた WebSocket のメッセージは、接続を切らずに捨てて数える。
- 別の OBS。session.json の pid と起動時刻のプロセスが動いていれば、2 つ目の OBS は待ち受けを始めない。pid だけで比べると、Windows が同じ番号を別のプロセスに使い直したときに見誤る。

トークンで守れる範囲:

- 守れるもの。ブラウザーのページや、別の Windows の利用者のプログラムが、送信側のふりをして接続すること。ページは session.json を読めず、別の利用者のプログラムは ACL に阻まれる。
- 守れないもの。同じ Windows の利用者として動く任意のプログラムは、session.json を読んで接続できる。cordmux はその接続を送信側と区別できない。そのプログラムは、すでに利用者のファイルを読み書きできる。
- 接続は暗号化しない。127.0.0.1 の中だけを通る。

読むファイル: `src-obs/ws/listener.cpp`、`src-obs/ws/secret.h`、`src-obs/platform/platform_win.cpp` (`write_session_file`)。

## 起動、再接続、異常時の動作

### 起動

`obs_module_load` (`plugin-main.cpp`) は、次の順に行う。

1. ログの出力先を登録し、バージョンをログに書く。WinSock を起動し、ソースの型を登録する。
2. settings.json を読む。ない (初回) なら既定値で動く。あるのに読めないときの動作は「設定の保存場所」にある。
3. 起動時の `vencordDir` の書き直し (「配布、インストール、更新、アンインストール」) を行う。
4. `Listener` と `Pipeline` を作り、`Pipeline::start()` でコールバックを登録してから、`Listener::try_open()` で待ち受けを始める。コールバックは io_thread が読むので、io_thread を動かす前に登録する。
5. `OsProbe` を起動し、ドックを追加する。configure の適用は、シーンコレクションを読み終えるまで保留する (「シーンコレクションを切り替えたとき」)。

OBS と Vesktop のどちらを先に起動してもよい。Vesktop が先なら、送信側が接続を試し続け、OBS が待ち受けを始めたら接続する。

`obs_module_unload` は、ドックのポーリング、`OsProbe`、`Pipeline` (`Listener` より先)、`Listener` を止め、自分が書いた session.json を削除する。終了した OBS の session.json を読んだ送信側が、無関係のポートに接続し続けないようにするためである。その後、`Pipeline` と割り当て表を、作ったのと逆の順に破棄する。

### 受信側の待ち受けと切断

`maintain_listener()` が、ドックのポーリングから 5 秒ごとに呼ばれる。待ち受けていない (別の OBS が動いていた、またはポートが空いていなかった) ときは、`try_open()` を呼び直す。別の OBS が終了すれば、この OBS が受け取れる。待ち受けていて Vesktop が接続していないときは、session.json が自分の書いた中身のままかを確認し、ないか違うか壊れていれば書き直す。アンインストールの最中と後は何もしない。

受信側は、ping (5 秒ごと) に pong が 2 回続けて返らないとき、hello が 5 秒以内に届かないとき、WebSocket が閉じたときに、接続を切る。

### 送信側の再接続

`transport/client.ts` の状態機械は、`idle`、`connecting`、`handshaking` (hello を送った後)、`established`、`waiting` (再試行の待ち) と進む。

- 待ち時間 (`transport/backoff.ts`)。切断を検出した直後は待たずに 1 回試し、続けて失敗すると 2 秒、4 秒、5 秒、5 秒と延ばす。上限が 5 秒なのは、OBS が待ち受けを始めてから 10 秒以内に再接続できるようにするためである。
- session.json を読めないとき (ファイルがない、形式が違うなど) は、理由を区別せずに次の機会を待つ。
- hello を送って 5 秒以内に hello_ok か hello_ng が届かなければ、接続を閉じて再試行を待つ。
- 受信側の監視 (`transport/watchdog.ts`)。OBS のプロセスが丸ごと止まると、ping が来ず、送る待ちが増え続ける。ping が 20 秒来ないか、送る待ちが音声の上限 (16 MiB) を超えたまま 15 秒続いたら、送信側から接続を切って接続し直す。止まった接続を持ち続けると、OBS が戻っても、たまった古いデータを送り終えるまで新しい映像と音声が遅れる。
- 確立したら、configure、log、facts、profiles、avatar を送り直す。切れたら、読み出しを全部止め、`media` を既定 (真) に戻す。`recording` は戻さない。視聴する人の範囲 (`receive_scope`) も戻さない (切れている間に全員の視聴を始めない)。再接続の直後に、受信側が `hello_ok` に続けて現在の範囲を送り直す。

### 切れたときに受信側が捨てるもの

`Pipeline::handle_disconnected()` は、次のようにする。

- 捨てるもの。経路、ストリームごとのデコードのスレッド、表示の待ち行列に残ったフレーム、保留した configure、直近の facts と profiles、ストリームの形。再接続すると同じ `stream_id` が別の人に使われることがあり、古い経路を残すと、前の映像が別の人の枠に出る。
- 残すもの。割り当て表、参加者ごとの音量とミュート、送信側のログのリングバッファー (接続していない間に何があったかを診断情報に残すため)、段階ごとの件数の合計。
- 映像。最後のフレームを残してから枠を空け (`link_lost_hold_ms`、既定 2 秒)、並べ直さない。経路を捨てても、切れたときに映っていた枠は配置の上では見えている枠として数える (`pipeline.h` の `LinkLostLayoutHold`)。割り当て表を残すので、同じ人が画面共有をし直せば同じ枠に映る。接続し直してから `reconnect_layout_hold_ms` (既定 10 秒) の間に画面のストリームが戻らない枠は、そのときに並べ直す。facts と profiles を捨てても、最後のフレームを残している間は、ドックが合成のソースに渡す参加者の並びを切れる前のままにし (`pipeline.h` の `link_lost_frame_held`)、名前ラベルと並びのアバターは、画像がまだ分からない間、前に渡した画像を使う (`dock/build_observation.h` の `decor_avatar`)。並びとアバターだけが先に変わると、残している映像と見た目が合わない。
- 音声。10 秒 (`kShareResumeWindowUs`) 以内に同じ人の画面のストリームが戻れば、同じ共有の続きとして扱い、外したミュートを戻さない。
- 録画。開いているセグメントを `disconnected` で閉じる。再接続したら、新しいセグメントをキーフレームから始め、間の欠落を録画ファイルの作成で埋める。

### 異常時の動作

ほかの異常時の動作は、それぞれの節にある。キューのあふれとハードウェアデコードの切り替えは「受信側」、エンコードの段階は「送信側」、依存箇所は「Discord の内部への依存箇所」、読めない設定のファイルは「設定の保存場所」、書けない保存先と、録画ファイルの作成の打ち切りと再開は「録画」にある。ここには、その節にない動作を並べる。

| 状況 | 動作 |
|---|---|
| 受信のコールバックで例外が起きた | `safe_call` で捕まえて、その 1 件を捨て、ログに書き、統計の `exceptions_caught` に数える |
| 録画中に空き容量が尽きた | 録画を止めて、録画ファイルの作成を始める。作成に足りない空き容量は、ドックのメッセージで伝える |
| 別の OBS が待ち受けている | 待ち受けを始めず、プラグインの読み込みは続ける。ドックのメッセージにする |
| 枠に載せている人の映像が届かないまま、`receive_restart_wait_ms` (既定 5 秒) たった | 受信をやり直す (下の「受信のやり直し」) |

### 受信のやり直し

Vesktop には映像が届いているのに、枠の映像が止まったままになることがある (枠のチェックを付け直すまで戻らない)。チェックの付け直しは、送信側の読み出しとエンコードを止めて始め直す。同じことを、枠の割り当てを変えずに自動で行う。

- 受信側 (`src-obs/pipeline/receive_restart.h` の `plan_receive_restart`、`pipeline.cpp` の `check_receive_restart`): 監視のスレッドが 1 秒ごとに、configure にある画面のストリームのうち、枠 (1 から 4) に載っている人の、最も新しいフレームの時刻を見る。最後のフレームから `receive_restart_wait_ms` たったら、その人の枠に載っているストリームに stream_check を送る。デコードの経路は作り直さない (フレームが届いていないので、デコーダーには何も来ていない)。合成のソースの子は空けないので、子の大きさと最後のフレームが残り、名前ラベルは枠に残る。届かないままなら 10 秒後、その 30 秒後、以後 30 秒ごとに頼み直し、届いたら間隔を最初に戻す。接続していない間、無効の間、ダミー映像の間は頼まない。頼んだこと (枠、ストリーム、届かなかった秒数、何回目か)、送信側の答え、戻ったこと (秒数) を、OBS のログに 1 行ずつ書く。sending (送信側は送れている) の答えは、ログに書くだけである。
- 受信側だけで閉じる監視 (`plan_decode_restart`): フレームは届き続けているのに、そのストリームのデコード済みのフレームが `receive_restart_wait_ms` の間出ないときは、デコードの経路を作り直し、キーフレームを求める。デコードの経路は、黙って出力を止めることがあるためである。`decode/h264_decoder.cpp` の `drain()` は、`convert_frame()` に失敗したフレーム (ハードウェアのデコーダーからの読み戻しの失敗、対応していないピクセルフォーマット) を、エラーにもキーフレームの要求にもせずに捨てる。avcodec がエラーを返し続けると、`push()` はキーフレームを求め続けるが、同じデコーダーを使い続けるので、ハードウェアのデバイスが失われると戻らない。作り直すのは、H264Decoder とハードウェアのデバイス、TimestampAligner、AvOffsetTranscriber、待ち行列である (`rebuild_screen_decode`)。`receive_restart_wait_ms` は、最後の出力より後に届いた最初のフレームから数える。最後の出力の時刻から数えると、静止した画面の共有で長く届かなかった後の 1 枚を、デコードし終える前に止まったとみなしてしまう。作り直す間も、合成のソースの子は空けない (`sources/screen_end.h` の `ReceiveRestarted`)。間隔の広げ方は stream_check と同じで、作り直したことと、戻ったことを OBS のログに 1 行ずつ書く。
- 送信側 (`src-vencord/core/stream_check.ts`、`index.ts` の `runStreamCheck`): 送れたフレームの数と、トラックのレシーバーの getStats() の inbound-rtp の packetsReceived と framesReceived を、1 秒あけて 2 回読む。RTP が届いているのに送れたフレームがなければ、同じトラックから読み出しとエンコードを始め直す (stream_id は替えない)。同じ人の、より新しい接続の映像が動いていれば、古いストリームを止める。RTP が届いていなければ (Discord が送っていない)、始め直さない。結果を stream_check_result で返す。確認の結果のログの行には、遅すぎて捨てたフレームの数 (`stale`) と、換算の定数を決め直した回数 (`reanchor`。「送信側: 送信側の時計への換算」) も書く。次に映像が止まったときに、原点のずれで捨てていたのか、ほかの原因なのかを見分けるためである。
- 送信側は、受信側の頼みとは別に、映像を送れないまま 5 秒たったストリームについて、トラックの状態 (readyState、muted)、接続の状態 (connectionState、iceConnectionState)、RTP が届いているか、読み出したフレームと遅すぎて捨てたフレームの数をログに書き (`core/video_watch.ts`)、同じ人の新しい接続の映像があれば、古いストリームを止める。
- stream_id を替えないのは、受信側の枠の割り当て、経路、録画のセグメントが stream_id と user_id に結び付くためである。同じ stream_id のまま新しいエンコーダで送ると、受信側と録画は、送信側がエンコーダを確保し直したとき (画質の上限の変更やフォールバック) と同じ道を通る。
- 履歴: 送信側が始め直した回数 (stream_check_result の restarted と superseded) と、受信側がデコードの経路を作り直した回数を、参加者ごとに数え (`PipelineSnapshot::receive_restarts`)、ドックの [状態] タブの履歴に「映像が届かなくなったため、受信をやり直しました: (名前)」と書く (`ledger/status_tab.cpp`)。

読むファイル: `src-obs/plugin-main.cpp`、`src-obs/pipeline/pipeline.cpp` (`handle_disconnected`、`check_receive_restart`)、`src-obs/pipeline/receive_restart.h`、`src-vencord/transport/`、`src-vencord/core/stream_check.ts`。

## 診断情報

設定ウィンドウの [バージョン情報] ページの [診断情報をエクスポート...] が、zip を 1 つ書き出す。zip の中にフォルダーはなく、6 つのファイルが入る (`src-obs/bundle`)。

| ファイル | 中身 |
|---|---|
| manifest.json | 書き出した時刻と時差、動いている部分のバージョンの一覧、Discord の内部への依存箇所の結果 (使えないと伝えられた id と、その後使えるようになった id)、ダミーを表示していた時間帯、受信側だけで分かる値 (待ち受けのポート番号、接続の回数、トークンの不一致の回数、ハンドシェイクの拒否、ハートビートの切断、Vesktop が動いているか、ハードウェアデコードから切り替えたストリームの数など)。読めなかった値は、キーを省かず `unknown` か 0 で書く |
| plugin.log | cordmux.dll の出来事の行のリングバッファー (設定の `plugin_event_log_lines` 行。既定 5000) |
| plugin-stats.log | cordmux.dll の統計の行のリングバッファー (設定の `plugin_log_ring_lines` 行。既定 200) |
| discord.log | 送信側から届いた出来事の行 (log の lines と alerts) のリングバッファー (設定の `js_event_log_lines` 行。既定 5000) |
| discord-stats.log | 送信側から届いた統計の行 (log の stats) のリングバッファー (設定の `js_log_ring_lines` 行。既定 200) |
| settings.json | 現在の設定 |

ログのリングバッファーは、上限を超えて捨てた行があれば、そのことを示す 1 行を先頭に書く。途中から始まるログを、そうと分からないまま渡さないためである。manifest.json にも、4 つのリングバッファーの捨てた行の数を別々に書く (`plugin_log_dropped`、`plugin_stats_log_dropped`、`discord_log_dropped`、`discord_stats_log_dropped`)。

ログの行は、出来事の行と統計の行に分けて保つ。出来事の行は、状態が変わったときの行 (接続、切断、トラックの追加と終了と mute、ゲートの開閉、エンコードの段階の変更、映像が届かないことと受信のやり直し、枠の割り当て、メッセージの表示と解消など) で、通話の全体を残す。統計の行は、数秒から 60 秒ごとの行 (受信側の `Stats ...`、届くたびに書く `Facts`、`Profiles`、`Avatar`、合成のソースの画像の用意の行、送信側の `clock origin`、`playout`、`latency`) で、直近の分だけを残す。受信側は、統計の行を `diag::logf(diag::LineKind::Stats, ...)` で書く。送信側は 3 種類に分け (`src-vencord/core/logbook.ts`)、異常に関わる出来事の行 (alerts) は、受信側が OBS のログに LOG_WARNING で書く (docs/wire_format.md の「メッセージの種類」の log の扱い)。出来事の行の既定の 5000 行は、4 時間の通話を残せる大きさとして決めた (`src-obs/config/settings.h` の `kEventLogDefaultLines`)。

10 秒ごとの統計の行 (`Stats stream=N video_latency` など) は `LOG_DEBUG` で書く。OBS は `--verbose` で起動したときだけ、この行をログに書く。読み方は `src-obs/pipeline/README.md` にある。

トークンと個人の情報は、次のように守る。

1. 伏せる (`RedactFn`)。各ファイルの中身から、トークンの文字列と、秘密の値を持つキー (キーを語に分けて、語が完全に一致したときだけ) の値を伏せる。
2. 個人の情報を置き換える。Discord のユーザー ID (17 桁から 20 桁の数) は `user-1`、`user-2` のような印にする。印の対応は全部のファイルで共通なので、ファイルをまたいで同じ人を追える。Windows の利用者のプロファイルのフォルダーは `%USERPROFILE%` にする。
3. 確認する (`RevealFn`)。作り終えた全文にトークンが残っていないかを確認する。残ったファイルは、除いた理由の 1 行に置き換え、ほかのファイルはそのまま書き出す。置き換えた後にまだ残っていれば、zip を書かない。ポート番号は確認しない。診断情報は利用者が自分で選んで渡すもので、ポート番号はトークンがなければ使えず、診断に必要な情報として manifest.json にも入れているためである。

トークンを知っているのは `plugin-main.cpp` だけで、ドックには関数として渡す。ログの行は、書く時点でトークンとポート番号の両方を調べ、含まれていれば行ごと捨てる (`guard_and_log`)。cordmux.dll のログは `diag::logf` で書く (CODESTYLE.md の「ログ」)。`blog()` を直接呼ぶと、この確認も plugin.log への記録も通らないためである。

zip は無圧縮で、時刻を引数で受け取るので、同じ入力から同じバイト列ができる。作るのと書くのは UI のスレッドの外で行い、保存と失敗のダイアログは `dock/` が表示する。

## Windows に依存する部分

cordmux は Windows の 64 ビットだけを対象にする。Windows の API を呼ぶ cordmux.dll のコードは、`src-obs/platform/platform_win.cpp` に限る。ほかのファイルは OS の名前を書かず、`test/lint/windows_api_confinement.py` が、platform/ の外 (tests/ を除く) に Windows の名前がないことをチェックする。ほかの OS に広げるときは、`platform.h` の関数を実装した `platform_<OS>.cpp` を追加する。送信側も同じで、Windows の名前 (`process.platform`、`LOCALAPPDATA`) を持つのは `src-vencord/paths.ts` だけである。

| 機能 | Windows の仕組み |
|---|---|
| ネットワークの準備 | WinSock の起動と終了 (`startup()`、`shutdown()`) |
| session.json | `%LOCALAPPDATA%\cordmux` に、現在の利用者だけが読める ACL (継承しない DACL) で書く。別の OBS が動いているかは、pid と `GetProcessTimes` の作成時刻で調べる |
| Vesktop の検出 | 実行ファイルの名前で、OBS と同じ利用者のプロセスを探す (SID と Windows のセッションで比べる)。管理者として起動した同じ利用者のプロセスは、SID を読めなくても、同じセッションなら数える |
| Vesktop の設定の場所 | Vesktop v1.6.9 の `DATA_DIR` (`src/main/constants.ts`) と同じ決め方で、環境変数 `VENCORD_USER_DATA_DIR` があればそこ、なければ RoamingAppData の既知のフォルダーの下の `Vesktop`。その下の `state.json` と `settings/settings.json` を使う。環境変数 `APPDATA` は読まない。実機テストは `VENCORD_USER_DATA_DIR` で差し替える。ポータブル版の Vesktop (exe の隣に `Uninstall Vesktop.exe` がない) には対応しない。動いているものを見つけたら、`[Vesktopに追加]` は書かずに伝える。書いた `state.json` の場所はバックアップ (`state_path`) に残し、元に戻すときはその場所を使う |
| ファイル | UTF-8 のパスを UTF-16 にして渡す。`std::ofstream` と `fopen` はパスをアクティブコードページの文字列として読み、ASCII でない保存先を開けない。書き込みは、同じフォルダーの一時ファイルに書いてから置き換える |
| 録画の子プロセス | `ffmpeg.exe` の起動、終了の待ち、停止。コマンドラインの引用符は `build_child_command_line()` で組み立てる |
| 録画のセッションの使用中のマーカー | 共有なしで開いたファイル。持っているプロセスが終了すると (クラッシュしても)OS が消す |

パスは UTF-8 で受け取り、UTF-8 で返す。パスの区切りは `/` で組み立て、Windows の API は `/` をそのまま受け付ける。

OS 以外の前提もある。OBS 33 以上の x64 版 (Arm64 版は対象外) が必要である。ポータブル版では libobs が相対パスを返すので、OBS の作業フォルダーを基準に絶対パスにしてから、画面に出し、Vesktop に書く (`platform::absolute_path`)。送信側は、Vesktop(Electron) の中の `MediaStreamTrackProcessor` と WebCodecs に依存する。対応する環境とバージョンは docs/spec.md の「対応する環境とバージョン」にある。
