// 送信モード用の簡易受け口。プラグインの WebSocket サーバがないときの代用。
// バイナリフレーム (type/streamId/timestampUs/flags/payload) を受け、
// type:streamId ごとに件数・合計バイト数・最新 timestampUs を集計して 5 秒ごとに出す。
import { WebSocketServer } from 'ws';

const port = Number(process.env.HARNESS_SINK_PORT || 4470);
const stats = new Map();
let totalFrames = 0;

const wss = new WebSocketServer({ port, host: '127.0.0.1' });
console.log(`[fake-sink] listening on ws://127.0.0.1:${port}`);

wss.on('connection', (ws) => {
  console.log('[fake-sink] connection opened');
  ws.on('message', (data) => {
    const buf = Buffer.isBuffer(data) ? data : Buffer.from(data);
    if (buf.length < 14) return;
    const view = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
    const type = view.getUint8(0);
    const streamId = view.getUint32(1, true);
    const timestampUs = view.getBigUint64(5, true);
    const key = `${type}:${streamId}`;
    const s = stats.get(key) || { count: 0, bytes: 0, lastTimestampUs: '0' };
    s.count++;
    s.bytes += buf.length;
    s.lastTimestampUs = timestampUs.toString();
    stats.set(key, s);
    totalFrames++;
  });
  ws.on('close', () => console.log('[fake-sink] connection closed'));
});

setInterval(() => {
  console.log(`[fake-sink] ${new Date().toISOString()} totalFrames=${totalFrames}`);
  for (const [key, s] of stats) {
    console.log(`  ${key}: count=${s.count} bytes=${s.bytes} lastTimestampUs=${s.lastTimestampUs}`);
  }
}, 5000);
