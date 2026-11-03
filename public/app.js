import { Chess } from './vendor/chess.js';
import { Board } from './board.js';
import { createLocalBackend } from './local.js';

// 서버 없이 브라우저(WASM)에서 실행하는 모드: config.js 가 켜거나, 주소에 ?local 을 붙이면 켜진다.
const LOCAL = window.LUNARIS_LOCAL === true || new URLSearchParams(location.search).has('local');
let backend = null;

// ===================== i18n =====================
const T = {
  ko: {
    tab_play: '대국', tab_live: '라이브 자가대국', color: '색', white: '백', black: '흑', random: '랜덤', thinktime: '생각 시간',
    lv1: '1 · 입문', lv2: '2 · 쉬움', lv3: '3 · 보통', lv4: '4 · 0.3초', lv5: '5 · 1초', lv6: '6 · 3초',
    newgame: '새 게임', undo: '무르기', flip: '판 뒤집기', btn_resign: '기권', golive: '● 라이브', recent: '최근 대국',
    pgn: 'PGN 내려받기 (최근 2MB)', foot: 'Lunaris는 직접 학습한 NNUE 평가 함수를 쓰는 UCI 체스 엔진입니다.',
    you: '나', engine: 'Lunaris', start: '「새 게임」을 눌러 시작하세요.', yourturn: '당신 차례입니다', thinking: 'Lunaris가 생각 중',
    connecting: '서버에 연결하는 중…', busy: '지금 자리가 가득 찼어요. 잠시 후 다시 시도해 주세요.', engine_failed: '엔진에 문제가 생겨 이 수를 다시 계산합니다. 새 게임을 눌러 주세요.',
    idle: '오래 움직임이 없어 연결을 종료했습니다.', reconnecting: '연결이 끊겼습니다. 다시 연결하는 중…',
    r_1_0: '백 승', r_0_1: '흑 승', r_d: '무승부', checkmate: '체크메이트', stalemate: '스테일메이트', repetition: '3회 동형 반복', insufficient: '기물 부족', fifty: '50수 규칙', resign: '기권', adjudication: '판정', maxplies: '수 제한',
    won: '당신이 이겼습니다!', lost: 'Lunaris가 이겼습니다', depth: '깊이', nodes: '노드', nps: '노드/초', score: '평가', moves_empty: '아직 수가 없습니다',
    live_title: 'Lunaris vs Lunaris', game: '게임', ms: 'ms/수', live_wait: '자가대국을 기다리는 중…', replay: '다시보기', following: '실시간', over: '종료',
    st_games: '누적 대국', st_wdl: '백 승 / 무 / 흑 승', st_plies: '평균 길이', st_adj: '판정 종료', st_nodes: '누적 탐색 노드', st_since: '집계 시작', plies: '수',
    local_note: '이 브라우저 안에서 돌아가는 자가대국입니다. 탭을 떠나면 멈춰요.', rand_open: '시작 국면은 다양성을 위한 무작위 오프닝입니다 (수 번호는 그 국면부터).', your_color_w: '백', your_color_b: '흑', eval_label: '평가',
  },
  en: {
    tab_play: 'Play', tab_live: 'Live self-play', color: 'Color', white: 'White', black: 'Black', random: 'Random', thinktime: 'Think time',
    lv1: '1 · Beginner', lv2: '2 · Easy', lv3: '3 · Normal', lv4: '4 · 0.3s', lv5: '5 · 1s', lv6: '6 · 3s',
    newgame: 'New game', undo: 'Take back', flip: 'Flip board', btn_resign: 'Resign', golive: '● Live', recent: 'Recent games',
    pgn: 'Download PGN (last 2 MB)', foot: 'Lunaris is a UCI chess engine with a self-trained NNUE evaluation.',
    you: 'You', engine: 'Lunaris', start: 'Press “New game” to start.', yourturn: 'Your move', thinking: 'Lunaris is thinking',
    connecting: 'Connecting…', busy: 'All seats are taken right now. Please try again shortly.', engine_failed: 'The engine hit a problem. Please start a new game.',
    idle: 'Disconnected after a long period of inactivity.', reconnecting: 'Connection lost. Reconnecting…',
    r_1_0: 'White wins', r_0_1: 'Black wins', r_d: 'Draw', checkmate: 'checkmate', stalemate: 'stalemate', repetition: 'threefold repetition', insufficient: 'insufficient material', fifty: '50-move rule', resign: 'resignation', adjudication: 'adjudication', maxplies: 'move limit',
    won: 'You won!', lost: 'Lunaris won', depth: 'Depth', nodes: 'Nodes', nps: 'Nodes/s', score: 'Eval', moves_empty: 'No moves yet',
    live_title: 'Lunaris vs Lunaris', game: 'Game', ms: 'ms/move', live_wait: 'Waiting for the self-play game…', replay: 'Replay', following: 'live', over: 'finished',
    st_games: 'Games played', st_wdl: 'White / Draw / Black', st_plies: 'Avg. length', st_adj: 'Adjudicated', st_nodes: 'Total nodes searched', st_since: 'Counting since', plies: 'plies',
    local_note: 'Self-play runs inside your browser and pauses when you leave this tab.', rand_open: 'Games start from random opening positions for variety (move numbers count from there).', your_color_w: 'White', your_color_b: 'Black', eval_label: 'Eval',
  },
};
let lang = (navigator.language || '').startsWith('ko') ? 'ko' : 'en';
try { lang = localStorage.getItem('lang') || lang; } catch {}
const t = (k) => T[lang][k] ?? k;
function applyLang() {
  document.documentElement.lang = lang;
  document.querySelectorAll('[data-i18n]').forEach((e) => { e.textContent = t(e.dataset.i18n); });
  $('lang').textContent = lang === 'ko' ? 'EN' : '한';
  renderPlay(); renderLive(); showTime();
}
const $ = (id) => document.getElementById(id);
const fmtN = (n) => (n >= 1e9 ? (n / 1e9).toFixed(2) + 'B' : n >= 1e6 ? (n / 1e6).toFixed(1) + 'M' : n >= 1e3 ? (n / 1e3).toFixed(0) + 'K' : String(n));

