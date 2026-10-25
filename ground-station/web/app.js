// Página do ESP B (docs/web.md, passo 4; contrato em docs/protocolo-websocket.md). Sem bibliotecas: o AP não tem
// internet. Recebe "hello", "tel" e "cmd" pelo WebSocket na porta 81 e envia {"cmd","arg","pin"}.
// As regras de exibição são as do TFT e do OLED: sem dado atual ("cur" falso) ou com a flag "valid" desligada, o
// valor vira "---", e o horizonte dá lugar à bandeira de falha (X vermelho com o motivo).
'use strict';

const VAL_ATT = 1, VAL_ATT_DEGRADED = 2, VAL_ALT = 4, VAL_HDG = 8, VAL_HDG_GYRO_ONLY = 16;
const ST_MPU_OK = 1, ST_MAG_OK = 2, ST_BARO_OK = 4, ST_CALIBRATING = 8;

const CMD_NAMES = {
  calibrate_gyro: 'calibrar giroscópio',
  calibrate_mag: 'calibrar magnetômetro',
  zero_attitude: 'zerar atitude',
  zero_altitude: 'zerar altitude',
  set_rate: 'mudar taxa',
  unknown: 'comando',
};
const STATE_NAMES = {
  sent: ['enviado', 'st-wait'],
  accepted: ['aceito, em curso', 'st-wait'],
  done: ['concluído', 'st-done'],
  rejected: ['recusado pelo A', 'st-bad'],
  no_response: ['sem resposta', 'st-bad'],
  no_completion: ['sem conclusão', 'st-bad'],
  busy: ['B ocupado', 'st-bad'],
  refused: ['recusado no B', 'st-bad'],
};
const LINK_NAMES = {
  ok: ['link OK', 'ok'],
  late: ['dado atrasado', 'warn'],
  lost: ['link perdido', 'bad'],
  no_signal: ['sem sinal', 'bad'],
};
const ALARM_NAMES = {
  none: ['sem alarme', 'none'],
  bank_caution: ['BANK ANGLE (bank > 35°)', 'caution'],
  attitude: ['ATTITUDE (atitude anormal)', 'warning'],
  att_invalid: ['ATITUDE INVÁLIDA', 'warning'],
  data_late: ['DADO ATRASADO', 'warning'],
  link_lost: ['LINK PERDIDO', 'warning'],
};

// O B manda "tel" a 30 Hz. Sem "tel" por TEL_STALE_MS, os dados viram indisponíveis mesmo com o socket aberto (a
// conexão pode morrer sem o onclose: Wi-Fi fora do alcance, B travado); sem "tel" por TEL_DEAD_MS, a página
// abandona o socket e reconecta
const TEL_STALE_MS = 1000, TEL_DEAD_MS = 5000;

const $ = (id) => document.getElementById(id);
let ws = null;
let last = null;            // última telemetria
let lastTelAt = 0;          // performance.now() da última "tel" (ou da abertura do socket)
let stale = false;          // socket aberto, mas sem "tel" há mais de TEL_STALE_MS
let decl = 0;               // declinação magnética (hello e tel)
let headingTrue = false;    // MAG ou VERD
let pinRequired = true;

// ---------- Armazenamento local (só conveniência: pode falhar em janela privada) ----------

function loadPref(key, fallback) {
  try { const v = sessionStorage.getItem(key); return v === null ? fallback : v; } catch (e) { return fallback; }
}
function savePref(key, value) {
  try { sessionStorage.setItem(key, value); } catch (e) { /* sem armazenamento: segue sem lembrar */ }
}

// ---------- WebSocket ----------

function connect() {
  // Já conectado ou conectando (um agendamento antigo de reconexão não abre um segundo socket), ou página
  // escondida (reconecta quando voltar a aparecer)
  if (ws || document.hidden) return;
  setBadge($('ws-state'), 'conectando', 'warn');
  ws = new WebSocket(`ws://${location.hostname || '192.168.4.1'}:81/`);
  ws.onopen = () => {
    setBadge($('ws-state'), 'conectado', 'ok');
    lastTelAt = performance.now();
    stale = false;
  };
  ws.onclose = dropSocket;
  ws.onerror = dropSocket;
  ws.onmessage = (ev) => {
    let m;
    try { m = JSON.parse(ev.data); } catch (e) { return; }
    if (m.type === 'hello') {
      decl = m.decl || 0;
      pinRequired = !!m.pin;
      $('pin-row').hidden = !pinRequired;
    } else if (m.type === 'tel') {
      last = m;
      lastTelAt = performance.now();
      if (stale) {
        stale = false;
        setBadge($('ws-state'), 'conectado', 'ok');
      }
      if (typeof m.decl === 'number') decl = m.decl;
      render();
    } else if (m.type === 'cmd') {
      logCommand(m);
    }
  };
}

