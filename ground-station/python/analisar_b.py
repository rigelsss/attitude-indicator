"""Figuras e resumo do log do monitor do ESP B, para a bancada 2 da semana 6 (docs/cliente-python.md): perdas no rádio
A → B, memória e trava de memória, aparelhos entrando e saindo do AP, WebSockets derrubados e o efeito de cada saída
nos outros. Os celulares não gravam log: é o log do B que mostra o lado deles.

Uso (a partir de ground-station/python):
  python analisar_b.py ..\\..\\firmware\\ground\\logs\\device-monitor-261026-150000.log
  python analisar_b.py <log a 30 Hz> <log a 20 Hz>      # cada um analisado, mais a comparação lado a lado
Cria, ao lado de cada log, analise-<nome do log>\\ (ou -2, -3...) com perdas_ab.png, memoria.png, aparelhos.png e
resumo.md; com mais de um log, também comparacao-<nome do primeiro>.md. Não abre janela.
"""

import argparse
import math
import os
import statistics
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.patches import Patch  # noqa: E402

from analisar import br, pasta_unica  # noqa: E402
from horizonte import figuras as F  # noqa: E402
from horizonte import log_b as B  # noqa: E402

FIGURAS = ("perdas_ab.png", "memoria.png", "aparelhos.png")
PAUSA_KB, RETOMA_KB = 32, 40   # WEB_HEAP_PAUSE_KB e WEB_HEAP_RESUME_KB (firmware/ground/include/config.h)


def _min(ms, t0):
    return (ms - t0) / 60000.0


def fig_perdas(log, t0):
    fig, eixos = plt.subplots(1, 1, figsize=(10, 3.4), squeeze=False)
    F.estilo()
    ax = eixos[0][0]
    F._eixo(ax, "Perdas no rádio A → B, por resumo de 1 s", "pacotes perdidos")
    for s in B.saidas(log):
        fim = s["voltou"] if s["voltou"] is not None else log.resumos[-1]["ms"]
        ax.axvspan(_min(s["saiu"], t0), _min(max(fim, s["saiu"] + 500), t0), color=F.GRADE, linewidth=0)
    for t, _motivo in log.reinicios:   # reinício do B: linha vertical com o rótulo
        ax.axvline(_min(t, t0), color=F.TINTA, linewidth=1.0)
        ax.annotate("reinício do B", (_min(t, t0), 1), xycoords=("data", "axes fraction"), xytext=(3, -10),
                    textcoords="offset points", fontsize=8, color=F.TINTA)
    xs = [_min(r["ms"], t0) for r in log.resumos]
    ax.plot(xs, [r["perdidos"] for r in log.resumos], color=F.SERIE[0], drawstyle="steps-post")
    ax.set_xlabel("tempo (min, relógio do B)")
    ax.set_ylim(bottom=0)
    ax.legend(handles=[Patch(color=F.GRADE, label="algum aparelho fora do AP")], loc="upper right")
    return fig


def fig_memoria(log, t0):
    F.estilo()
    fig, eixos = plt.subplots(1, 1, figsize=(10, 3.4), squeeze=False)
    ax = eixos[0][0]
    F._eixo(ax, "Memória livre do B (heap)", "kB")
    for inicio, dur in log.pausas:
        ax.axvspan(_min(inicio, t0), _min(inicio + max(dur, 300), t0), color=F.AVISO, linewidth=0)
    com_heap = [r for r in log.resumos if r["heap"] is not None]
    ax.plot([_min(r["ms"], t0) for r in com_heap], [r["heap"] for r in com_heap], color=F.SERIE[0])
    for kb, texto in ((PAUSA_KB, f"pausa abaixo de {PAUSA_KB} kB"), (RETOMA_KB, f"retoma acima de {RETOMA_KB} kB")):
        ax.axhline(kb, color=F.TINTA_2, linewidth=0.8)
        ax.annotate(texto, (1, kb), xycoords=("axes fraction", "data"), xytext=(-3, 3), textcoords="offset points",
                    ha="right", fontsize=8, color=F.TINTA_2,
                    bbox=dict(facecolor=F.SUPERFICIE, edgecolor="none", pad=1))
    ax.set_xlabel("tempo (min, relógio do B)")
    ax.set_ylim(bottom=0)
    ax.legend(handles=[Patch(color=F.AVISO, label="telemetria pausada para todos")], loc="lower left")
    return fig


