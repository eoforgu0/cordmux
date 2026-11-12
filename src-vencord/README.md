# src-vencord/

送信側。Vesktop の中で動く Vencord の userplugin で、通話の画面共有と声を受信側 (OBS の cordmux.dll) に送る。CI はこのフォルダーを Vencord の `src/userplugins/cordmux/` にコピーしてビルドする (`vencord.lock.json` と `tests/` はコピーしない)。仕組みは docs/architecture.md の「映像と音声の流れ」にある。

## 入口

- `index.ts`: プラグインの本体 (`definePlugin`)。`adapter/`、`core/`、`encode/`、`audio/`、`transport/` をつなぐ。
- `native.ts`: Vesktop のメインプロセスで動く部分。トークンファイルを読む `readSession()` だけを持つ。レンダラーからは `VencordNative.pluginHelpers.cordmux` で呼び出す。
- `ledger.ts`: 依存箇所の一覧 `CONTACT_LEDGER`。

## フォルダーとファイル

- `adapter/`: Discord の内部を読み、core のイベントとトラックにする部分。core との境界は `adapter/types.ts` の `Adapter` インターフェイスで、実装は `adapter/vencord.ts` (製品) と `tests/fake_adapter.ts` (単体テスト) にある。
- `core/`: 状態と判断を持つ純粋関数 (誰の画面共有を視聴するか、facts の作成、トラックの読み出しのゲートなど)。
- `encode/`: 画面共有の映像を WebCodecs でエンコードする部分。
- `audio/`: 声と画面共有の音声を、PCM のまま送る部分。
- `transport/`: 受信側との WebSocket の接続。
- `paths.ts`: Windows に依存する処理 (トークンファイルを置くフォルダー)。
- `version.ts`、`vesktop_version.ts`: 送信側のバージョンと、Vesktop のバージョン。
- `vencord.lock.json`: ビルドに使う Vencord のタグとコミット。CI が読む。
- `tests/`: 単体テスト (vitest)。

## 依存

- adapter は、Discord の事実を core のイベント (`core/events.ts` の `CoreEvent`) にするだけで、判断を持たない。依存箇所が見つからないときの扱いと、誰の画面共有を視聴するかは、core が決める。
- `core/`、`ledger.ts`、`adapter/types.ts`、`adapter/connections.ts` は Vencord を import しない。Vencord を import すると、そのファイルを vitest でテストできなくなるためである。
- `tests/fake_adapter.ts` の `killedAdapters()` は、依存箇所の一覧の行を 1 つずつ使えなくした `Adapter` を作る。`tests/fake_adapter.test.ts` は、使えなくした依存箇所が `warnings()` に出ることと、映像と音声を止める判断が出ないことを確認する。`tests/fake_adapter_count.test.ts` は、作った `Adapter` と一覧の行が、テストに書いた 9 つの id と過不足なく一致することを確認する。

## 型のチェック

Vencord のエイリアス (`@utils/types`、`@webpack`、`@webpack/common` など) は、Vencord の pnpm workspace の中でしか解決できない。そのため、ファイルを 2 つに分けてチェックする。

- Vencord を import するファイルと、ブラウザーにしかない API (`MediaStreamTrackProcessor` など) を直接使うファイル (`index.ts`、`native.ts`、`adapter/vencord.ts`、`encode/track_source.ts`、`audio/track_source.ts`) は、Vencord の `pnpm test:ts` でチェックする。リポジトリのルートの `tsconfig.json` の `exclude` に、これらのファイルを入れている。ただし、`exclude` は `include` のパターンから外すだけなので、`tests/` のファイルが import する `audio/track_source.ts` は、リポジトリのルートの `tsc --noEmit` もチェックする。
- ほかのファイルと `tests/` は、リポジトリのルートの `tsc --noEmit` と vitest でチェックする。このフォルダーで vitest が読むのは、`tests/**/*.test.ts` だけである (`vitest.config.ts` の `include`)。

Vencord を import するファイルか、ブラウザーにしかない API を使うファイルを追加するときは、`tsconfig.json` の `exclude` にも追加する。ローカルで `pnpm test:ts` を実行する手順は、docs/testing.md の「TypeScript の単体テスト」にある。
