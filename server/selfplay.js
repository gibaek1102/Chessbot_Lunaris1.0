// 자가대국 워커: 서버에서 fork 되어 계속 대국을 두고, 진행 상황을 IPC로 올려 보낸다.
// 단독 실행도 가능: node server/selfplay.js
import { Chess } from 'chess.js';
import os from 'node:os';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { Engine, MOVE_RE } from './engine.js';

const here = path.dirname(fileURLToPath(import.meta.url));
const DATA_DIR = process.env.DATA_DIR || path.join(here, '..', 'data');
const ENGINE_PATH = process.env.ENGINE_PATH || path.join(here, '..', 'bin', 'lunaris');
const MOVETIME = +(process.env.SELFPLAY_MOVETIME || 250);       // ms/수
const HASH = +(process.env.SELFPLAY_HASH || 64);
const THREADS = +(process.env.SELFPLAY_THREADS || 1);
const NET_B = process.env.SELFPLAY_NET_B || null;               // 지정하면 흑 엔진은 다른 넷 파일 사용
const MAX_PLIES = 400;
const RECENT_KEEP = 50;

fs.mkdirSync(DATA_DIR, { recursive: true });
const STATS_FILE = path.join(DATA_DIR, 'stats.json');
const JSONL_FILE = path.join(DATA_DIR, 'games.jsonl');
const PGN_FILE = path.join(DATA_DIR, 'games.pgn');

const emit = (m) => { if (process.send) process.send(m); };
try { os.setPriority(19); } catch {}   // 사람 대국이 항상 우선 (이후 띄우는 엔진도 이 우선순위를 상속)

// ---- 통계 ----
let stats = { games: 0, white: 0, black: 0, draws: 0, plies: 0, adjudicated: 0, since: Date.now(), nodes: 0 };
let recent = [];
try {
  const j = JSON.parse(fs.readFileSync(STATS_FILE, 'utf8'));
  stats = { ...stats, ...j.stats }; recent = j.recent ?? [];
} catch {}
let nextId = (recent.at(-1)?.id ?? stats.games) + 1;

function saveStats() {
  const tmp = STATS_FILE + '.tmp';
  fs.writeFileSync(tmp, JSON.stringify({ stats, recent }));
  fs.renameSync(tmp, STATS_FILE);
}
const summary = (g) => ({ id: g.id, ts: g.ts, result: g.result, reason: g.reason, plies: g.moves.length, opening: g.opening, white: g.white, black: g.black });
const publishStats = () => emit({ type: 'stats', stats, recent: recent.map((g) => ({ ...g })) });

// ---- 오프닝 ----
const openings = fs.readFileSync(path.join(here, 'openings.epd'), 'utf8').split('\n').map((s) => s.trim()).filter(Boolean)
  .map((l) => l.split(' ').slice(0, 4).join(' ') + ' 0 1')
  .filter((f) => { try { new Chess(f); return true; } catch { return false; } });

const fmtEval = (e) => (e.mate !== null ? `#${e.mate}` : ((e.cp >= 0 ? '+' : '') + (e.cp / 100).toFixed(2)));

