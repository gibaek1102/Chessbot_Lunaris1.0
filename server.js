import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fork } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { WebSocketServer } from 'ws';
import { PlaySession } from './play.js';

const here = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.join(here, '..');
const PUBLIC = path.join(ROOT, 'public');
const DATA_DIR = process.env.DATA_DIR || path.join(ROOT, 'data');
const ENGINE_PATH = process.env.ENGINE_PATH || path.join(ROOT, 'bin', 'lunaris');
const PORT = +(process.env.PORT || 3000);
const MAX_PLAY = +(process.env.PLAY_MAX_ENGINES || 4);          // 동시에 띄울 사람용 엔진 수
const MAX_PER_IP = +(process.env.PLAY_MAX_PER_IP || 2);
const IDLE_MS = +(process.env.PLAY_IDLE_MIN || 20) * 60_000;
const SELFPLAY = process.env.SELFPLAY !== '0';
const ALLOWED_ORIGINS = (process.env.ALLOWED_ORIGINS || 'https://chess.ericlee.dev').split(',').map((s) => s.trim()).filter(Boolean);
const TRUST_PROXY = process.env.TRUST_PROXY !== '0';             // Caddy/nginx 뒤에서 X-Forwarded-For 사용

if (!fs.existsSync(ENGINE_PATH)) { console.error('엔진 실행 파일이 없습니다:', ENGINE_PATH, '(먼저 ./build.sh)'); process.exit(1); }

// ---------- 엔진 슬롯 풀 ----------
const perIp = new Map();
let active = 0;
const pool = {
  acquire(ip) {
    if (active >= MAX_PLAY || (perIp.get(ip) ?? 0) >= MAX_PER_IP) return null;
    active++; perIp.set(ip, (perIp.get(ip) ?? 0) + 1);
    let released = false;
    return () => {
      if (released) return; released = true;
      active--; const n = (perIp.get(ip) ?? 1) - 1; n <= 0 ? perIp.delete(ip) : perIp.set(ip, n);
    };
  },
};

// ---------- 자가대국 워커 ----------
let currentChild = null;
let liveGame = null, statsMsg = { stats: null, recent: [] };
const liveSubs = new Set();
function startSelfplay() {
  const child = fork(path.join(here, 'selfplay.js'), [], { env: { ...process.env, DATA_DIR, ENGINE_PATH }, stdio: ['ignore', 'inherit', 'inherit', 'ipc'] });
  child.on('message', (m) => {
    if (m.type === 'live') { liveGame = m.game; broadcast({ type: 'live', game: m.game }); }
    else if (m.type === 'stats') { statsMsg = { stats: m.stats, recent: m.recent }; broadcast({ type: 'stats', stats: m.stats, recent: m.recent.map(slim) }); }
  });
  child.on('exit', (code) => { console.error('[server] selfplay exited', code, '- restart in 5s'); setTimeout(startSelfplay, 5000); });
  currentChild = child;
}
const slim = ({ moves, evals, startFen, ...rest }) => ({ ...rest, plies: moves.length });
function broadcast(obj) {
  const s = JSON.stringify(obj);
  for (const ws of liveSubs) if (ws.readyState === 1) ws.send(s);
}

