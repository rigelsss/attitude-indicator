"""Séries para os gráficos e números para o relatório (docs/cliente-python.md, passo 5). Lógica pura: recebe as linhas
dos CSV do cliente (dicts de texto, como o leitor devolve) e devolve listas de números; sem matplotlib.

Regras dos buracos (NaN, para a linha não fingir um dado que não existe):
  - campo vazio no CSV;
  - dado não atual (cur = 0): roll, pitch, proa e altitude;
  - flag de validade desligada: VAL_ATT (roll e pitch), VAL_HDG (proa), VAL_ALT (altitude);
  - mais de LACUNA_S entre duas telemetrias, ou troca de sessão: um buraco entre os dois pontos.
O tempo de tudo é o carimbo de chegada no PC (pc_ns, relógio monotônico), o mesmo nos dois CSV.
"""

import math
from collections import deque

from .metricas import dif_seq16, percentil

NAN = float("nan")
VAL_ATT, VAL_ALT, VAL_HDG = 1, 4, 8   # telemetry.h (ValidFlags)
LACUNA_S = 1.0                        # o mesmo 1 s de "sem telemetria"
SEQ_REINICIO = 30000                  # salto de seq acima disso: o A reiniciou (não é perda)

# Orçamento de latência: as partes que não são medidas pelo cliente (docs/radio.md, docs/web.md)
ESPERA_RADIO_MS = 20.0   # a radio_task envia a 50 Hz a última cópia do estado: espera de 0 a 20 ms
ESPERA_WS_MS = 35.0      # a web_task monta a telemetria a cada ~35 ms (medido em 25/10/2026): espera de 0 a 35 ms


def num(texto):
    """Texto do CSV → float; vazio ou inválido → NaN."""
    if texto is None or texto == "":
        return NAN
    try:
        return float(texto)
    except ValueError:
        return NAN


def _inteiro(texto, padrao=0):
    v = num(texto)
    return padrao if math.isnan(v) else int(v)


def ponto_tel(linha):
    """Uma linha do CSV de telemetria → o ponto que os gráficos usam, com os buracos já aplicados."""
    cur = linha.get("cur") == "1"
    valid = _inteiro(linha.get("valid"))
    atitude_ok = cur and bool(valid & VAL_ATT)
    return {
        "t": int(linha["pc_ns"]) / 1e9,
        "unix": num(linha.get("pc_unix")),
        "sessao": linha.get("sessao", ""),
        "n": _inteiro(linha.get("n"), None),
        "cur": cur,
        "roll": num(linha.get("roll")) if atitude_ok else NAN,
        "pitch": num(linha.get("pitch")) if atitude_ok else NAN,
        "hdg": num(linha.get("hdg")) if cur and valid & VAL_HDG else NAN,
        "alt": num(linha.get("alt")) if cur and valid & VAL_ALT else NAN,
        "seq": _inteiro(linha.get("seq"), None),
        "t_a": _inteiro(linha.get("t"), None),     # millis() do A no pacote: volta = A reiniciado
        "tb": _inteiro(linha.get("tb"), None),     # millis() do B na montagem da telemetria
        "lost": _inteiro(linha.get("lost"), None),
        "loss_pct": num(linha.get("loss_pct")),
        "delay_ms": num(linha.get("delay_ms")),
        "atraso_bpc_ms": num(linha.get("atraso_bpc_ms")),
        "loop_us": num(linha.get("loop_us")),
        "link": linha.get("link_state", ""),
        "puladas": _inteiro(linha.get("puladas_ws")),
    }


def pontos_rtt(eventos):
    """Os eventos "rtt" do CSV de eventos → [(t, ms)]."""
    saida = []
    for e in eventos:
        if e.get("tipo") == "rtt":
            ms = num(e.get("detalhe"))
            if not math.isnan(ms):
                saida.append((int(e["pc_ns"]) / 1e9, ms))
    return saida


def com_buracos(pontos, campo, origem=0.0, lacuna_s=LACUNA_S):
    """xs (t − origem, em s) e ys de um campo, com um NaN entre dois pontos separados por mais de lacuna_s ou de
    sessões diferentes."""
    xs, ys = [], []
    anterior = None
    for p in pontos:
        if anterior is not None and (p["t"] - anterior["t"] > lacuna_s or p["sessao"] != anterior["sessao"]):
            xs.append((anterior["t"] + p["t"]) / 2 - origem)
            ys.append(NAN)
        xs.append(p["t"] - origem)
        ys.append(p[campo])
        anterior = p
    return xs, ys