async function playGame(white, black, startFen, openingIdx) {
  const game = new Chess(startFen);
  const moves = [], evals = [];
  const gid = nextId++;
  const meta = { id: gid, ts: Date.now(), white: white.name, black: black.name, startFen, opening: openingIdx };
  white.eng.newGame(); black.eng.newGame();
  let result = null, reason = null;
  const live = (extra = {}) => emit({ type: 'live', game: { ...meta, moves, evals, fen: game.fen(), result, reason, ...extra } });
  live();

  while (!game.isGameOver()) {
    if (moves.length >= MAX_PLIES) { result = '1/2-1/2'; reason = 'maxplies'; break; }
    const side = game.turn() === 'w' ? white : black;
    const r = await side.eng.search({ startFen, moves, go: `movetime ${MOVETIME}`, timeoutMs: MOVETIME + 8000 });
    if (!MOVE_RE.test(r.best ?? '')) throw new Error('bad bestmove ' + r.best);
    let mv;
    try { mv = game.move({ from: r.best.slice(0, 2), to: r.best.slice(2, 4), promotion: r.best[4] }); } catch { mv = null; }
    if (!mv) throw new Error('illegal bestmove ' + r.best);
    const s = mv.color === 'w' ? 1 : -1;
    const ev = { cp: r.cp === null ? null : r.cp * s, mate: r.mate === null ? null : r.mate * s, depth: r.depth };
    moves.push(r.best); evals.push(ev);
    stats.nodes += r.nodes;
    game.setComment(`${fmtEval(ev)}/${ev.depth}`);
    live();

    // 판정(adjudication): 대국이 한없이 길어지는 것을 막는다. 종료 사유에 항상 표시됨.
    const n = evals.length;
    const big = (e) => (e.mate !== null ? Math.sign(e.mate) * 10000 : e.cp);
    if (n >= 40) {
      const last = evals.slice(-6).map(big);
      if (last.every((v) => v >= 1000)) { result = '1-0'; reason = 'adjudication'; break; }
      if (last.every((v) => v <= -1000)) { result = '0-1'; reason = 'adjudication'; break; }
    }
    if (n >= 100 && evals.slice(-12).every((e) => e.mate === null && Math.abs(e.cp) <= 5)) { result = '1/2-1/2'; reason = 'adjudication'; break; }
    await new Promise((r2) => setImmediate(r2));
  }

  if (!result) {
    if (game.isCheckmate()) { result = game.turn() === 'w' ? '0-1' : '1-0'; reason = 'checkmate'; }
    else {
      result = '1/2-1/2';
      reason = game.isStalemate() ? 'stalemate' : game.isThreefoldRepetition() ? 'repetition' : game.isInsufficientMaterial() ? 'insufficient' : 'fifty';
    }
  }
  game.header('Event', 'Lunaris self-play', 'Site', 'chess.ericlee.dev', 'Date', new Date(meta.ts).toISOString().slice(0, 10).replace(/-/g, '.'),
    'Round', String(gid), 'White', white.name, 'Black', black.name, 'Result', result, 'Termination', reason, 'TimeControl', `${MOVETIME}ms/move`);
  const full = { ...meta, moves, evals, result, reason };
  fs.appendFileSync(JSONL_FILE, JSON.stringify({ ...full, startFen }) + '\n');
  fs.appendFileSync(PGN_FILE, game.pgn() + '\n\n');

  stats.games++; stats.plies += moves.length;
  if (result === '1-0') stats.white++; else if (result === '0-1') stats.black++; else stats.draws++;
  if (reason === 'adjudication') stats.adjudicated++;
  recent.push(full); if (recent.length > RECENT_KEEP) recent.shift();
  saveStats();
  live({ final: true });
  publishStats();
}

async function startEngines() {
  const mk = async (name, evalFile) => ({ name, eng: await new Engine(ENGINE_PATH, { hash: HASH, threads: THREADS, moveOverhead: 10, evalFile }).start() });
  return [await mk('Lunaris', null), await mk(NET_B ? 'Lunaris (net B)' : 'Lunaris', NET_B)];
}

async function main() {
  publishStats();
  let engines = null, idx = Math.floor(Math.random() * openings.length), swap = false, fails = 0;
  for (;;) {
    try {
      engines ??= await startEngines();
      // 같은 오프닝을 색을 바꿔 두 번씩 둔다 (두 엔진이 다르면 운 보정, 같으면 다양성)
      const [a, b] = swap ? [engines[1], engines[0]] : engines;
      await playGame(a, b, openings[idx], idx);
      if (swap) idx = Math.floor(Math.random() * openings.length);
      swap = !swap; fails = 0;
    } catch (e) {
      fails++;
      console.error('[selfplay] error:', e.message);
      engines?.forEach((x) => x.eng.kill()); engines = null;
      await new Promise((r) => setTimeout(r, Math.min(30000, 1000 * 2 ** fails)));
    }
  }
}

process.on('SIGTERM', () => process.exit(0));
process.on('disconnect', () => process.exit(0));   // 부모(서버)가 죽으면 같이 종료
main();
