// 브라우저에서 WASM 엔진을 쓰는 래퍼. server/engine.js 와 같은 모양(start/search/newGame/kill).
export const MOVE_RE = /^[a-h][1-8][a-h][1-8][qrbn]?$/;

export class LocalEngine {
  constructor({ hash = 16 } = {}) { this.hash = hash; this.worker = null; this.dead = false; this.waiters = new Set(); }

  async start() {
    const base = new URL('./', import.meta.url).href;
    this.worker = new Worker(new URL('./engine-worker.js', import.meta.url));
    this.worker.onmessage = (e) => {
      const d = e.data;
      if (d.line !== undefined) for (const w of [...this.waiters]) w.onLine(d.line);
      else if (d.ready) for (const w of [...this.waiters]) w.onLine('\u0000ready');
      else if (d.error) this._die(new Error(d.error));
    };
    this.worker.onerror = (e) => this._die(new Error(e.message || 'worker error'));
    const ready = this.waitLine((l) => l === '\u0000ready', 30000);
    this.worker.postMessage({ init: true, base, hash: this.hash });
    await ready;
    this.send('uci');
    await this.waitLine((l) => l === 'uciok', 5000);
    this.send('setoption name Move Overhead value 5');
    return this;
  }

  _die(err) {
    if (this.dead) return;
    this.dead = true;
    try { this.worker?.terminate(); } catch {}
    for (const w of [...this.waiters]) w.onDead(err);
  }

  send(cmd) { if (!this.dead) this.worker.postMessage({ cmd }); }

  waitLine(test, ms) {
    return new Promise((resolve, reject) => {
      if (this.dead) return reject(new Error('engine dead'));
      const done = () => { clearTimeout(timer); this.waiters.delete(w); };
      const w = { onLine: (l) => { if (test(l)) { done(); resolve(l); } }, onDead: (e) => { done(); reject(e || new Error('engine died')); } };
      const timer = setTimeout(() => { done(); this._die(new Error('timeout')); reject(new Error('engine timeout')); }, ms);
      this.waiters.add(w);
    });
  }

  newGame() { this.send('ucinewgame'); }

  // 반환: { best, cp, mate, depth, nodes, nps }  (cp/mate 는 둘 차례 기준)
  async search({ startFen = null, moves = [], go, timeoutMs }) {
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
      }
      return l.startsWith('bestmove');
    }, timeoutMs);
    const base = startFen ? `position fen ${startFen}` : 'position startpos';
    this.send(moves.length ? `${base} moves ${moves.join(' ')}` : base);
    this.send(`go ${go}`);
    const line = await p;
    return { best: line.split(' ')[1], cp: info.cp ?? null, mate: info.mate ?? null, depth: info.depth ?? 0, nodes: info.nodes ?? 0, nps: info.nps ?? 0 };
  }

  kill() { this._die(new Error('killed')); }
}