// ===================== 공통 유틸 =====================
const uciOf = (m) => m.from + m.to + (m.promotion ?? '');
function kingSquare(game) {
  const turn = game.turn();
  const b = game.board();
  for (let r = 0; r < 8; r++) for (let f = 0; f < 8; f++) { const p = b[r][f]; if (p && p.type === 'k' && p.color === turn) return 'abcdefgh'[f] + (8 - r); }
  return null;
}
function applyUci(game, uci) {
  return game.move({ from: uci.slice(0, 2), to: uci.slice(2, 4), promotion: uci[4] });
}
function evalPct(ev) {
  if (!ev) return 50;
  if (ev.mate !== null && ev.mate !== undefined) return ev.mate > 0 ? 100 : 0;
  return 100 / (1 + Math.exp(-ev.cp / 400));   // 엔진의 점수 스케일(400)에 맞춤
}
function evalText(ev) {
  if (!ev) return '';
  if (ev.mate !== null && ev.mate !== undefined) return '#' + Math.abs(ev.mate);
  return (ev.cp >= 0 ? '+' : '−') + (Math.abs(ev.cp) / 100).toFixed(1);
}
function setEvalBar(el, ev, flipped) {
  el.classList.toggle('flip', !!flipped);
  el.querySelector('.fill').style.height = (flipped ? 100 - evalPct(ev) : evalPct(ev)) + '%';
  el.title = ev ? evalText(ev) : '';
}
function renderMoves(el, sans, cur, onClick) {
  el.replaceChildren();
  if (!sans.length) { const d = document.createElement('div'); d.className = 'empty'; d.textContent = t('moves_empty'); el.appendChild(d); return; }
  for (let i = 0; i < sans.length; i++) {
    if (i % 2 === 0) { const n = document.createElement('div'); n.className = 'n'; n.textContent = i / 2 + 1 + '.'; el.appendChild(n); }
    const m = document.createElement('div');
    m.className = 'm' + (onClick ? ' click' : '') + (i + 1 === cur ? ' cur' : '');
    m.textContent = sans[i];
    if (onClick) m.onclick = () => onClick(i + 1);
    el.appendChild(m);
  }
  if (sans.length % 2 === 1) el.appendChild(document.createElement('div'));
  const c = el.querySelector('.cur'); if (c) c.scrollIntoView({ block: 'nearest' }); else el.scrollTop = el.scrollHeight;
}
const legalTargetsFor = (game) => (sq) => {
  const seen = new Map();
  for (const m of game.moves({ square: sq, verbose: true })) seen.set(m.to, { to: m.to, promo: !!m.promotion });
  return [...seen.values()];
};
const reasonText = (r) => t(r) || r;
const resultText = (r) => (r === '1-0' ? t('r_1_0') : r === '0-1' ? t('r_0_1') : t('r_d'));