def fig_aparelhos(log, t0, t_fim):
    F.estilo()
    macs = []
    for _ms, mac, _a, _n in log.ap:
        if mac not in macs:
            macs.append(mac)
    fig, eixos = plt.subplots(1, 1, figsize=(10, 1.2 + 0.45 * max(1, len(macs))), squeeze=False)
    ax = eixos[0][0]
    F._eixo(ax, "Aparelhos no AP do B", None)
    ips = B.mac_para_ip(log)
    for i, mac in enumerate(macs):
        dentro = None
        eventos = [(ms, a) for ms, m, a, _ in log.ap if m == mac]
        if eventos and eventos[0][1] == "saiu":   # já estava no AP antes do log começar
            dentro = t0
        for ms, acao in eventos:
            if acao == "entrou" and dentro is None:
                dentro = ms
            elif acao == "saiu" and dentro is not None:
                ax.broken_barh([(_min(dentro, t0), max(_min(ms, t0) - _min(dentro, t0), 0.005))], (i - 0.35, 0.7),
                               facecolor=F.SERIE[0])
                dentro = None
        if dentro is not None:
            ax.broken_barh([(_min(dentro, t0), _min(t_fim, t0) - _min(dentro, t0))], (i - 0.35, 0.7),
                           facecolor=F.SERIE[0])
    ax.set_yticks(range(len(macs)), [f"{m} ({ips.get(m, 'IP?')})" for m in macs])
    ax.set_ylim(-0.6, len(macs) - 0.4)
    ax.grid(axis="y", visible=False)
    ax.set_xlabel("tempo (min, relógio do B); os vazios são o aparelho fora do AP")
    return fig


def resumo(log, nome):
    """Os números do log, num dict (para o resumo.md e a comparação)."""
    t0 = log.resumos[0]["ms"] if log.resumos else 0
    t_fim = log.resumos[-1]["ms"] if log.resumos else 0
    perd, rec, pct = B.perdas_link(log)
    heaps = [r["heap"] for r in log.resumos if r["heap"] is not None]
    efeitos = B.efeito_das_saidas(log)
    longas = [e for e in efeitos if e["duracao"] is None or e["duracao"] > 1000]
    return {
        "nome": nome, "carimbo": log.carimbo, "taxa_hz": log.taxa_hz, "t0": t0, "t_fim": t_fim,
        "duracao_min": (t_fim - t0) / 60000.0, "estimado": any(r["estimado"] for r in log.resumos),
        "reinicios": log.reinicios, "corrupcoes": log.corrupcoes,
        "aparelhos": len({mac for _m, mac, _a, _n in log.ap}), "perdidos": perd, "recebidos": rec, "perdas_pct": pct,
        "heap_min": min(heaps) if heaps else None, "heap_mediana": statistics.median(heaps) if heaps else None,
        "pausas_n": len(log.pausas), "pausas_s": sum(d for _i, d in log.pausas) / 1000.0,
        "pausa_max_ms": max((d for _i, d in log.pausas), default=0), "saidas": efeitos, "saidas_longas": longas,
        "clientes": B.clientes_ws(log), "clientes_c": log.clientes_c,
        "derrubados": sum(d["derrubado"] for d in B.clientes_ws(log).values()),
        "derrubados_trava": B.derrubadas_trava(log),
    }


