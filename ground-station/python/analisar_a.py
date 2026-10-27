"""Jitter do loop do ESP A para o relatório (docs/cliente-python.md, semana 6): figuras e resumo a partir do fluxo de
atitude ("a") do monitor do A.

Uso (a partir de ground-station/python):
  python analisar_a.py ..\\..\\firmware\\aircraft\\logs\\device-monitor-261022-103255.log
  python analisar_a.py <log 1> <log 2> ...      # cada um analisado, mais a comparação lado a lado
Cria, ao lado de cada log, analise-<nome do log>\\ (ou -2, -3...) com dt.png, jitter.png, filtro.png e resumo.md;
com mais de um log, também comparacao-a-<nome do primeiro>.md. Não abre janela.
"""

import argparse
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

from analisar import br, pasta_unica  # noqa: E402
from horizonte import figuras as F  # noqa: E402
from horizonte import log_a as A  # noqa: E402

FIGURAS = ("dt.png", "jitter.png", "filtro.png")


def fig_dt(log):
    """O dt máximo e o mínimo de cada janela, em dois painéis com a mesma escala (numa só, as linhas coincidem quase
    sempre em 5,01 ms, e uma cobre a outra)."""
    F.estilo()
    fig, eixos = plt.subplots(2, 1, figsize=(10, 4.8), sharex=True, sharey=True, squeeze=False)
    fig.subplots_adjust(hspace=0.45)
    t = [j["t_s"] for j in log.janelas]
    lo = min(j["dt_min"] for j in log.janelas)
    hi = max(j["dt_max"] for j in log.janelas)
    margem = max(0.02, (hi - lo) * 0.3)
    for ax, chave, titulo in ((eixos[0][0], "dt_max", "Maior intervalo entre amostras do MPU, por janela de ~100 ms"),
                              (eixos[1][0], "dt_min", "Menor intervalo entre amostras do MPU, por janela de ~100 ms")):
        F._eixo(ax, titulo, "ms")
        ax.plot(t, [j[chave] for j in log.janelas], color=F.SERIE[0], drawstyle="steps-pre")
        ax.axhline(A.PERIODO_MS, color=F.TINTA_2, linewidth=0.8)
        ax.annotate("5 ms (200 Hz)", (1, A.PERIODO_MS), xycoords=("axes fraction", "data"), xytext=(-3, 3),
                    textcoords="offset points", ha="right", fontsize=8, color=F.TINTA_2,
                    bbox=dict(facecolor=F.SUPERFICIE, edgecolor="none", pad=1))
        ax.set_ylim(min(lo, A.PERIODO_MS) - margem, max(hi, A.PERIODO_MS) + margem)
    eixos[1][0].set_xlabel("tempo (s, desde o início do fluxo); o dt do log tem resolução de 10 µs")
    return fig


def fig_jitter(log):
    F.estilo()
    fig, eixos = plt.subplots(1, 2, figsize=(10, 3.6), squeeze=False)
    com_par = [j for j in log.janelas if j["n"] > 1]
    pico = [(j["dt_max"] - j["dt_min"]) * 1000.0 for j in com_par]
    F._histograma(eixos[0][0], pico, "Pico a pico do dt por janela", "µs", limite=max(40.0, max(pico, default=0) + 10),
                  passo=10)
    atraso = [float(j["atraso"]) for j in log.janelas]
    F._histograma(eixos[0][1], atraso, "Atraso do pulso até a leitura (máx. da janela)", "µs",
                  limite=max(150.0, max(atraso, default=0) * 1.1), passo=2)
    return fig


def fig_filtro(log):
    F.estilo()
    fig, eixos = plt.subplots(1, 1, figsize=(10, 3.4), squeeze=False)
    v = [float(j["filtro_max"]) for j in log.janelas]
    F._histograma(eixos[0][0], v, "Custo do filtro de atitude (máx. da janela)", "µs",
                  limite=max(200.0, max(v, default=0) * 1.1), passo=4)
    return fig


def _linha(nome, unidade, st, casas=0):
    if not st:
        return f"| {nome} | {unidade} | sem dados | | | | |"
    return (f"| {nome} | {unidade} | {br(st['min'], casas)} | {br(st['mediana'], casas)} | {br(st['p95'], casas)} | "
            f"{br(st['p99'], casas)} | {br(st['max'], casas)} |")