// ---------- HTTP ----------
const MIME = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8', '.svg': 'image/svg+xml', '.png': 'image/png', '.ico': 'image/x-icon', '.json': 'application/json', '.txt': 'text/plain; charset=utf-8' };
const SEC = {
  'X-Content-Type-Options': 'nosniff',
  'Referrer-Policy': 'no-referrer',
  'X-Frame-Options': 'DENY',
  'Content-Security-Policy': "default-src 'self'; connect-src 'self' wss: ws:; style-src 'self' 'unsafe-inline'; img-src 'self' data:; frame-ancestors 'none'",
};
const json = (res, obj, code = 200) => { res.writeHead(code, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store', ...SEC }); res.end(JSON.stringify(obj)); };

const server = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://x');
  if (req.method !== 'GET' && req.method !== 'HEAD') { res.writeHead(405); return res.end(); }
  const p = url.pathname;

  if (p === '/api/stats') return json(res, { engine: 'Lunaris 1.2.1', playActive: active, playMax: MAX_PLAY, selfplay: SELFPLAY, stats: statsMsg.stats, recent: statsMsg.recent.map(slim) });
  if (p.startsWith('/api/game/')) {
    const g = statsMsg.recent.find((x) => String(x.id) === p.slice(10));
    return g ? json(res, g) : json(res, { error: 'not found' }, 404);
  }
  if (p === '/games.pgn') {   // 최근 최대 2MB만 내려준다
    const f = path.join(DATA_DIR, 'games.pgn');
    fs.stat(f, (err, st) => {
      if (err) { res.writeHead(404); return res.end('no games yet'); }
      const start = Math.max(0, st.size - 2_000_000);
      res.writeHead(200, { 'Content-Type': 'application/x-chess-pgn', 'Content-Disposition': 'attachment; filename="lunaris-selfplay.pgn"', ...SEC });
      fs.createReadStream(f, { start }).pipe(res);
    });
    return;
  }

  // 정적 파일 (경로 탈출 차단)
  let rel = decodeURIComponent(p);
  if (rel.endsWith('/')) rel += 'index.html';
  const file = path.normalize(path.join(PUBLIC, rel));
  if (!file.startsWith(PUBLIC + path.sep)) { res.writeHead(403); return res.end(); }
  fs.readFile(file, (err, buf) => {
    if (err) { res.writeHead(404, { 'Content-Type': 'text/plain' }); return res.end('Not found'); }
    const ext = path.extname(file);
    res.writeHead(200, { 'Content-Type': MIME[ext] || 'application/octet-stream', 'Cache-Control': ext === '.html' ? 'no-cache' : 'public, max-age=300', ...SEC });
    res.end(req.method === 'HEAD' ? undefined : buf);
  });
});

// ---------- WebSocket ----------
const wss = new WebSocketServer({ server, path: '/ws', maxPayload: 1024 });
const clientIp = (req) => (TRUST_PROXY && req.headers['x-forwarded-for']?.split(',')[0].trim()) || req.socket.remoteAddress || 'unknown';

wss.on('connection', (ws, req) => {
  const origin = req.headers.origin;
  if (origin && !ALLOWED_ORIGINS.includes(origin) && !/^https?:\/\/(localhost|127\.0\.0\.1)(:\d+)?$/.test(origin)) return ws.close(1008, 'origin');

  const ip = clientIp(req);
  const send = (o) => { if (ws.readyState === 1) ws.send(JSON.stringify(o)); };
  const session = new PlaySession({ send, pool, enginePath: ENGINE_PATH, ip });
  let tokens = 20, last = Date.now();   // 초당 약 5메시지 한도

  const idle = setInterval(() => { if (Date.now() - session.lastActive > IDLE_MS) { send({ type: 'idle' }); ws.close(); } }, 30_000);
  ws.isAlive = true;
  ws.on('pong', () => { ws.isAlive = true; });

  ws.on('message', (data) => {
    const now = Date.now(); tokens = Math.min(20, tokens + (now - last) / 200); last = now;
    if (--tokens < 0) return;
    let msg;
    try { msg = JSON.parse(data.toString()); } catch { return; }
    if (!msg || typeof msg.type !== 'string') return;
    if (msg.type === 'sub_live') {
      liveSubs.add(ws);
      send({ type: 'stats', stats: statsMsg.stats, recent: statsMsg.recent.map(slim) });
      if (liveGame) send({ type: 'live', game: liveGame });
      return;
    }
    if (msg.type === 'unsub_live') { liveSubs.delete(ws); return; }
    session.handle(msg).catch(() => {});
  });
  ws.on('close', () => { clearInterval(idle); liveSubs.delete(ws); session.close(); });
  ws.on('error', () => {});
});

setInterval(() => {   // 끊어진 연결 정리
  for (const ws of wss.clients) { if (!ws.isAlive) { ws.terminate(); continue; } ws.isAlive = false; ws.ping(); }
}, 30_000);

server.listen(PORT, () => {
  console.log(`[server] http://localhost:${PORT}  engine=${ENGINE_PATH}  play slots=${MAX_PLAY}  selfplay=${SELFPLAY}`);
  if (SELFPLAY) startSelfplay();
});

process.on('exit', () => currentChild?.kill());
for (const sig of ['SIGINT', 'SIGTERM']) process.on(sig, () => process.exit(0));