def escrever_resumo(r, pasta):
    t0 = r["t0"]
    L = [f"# Análise do log do B {r['nome']}", "",
         f"- Firmware: compilado em {r['carimbo'] or '— (o log não pegou o boot; tecle c)'}; telemetria no WebSocket: "
         f"{str(r['taxa_hz']) + ' Hz' if r['taxa_hz'] else '— (sem c no log)'}",
         f"- Duração: {br(r['duracao_min'], 1)} min (relógio do B); aparelhos que entraram ou saíram do AP durante o log: {r['aparelhos']}",
         (f"- **Reinícios do B: {len(r['reinicios'])}** ("
          + "; ".join(f"{br(_min(t, t0), 2)} min, {m}" for t, m in r["reinicios"]) + ")"
          if r["reinicios"] else "- Reinícios do B: nenhum"),
         (f"- **Corrupções do heap: {len(r['corrupcoes'])}** ("
          + "; ".join(f"{br(_min(t, t0), 2)} min: `{x}`" for t, x in r["corrupcoes"]) + ")"
          if r["corrupcoes"] else "- Corrupções do heap: nenhuma"),
         ("- Os resumos `[r]` deste log não têm o instante (firmware anterior a 26/10/2026): o tempo de cada um foi "
          "estimado pela soma das janelas, com erro de até ~1 s." if r["estimado"] else ""), "",
         "## Rádio A → B", "",
         f"- {br(r['perdidos'])} perdidos em {br(r['perdidos'] + r['recebidos'])} pacotes com o link ativo "
         f"(**{br(r['perdas_pct'], 2)} %**).", "",
         "## Memória do B", "",
         f"- Heap: mínimo {br(r['heap_min'])} kB, mediana {br(r['heap_mediana'])} kB.",
         f"- Trava de memória: **{br(r['pausas_n'])} pausas da telemetria para todos**, {br(r['pausas_s'], 1)} s no "
         f"total, a maior de {br(r['pausa_max_ms'])} ms; **{br(r['derrubados_trava'])} clientes derrubados pela "
         f"trava** (o culpado parado, em vez de pausar todos).", "",
         "## WebSocket por IP", "",
         "| IP | conexões | desconexões | derrubadas | telemetrias enviadas* | puladas* | máx. sem confirmação* (B) |",
         "|---|---|---|---|---|---|---|"]
    for ip, d in sorted(r["clientes"].items()):
        c = r["clientes_c"].get(ip)
        extra = f"{br(c[0])} | {br(c[1])} | {br(c[2])}" if c else "— | — | —"
        L.append(f"| {ip} | {d['conectado']} | {d['desconectado']} | {d['derrubado']} | {extra} |")
    L += ["", "\\* do último `c` do log, só da conexão aberta naquele momento.", "",
          "## Saídas do AP e o efeito nos outros", "",
          f"Janela de cada saída: de {B.JANELA_SAIDA_ANTES_MS // 1000} s antes da saída até "
          f"{B.JANELA_SAIDA_DEPOIS_MS // 1000} s depois da volta (o consumo de memória começa antes da saída, com o "
          "aparelho ainda associado e fora do canal).", "",
          f"- Saídas: {len(r['saidas'])}, das quais {len(r['saidas_longas'])} com mais de 1 s fora.", ""]
    if r["saidas"]:
        L += ["| min | aparelho (IP) | fora (s) | pausa para todos (ms) | perdidos A → B na janela | outros derrubados |",
              "|---|---|---|---|---|---|"]
        for e in r["saidas"]:
            fora = "não voltou" if e["duracao"] is None else br(e["duracao"] / 1000.0, 1)
            L.append(f"| {br(_min(e['saiu'], t0), 2)} | {e['mac']} ({e['ip'] or '?'}) | {fora} | {br(e['pausa_ms'])} | "
                     f"{br(e['perdidos'])} | {', '.join(e['derrubados_outros']) or '—'} |")
        L.append("")
    L += ["## Figuras", ""] + [f"- ![{f[:-4]}]({f})" for f in FIGURAS] + [""]
    with open(os.path.join(pasta, "resumo.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(l for l in L if l is not None))


def analisar_log(caminho):
    with open(caminho, encoding="utf-8", errors="replace") as f:
        log = B.ler(f)
    if not log.resumos:
        raise ValueError(f"nenhum resumo [r] em {caminho}: o r estava ligado?")
    nome = os.path.splitext(os.path.basename(caminho))[0]
    pasta = pasta_unica(os.path.join(os.path.dirname(os.path.abspath(caminho)), f"analise-{nome}"))
    os.makedirs(pasta)
    r = resumo(log, nome)
    for fig, arquivo in zip((fig_perdas(log, r["t0"]), fig_memoria(log, r["t0"]),
                             fig_aparelhos(log, r["t0"], r["t_fim"])), FIGURAS):
        fig.savefig(os.path.join(pasta, arquivo), dpi=130, bbox_inches="tight")
        plt.close(fig)
    escrever_resumo(r, pasta)
    return pasta, r


def comparar(resumos, destino):
    linhas = [("log", lambda r: r["nome"]),
              ("telemetria no WebSocket", lambda r: f"{r['taxa_hz']} Hz" if r["taxa_hz"] else "—"),
              ("duração (min)", lambda r: br(r["duracao_min"], 1)),
              ("aparelhos que entraram ou saíram do AP", lambda r: str(r["aparelhos"])),
              ("perdas A → B", lambda r: f"{br(r['perdas_pct'], 2)} % ({br(r['perdidos'])})"),
              ("heap mínimo / mediana (kB)", lambda r: f"{br(r['heap_min'])} / {br(r['heap_mediana'])}"),
              ("pausas da trava (n / s / maior ms)",
               lambda r: f"{br(r['pausas_n'])} / {br(r['pausas_s'], 1)} / {br(r['pausa_max_ms'])}"),
              ("saídas do AP (> 1 s)", lambda r: f"{len(r['saidas'])} ({len(r['saidas_longas'])})"),
              ("WebSockets derrubados (pela trava)", lambda r: f"{r['derrubados']} ({r['derrubados_trava']})"),
              ("**reinícios do B / corrupções do heap**",
               lambda r: f"**{len(r['reinicios'])} / {len(r['corrupcoes'])}**")]
    L = ["# Comparação dos logs do B", "", "| | " + " | ".join(f"log {i + 1}" for i in range(len(resumos))) + " |",
         "|---|" + "---|" * len(resumos)]
    L += [f"| {nome} | " + " | ".join(f(r) for r in resumos) + " |" for nome, f in linhas]
    with open(destino, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(L) + "\n")


def main():
    for f in (sys.stdout, sys.stderr):
        try:
            f.reconfigure(errors="replace")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(description="Figuras e resumo do log do monitor do ESP B")
    ap.add_argument("logs", nargs="+", help="um ou mais device-monitor-*.log do B")
    a = ap.parse_args()
    resumos = []
    for caminho in a.logs:
        pasta, r = analisar_log(caminho)
        resumos.append(r)
        print(f"[analisar_b] {os.path.basename(caminho)}: {br(r['perdas_pct'], 2)} % de perdas A → B, "
              f"{r['pausas_n']} pausas da trava; figuras e resumo em {pasta}")
    if len(resumos) > 1:
        base = os.path.join(os.path.dirname(os.path.abspath(a.logs[0])), f"comparacao-{resumos[0]['nome']}")
        destino = base + ".md"
        k = 2
        while os.path.exists(destino):
            destino = f"{base}-{k}.md"
            k += 1
        comparar(resumos, destino)
        print(f"[analisar_b] comparação em {destino}")


if __name__ == "__main__":
    main()
