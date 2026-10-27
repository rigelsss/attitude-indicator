"""Leitura do log do monitor do ESP B (firmware/ground/logs/device-monitor-*.log) para a bancada 2 da semana 6
(docs/cliente-python.md). Lógica pura: recebe as linhas do log e devolve listas e números; sem matplotlib.

O que sai do log (os tempos em ms do B, o millis() dele):
  - resumos de 1 s ("[r]"): estado do link, recebidos, perdidos, heap e maior bloco;
  - aparelhos no AP: entradas e saídas por MAC, e o IP entregue pelo DHCP logo depois de cada entrada;
  - WebSockets: conectado (IP), desconectado, derrubado (motivo), por número de cliente;
  - pausas da trava de memória (início e duração);
  - o último bloco do "c": a taxa da telemetria e uma linha por cliente do WebSocket.

Reinícios do B (o relógio volta a zero; "rst:" na serial): os instantes seguem contínuos, somando o último instante
de antes do reinício, e cada reinício e cada corrupção do heap ficam registrados (investigação do travamento de
26/10/2026).

O "[r]" ganhou o instante em 26/10/2026 ("[r] 123456 ms: link ..."). Nos logs anteriores, sem ele, o instante de
cada resumo é estimado pela soma das janelas desde o último evento com ms (erro de até ~1 s).
"""

import re
from dataclasses import dataclass, field

RE_R = re.compile(r"^\[r\] (?:(\d+) ms: )?link (.+?) \| rec (\d+) em (\d+) ms \| perdidos (\d+)")
RE_HEAP = re.compile(r"\| heap (\d+) kB \(bloco (\d+) kB\)")
RE_EVENTO = re.compile(r"^\[(\w+)\] (\d+) ms: (.*)")
RE_AP = re.compile(r"celular (\S+) (entrou|saiu) \| no AP agora: (\d+)")
RE_IP = re.compile(r"IP (\S+) entregue pelo DHCP")
RE_WS = re.compile(r"cliente (\d+) (conectado \((\S+)\)|desconectado|derrubado: (.*))")
RE_PAUSA = re.compile(r"telemetria retomada: .*pausada por (\d+) ms")
RE_TAXA = re.compile(r"^página: .*telemetria (\d+) Hz")
RE_CLIENTE = re.compile(r"^ws cliente (\d+): (\S+) \| telemetrias enviadas (\d+), puladas (\d+) .*\(máx (\d+),")
RE_CARIMBO = re.compile(r"compilado em (\S+ \S+)")
RE_REINICIO = re.compile(r"^rst:0x[0-9a-f]+ \((\w+)\)")
RE_CORRUPCAO = re.compile(r"CORRUPT HEAP: (.*)|\[heap\] (\d+) ms: CORRUPÇÃO")
JANELA_SAIDA_ANTES_MS = 3000    # o consumo de memória começa ~3 s antes da saída do AP (o aparelho fora do canal)
JANELA_SAIDA_DEPOIS_MS = 2000


@dataclass
class LogB:
    resumos: list = field(default_factory=list)    # dicts: ms, estimado, estado, rec, janela_ms, perdidos, heap, bloco
    ap: list = field(default_factory=list)         # (ms, mac, "entrou"/"saiu", no_ap)
    ips: list = field(default_factory=list)        # (ms, ip)
    ws: list = field(default_factory=list)         # (ms, cliente, "conectado"/"desconectado"/"derrubado", ip/motivo)
    pausas: list = field(default_factory=list)     # (início ms, duração ms)
    link: list = field(default_factory=list)       # (ms, texto)
    taxa_hz: int = None                            # do último "c" (logs a partir de 26/10/2026)
    clientes_c: dict = field(default_factory=dict)  # IP → (enviadas, puladas, máx. sem confirmação), do último "c"
    carimbo: str = None
    reinicios: list = field(default_factory=list)   # (instante contínuo do último dado antes, motivo do rst)
    corrupcoes: list = field(default_factory=list)  # (instante contínuo, texto): CORRUPT HEAP e a verificação do B


