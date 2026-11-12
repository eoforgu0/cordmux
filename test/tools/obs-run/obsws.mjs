// obs-websocket v5 の最小クライアント (Node 22+ の組み込み WebSocket を使う。依存なし)
// 認証なし・127.0.0.1 だけ。実機の実行の自動操作用。
//
// call() は応答が無限に来ない場合 (OBS が固まった・接続だけ生きている等) を握り続けると、実行スクリプト
// (run-window.mjs) の期限判定が動作しなくなる。そこで時間切れ (既定 DEFAULT_CALL_TIMEOUT_MS) を持たせ、
// 超えたら reject して pending から取り除く。

const DEFAULT_CALL_TIMEOUT_MS = 30000;

// Identify の eventSubscriptions に渡す購読のフラグ (obs-websocket v5 の EventSubscription)。既定は 0 (何も購読しない)。
// InputVolumeMeters は 50 ms ごとに届く高頻度のメッセージなので、読む間だけ別の接続で購読する。
export const OBS_EVENT_SUBSCRIPTION_INPUT_VOLUME_METERS = 1 << 16;

export class ObsWs {
  // opts.eventSubscriptions: Identify で購読するメッセージ (ビット和。既定は 0)。
  constructor(port, opts = {}) {
    this.port = port;
    this.eventSubscriptions = Number.isInteger(opts.eventSubscriptions) && opts.eventSubscriptions >= 0 ? opts.eventSubscriptions : 0;
    this.seq = 0;
    this.pending = new Map();
    this.events = [];
  }

  // 接続して Identify まで済ませる。OBS の起動待ちのために timeoutMs の間は再試行する
  async connect(timeoutMs = 60000) {
    const deadline = Date.now() + timeoutMs;
    let lastErr = null;
    while (Date.now() < deadline) {
      try {
        await this.#connectOnce();
        return;
      } catch (e) {
        lastErr = e;
        await new Promise((r) => setTimeout(r, 1000));
      }
    }
    throw new Error("obs-websocket に繋がらない: " + (lastErr && lastErr.message));
  }

  #connectOnce() {
    return new Promise((resolve, reject) => {
      const ws = new WebSocket(`ws://127.0.0.1:${this.port}`);
      let identified = false;
      ws.onerror = () => reject(new Error("接続失敗"));
      ws.onclose = () => {
        if (!identified) reject(new Error("接続が閉じた"));
        for (const [, p] of this.pending) p.reject(new Error("接続が閉じた"));
        this.pending.clear();
      };
      ws.onmessage = (ev) => {
        const msg = JSON.parse(ev.data);
        if (msg.op === 0) {
          // Hello -> Identify (認証なし前提)
          ws.send(JSON.stringify({ op: 1, d: { rpcVersion: 1, eventSubscriptions: this.eventSubscriptions } }));
        } else if (msg.op === 2) {
          identified = true;
          this.ws = ws;
          resolve();
        } else if (msg.op === 7) {
          const p = this.pending.get(msg.d.requestId);
          if (!p) return;
          this.pending.delete(msg.d.requestId);
          if (msg.d.requestStatus.result) p.resolve(msg.d.responseData || {});
          else p.reject(new Error(`${msg.d.requestType}: ${msg.d.requestStatus.code} ${msg.d.requestStatus.comment || ""}`));
        } else if (msg.op === 5) {
          this.events.push(msg.d);
        }
      };
    });
  }

  // timeoutMs <= 0 は時限なし (旧来どおり無期限に待つ。既定の呼び出し元は付けない)。
  call(requestType, requestData = {}, timeoutMs = DEFAULT_CALL_TIMEOUT_MS) {
    const requestId = String(++this.seq);
    return new Promise((resolve, reject) => {
      const timer =
        timeoutMs > 0
          ? setTimeout(() => {
              this.pending.delete(requestId);
              reject(new Error(`${requestType}: 応答が ${timeoutMs}ms 以内に来ない (timeout)`));
            }, timeoutMs)
          : null;
      this.pending.set(requestId, {
        resolve: (v) => {
          if (timer) clearTimeout(timer);
          resolve(v);
        },
        reject: (e) => {
          if (timer) clearTimeout(timer);
          reject(e);
        },
      });
      this.ws.send(JSON.stringify({ op: 6, d: { requestType, requestId, requestData } }));
    });
  }

  close() {
    if (this.ws) this.ws.close();
  }
}