def por_segundo(pontos, origem=0.0):
    """Telemetrias recebidas em cada segundo inteiro (t − origem), com 0 nos segundos sem nenhuma."""
    if not pontos:
        return [], []
    contagem = {}
    for p in pontos:
        s = math.floor(p["t"] - origem)
        contagem[s] = contagem.get(s, 0) + 1
    inicio, fim = min(contagem), max(contagem)
    xs = list(range(inicio, fim + 1))
    return xs, [contagem.get(s, 0) for s in xs]


def perdas_por_intervalo(pontos, intervalo_s=1.0, origem=0.0):
    """Pacotes A → B perdidos em cada intervalo, pelo "lost" acumulado do B. Um "lost" que volta (B reiniciado)
    recomeça a conta sem perda negativa. Devolve (xs do início de cada intervalo, perdidos)."""
    caixas = {}
    anterior = None
    for p in pontos:
        if p["lost"] is None:
            continue
        if anterior is not None:
            d = p["lost"] - anterior
            if d > 0:
                k = math.floor((p["t"] - origem) / intervalo_s)
                caixas[k] = caixas.get(k, 0) + d
        anterior = p["lost"]
    if not caixas:
        return [], []
    validos = [p for p in pontos if p["lost"] is not None]
    k0 = math.floor((validos[0]["t"] - origem) / intervalo_s)
    k1 = math.floor((validos[-1]["t"] - origem) / intervalo_s)
    xs = [k * intervalo_s for k in range(k0, k1 + 1)]
    return xs, [caixas.get(k, 0) for k in range(k0, k1 + 1)]


