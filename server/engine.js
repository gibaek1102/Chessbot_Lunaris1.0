// UCI 엔진 프로세스 래퍼. 한 인스턴스 = 엔진 프로세스 1개.
import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';

export const MOVE_RE = /^[a-h][1-8][a-h][1-8][qrbn]?$/;

export class Engine {
  constructor(path, { hash = 32, threads = 1, moveOverhead = 30, evalFile = null, nice = false } = {}) {
    this.path = path;
    this.opts = { hash, threads, moveOverhead, evalFile };
    this.proc = null;
    this.dead = false;
    this.waiters = new Set();
  }

  async start() {
    this.proc = spawn(this.path, [], { stdio: ['pipe', 'pipe', 'ignore'] });
    this.proc.on('exit', () => this._onExit());
    this.proc.on('error', () => this._onExit());
    this.proc.stdin.on('error', () => {});
    createInterface({ input: this.proc.stdout }).on('line', (l) => {
      for (const w of [...this.waiters]) w.onLine(l);
    });
    this.send('uci');
    await this.waitLine((l) => l === 'uciok', 5000);
    const o = this.opts;
    this.send(`setoption name Hash value ${o.hash}`);
    this.send(`setoption name Threads value ${o.threads}`);
    this.send(`setoption name Move Overhead value ${o.moveOverhead}`);
    if (o.evalFile) this.send(`setoption name EvalFile value ${o.evalFile}`);
    await this.ready();
    return this;
  }

  _onExit() {
    if (this.dead) return;
    this.dead = true;
    for (const w of [...this.waiters]) w.onDead();
  }

  send(line) {
    if (this.dead || !this.proc?.stdin.writable) return;
    this.proc.stdin.write(line + '\n');
  }

  // test(line)이 true를 돌려주는 첫 줄을 기다린다.
  waitLine(test, ms) {
    return new Promise((resolve, reject) => {
      if (this.dead) return reject(new Error('engine dead'));
      const w = {
        onLine: (l) => {
          if (test(l)) { done(); resolve(l); }
        },
        onDead: () => { done(); reject(new Error('engine died')); },
      };
      const timer = setTimeout(() => { done(); reject(new Error('engine timeout')); }, ms);
      const done = () => { clearTimeout(timer); this.waiters.delete(w); };
      this.waiters.add(w);
    });
  }

  async ready() {
    this.send('isready');
    await this.waitLine((l) => l === 'readyok', 10000);
  }

  newGame() { this.send('ucinewgame'); }

  // go: 'movetime 500' | 'nodes 5000' | 'wtime .. btime ..' 등
  // 반환: { best, cp, mate, depth, nodes, nps, pv }  (cp/mate는 둘 차례 기준)
  async search({ startFen = null, moves = [], go, timeoutMs }) {
    await this.ready();
    const base = startFen ? `position fen ${startFen}` : 'position startpos';
    this.send(moves.length ? `${base} moves ${moves.join(' ')}` : base);
    const info = {};
    const p = this.waitLine((l) => {
      if (l.startsWith('info ') && l.includes(' score ') && !l.startsWith('info string')) {
        const m = (re) => { const r = re.exec(l); return r ? r[1] : null; };
        const cp = m(/ score cp (-?\d+)/), mate = m(/ score mate (-?\d+)/);
        if (cp !== null) { info.cp = +cp; info.mate = null; }
        if (mate !== null) { info.mate = +mate; info.cp = null; }
        info.depth = +(m(/ depth (\d+)/) ?? info.depth ?? 0);
        info.nodes = +(m(/ nodes (\d+)/) ?? info.nodes ?? 0);
        info.nps = +(m(/ nps (\d+)/) ?? info.nps ?? 0);
        const pv = / pv (.+)$/.exec(l);
        if (pv) info.pv = pv[1].split(' ').slice(0, 8);
      }
      return l.startsWith('bestmove');
    }, timeoutMs);
    this.send(`go ${go}`);
    let line;
    try { line = await p; } catch (e) { this.kill(); throw e; }
    const best = line.split(' ')[1];
    return { best, cp: info.cp ?? null, mate: info.mate ?? null, depth: info.depth ?? 0, nodes: info.nodes ?? 0, nps: info.nps ?? 0, pv: info.pv ?? [] };
  }

  kill() {
    if (this.dead) { return; }
    this.dead = true;
    try { this.proc.kill('SIGKILL'); } catch {}
    for (const w of [...this.waiters]) w.onDead();
  }
}