// Abandona o socket atual (fechado, com erro ou mudo) e agenda a reconexão. Os handlers saem antes do close():
// um socket mudo pode demorar a disparar o onclose, e a reconexão não pode acontecer duas vezes
// Fecha o socket atual sem reconectar. Os handlers saem antes do close(): um socket mudo pode demorar a disparar
// o onclose
function closeSocket() {
  if (!ws) return false;
  const old = ws;
  ws = null;
  old.onopen = old.onclose = old.onerror = old.onmessage = null;
  try { old.close(); } catch (e) { /* já fechado */ }
  stale = false;
  last = null;
  return true;
}

// Abandona o socket atual (fechado, com erro ou mudo) e agenda a reconexão
function dropSocket() {
  if (!closeSocket()) return;
  setBadge($('ws-state'), 'desconectado', 'bad');
  render();
  setTimeout(connect, 2000);   // reconexão
}

function checkTelemetry() {
  if (!ws || ws.readyState !== WebSocket.OPEN) return;
  const quiet = performance.now() - lastTelAt;
  if (quiet > TEL_DEAD_MS) {
    dropSocket();
  } else if (quiet > TEL_STALE_MS && !stale) {
    stale = true;
    last = null;   // nada de atitude congelada: bandeira de falha e "---"
    setBadge($('ws-state'), 'sem telemetria', 'warn');
    render();
  }
}

function send(cmd, arg) {
  if (!ws || ws.readyState !== WebSocket.OPEN) {
    logCommand({ cmd, state: 'refused', reason: 'sem conexão com o ESP B' });
    return;
  }
  const msg = { cmd, pin: $('pin').value };
  if (arg !== undefined) msg.arg = arg;
  ws.send(JSON.stringify(msg));
}

// ---------- Painéis ----------

function setBadge(el, text, cls) {
  el.textContent = text;
  el.className = `badge ${cls}`;
}

function setVal(id, text, cls) {
  const el = $(id);
  el.textContent = text;
  el.className = cls || (text === '---' ? 'na' : '');
}

const fmt = (v, d, sign) => (typeof v === 'number' && isFinite(v))
  ? (sign && v >= 0 ? '+' : '') + (Math.abs(v) < 0.5 * Math.pow(10, -d) ? 0 : v).toFixed(d)
  : null;

function headingShown(h) {
  let x = headingTrue ? h + decl : h;
  x = ((x % 360) + 360) % 360;
  return Math.round(x) % 360;
}