// ===================== WebSocket =====================
let ws = null, wsOpen = false, retry = 0, wantLive = false, reconnectTimer = null;
const outbox = [];
function connect() {
  if (LOCAL) { backend = createLocalBackend(onMessage); wsOpen = true; renderPlay(); return; }
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  ws = new WebSocket(`${proto}://${location.host}/ws`);
  ws.onopen = () => { wsOpen = true; retry = 0; showToast(null); outbox.splice(0).forEach((m) => ws.send(m)); if (wantLive) send({ type: 'sub_live' }); renderPlay(); };
  ws.onmessage = (e) => { let m; try { m = JSON.parse(e.data); } catch { return; } onMessage(m); };
  ws.onclose = () => {
    wsOpen = false; play.thinking = false;
    if (!play.closedByServer) showToast(t('reconnecting'));
    clearTimeout(reconnectTimer);
    reconnectTimer = setTimeout(connect, Math.min(8000, 800 * 2 ** retry++));
    renderPlay();
  };
}
function send(obj) {
  if (backend) return backend.send(obj);
  const s = JSON.stringify(obj);
  if (ws && ws.readyState === 1) ws.send(s);
  else if (obj.type === 'sub_live') { /* onopen에서 다시 보냄 */ }
}
let toastEl = null;
function showToast(msg) {
  if (!msg) { toastEl?.remove(); toastEl = null; return; }
  if (!toastEl) { toastEl = document.createElement('div'); toastEl.className = 'toast'; document.body.appendChild(toastEl); }
  toastEl.textContent = msg;
}

function onMessage(m) {
  switch (m.type) {
    case 'state': return onPlayState(m);
    case 'busy': play.busy = true; play.thinking = false; showToast(t('busy')); setTimeout(() => showToast(null), 4000); return renderPlay();
    case 'error': if (m.code === 'engine_failed') { showToast(t('engine_failed')); setTimeout(() => showToast(null), 5000); } return;
    case 'idle': play.closedByServer = true; showToast(t('idle')); return;
    case 'live': return onLive(m.game);
    case 'stats': liveStats = m; return renderLive();
  }
}

// ===================== PLAY =====================
const play = { game: new Chess(), humanColor: 'w', thinking: false, over: null, eval: null, started: false, level: 3, pendingNew: false, busy: false, closedByServer: false, lastMove: null };
const pBoard = new Board($('p-board'), {
  legalTargets: (sq) => legalTargetsFor(play.game)(sq),
  onMove: ({ from, to, promotion }) => {
    const mv = (() => { try { return play.game.move({ from, to, promotion }); } catch { return null; } })();
    if (!mv) return;
    play.lastMove = uciOf(mv); play.thinking = true;
    send({ type: 'move', uci: uciOf(mv) });
    renderPlay();
  },
});

