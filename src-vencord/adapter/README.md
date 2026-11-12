# src-vencord/adapter

Discord と Vencord の内部から事実を読み、core/ に渡す部分。判断は core/ が持ち、ここは事実を読むことと、視聴を始めて止めることと、[移動] と [退出] の実行だけをする。Discord の内部への依存箇所と、接続と人の対応付けの考え方は、docs/architecture.md の「Discord の内部への依存箇所」にある。

## 入口

| ファイル | 中身 |
|---|---|
| `types.ts` | core とのインターフェイス `Adapter` と、`TrackEvent`、`Snapshot` などの型 |
| `vencord.ts` | `createVencordAdapter()`。Discord と Vencord の内部を読む `Adapter` の実装。RTCPeerConnection と voice gateway の WebSocket を差し替え、接続とトラックを `ConnectionTracker` に渡す |
| `connections.ts` | `ConnectionTracker`。voice gateway と RTCPeerConnection を対応付け、接続ごとの pcId を決め、Go Live の接続で画面共有している人を特定する |
| `default_avatar.ts` | `defaultAvatarUrl()`。自分のアバターを設定していない人の、既定のアバターの CDN の URL を作る |
| `username.ts` | `uniqueUsername()`。Discord の中で 1 人に 1 つのユーザー名を作る |

`Adapter` のもう 1 つの実装は、テスト用の adapter (`tests/fake_adapter.ts`) である。

## 依存

- Vencord (`@webpack`、`@webpack/common`) を import するのは `vencord.ts` だけである。そのため `vencord.ts` は `tsconfig.json` の `exclude` でリポジトリのルートの tsc から外し、Vencord の `pnpm test:ts` でチェックする (`src-vencord/README.md`)。
- ほかのファイルは Vencord も DOM の型も使わないので、リポジトリのルートの tsc と vitest でテストできる (`tests/adapter_*.test.ts`)。`types.ts` が import するのは、`core/events.ts` と `ledger.ts` の型だけである。
- Discord の内部への依存箇所の一覧は `ledger.ts` にある。