def ler(linhas):
    """Linhas do log do B → LogB."""
    log = LogB()
    ancora = None   # (ms do último evento, soma das janelas dos resumos sem ms depois dele)
    base = 0        # o último instante contínuo antes de um reinício: o tempo segue contínuo depois dele
    ultimo = 0      # último instante contínuo visto
    for bruta in linhas:
        linha = bruta.rstrip("\r\n")
        m = RE_REINICIO.search(linha)
        if m:
            log.reinicios.append((ultimo, m.group(1)))
            base, ancora = ultimo, None
            continue
        c = RE_CORRUPCAO.search(linha)
        if c:   # o texto a partir da marca (às vezes ela cai no meio de um resumo)
            # A verificação do B traz o próprio instante ("[heap] T ms: ..."); o CORRUPT HEAP do núcleo não, e fica
            # com o último instante visto (revisão de 27/10/2026: antes, os dois ficavam com o último instante)
            quando = base + int(c.group(2)) if c.group(2) else ultimo
            log.corrupcoes.append((quando, linha[c.start():].strip()[:120]))
        m = RE_R.match(linha)
        if m:
            janela = int(m.group(4))
            if m.group(1) is not None:
                ms, estimado = base + int(m.group(1)), False
                ancora = (ms, 0)
            elif ancora is not None:
                ms, estimado = ancora[0] + ancora[1] + janela, True
                ancora = (ancora[0], ancora[1] + janela)
            else:
                continue   # resumo sem ms antes de qualquer evento: sem como situar no tempo
            ultimo = max(ultimo, ms)
            h = RE_HEAP.search(linha)
            log.resumos.append({"ms": ms, "estimado": estimado, "estado": m.group(2), "rec": int(m.group(3)),
                                "janela_ms": janela, "perdidos": int(m.group(5)),
                                "heap": int(h.group(1)) if h else None, "bloco": int(h.group(2)) if h else None})
            continue
        m = RE_EVENTO.match(linha)
        if m:
            tipo, ms, texto = m.group(1), base + int(m.group(2)), m.group(3)
            ancora = (ms, 0)
            ultimo = max(ultimo, ms)
            if tipo == "ap":
                a = RE_AP.search(texto)
                if a:
                    log.ap.append((ms, a.group(1), a.group(2), int(a.group(3))))
                i = RE_IP.search(texto)
                if i:
                    log.ips.append((ms, i.group(1)))
            elif tipo == "ws":
                w = RE_WS.search(texto)
                if w:
                    if w.group(3):
                        log.ws.append((ms, int(w.group(1)), "conectado", w.group(3)))
                    elif w.group(4) is not None:
                        log.ws.append((ms, int(w.group(1)), "derrubado", w.group(4)))
                    else:
                        log.ws.append((ms, int(w.group(1)), "desconectado", None))
            elif tipo == "web":
                p = RE_PAUSA.search(texto)
                if p:
                    log.pausas.append((ms - int(p.group(1)), int(p.group(1))))
            elif tipo == "link":
                log.link.append((ms, texto))
            continue
        m = RE_TAXA.match(linha)
        if m:
            log.taxa_hz = int(m.group(1))
        m = RE_CLIENTE.match(linha)
        if m:
            log.clientes_c[m.group(2)] = (int(m.group(3)), int(m.group(4)), int(m.group(5)))
        m = RE_CARIMBO.search(linha)
        if m and log.carimbo is None:
            log.carimbo = m.group(1)
    return log


def mac_para_ip(log):
    """MAC → IP: o IP entregue pelo DHCP até 1,5 s depois de uma entrada no AP."""
    mapa = {}
    for ms, mac, acao, _ in log.ap:
        if acao != "entrou":
            continue
        for ms_ip, ip in log.ips:
            if 0 <= ms_ip - ms <= 1500:
                mapa[mac] = ip
                break
    return mapa


def saidas(log):
    """As saídas do AP com a volta: [{"mac", "saiu", "voltou" (None se não voltou), "duracao"}]."""
    pendentes, saida = {}, []
    for ms, mac, acao, _ in log.ap:
        if acao == "saiu":
            pendentes[mac] = {"mac": mac, "saiu": ms, "voltou": None, "duracao": None}
            saida.append(pendentes[mac])
        elif mac in pendentes:
            s = pendentes.pop(mac)
            s["voltou"] = ms
            s["duracao"] = ms - s["saiu"]
    return saida


def _sobreposicao(a0, a1, b0, b1):
    return max(0, min(a1, b1) - max(a0, b0))


def efeito_das_saidas(log, fim_ms=None):
    """Para cada saída do AP: a pausa da telemetria para todos e as perdas A → B na janela da saída (de 3 s antes da
    saída até 2 s depois da volta), e as derrubadas de clientes de outros IPs nessa janela."""
    fim_ms = fim_ms if fim_ms is not None else (log.resumos[-1]["ms"] if log.resumos else 0)
    ips = mac_para_ip(log)
    saida = []
    for s in saidas(log):
        ip_cliente = {}   # o B reaproveita os números de cliente: o mapa é refeito em ordem a cada saída
        j0 = s["saiu"] - JANELA_SAIDA_ANTES_MS
        j1 = (s["voltou"] if s["voltou"] is not None else fim_ms) + JANELA_SAIDA_DEPOIS_MS
        pausa = sum(_sobreposicao(j0, j1, i, i + d) for i, d in log.pausas)
        perdidos = sum(r["perdidos"] for r in log.resumos if j0 <= r["ms"] <= j1)
        derrubados_outros = []
        for ms, cliente, acao, info in log.ws:
            if acao == "conectado":
                ip_cliente[cliente] = info
            elif acao == "derrubado" and j0 <= ms <= j1 and ip_cliente.get(cliente) != ips.get(s["mac"]):
                derrubados_outros.append(ip_cliente.get(cliente, "?"))
        saida.append(dict(s, ip=ips.get(s["mac"]), janela_ms=j1 - j0, pausa_ms=pausa, perdidos=perdidos,
                          derrubados_outros=derrubados_outros))
    return saida


def perdas_link(log):
    """Perdas A → B nos resumos com o link ativo: (perdidos, recebidos, %)."""
    ok = [r for r in log.resumos if r["estado"] in ("OK", "ok")]
    perd = sum(r["perdidos"] for r in ok)
    rec = sum(r["rec"] for r in ok)
    return perd, rec, (100.0 * perd / (perd + rec) if perd + rec else float("nan"))


def derrubadas_trava(log):
    """WebSockets derrubados pela trava de memória (o culpado, em vez de pausar todos; firmware de 26/10/2026)."""
    return sum(1 for _ms, _c, acao, motivo in log.ws if acao == "derrubado" and motivo.startswith("trava de memória"))


def clientes_ws(log):
    """Por IP: conexões, desconexões e derrubadas do WebSocket."""
    ip_cliente, por_ip = {}, {}
    for _ms, cliente, acao, info in log.ws:
        if acao == "conectado":
            ip_cliente[cliente] = info
        ip = info if acao == "conectado" else ip_cliente.get(cliente, "?")
        d = por_ip.setdefault(ip, {"conectado": 0, "desconectado": 0, "derrubado": 0})
        d[acao] += 1
    return por_ip