function onPlayState(s) {
  play.busy = false;
  const g = new Chess();
  for (const u of s.moves) { try { applyUci(g, u); } catch { break; } }
  play.game = g;
  play.humanColor = s.humanColor;
  play.thinking = s.thinking; play.over = s.over; play.eval = s.eval; play.started = true;
  play.lastMove = s.moves.at(-1) ?? null;
  if (play.pendingNew) { pBoard.flipped = s.humanColor === 'b'; play.pendingNew = false; }
  renderPlay();
}

function renderPlay() {
  const g = play.game;
  const myTurn = play.started && !play.over && !play.thinking && g.turn() === play.humanColor;
  pBoard.set({ fen: g.fen(), lastMove: play.lastMove, interactive: myTurn ? play.humanColor : null, check: g.isCheck() ? kingSquare(g) : null });
  const sans = g.history();
  renderMoves($('p-moves'), sans, sans.length);
  const st = $('p-status');
  st.className = 'card status';
  if (!wsOpen) st.textContent = t('connecting');
  else if (!play.started) st.textContent = t('start');
  else if (play.over) {
    st.classList.add('over');
    const humanWon = (play.over.result === '1-0' && play.humanColor === 'w') || (play.over.result === '0-1' && play.humanColor === 'b');
    const head = play.over.result === '1/2-1/2' ? t('r_d') : humanWon ? t('won') : t('lost');
    st.textContent = `${head} · ${reasonText(play.over.reason)}`;
  } else if (play.thinking) { st.innerHTML = ''; st.append(t('thinking')); const d = document.createElement('span'); d.className = 'dots'; st.appendChild(d); }
  else st.textContent = t('yourturn');
  // 엔진 정보
  const ev = play.eval, info = $('p-info');
  info.replaceChildren();
  if (ev) for (const [k, v] of [[t('score'), evalText(ev)], [t('depth'), ev.depth], [t('nodes'), fmtN(ev.nodes)], [t('nps'), fmtN(ev.nps)]]) {
    const a = document.createElement('span'); a.textContent = k; const b = document.createElement('b'); b.textContent = v; info.append(a, b);
  }
  info.classList.toggle('hidden', !ev);
  setEvalBar($('p-eval'), play.started ? ev : null, pBoard.flipped);
  const hc = play.humanColor;
  const plate = (col, isYou) => `<b>${isYou ? t('you') : t('engine')}</b><span>${col === 'w' ? t('your_color_w') : t('your_color_b')}</span>`;
  const top = pBoard.flipped ? 'w' : 'b';
  $('p-top').innerHTML = play.started ? plate(top, top === hc) : '';
  $('p-bot').innerHTML = play.started ? plate(top === 'w' ? 'b' : 'w', (top === 'w' ? 'b' : 'w') === hc) : '';
  $('p-undo').disabled = !play.started || play.thinking || g.history().length === 0;
  $('p-resign').disabled = !play.started || !!play.over;
}

// 생각 시간 슬라이더 (ms). 서버 모드에서는 서버가 상한(기본 10초)으로 제한한다.
const TIMES = [50, 100, 200, 300, 500, 1000, 2000, 3000, 5000, 10000, 20000, 30000];
const fmtTime = (ms) => (lang === 'ko' ? `${ms / 1000}초` : `${ms / 1000} s`);
function showTime() { $('p-time-val').textContent = fmtTime(TIMES[+$('p-time').value]); }
try { const sv = localStorage.getItem('lunaris-time'); if (sv !== null && TIMES[+sv]) $('p-time').value = sv; } catch {}
$('p-time').oninput = showTime;
$('p-time').onchange = () => { try { localStorage.setItem('lunaris-time', $('p-time').value); } catch {} };
$('p-new').onclick = () => {
  if (!wsOpen) return;
  play.pendingNew = true; play.closedByServer = false;
  send({ type: 'new', color: $('p-color').value === 'r' ? 'random' : $('p-color').value, movetime: TIMES[+$('p-time').value] });
};
$('p-undo').onclick = () => send({ type: 'undo' });
$('p-resign').onclick = () => send({ type: 'resign' });
$('p-flip').onclick = () => { pBoard.flip(); renderPlay(); };

