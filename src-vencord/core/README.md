# src-vencord/core

adapter/ が読んだ事実 (`events.ts` の `CoreEvent`) から判断を作る純粋関数。誰を視聴するか、警告、facts と profiles の中身、読み出しとエンコードを許すかを決める。Vencord を import しないので、リポジトリのルートの tsc と vitest でテストできる (`tests/core_*.test.ts`)。

## 入口

| ファイル | 中身 |
|---|---|
| `events.ts` | adapter が core に渡すイベントの型 `CoreEvent` |
| `state.ts` | 状態 `CoreState` と、ただ 1 つの遷移関数 `reduce()`。状態から `warnings()`、`decisions()`、`userIdForSsrc()` を返す |
| `facts.ts` | 制御用の JSON の facts と profiles の中身を作る `buildFacts()` と `buildProfiles()`。アバターを取得する相手を決める `avatarFetchTargets()` と `avatarFetchesToStart()`。avatar を送る人と数を決める `AvatarOutbox` |
| `avatar.ts` | avatar の avatar_png を 1 人ぶん作る `fetchAvatarPng()` |
| `gate.ts` | 読み出しとエンコードを許すゲート。純粋関数の `planGate()` と、状態を持つ `createMediaGate()` |
| `wire.ts` | ワイヤー形式のバイナリメッセージのヘッダーをエンコードする `encodeHeader()` と、デコードする `decodeHeader()` |
| `logbook.ts` | 送信側のログの行を、出来事の行、異常に関わる出来事の行、統計の行に分けて、受信側へ送るまで保つ `LogBook` |
| `stream_check.ts` | 映像が届かないストリームについて、読み出しとエンコードを始め直すかを、getStats() の受信の数などの証拠で決める `decideStreamCheck()`。同じ人の新しい接続の映像を探す `newerScreenStreamOf()` |
| `video_watch.ts` | 送信側が、映像を送れないまま 5 秒たったことと、送れるようになったことに気付く `watchVideo()` |

## 依存

- このフォルダーのほかに読み込むのは、`ledger.ts` (依存箇所の id の型)、`adapter/types.ts` の型、`encode/ladder.ts` (エンコードの段階の文字列)、`transport/control.ts` の型だけである。
- ネットワークと DOM は使わない。アバターの取得と再エンコード (`avatar.ts`) と、読み出しを始める処理と止める処理 (`gate.ts`) は、呼び出し元の `index.ts` が関数として渡す。
- ワイヤー形式は、docs/wire_format.md の「バイナリメッセージ」と「制御用の JSON」にある。
