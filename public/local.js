// 서버 없이 브라우저 안에서 동작하는 백엔드. server/play.js + server/selfplay.js 의 브라우저 버전이다.
// app.js 와는 WebSocket 때와 같은 메시지(state / live / stats ...)로 대화한다.
import { Chess } from './vendor/chess.js';
import { LocalEngine, MOVE_RE } from './local-engine.js';

const clampMs = (v, max) => Math.max(20, Math.min(max, Math.round(+v) || 1000));
const SELF_MOVETIME = 300, SELF_PACE = 1500, GAME_PAUSE = 4000, MAX_PLIES = 400, RECENT_KEEP = 50, STORE = 'lunaris-local-stats-v1';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const uciOf = (m) => m.from + m.to + (m.promotion ?? '');
const mvTry = (g, u) => { try { return g.move({ from: u.slice(0, 2), to: u.slice(2, 4), promotion: u[4] }); } catch { return null; } };
const fmtEval = (e) => (e.mate !== null ? `#${e.mate}` : ((e.cp >= 0 ? '+' : '') + (e.cp / 100).toFixed(2)));

export function createLocalBackend(emit) {
  // ------------------------ 사람 대국 ------------------------
  const play = { engine: null, game: new Chess(), humanColor: 'w', movetime: 1000, busy: false, over: null, epoch: 0, lastEval: null };

  const state = () => ({
    type: 'state', moves: play.game.history({ verbose: true }).map(uciOf), fen: play.game.fen(),
    humanColor: play.humanColor, movetime: play.movetime, thinking: play.busy, over: play.over, eval: play.lastEval,
  });

  async function ensureEngine() {
    if (play.engine && !play.engine.dead) return true;
    try { play.engine = await new LocalEngine({ hash: 16 }).start(); return true; } catch { play.engine = null; return false; }
  }

  function checkOver() {
    const g = play.game;
    if (!g.isGameOver()) return false;
    let result = '1/2-1/2', reason = 'fifty';
    if (g.isCheckmate()) { result = g.turn() === 'w' ? '0-1' : '1-0'; reason = 'checkmate'; }
    else if (g.isStalemate()) reason = 'stalemate';
    else if (g.isThreefoldRepetition()) reason = 'repetition';
    else if (g.isInsufficientMaterial()) reason = 'insufficient';
    play.over = { result, reason };
    return true;
  }

  async function engineMove() {
    const epoch = play.epoch;
    play.busy = true; emit(state());
    try {
      const moves = play.game.history({ verbose: true }).map(uciOf);
      let r = null;
      for (let attempt = 0; attempt < 3 && !r; attempt++) {   // 엔진이 죽으면 새 워커로 다시 시도한다
        try {
          if (!(await ensureEngine())) throw new Error('no engine');
          r = await play.engine.search({ moves, go: 'movetime ' + play.movetime, timeoutMs: play.movetime + 20000 });
        } catch (e) {
          if (epoch !== play.epoch) return;
          play.engine?.kill(); play.engine = null;
          if (attempt === 2) throw e;
        }
      }
      if (epoch !== play.epoch) return;
      let mv = MOVE_RE.test(r.best ?? '') ? mvTry(play.game, r.best) : null;
      if (!mv) {
        r = await play.engine.search({ moves, go: 'nodes 2000', timeoutMs: 20000 });
        if (epoch !== play.epoch) return;
        mv = (MOVE_RE.test(r.best ?? '') ? mvTry(play.game, r.best) : null) ?? play.game.move(play.game.moves({ verbose: true })[0]);
      }
      const s = mv.color === 'w' ? 1 : -1;
      play.lastEval = { cp: r.cp === null ? null : r.cp * s, mate: r.mate === null ? null : r.mate * s, depth: r.depth, nodes: r.nodes, nps: r.nps };
      play.busy = false; checkOver(); emit(state());
    } catch {
      if (epoch !== play.epoch) return;
      play.busy = false; play.engine?.kill(); play.engine = null;
      emit({ type: 'error', code: 'engine_failed' }); emit(state());
    }
  }

  async function newGame({ color, movetime }) {
    play.epoch++;
    play.movetime = clampMs(movetime, 3600000);
    play.humanColor = color === 'b' ? 'b' : color === 'w' ? 'w' : Math.random() < 0.5 ? 'w' : 'b';
    play.game = new Chess(); play.over = null; play.lastEval = null; play.busy = false;
    if (play.engine) { play.engine.kill(); play.engine = null; }   // 탐색 중일 수 있는 워커를 확실히 정리
    if (!(await ensureEngine())) { emit({ type: 'error', code: 'engine_failed' }); return emit(state()); }
    play.engine.newGame();
    emit(state());
    if (play.game.turn() !== play.humanColor) await engineMove();
  }

  async function humanMove({ uci }) {
    if (play.busy || play.over || play.game.turn() !== play.humanColor) return;
    if (typeof uci !== 'string' || !MOVE_RE.test(uci) || !mvTry(play.game, uci)) return emit({ type: 'error', code: 'illegal' });
    if (checkOver()) return emit(state());
    await engineMove();
  }

  async function undo() {
    if (play.busy) return;
    if (play.game.history().length === 0) return;
    play.over = null; play.game.undo();
    if (play.game.turn() !== play.humanColor && play.game.history().length > 0) play.game.undo();
    play.lastEval = null; emit(state());
    if (play.game.turn() !== play.humanColor) await engineMove();
  }

  function resign() {
    if (play.over) return;
    play.epoch++; play.busy = false;
    if (play.engine) { play.engine.kill(); play.engine = null; }   // 동기 탐색 중인 워커는 종료시켜야 멈춘다
    play.over = { result: play.humanColor === 'w' ? '0-1' : '1-0', reason: 'resign' };
    emit(state());
  }

  // ------------------------ 자가대국 (라이브 탭을 보는 동안만) ------------------------
  let stats = { games: 0, white: 0, black: 0, draws: 0, plies: 0, adjudicated: 0, since: Date.now(), nodes: 0 };
  let recent = [], nextId = 1, liveGame = null, selfRunning = false, selfToken = 0, engines = null, openings = null;
  try {
    const j = JSON.parse(localStorage.getItem(STORE) || 'null');
    if (j) { stats = { ...stats, ...j.stats }; nextId = stats.games + 1; }
  } catch {}
  const saveStats = () => { try { localStorage.setItem(STORE, JSON.stringify({ stats })); } catch {} };
  const slim = ({ moves, evals, startFen, ...rest }) => ({ ...rest, plies: moves.length });
  const publishStats = () => emit({ type: 'stats', stats, recent: recent.map(slim) });

  async function loadOpenings() {
    if (openings) return openings;
    const txt = await (await fetch(new URL('./openings.epd', import.meta.url))).text();
    openings = txt.split('\n').map((s) => s.trim()).filter(Boolean).map((l) => l.split(' ').slice(0, 4).join(' ') + ' 0 1')
      .filter((f) => { try { new Chess(f); return true; } catch { return false; } });
    return openings;
  }

  async function playSelfGame(token, white, black, startFen, openingIdx) {
    const game = new Chess(startFen), moves = [], evals = [];
    const meta = { id: nextId++, ts: Date.now(), white: 'Lunaris', black: 'Lunaris', startFen, opening: openingIdx };
    let result = null, reason = null;
    const live = (extra = {}) => { liveGame = { ...meta, moves: [...moves], evals: [...evals], fen: game.fen(), result, reason, ...extra }; emit({ type: 'live', game: liveGame }); };
    white.newGame(); black.newGame(); live();
    while (!game.isGameOver()) {
      if (token !== selfToken) return;
      if (moves.length >= MAX_PLIES) { result = '1/2-1/2'; reason = 'maxplies'; break; }
      const side = game.turn() === 'w' ? white : black;
      const t0 = performance.now();
      const r = await side.search({ startFen, moves, go: `movetime ${SELF_MOVETIME}`, timeoutMs: SELF_MOVETIME + 15000 });
      if (token !== selfToken) return;
      const rest = SELF_PACE - (performance.now() - t0);   // 사람이 따라볼 수 있도록 한 수를 최소 SELF_PACE ms 간격으로 보여준다
      if (rest > 0) { await sleep(rest); if (token !== selfToken) return; }
      if (!MOVE_RE.test(r.best ?? '')) throw new Error('bad bestmove');
      const mv = mvTry(game, r.best);
      if (!mv) throw new Error('illegal bestmove');
      const s = mv.color === 'w' ? 1 : -1;
      const ev = { cp: r.cp === null ? null : r.cp * s, mate: r.mate === null ? null : r.mate * s, depth: r.depth };
      moves.push(r.best); evals.push(ev); stats.nodes += r.nodes; live();
      const n = evals.length, big = (e) => (e.mate !== null ? Math.sign(e.mate) * 10000 : e.cp);
      if (n >= 40) {
        const last = evals.slice(-6).map(big);
        if (last.every((v) => v >= 1000)) { result = '1-0'; reason = 'adjudication'; break; }
        if (last.every((v) => v <= -1000)) { result = '0-1'; reason = 'adjudication'; break; }
      }
      if (n >= 100 && evals.slice(-12).every((e) => e.mate === null && Math.abs(e.cp) <= 5)) { result = '1/2-1/2'; reason = 'adjudication'; break; }
    }
    if (!result) {
      if (game.isCheckmate()) { result = game.turn() === 'w' ? '0-1' : '1-0'; reason = 'checkmate'; }
      else { result = '1/2-1/2'; reason = game.isStalemate() ? 'stalemate' : game.isThreefoldRepetition() ? 'repetition' : game.isInsufficientMaterial() ? 'insufficient' : 'fifty'; }
    }
    stats.games++; stats.plies += moves.length;
    if (result === '1-0') stats.white++; else if (result === '0-1') stats.black++; else stats.draws++;
    if (reason === 'adjudication') stats.adjudicated++;
    recent.push({ ...meta, moves, evals, result, reason }); if (recent.length > RECENT_KEEP) recent.shift();
    saveStats(); live({ final: true }); publishStats();
  }

  async function selfLoop(token) {
    const ops = await loadOpenings();
    let idx = Math.floor(Math.random() * ops.length), fails = 0;
    while (token === selfToken) {
      try {
        engines ??= [await new LocalEngine({ hash: 8 }).start(), await new LocalEngine({ hash: 8 }).start()];
        if (token !== selfToken) break;
        await playSelfGame(token, engines[0], engines[1], ops[idx], idx);
        idx = Math.floor(Math.random() * ops.length); fails = 0;
        if (token === selfToken) await sleep(GAME_PAUSE);   // 끝난 판의 결과를 잠깐 보여준다
      } catch {
        if (token !== selfToken) break;
        engines?.forEach((e) => e.kill()); engines = null;
        if (++fails >= 3) { selfRunning = false; emit({ type: 'error', code: 'engine_failed' }); return; }
        await new Promise((r) => setTimeout(r, 1000 * fails));
      }
    }
  }

  function startSelf() {
    publishStats(); if (liveGame) emit({ type: 'live', game: liveGame });
    if (selfRunning) return;
    selfRunning = true; const token = ++selfToken;
    selfLoop(token).finally(() => { if (token === selfToken) selfRunning = false; });
  }
  function stopSelf() {   // 탭을 떠나면 CPU를 쓰지 않도록 멈춘다 (진행 중이던 판은 버림)
    selfToken++; selfRunning = false;
    engines?.forEach((e) => e.kill()); engines = null; liveGame = null;
  }

  function getGame(id) { return recent.find((g) => String(g.id) === String(id)) ?? null; }

  function pgn() {
    return recent.map((gm) => {
      const g = new Chess(gm.startFen);
      gm.moves.forEach((u, i) => { mvTry(g, u); const e = gm.evals[i]; if (e) g.setComment(`${fmtEval(e)}/${e.depth}`); });
      g.header('Event', 'Lunaris self-play (browser)', 'Site', location.host, 'Round', String(gm.id), 'White', gm.white, 'Black', gm.black, 'Result', gm.result, 'Termination', gm.reason);
      return g.pgn();
    }).join('\n\n');
  }

  return {
    local: true,
    getGame, pgn,
    send(msg) {
      switch (msg.type) {
        case 'new': return void newGame(msg);
        case 'move': return void humanMove(msg);
        case 'undo': return void undo();
        case 'resign': return void resign();
        case 'sub_live': return startSelf();
        case 'unsub_live': return stopSelf();
      }
    },
  };
}