function render() {
  const t = last;
  const cur = !!(t && t.cur);
  const valid = t ? t.valid || 0 : 0;
  const att = cur && (valid & VAL_ATT) && fmt(t.roll, 1) !== null && fmt(t.pitch, 1) !== null;

  setVal('v-roll', att ? `${fmt(t.roll, 1, true)}°` : '---');
  setVal('v-pitch', att ? `${fmt(t.pitch, 1, true)}°` : '---');
  const hdgOk = att && (valid & VAL_HDG) && fmt(t.hdg, 0) !== null;
  const gyroOnly = hdgOk && (valid & VAL_HDG_GYRO_ONLY);
  setVal('v-hdg', hdgOk ? `${String(headingShown(t.hdg)).padStart(3, '0')}°${gyroOnly ? ' GIRO' : ''}` : '---',
         gyroOnly ? 'warn' : null);
  const altOk = cur && (valid & VAL_ALT);
  setVal('v-alt', altOk && fmt(t.alt, 2) !== null ? `${fmt(t.alt, 2, true)} m` : '---');
  setVal('v-hpa', altOk && fmt(t.hpa, 1) !== null ? `${fmt(t.hpa, 1)} hPa` : '---');
  setVal('v-temp', altOk && fmt(t.temp, 1) !== null ? `${fmt(t.temp, 1)} °C` : '---');
  setVal('v-gz', att && fmt(t.gz, 1) !== null ? `${fmt(t.gz, 1, true)} °/s` : '---');

  const link = t ? t.link || {} : {};
  const ln = LINK_NAMES[link.state] || LINK_NAMES.no_signal;
  setBadge($('link-state'), t ? ln[0] : 'sem dados', t ? ln[1] : 'bad');
  setVal('l-rx', link.rx_hz !== undefined ? `${link.rx_hz} pacotes/s` : '---');
  setVal('l-lost', link.lost !== undefined ? `${link.lost} (${fmt(link.loss_pct, 2)} %)` : '---');
  setVal('l-drops', link.drops !== undefined ? String(link.drops) : '---');
  setVal('l-age', link.age_ms !== undefined ? `${link.age_ms} ms` : '---');
  setVal('l-delay', link.delay_ms !== undefined ? `${link.delay_ms} ms` : '---');
  if (t && t.status !== undefined) {
    const s = t.status;
    const ok = (bit) => (s & bit ? 'ok' : '--');
    setVal('l-sensors', `MPU ${ok(ST_MPU_OK)} · BARO ${ok(ST_BARO_OK)} · MAG ${ok(ST_MAG_OK)}`);
    setVal('l-cal', s & ST_CALIBRATING ? 'em curso' : '—');
  } else {
    setVal('l-sensors', '---');
    setVal('l-cal', '---');
  }

  const al = t ? t.alarm || {} : {};
  const an = ALARM_NAMES[al.kind] || ALARM_NAMES.none;
  const alarmEl = $('alarm');
  alarmEl.textContent = an[0] + (al.acked ? ' (reconhecido)' : '');
  alarmEl.className = `alarm ${an[1]}${al.acked ? ' acked' : ''}`;

  drawPfd(t, cur, valid, att, hdgOk, gyroOnly, al.kind);
}

// ---------- Horizonte (mesmos sinais do TFT: roll à direita gira o horizonte no sentido anti-horário) ----------

const canvas = $('pfd');
const ctx = canvas.getContext('2d');
const W = canvas.width, H = canvas.height, CX = W / 2, CY = H / 2;
const PPD = 8;                 // pixels por grau (±30° visíveis, como os 4 px/° do TFT em 240 px)
const ROLL_R = 200;            // raio da escala de roll
const SKY = '#0066cc', GROUND = '#8b4513', WHITE = '#ffffff', YELLOW = '#ffd400', RED = '#ff3030',
      AMBER = '#ffb000';

function drawPfd(t, cur, valid, att, hdgOk, gyroOnly, alarmKind) {
  ctx.setTransform(1, 0, 0, 1, 0, 0);
  if (!att) {
    drawFlag(!t ? (stale ? 'SEM TELEMETRIA' : 'SEM DADOS')
           : t.link && t.link.state === 'no_signal' ? 'SEM SINAL'
           : t.link && t.link.state === 'lost' ? 'LINK PERDIDO'
           : t.link && t.link.state === 'late' ? 'DADO ATRASADO'
           : 'ATT INVALIDA');
  } else {
    const roll = t.roll * Math.PI / 180;
    // Mundo: gira com −roll (o horizonte vai no sentido anti-horário com roll à direita) e desce com o pitch
    ctx.save();
    ctx.translate(CX, CY);
    ctx.rotate(-roll);
    ctx.translate(0, t.pitch * PPD);
    ctx.fillStyle = SKY;
    ctx.fillRect(-W * 2, -H * 4, W * 4, H * 4);
    ctx.fillStyle = GROUND;
    ctx.fillRect(-W * 2, 0, W * 4, H * 4);
    ctx.fillStyle = WHITE;
    ctx.fillRect(-W * 2, -1.5, W * 4, 3);   // horizonte
    // Escala de pitch: traço a cada 5°, número a cada 10°, até ±25° do pitch atual
    ctx.strokeStyle = WHITE;
    ctx.lineWidth = 3;
    ctx.font = 'bold 22px system-ui, sans-serif';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    const lo = Math.ceil((t.pitch - 25) / 5) * 5;
    for (let m = lo; m <= t.pitch + 25; m += 5) {
      if (m === 0 || m < -90 || m > 90) continue;
      const y = -m * PPD;
      const half = m % 10 === 0 ? 60 : 24;
      ctx.beginPath();
      ctx.moveTo(-half, y);
      ctx.lineTo(half, y);
      ctx.stroke();
      if (m % 10 === 0) {
        ctx.fillText(String(Math.abs(m)), -half - 24, y);
        ctx.fillText(String(Math.abs(m)), half + 24, y);
      }
    }
    ctx.restore();

    drawRollScale(roll);
    drawAircraft();
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    if (valid & VAL_ATT_DEGRADED) label('ATT DEGRADADA', CX, 104, 26, AMBER);
    if (alarmKind === 'attitude') label('ATTITUDE', CX, 360, 44, RED);
    else if (alarmKind === 'bank_caution') label('BANK ANGLE', CX, 360, 44, AMBER);
  }
  drawHeading(t, hdgOk, gyroOnly);
  ctx.fillStyle = cur ? '#3fb950' : RED;   // ponto do link
  ctx.beginPath();
  ctx.arc(16, 16, 8, 0, 2 * Math.PI);
  ctx.fill();
}

