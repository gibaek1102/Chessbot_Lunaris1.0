// 사람 vs Lunaris 대국 세션 (WebSocket 연결 1개 = 세션 1개)
import { Chess } from 'chess.js';
import { Engine, MOVE_RE } from './engine.js';

// 레벨은 엔진 옵션이 아니라 '탐색량'으로만 조절한다 (Elo 표기는 하지 않음).
export const LEVELS = {
  1: 'nodes 30',
  2: 'nodes 300',
  3: 'nodes 3000',
  4: 'movetime 300',
  5: 'movetime 1000',
  6: 'movetime 3000',
};

const START_FEN = new Chess().fen();

export class PlaySession {
  constructor({ send, pool, enginePath, ip }) {
    this.send = send;          // (obj) => void
    this.pool = pool;          // { acquire(ip) -> release fn | null }
    this.enginePath = enginePath;
    this.ip = ip;
    this.engine = null;
    this.release = null;
    this.game = new Chess();
    this.humanColor = 'w';
    this.level = 3;
    this.busy = false;
    this.over = null;          // { result, reason }
    this.epoch = 0;            // 새 게임/종료 시 증가 → 늦게 도착한 엔진 응답 무시
    this.lastEval = null;
    this.touch();
  }

  touch() { this.lastActive = Date.now(); }

  async ensureEngine() {
    if (this.engine && !this.engine.dead) return true;
    if (!this.release) {
      this.release = this.pool.acquire(this.ip);
      if (!this.release) return false;
    }
    try {
      this.engine = await new Engine(this.enginePath, { hash: 32, threads: 1, moveOverhead: 50 }).start();
    } catch {
      this.engine = null;
      return false;
    }
    return true;
  }

  state() {
    return {
      type: 'state',
      moves: this.game.history({ verbose: true }).map((m) => m.from + m.to + (m.promotion ?? '')),
      fen: this.game.fen(),
      humanColor: this.humanColor,
      level: this.level,
      thinking: this.busy,
      over: this.over,
      eval: this.lastEval,
    };
  }

  async handle(msg) {
    this.touch();
    switch (msg.type) {
      case 'new': return this.newGame(msg);
      case 'move': return this.humanMove(msg);
      case 'undo': return this.undo();
      case 'resign': return this.resign();
      default: return;
    }
  }

  async newGame({ color, level }) {
    this.epoch++;
    this.engine?.send('stop');
    const lv = Number.isInteger(level) && LEVELS[level] ? level : 3;
    this.level = lv;
    this.humanColor = color === 'b' ? 'b' : color === 'w' ? 'w' : Math.random() < 0.5 ? 'w' : 'b';
    this.game = new Chess();
    this.over = null;
    this.lastEval = null;
    this.busy = false;
    if (!(await this.ensureEngine())) {
      return this.send({ type: 'busy' });
    }
    this.engine.newGame();
    this.send(this.state());
    if (this.game.turn() !== this.humanColor) await this.engineMove();
  }

  async humanMove({ uci }) {
    if (this.busy || this.over || this.game.turn() !== this.humanColor) return;
    if (typeof uci !== 'string' || !MOVE_RE.test(uci)) return this.send({ type: 'error', code: 'bad_move' });
    let mv;
    try {
      mv = this.game.move({ from: uci.slice(0, 2), to: uci.slice(2, 4), promotion: uci[4] });
    } catch { mv = null; }
    if (!mv) return this.send({ type: 'error', code: 'illegal' });
    if (this.checkOver()) return this.send(this.state());
    await this.engineMove();
  }

  async engineMove() {
    const epoch = this.epoch;
    this.busy = true;
    this.send(this.state());
    try {
      if (!(await this.ensureEngine())) throw new Error('no engine');
      const moves = this.game.history({ verbose: true }).map((m) => m.from + m.to + (m.promotion ?? ''));
      let r = await this.engine.search({ moves, go: LEVELS[this.level], timeoutMs: 15000 });
      if (epoch !== this.epoch) return;
      let mv = this.tryMove(r.best);
      if (!mv) {   // 방어: 엔진이 불법수를 내면 한 번 더 시도 후 무작위 합법수
        r = await this.engine.search({ moves, go: 'nodes 2000', timeoutMs: 15000 });
        if (epoch !== this.epoch) return;
        mv = this.tryMove(r.best) ?? this.game.move(this.game.moves({ verbose: true })[0]);
      }
      const s = mv.color === 'w' ? 1 : -1;   // 엔진 점수(둔 쪽 기준) → 백 기준
      this.lastEval = { cp: r.cp === null ? null : r.cp * s, mate: r.mate === null ? null : r.mate * s, depth: r.depth, nodes: r.nodes, nps: r.nps };
      this.busy = false;
      this.checkOver();
      this.send(this.state());
    } catch {
      if (epoch !== this.epoch) return;
      this.busy = false;
      this.engine?.kill(); this.engine = null;
      this.send({ type: 'error', code: 'engine_failed' });
      this.send(this.state());
    }
  }

  tryMove(uci) {
    if (typeof uci !== 'string' || !MOVE_RE.test(uci)) return null;
    try { return this.game.move({ from: uci.slice(0, 2), to: uci.slice(2, 4), promotion: uci[4] }); } catch { return null; }
  }

  checkOver() {
    const g = this.game;
    if (!g.isGameOver()) return false;
    let result = '1/2-1/2', reason = 'draw';
    if (g.isCheckmate()) { result = g.turn() === 'w' ? '0-1' : '1-0'; reason = 'checkmate'; }
    else if (g.isStalemate()) reason = 'stalemate';
    else if (g.isThreefoldRepetition()) reason = 'repetition';
    else if (g.isInsufficientMaterial()) reason = 'insufficient';
    else reason = 'fifty';
    this.over = { result, reason };
    return true;
  }

  async undo() {
    if (this.busy || !this.engine) return;
    const n = this.game.history().length;
    if (n === 0) return;
    this.over = null;
    this.game.undo();
    if (this.game.turn() !== this.humanColor && this.game.history().length > 0) this.game.undo();
    this.lastEval = null;
    this.send(this.state());
    if (this.game.turn() !== this.humanColor) await this.engineMove();
  }

  resign() {
    if (this.over) return;
    this.epoch++;
    this.engine?.send('stop');
    this.busy = false;
    this.over = { result: this.humanColor === 'w' ? '0-1' : '1-0', reason: 'resign' };
    this.send(this.state());
  }

  close() {
    this.epoch++;
    this.engine?.kill();
    this.engine = null;
    this.release?.();
    this.release = null;
  }
}

export { START_FEN };