def escrever_resumo(r, nome, carimbo, pasta):
    L = [f"# Jitter do loop do A: {nome}", "",
         f"- Firmware: compilado em {carimbo or '— (o log não pegou o boot)'}",
         f"- {br(r['janelas'])} janelas de ~100 ms, {br(r['amostras'])} amostras fundidas, {br(r['duracao_s'], 1)} s "
         f"de fluxo", "",
         "## Intervalo entre amostras (período nominal: 5 ms, 200 Hz)", "",
         f"- dt médio: **{br(r['dt_medio'], 2)} ms** (~{br(1000.0 / r['dt_medio'], 0)} Hz; o dt do log tem 2 casas, então a média também); menor dt visto "
         f"{br(r['dt_min'], 2)} ms, maior {br(r['dt_max'], 2)} ms.",
         f"- Janelas com todo dt a até 10 µs dos 5 ms: {br(r['janelas_ate_10us'])} de {br(r['janelas'])} "
         f"({br(100.0 * r['janelas_ate_10us'] / r['janelas'], 1)} %).",
         "- O dt sai do log com 2 casas (resolução de 10 µs): pico a pico e desvio são múltiplos de 10 µs.", "",
         "| Medida (por janela) | Unidade | mín. | mediana | p95 | p99 | máx. |", "|---|---|---|---|---|---|---|",
         _linha("pico a pico do dt (máx. − mín.)", "µs", r["pico_us"]),
         _linha("maior desvio do dt em relação a 5 ms", "µs", r["desvio_us"]),
         _linha("atraso do pulso do INT até a leitura (máx.)", "µs", r["atraso_us"]),
         _linha("custo do filtro (máx.)", "µs", r["filtro_max_us"]), "",
         f"- Custo médio do filtro: {br(r['filtro_med_us'], 0)} µs por amostra.",
         f"- Quadros ambíguos: {br(r['ambiguos'])}; pulsos sem leitura (descartados): {br(r['descartados'])}; "
         f"reinícios do filtro: {br(r['reinicios'])}.", "",
         "## Figuras", ""] + [f"- ![{f[:-4]}]({f})" for f in FIGURAS] + [""]
    with open(os.path.join(pasta, "resumo.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(L))


def analisar_log(caminho):
    with open(caminho, encoding="utf-8", errors="replace") as f:
        log = A.ler(f)
    r = A.resumo(log)
    if r is None:
        raise ValueError(f"nenhuma linha dt do fluxo a em {caminho}: o a estava ligado?")
    nome = os.path.splitext(os.path.basename(caminho))[0]
    pasta = pasta_unica(os.path.join(os.path.dirname(os.path.abspath(caminho)), f"analise-{nome}"))
    os.makedirs(pasta)
    for fig, arquivo in zip((fig_dt(log), fig_jitter(log), fig_filtro(log)), FIGURAS):
        fig.savefig(os.path.join(pasta, arquivo), dpi=130, bbox_inches="tight")
        plt.close(fig)
    escrever_resumo(r, nome, log.carimbo, pasta)
    return pasta, dict(r, nome=nome, carimbo=log.carimbo)


def comparar(resumos, destino):
    def st(chave, campo="mediana"):
        return lambda r: br(r[chave][campo], 0) if r[chave] else "—"
    linhas = [("log", lambda r: r["nome"]), ("firmware", lambda r: r["carimbo"] or "—"),
              ("fluxo (s) / amostras", lambda r: f"{br(r['duracao_s'], 0)} / {br(r['amostras'])}"),
              ("dt médio (ms, resolução de 10 µs)", lambda r: br(r["dt_medio"], 2)),
              ("menor / maior dt (ms)", lambda r: f"{br(r['dt_min'], 2)} / {br(r['dt_max'], 2)}"),
              ("pico a pico do dt, mediana / máx. (µs)",
               lambda r: f"{st('pico_us')(r)} / {st('pico_us', 'max')(r)}"),
              ("janelas com dt a até 10 µs de 5 ms", lambda r: f"{br(100.0 * r['janelas_ate_10us'] / r['janelas'], 1)} %"),
              ("atraso do pulso, mediana / máx. (µs)",
               lambda r: f"{st('atraso_us')(r)} / {st('atraso_us', 'max')(r)}"),
              ("filtro médio / máx. (µs)", lambda r: f"{br(r['filtro_med_us'], 0)} / {st('filtro_max_us', 'max')(r)}"),
              ("ambíguos / descartados / reinícios",
               lambda r: f"{r['ambiguos']} / {r['descartados']} / {r['reinicios']}")]
    L = ["# Comparação do jitter do loop do A", "",
         "| | " + " | ".join(f"log {i + 1}" for i in range(len(resumos))) + " |", "|---|" + "---|" * len(resumos)]
    L += [f"| {nome} | " + " | ".join(f(r) for r in resumos) + " |" for nome, f in linhas]
    with open(destino, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(L) + "\n")


def main():
    for f in (sys.stdout, sys.stderr):
        try:
            f.reconfigure(errors="replace")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(description="Jitter do loop do ESP A a partir do fluxo a")
    ap.add_argument("logs", nargs="+", help="um ou mais device-monitor-*.log do A com o fluxo a")
    a = ap.parse_args()
    resumos = []
    for caminho in a.logs:
        pasta, r = analisar_log(caminho)
        resumos.append(r)
        print(f"[analisar_a] {os.path.basename(caminho)}: dt médio {br(r['dt_medio'], 2)} ms, pico a pico máx. "
              f"{br(r['pico_us']['max'], 0) if r['pico_us'] else '—'} µs; figuras e resumo em {pasta}")
    if len(resumos) > 1:
        base = os.path.join(os.path.dirname(os.path.abspath(a.logs[0])), f"comparacao-a-{resumos[0]['nome']}")
        destino, k = base + ".md", 2
        while os.path.exists(destino):
            destino = f"{base}-{k}.md"
            k += 1
        comparar(resumos, destino)
        print(f"[analisar_a] comparação em {destino}")


if __name__ == "__main__":
    main()