function label(text, x, y, size, color) {
  ctx.font = `bold ${size}px system-ui, sans-serif`;
  const w = ctx.measureText(text).width + 16;
  ctx.fillStyle = '#000';
  ctx.fillRect(x - w / 2, y - size * 0.65, w, size * 1.3);
  ctx.fillStyle = color;
  ctx.fillText(text, x, y);
}

function drawRollScale(roll) {
  ctx.strokeStyle = WHITE;
  ctx.lineWidth = 3;
  for (const a of [-60, -45, -30, -20, -10, 10, 20, 30, 45, 60]) {
    const r = a * Math.PI / 180, len = a % 30 === 0 ? 24 : 14;
    ctx.beginPath();
    ctx.moveTo(CX + ROLL_R * Math.sin(r), CY - ROLL_R * Math.cos(r));
    ctx.lineTo(CX + (ROLL_R + len) * Math.sin(r), CY - (ROLL_R + len) * Math.cos(r));
    ctx.stroke();
  }
  ctx.fillStyle = WHITE;   // índice do zero, fixo
  ctx.beginPath();
  ctx.moveTo(CX, CY - ROLL_R);
  ctx.lineTo(CX - 14, CY - ROLL_R - 22);
  ctx.lineTo(CX + 14, CY - ROLL_R - 22);
  ctx.fill();
  // Ponteiro do lado do céu: com roll à direita, vai para a esquerda da escala
  ctx.save();
  ctx.translate(CX, CY);
  ctx.rotate(-roll);
  ctx.fillStyle = YELLOW;
  ctx.beginPath();
  ctx.moveTo(0, -ROLL_R + 4);
  ctx.lineTo(-14, -ROLL_R + 28);
  ctx.lineTo(14, -ROLL_R + 28);
  ctx.fill();
  ctx.restore();
}

function drawAircraft() {
  const bar = (x, y, w, h) => {
    ctx.fillStyle = '#000';
    ctx.fillRect(x - 2, y - 2, w + 4, h + 4);
    ctx.fillStyle = YELLOW;
    ctx.fillRect(x, y, w, h);
  };
  bar(CX - 140, CY - 4, 90, 9);
  bar(CX + 50, CY - 4, 90, 9);
  bar(CX - 58, CY - 4, 9, 24);
  bar(CX + 49, CY - 4, 9, 24);
  bar(CX - 6, CY - 6, 13, 13);
}

function drawHeading(t, hdgOk, gyroOnly) {
  const x = CX, y = H - 34;
  ctx.fillStyle = '#000';
  ctx.fillRect(x - 66, y - 30, 132, 60);
  ctx.strokeStyle = WHITE;
  ctx.lineWidth = 2;
  ctx.strokeRect(x - 66, y - 30, 132, 60);
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  ctx.font = 'bold 40px system-ui, sans-serif';
  ctx.fillStyle = gyroOnly ? AMBER : WHITE;
  ctx.fillText(hdgOk ? String(headingShown(t.hdg)).padStart(3, '0') + (gyroOnly ? '~' : '') : '---', x, y + 2);
  ctx.font = '16px system-ui, sans-serif';
  ctx.textAlign = 'left';
  ctx.fillStyle = WHITE;
  ctx.fillText(headingTrue ? 'VERD' : 'MAG', x + 72, y - 18);
}

