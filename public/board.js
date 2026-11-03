// 의존성 없는 SVG 체스판. 클릭 이동 + 드래그 이동.
const GLYPH = { k: '♚', q: '♛', r: '♜', b: '♝', n: '♞', p: '♟' };   // 채워진 글리프를 색만 바꿔 사용
const FILES = 'abcdefgh';
const FONT = '"Segoe UI Symbol", "Noto Sans Symbols 2", "DejaVu Sans", "Apple Symbols", sans-serif';   // style.css 의 .pc 와 같아야 함
let METRICS = null;
// 글꼴마다 기물 글리프의 크기와 위치가 다르다. 실제로 그려지는 영역을 재서 칸 중앙에 맞춘다.
function metrics() {
  if (METRICS) return METRICS;
  const ctx = document.createElement('canvas').getContext('2d');
  ctx.font = `100px ${FONT}`;
  const m = {}; let maxDim = 1;
  for (const k in GLYPH) {
    const r = ctx.measureText(GLYPH[k] + '\uFE0E');
    m[k] = { a: r.actualBoundingBoxAscent, d: r.actualBoundingBoxDescent, l: r.actualBoundingBoxLeft, r: r.actualBoundingBoxRight };
    maxDim = Math.max(maxDim, m[k].a + m[k].d, m[k].l + m[k].r);
  }
  METRICS = maxDim > 20 ? { m, scale: 8.2 / maxDim } : { m: null, scale: 0.082 };   // 가장 큰 기물이 칸의 82%
  return METRICS;
}

export class Board {
  // opts.legalTargets(sq) -> [{to, promo:boolean}]   opts.onMove({from,to,promotion})
  constructor(root, opts = {}) {
    this.root = root; this.opts = opts;
    this.flipped = false; this.pieces = new Map(); this.lastMove = null; this.check = null;
    this.selected = null; this.targets = []; this.interactive = null; this.drag = null;
    const NS = 'http://www.w3.org/2000/svg';
    this.svg = document.createElementNS(NS, 'svg');
    this.svg.setAttribute('viewBox', '0 0 80 80');
    this.svg.classList.add('board');
    root.appendChild(this.svg);
    this.svg.addEventListener('pointerdown', (e) => this.down(e));
    this.svg.addEventListener('pointermove', (e) => this.move(e));
    this.svg.addEventListener('pointerup', (e) => this.up(e));
    this.svg.addEventListener('pointercancel', () => this.cancelDrag());
    this.promoEl = document.createElement('div');
    this.promoEl.className = 'promo hidden';
    root.appendChild(this.promoEl);
    this.render();
  }

  // fen: 말 배치, interactive: 'w' | 'b' | null(조작 불가)
  set({ fen, lastMove = null, interactive = null, check = null }) {
    this.pieces = new Map();
    const rows = fen.split(' ')[0].split('/');
    rows.forEach((row, r) => { let f = 0; for (const ch of row) { if (/\d/.test(ch)) f += +ch; else { this.pieces.set(FILES[f] + (8 - r), ch); f++; } } });
    this.lastMove = lastMove; this.interactive = interactive; this.check = check;
    this.selected = null; this.targets = []; this.hidePromo();
    this.render();
  }

  flip() { this.flipped = !this.flipped; this.render(); }

  xy(sq) {
    let f = FILES.indexOf(sq[0]), r = 8 - +sq[1];
    if (this.flipped) { f = 7 - f; r = 7 - r; }
    return [f * 10, r * 10];
  }
  sqAt(e) {
    const b = this.svg.getBoundingClientRect();
    let f = Math.floor(((e.clientX - b.left) / b.width) * 8), r = Math.floor(((e.clientY - b.top) / b.height) * 8);
    if (f < 0 || f > 7 || r < 0 || r > 7) return null;
    if (this.flipped) { f = 7 - f; r = 7 - r; }
    return FILES[f] + (8 - r);
  }