def perdas_por_minuto(pontos):
    """[(minuto desde o primeiro ponto, perdidos, enviados pelo A, %)]. Os enviados vêm do avanço do seq (com a
    volta de 16 bits). Um A reiniciado não conta: o carimbo t do A voltou (revisão de 27/10/2026: só o limiar do seq
    não bastava, porque 50000 → 5 dá 15 541 pela volta e passava por enviados), ou o salto passa de SEQ_REINICIO."""
    com_a = [p for p in pontos if p["seq"] is not None and p["lost"] is not None]
    if not com_a:
        return []
    t0 = com_a[0]["t"]
    minutos = {}
    for a, b in zip(com_a, com_a[1:]):
        m = int((b["t"] - t0) // 60)
        perd, env = minutos.get(m, (0, 0))
        d_seq = dif_seq16(b["seq"], a["seq"])
        d_lost = b["lost"] - a["lost"]
        reiniciou = a["t_a"] is not None and b["t_a"] is not None and b["t_a"] < a["t_a"]
        if d_seq < SEQ_REINICIO and not reiniciou:
            env += d_seq
        if d_lost > 0:
            perd += d_lost
        minutos[m] = (perd, env)
    return [(m, perd, env, 100.0 * perd / env if env else NAN) for m, (perd, env) in sorted(minutos.items())]


def intervalos_ms(pontos):
    """Intervalos entre chegadas no PC, dentro de cada sessão (ms)."""
    saida = []
    for a, b in zip(pontos, pontos[1:]):
        if a["sessao"] == b["sessao"]:
            saida.append((b["t"] - a["t"]) * 1000.0)
    return saida


def estatisticas(valores):
    """n, mín, mediana, p95, p99 e máx, ignorando NaN. None sem valores."""
    v = sorted(x for x in valores if not math.isnan(x))
    if not v:
        return None
    return {"n": len(v), "min": v[0], "mediana": percentil(v, 50), "p95": percentil(v, 95),
            "p99": percentil(v, 99), "max": v[-1]}


def sessoes(eventos):
    """As conexões do CSV de eventos: [{"sessao", "inicio", "fim", "motivo"}] (fim None se não terminou)."""
    saida = []
    atual = None
    for e in eventos:
        t = int(e["pc_ns"]) / 1e9
        if e.get("tipo") == "conectado":
            atual = {"sessao": e.get("sessao"), "inicio": t, "fim": None, "motivo": None}
            saida.append(atual)
        elif e.get("tipo") == "desconectado" and atual is not None and atual["fim"] is None:
            atual["fim"] = t
            atual["motivo"] = e.get("detalhe")
    return saida


def periodos_sem_telemetria(eventos):
    """Os trechos com a conexão aberta e a telemetria parada: [(início, fim)], pelo estado "sem_telemetria" até o
    "ok" seguinte (ou até a desconexão)."""
    saida = []
    inicio = None
    for e in eventos:
        t = int(e["pc_ns"]) / 1e9
        tipo, detalhe = e.get("tipo"), e.get("detalhe")
        if tipo == "telemetria" and detalhe == "sem_telemetria" and inicio is None:
            inicio = t
        elif inicio is not None and (tipo == "desconectado" or (tipo == "telemetria" and detalhe == "ok")):
            saida.append((inicio, t))
            inicio = None
    return saida


def periodo_ws_ms(pontos):
    """O período da telemetria do WebSocket, medido no relógio do B: a mediana do avanço do tb entre telemetrias
    consecutivas (n + 1, mesma sessão). ~35 ms a 30 Hz, ~50 ms a 20 Hz. None sem pares."""
    difs = sorted(b["tb"] - a["tb"] for a, b in zip(pontos, pontos[1:])
                  if a["sessao"] == b["sessao"] and a["n"] is not None and b["n"] == a["n"] + 1
                  and a["tb"] is not None and b["tb"] is not None and b["tb"] >= a["tb"])
    return percentil(difs, 50) if difs else None


def orcamento_latencia(loop_us, delay_ms, atraso_bpc_ms, rtt_ms, periodo_ws=None):
    """Partes da latência do dado, da amostra no A até o PC: [(parte, mediana ms, p99 ms, como)]. As medidas vêm das
    estatísticas; as esperas, dos períodos das tasks (média = metade do período). Os atrasos A → B e B → PC são
    relativos ao menor valor visto (relógios sem sincronia); o RTT/2 dá a ordem da base B → PC. periodo_ws: o período
    da web_task medido na gravação (periodo_ws_ms); sem ele, ESPERA_WS_MS (30 Hz), e o texto diz que é a hipótese."""
    def med(st, escala=1.0):
        return (st["mediana"] * escala, st["p99"] * escala) if st else (NAN, NAN)
    partes = [
        ("fusão no A (pulso do INT → atitude pronta)", *med(loop_us, 1e-3), "medida (loop_us)"),
        ("espera da radio_task (envio a 50 Hz)", ESPERA_RADIO_MS / 2, ESPERA_RADIO_MS, "estimada (0 a 20 ms)"),
        ("ar e recepção A → B, acima do mínimo", *med(delay_ms), "medida (delay_ms, relativa)"),
        (f"espera da web_task (telemetria a cada {periodo_ws or ESPERA_WS_MS:.0f} ms)", (periodo_ws or ESPERA_WS_MS) / 2,
         periodo_ws or ESPERA_WS_MS,
         f"estimada (0 a {periodo_ws:.0f} ms, período medido pelo tb)" if periodo_ws
         else f"estimada (0 a {ESPERA_WS_MS:.0f} ms, supondo 30 Hz: sem tb na gravação)"),
        ("rede B → PC, acima do mínimo", *med(atraso_bpc_ms), "medida (tb, relativa)"),
        ("base da rede B → PC (RTT/2)", *(x / 2 for x in med(rtt_ms)), "medida (ping)"),
    ]
    return partes


class Janela:
    """Os últimos `segundos` de pontos, para os gráficos ao vivo."""

    def __init__(self, segundos=30.0):
        self.segundos = segundos
        self.pontos = deque()
        self.rtt = deque()

    def limpar(self):
        self.pontos.clear()
        self.rtt.clear()

    def acrescentar(self, pontos=(), rtt=()):
        self.pontos.extend(pontos)
        self.rtt.extend(rtt)

    def cortar(self, agora):
        limite = agora - self.segundos
        while self.pontos and self.pontos[0]["t"] < limite:
            self.pontos.popleft()
        while self.rtt and self.rtt[0][0] < limite:
            self.rtt.popleft()