// ===================== LIVE =====================
let liveStats = null;
const lv = { game: null, ply: 0, follow: true, replayId: null, chess: null, sans: [], ucis: [] };
const lBoard = new Board($('l-board'), {});
lBoard.set({ fen: new Chess().fen() });

function loadGame(gm) {
  const c = new Chess(gm.startFen);
  const sans = [];
  for (const u of gm.moves) { try { sans.push(applyUci(c, u).san); } catch { break; } }
  lv.sans = sans; lv.ucis = gm.moves.slice(0, sans.length);
}
function onLive(gm) {
  if (lv.replayId !== null) { lv.liveBuf = gm; return; }   // 다시보기 중이면 보관만
  lv.liveBuf = gm;
  lv.game = gm;
  loadGame(gm);
  if (lv.follow) lv.ply = lv.sans.length;
  renderLive();
}
function fenAt(gm, ply) {
  const c = new Chess(gm.startFen);
  for (let i = 0; i < ply; i++) applyUci(c, lv.ucis[i]);
  return c;
}
function renderLive() {
  const gm = lv.game;
  const head = $('l-head');
  if (!gm) { head.innerHTML = `<div class="dim">${t('live_wait')}${LOCAL ? '<br>' + t('local_note') : ''}</div>`; renderLiveStats(); return; }
  const c = fenAt(gm, Math.min(lv.ply, lv.sans.length));
  const last = lv.ply > 0 ? lv.ucis[lv.ply - 1] : null;
  lBoard.set({ fen: c.fen(), lastMove: last, interactive: null, check: c.isCheck() ? kingSquare(c) : null });
  const ev = lv.ply > 0 ? gm.evals?.[lv.ply - 1] : null;
  setEvalBar($('l-eval'), ev ?? null, false);
  const ongoing = !gm.result;
  const over = gm.result ? `${resultText(gm.result)} · ${reasonText(gm.reason)}` : '';
  const evLine = ev ? `<div class="dim">${t('score')} <b style="color:var(--text)">${evalText(ev)}</b> · ${t('depth')} ${ev.depth}</div>` : '';
  head.innerHTML = `<div class="t">${t('live_title')}</div><div class="dim">${t('game')} #${gm.id} · ${lv.replayId !== null ? t('replay') : ongoing ? t('following') : t('over')}${over ? ' · ' + over : ''}</div>${evLine}<div class="dim" style="font-size:12px;margin-top:4px">${t('rand_open')}${LOCAL ? ' ' + t('local_note') : ''}</div>`;
  $('l-top').innerHTML = `<b>${gm.black}</b><span>${t('black')}</span>`;
  $('l-bot').innerHTML = `<b>${gm.white}</b><span>${t('white')}</span>`;
  renderMoves($('l-moves'), lv.sans, lv.ply, (p) => { lv.ply = p; lv.follow = false; renderLive(); });
  $('l-golive').classList.toggle('hidden', lv.follow && lv.replayId === null);
  $('l-first').disabled = $('l-prev').disabled = lv.ply === 0;
  $('l-next').disabled = $('l-last').disabled = lv.ply >= lv.sans.length;
  renderLiveStats();
}
function renderLiveStats() {
  const s = liveStats?.stats;
  const box = $('l-stats');
  if (!s) { box.innerHTML = ''; box.classList.add('hidden'); } else {
    box.classList.remove('hidden');
    const n = Math.max(1, s.games);
    const pw = (s.white / n) * 100, pd = (s.draws / n) * 100, pb = (s.black / n) * 100;
    box.innerHTML = `<div class="bar"><i class="w" style="width:${pw}%"></i><i class="d" style="width:${pd}%"></i><i class="b" style="width:${pb}%"></i></div>
      <div class="kv"><span>${t('st_games')}</span><b>${s.games}</b>
      <span>${t('st_wdl')}</span><b>${s.white} / ${s.draws} / ${s.black}</b>
      <span>${t('st_plies')}</span><b>${s.games ? Math.round(s.plies / s.games) : 0} ${t('plies')}</b>
      <span>${t('st_adj')}</span><b>${s.adjudicated}</b>
      <span>${t('st_nodes')}</span><b>${fmtN(s.nodes)}</b>
      <span>${t('st_since')}</span><b>${new Date(s.since).toLocaleDateString()}</b></div>`;
  }
  const list = $('l-recent');
  list.replaceChildren();
  for (const g of [...(liveStats?.recent ?? [])].reverse()) {
    const d = document.createElement('div');
    d.className = 'g' + (lv.replayId === g.id ? ' sel' : '');
    d.innerHTML = `<span>#${g.id}</span><span class="r">${g.result === '1/2-1/2' ? '½–½' : g.result}</span><span class="x">${g.plies} · ${reasonText(g.reason)}</span>`;
    d.onclick = () => openReplay(g.id);
    list.appendChild(d);
  }
}
async function openReplay(id) {
  try {
    const gm = backend ? backend.getGame(id) : await (await fetch('/api/game/' + id)).json();
    if (!gm || gm.error) return;
    lv.replayId = id; lv.game = gm; lv.follow = false; loadGame(gm); lv.ply = 0; renderLive();
  } catch {}
}
function goLive() {
  lv.replayId = null; lv.follow = true;
  if (lv.liveBuf) { lv.game = lv.liveBuf; loadGame(lv.game); lv.ply = lv.sans.length; }
  renderLive();
}
const scrub = (fn) => () => { lv.follow = false; lv.ply = Math.max(0, Math.min(lv.sans.length, fn(lv.ply))); renderLive(); };
$('l-first').onclick = scrub(() => 0);
$('l-prev').onclick = scrub((p) => p - 1);
$('l-next').onclick = scrub((p) => p + 1);
$('l-last').onclick = scrub(() => lv.sans.length);
$('l-golive').onclick = goLive;
document.addEventListener('keydown', (e) => {
  if ($('view-live').classList.contains('hidden') || /SELECT|INPUT/.test(e.target.tagName)) return;
  if (e.key === 'ArrowLeft') $('l-prev').click(); else if (e.key === 'ArrowRight') $('l-next').click();
});