  render() {
    const NS = 'http://www.w3.org/2000/svg';
    const el = (n, a, txt) => { const e = document.createElementNS(NS, n); for (const k in a) e.setAttribute(k, a[k]); if (txt != null) e.textContent = txt; return e; };
    const g = el('g', {});
    for (let r = 0; r < 8; r++) for (let f = 0; f < 8; f++) {
      const sq = FILES[this.flipped ? 7 - f : f] + (this.flipped ? r + 1 : 8 - r);
      const light = (f + r) % 2 === 0;
      let cls = light ? 'sq-l' : 'sq-d';
      if (this.lastMove && (sq === this.lastMove.slice(0, 2) || sq === this.lastMove.slice(2, 4))) cls += ' sq-last';
      if (sq === this.selected) cls += ' sq-sel';
      g.appendChild(el('rect', { x: f * 10, y: r * 10, width: 10, height: 10, class: cls }));
      if (this.check === sq) g.appendChild(el('rect', { x: f * 10, y: r * 10, width: 10, height: 10, class: 'sq-check' }));
    }
    // 좌표
    for (let i = 0; i < 8; i++) {
      const file = FILES[this.flipped ? 7 - i : i], rank = this.flipped ? i + 1 : 8 - i;
      g.appendChild(el('text', { x: i * 10 + 9.4, y: 79, class: 'coord ' + ((i + 7) % 2 === 0 ? 'cl' : 'cd'), 'text-anchor': 'end' }, file));
      g.appendChild(el('text', { x: 0.7, y: i * 10 + 3, class: 'coord ' + (i % 2 === 0 ? 'cd' : 'cl') }, String(rank)));
    }
    for (const t of this.targets) {
      const [x, y] = this.xy(t.to);
      g.appendChild(this.pieces.has(t.to)
        ? el('circle', { cx: x + 5, cy: y + 5, r: 4.6, class: 'tgt-cap' })
        : el('circle', { cx: x + 5, cy: y + 5, r: 1.7, class: 'tgt' }));
    }
    for (const [sq, p] of this.pieces) {
      if (this.drag && this.drag.from === sq) continue;
      const [x, y] = this.xy(sq);
      g.appendChild(this.pieceEl(p, x + 5, y + 5));
    }
    if (this.drag) g.appendChild(this.pieceEl(this.drag.piece, this.drag.x, this.drag.y + 1.6, 'drag'));
    this.svg.replaceChildren(g);
  }

  pieceEl(p, cx, cy, extra = '') {   // (cx, cy) = 기물이 놓일 칸의 중앙
    const white = p === p.toUpperCase();
    const t = document.createElementNS('http://www.w3.org/2000/svg', 'text');
    const { m, scale } = metrics(), g = m && m[p.toLowerCase()], size = 100 * scale;
    t.setAttribute('x', g ? cx - ((g.r - g.l) * scale) / 2 : cx);
    t.setAttribute('y', g ? cy + ((g.a - g.d) * scale) / 2 : cy + size * 0.35);
    t.setAttribute('text-anchor', g ? 'start' : 'middle');
    t.setAttribute('font-size', size); t.setAttribute('class', `pc ${white ? 'pw' : 'pb'} ${extra}`);
    t.textContent = GLYPH[p.toLowerCase()] + '︎';   // 텍스트 글리프로 강제 (이모지 방지)
    return t;
  }

  ownPiece(sq) {
    const p = this.pieces.get(sq);
    return p && this.interactive && (p === p.toUpperCase()) === (this.interactive === 'w');
  }

  down(e) {
    if (!this.interactive) return;
    const sq = this.sqAt(e); if (!sq) return;
    this.hidePromo();
    if (this.selected && this.targets.some((t) => t.to === sq)) { this.tryMove(this.selected, sq); return; }
    if (this.ownPiece(sq)) {
      this.selected = sq;
      this.targets = this.opts.legalTargets?.(sq) ?? [];
      const b = this.svg.getBoundingClientRect();
      this.drag = { from: sq, piece: this.pieces.get(sq), x: ((e.clientX - b.left) / b.width) * 80, y: ((e.clientY - b.top) / b.height) * 80 - 1.6, moved: false };
      this.svg.setPointerCapture(e.pointerId);
    } else { this.selected = null; this.targets = []; }
    this.render();
  }
  move(e) {
    if (!this.drag) return;
    const b = this.svg.getBoundingClientRect();
    this.drag.x = ((e.clientX - b.left) / b.width) * 80; this.drag.y = ((e.clientY - b.top) / b.height) * 80 - 1.6;
    this.drag.moved = true;
    this.render();
  }
  up(e) {
    if (!this.drag) return;
    const d = this.drag; this.drag = null;
    const sq = this.sqAt(e);
    if (d.moved && sq && sq !== d.from && this.targets.some((t) => t.to === sq)) return this.tryMove(d.from, sq);
    if (d.moved) { this.selected = null; this.targets = []; }   // 허용되지 않는 곳에 놓으면 선택 해제
    this.render();
  }
  cancelDrag() { this.drag = null; this.render(); }

  tryMove(from, to) {
    const t = this.targets.find((x) => x.to === to);
    this.selected = null; this.targets = []; this.drag = null;
    if (t?.promo) { this.render(); return this.askPromo(from, to); }
    this.render();
    this.opts.onMove?.({ from, to });
  }

  askPromo(from, to) {
    const white = this.interactive === 'w';
    this.promoEl.replaceChildren();
    for (const p of ['q', 'r', 'b', 'n']) {
      const b = document.createElement('button');
      b.className = 'promo-btn ' + (white ? 'pw' : 'pb');
      b.textContent = GLYPH[p] + '︎';
      b.onclick = () => { this.hidePromo(); this.opts.onMove?.({ from, to, promotion: p }); };
      this.promoEl.appendChild(b);
    }
    this.promoEl.classList.remove('hidden');
  }
  hidePromo() { this.promoEl.classList.add('hidden'); }
}