// Bandeira de falha: a atitude some, e um X vermelho com o motivo ocupa a tela
function drawFlag(reason) {
  ctx.fillStyle = '#000';
  ctx.fillRect(0, 0, W, H);
  ctx.strokeStyle = RED;
  ctx.lineWidth = 6;
  ctx.beginPath();
  ctx.moveTo(0, 0); ctx.lineTo(W, H);
  ctx.moveTo(W, 0); ctx.lineTo(0, H);
  ctx.stroke();
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  label('ATT', CX, CY - 30, 52, RED);
  label(reason, CX, CY + 30, 28, WHITE);
}

// ---------- Comandos ----------

const pending = {};   // seq → linha do registro (só as linhas que ainda aparecem)

function logCommand(m) {
  const log = $('cmd-log');
  const name = CMD_NAMES[m.cmd] || m.cmd;
  const st = STATE_NAMES[m.state] || [m.state, ''];
  const text = `${name}${m.cmd === 'set_rate' && m.arg ? ` (${m.arg} Hz)` : ''}`;
  const detail = st[0] + (m.reason ? `: ${m.reason}` : '') + (m.origin && m.origin !== 'página' ? ` · ${m.origin}` : '');
  let li = m.seq !== undefined ? pending[m.seq] : null;
  if (!li) {
    li = document.createElement('li');
    li.innerHTML = '<span class="c"></span><span class="s"></span>';
    log.prepend(li);
    if (m.seq !== undefined) {
      pending[m.seq] = li;
      li.dataset.seq = m.seq;
    }
    // Uma linha que sai do registro sai também do pending: um evento posterior do mesmo comando (ainda em curso)
    // cria uma linha nova no topo, em vez de escrever numa linha que já não aparece
    while (log.children.length > 10) {
      const old = log.lastChild;
      if (old.dataset.seq !== undefined) delete pending[old.dataset.seq];
      old.remove();
    }
  }
  li.querySelector('.c').textContent = text;
  const s = li.querySelector('.s');
  s.textContent = detail;
  s.className = `s ${st[1]}`;
}

function setupCommands() {
  for (const btn of document.querySelectorAll('button[data-cmd]')) {
    const original = btn.textContent;
    let timer = null;
    btn.addEventListener('click', () => {
      // Calibrações: confirmação em dois toques, sem diálogos do navegador
      if (btn.dataset.confirm && !btn.classList.contains('confirming')) {
        btn.classList.add('confirming');
        btn.textContent = btn.dataset.confirm;
        timer = setTimeout(() => { btn.classList.remove('confirming'); btn.textContent = original; }, 4000);
        return;
      }
      clearTimeout(timer);
      btn.classList.remove('confirming');
      btn.textContent = original;
      send(btn.dataset.cmd);
    });
  }
  $('rate-apply').addEventListener('click', () => send('set_rate', Number($('rate').value)));
  $('pin').value = loadPref('pin', '');
  $('pin').addEventListener('change', () => savePref('pin', $('pin').value));
  headingTrue = loadPref('hdgTrue', '0') === '1';
  $('hdg-ref').textContent = headingTrue ? 'VERD' : 'MAG';
  $('hdg-ref').addEventListener('click', () => {
    headingTrue = !headingTrue;
    $('hdg-ref').textContent = headingTrue ? 'VERD' : 'MAG';
    savePref('hdgTrue', headingTrue ? '1' : '0');
    render();
  });
}

setupCommands();
render();
connect();
setInterval(checkTelemetry, 250);

// Página saindo (recarregar, fechar, trocar de endereço): fecha o socket na hora. Sem isso, o navegador do celular
// às vezes deixa o socket da página antiga aberto por dezenas de segundos (bancada de 24/10/2026: 28 s), ocupando
// uma das vagas do WebSocket do B. Voltando do cache do navegador (persisted), reconecta
window.addEventListener('pagehide', closeSocket);
window.addEventListener('pageshow', (ev) => {
  if (ev.persisted) connect();
});

// Página escondida (tela apagada, outro app, outra aba): fecha o socket e só reconecta quando ela voltar. Uma aba
// esquecida ou um celular de tela apagada não ocupa vaga nem memória no B (com 8 celulares, as vagas e o heap são
// contados), e o B não precisa esperar WS_STALL_MS para derrubá-lo. Na volta, o B manda o hello e os comandos
// recentes
document.addEventListener('visibilitychange', () => {
  if (document.hidden) {
    if (closeSocket()) {
      setBadge($('ws-state'), 'pausado', 'warn');
      render();
    }
  } else {
    connect();
  }
});