// ===================== 탭 / 언어 =====================
function showTab(name) {
  document.querySelectorAll('.tab').forEach((b) => b.classList.toggle('active', b.dataset.tab === name));
  $('view-play').classList.toggle('hidden', name !== 'play');
  $('view-live').classList.toggle('hidden', name !== 'live');
  const live = name === 'live';
  if (live !== wantLive) { wantLive = live; send({ type: live ? 'sub_live' : 'unsub_live' }); }
  history.replaceState(null, '', live ? '#live' : '#play');
  if (live) renderLive();
}
document.querySelectorAll('.tab').forEach((b) => { b.onclick = () => showTab(b.dataset.tab); });
$('lang').onclick = () => { lang = lang === 'ko' ? 'en' : 'ko'; try { localStorage.setItem('lang', lang); } catch {} applyLang(); };

applyLang();
connect();
showTab(location.hash === '#live' ? 'live' : 'play');
if (LOCAL) {
  $('ver').textContent = 'Lunaris 1.2.1 · NNUE · WASM';
  $('l-pgn').addEventListener('click', (e) => {
    e.preventDefault();
    const url = URL.createObjectURL(new Blob([backend.pgn()], { type: 'application/x-chess-pgn' }));
    const a = document.createElement('a'); a.href = url; a.download = 'lunaris-selfplay.pgn'; a.click();
    setTimeout(() => URL.revokeObjectURL(url), 2000);
  });
} else fetch('./api/stats').then((r) => r.json()).then((j) => { if (j.engine) $('ver').textContent = j.engine + ' · NNUE'; }).catch(() => {});
