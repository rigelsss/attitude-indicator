"""Figuras e resumo de uma gravação do cliente, para o relatório (docs/cliente-python.md, passo 5).

Uso (a partir de ground-station/python):
  python analisar.py logs\\261025-193532-tel.csv
Cria logs\\analise-261025-193532\\ (ou -2, -3... se já existir) com telemetria.png, jitter.png, latencia.png,
perdas.png, conexoes.png e resumo.md. Lê o -eventos.csv do mesmo nome, se existir. Não abre janela.
"""

import argparse
import math
import os
import sys
import time

import matplotlib

matplotlib.use("Agg")   # só arquivos, nunca janela
import matplotlib.pyplot as plt  # noqa: E402

from horizonte import figuras as F  # noqa: E402
from horizonte import series as S  # noqa: E402
from horizonte.leitor import LeitorCSV, caminho_eventos  # noqa: E402

FIGURAS = ("telemetria.png", "jitter.png", "latencia.png", "perdas.png", "conexoes.png")


def br(x, casas=1):
    """Número em português (vírgula decimal); NaN ou None → "—"."""
    if x is None or (isinstance(x, float) and math.isnan(x)):
        return "—"
    if isinstance(x, int):
        return f"{x:,}".replace(",", " ")
    return f"{x:,.{casas}f}".replace(",", " ").replace(".", ",")


def pasta_unica(base):
    """base, ou base-2, base-3... a primeira que não existe (nunca sobrescreve uma análise)."""
    final, k = base, 2
    while os.path.exists(final):
        final = f"{base}-{k}"
        k += 1
    return final


def _linha_estat(nome, unidade, st, casas=1):
    if not st:
        return f"| {nome} | {unidade} | sem dados | | | | | |"
    return (f"| {nome} | {unidade} | {br(st['n'])} | {br(st['min'], casas)} | {br(st['mediana'], casas)} | "
            f"{br(st['p95'], casas)} | {br(st['p99'], casas)} | {br(st['max'], casas)} |")


def analisar(caminho_tel, saida=None):
    """Gera as figuras e o resumo. Devolve a pasta criada."""
    nome = os.path.basename(caminho_tel)
    if not nome.endswith("-tel.csv"):
        raise ValueError(f"não é um CSV de telemetria do cliente: {caminho_tel}")
    nome = nome[: -len("-tel.csv")]
    pontos = [S.ponto_tel(l) for l in LeitorCSV(caminho=caminho_tel).ler_novas()]
    ev_caminho = caminho_eventos(caminho_tel)
    eventos = LeitorCSV(caminho=ev_caminho).ler_novas() if os.path.exists(ev_caminho) else []
    pasta = pasta_unica(saida or os.path.join(os.path.dirname(os.path.abspath(caminho_tel)), f"analise-{nome}"))
    os.makedirs(pasta)

    com_a = [p for p in pontos if p["link"] in ("ok", "late")]
    rtt = [ms for _, ms in S.pontos_rtt(eventos)]
    intervalos = S.intervalos_ms(pontos)
    st = {
        "intervalo": S.estatisticas(intervalos),
        "loop_us": S.estatisticas([p["loop_us"] for p in com_a]),
        "delay_ms": S.estatisticas([p["delay_ms"] for p in com_a]),
        "atraso_bpc_ms": S.estatisticas([p["atraso_bpc_ms"] for p in pontos]),
        "rtt": S.estatisticas(rtt),
    }
    minutos = S.perdas_por_minuto(pontos)
    t0 = pontos[0]["t"] if pontos else 0.0
    t_fim = pontos[-1]["t"] if pontos else 0.0
    puladas = {}
    for p in pontos:
        m = int((p["t"] - t0) // 60)
        puladas[m] = puladas.get(m, 0) + p["puladas"]
    sess = S.sessoes(eventos)
    sem_tel = S.periodos_sem_telemetria(eventos)

    # ---------- figuras ----------
    figs = (
        F.fig_telemetria(plt, pontos),
        F.fig_jitter(plt, intervalos),
        F.fig_latencia(plt, [p["loop_us"] for p in com_a], [p["delay_ms"] for p in com_a],
                       [p["atraso_bpc_ms"] for p in pontos], rtt),
        F.fig_perdas(plt, minutos, sorted(puladas.items())),
        F.fig_conexoes(plt, sess, sem_tel, t0, t_fim),
    )
    for fig, arquivo in zip(figs, FIGURAS):
        fig.savefig(os.path.join(pasta, arquivo), dpi=130, bbox_inches="tight")
        plt.close(fig)

    # ---------- resumo ----------
    inicio = time.strftime("%d/%m/%Y %H:%M:%S", time.localtime(pontos[0]["unix"])) if pontos else "—"
    duracao = t_fim - t0
    perd = sum(m[1] for m in minutos)
    env = sum(m[2] for m in minutos)
    total_puladas = sum(p["puladas"] for p in pontos)
    lacunas = sum(1 for x in intervalos if x > S.LACUNA_S * 1000)
    sem_conexao = sum(b["inicio"] - a["fim"] for a, b in zip(sess, sess[1:]) if a["fim"] is not None)
    L = [f"# Análise da gravação {nome}", "",
         f"- Arquivo: `{os.path.basename(caminho_tel)}`" + (" e o de eventos" if eventos else " (sem o de eventos)"),
         f"- Início: {inicio}; duração: {br(duracao / 60, 1)} min; {br(len(pontos))} telemetrias, "
         f"{br(len(com_a))} com o link A → B ativo",
         f"- Conexões: {br(len(sess))}; tempo sem conexão: {br(sem_conexao, 1)} s; trechos sem telemetria com a "
         f"conexão aberta: {br(len(sem_tel))} ({br(sum(b - a for a, b in sem_tel), 1)} s)", ""]
    L += ["## Entrega no PC (jitter)", "",
          "| Medida | Unidade | n | mín. | mediana | p95 | p99 | máx. |", "|---|---|---|---|---|---|---|---|",
          _linha_estat("intervalo entre telemetrias", "ms", st["intervalo"]), "",
          f"Intervalos acima de {br(S.LACUNA_S, 0)} s (lacunas, dentro da mesma conexão): {br(lacunas)}.", ""]
    L += ["## Latência", "",
          "| Medida | Unidade | n | mín. | mediana | p95 | p99 | máx. |", "|---|---|---|---|---|---|---|---|",
          _linha_estat("fusão no A (loop_us)", "µs", st["loop_us"], 0),
          _linha_estat("atraso relativo A → B (delay_ms)", "ms", st["delay_ms"], 0),
          _linha_estat("atraso relativo B → PC (tb)", "ms", st["atraso_bpc_ms"]),
          _linha_estat("RTT PC ↔ B (ping)", "ms", st["rtt"]), "",
          "Os atrasos relativos são medidos contra o menor valor visto, porque os relógios do A, do B e do PC não "
          "estão sincronizados.", "",
          "### Orçamento de latência (da amostra no A até o PC)", "",
          "| Parte | mediana (ms) | p99 / máx. (ms) | Como |", "|---|---|---|---|"]
    soma_med = soma_p99 = 0.0
    periodo = S.periodo_ws_ms(pontos)
    for parte, med, p99, como in S.orcamento_latencia(st["loop_us"], st["delay_ms"], st["atraso_bpc_ms"], st["rtt"],
                                                       periodo):
        L.append(f"| {parte} | {br(med, 2)} | {br(p99, 2)} | {como} |")
        soma_med += 0 if math.isnan(med) else med
        soma_p99 += 0 if math.isnan(p99) else p99
    L += [f"| **total** | **{br(soma_med, 1)}** | **{br(soma_p99, 1)}** | soma das partes |", ""]
    L += ["## Perdas", "",
          f"- Rádio A → B: {br(perd)} pacotes perdidos de {br(env)} enviados pelo A "
          f"({br(100 * perd / env if env else float('nan'), 2)} %).",
          f"- Telemetrias puladas pelo B para o PC (WebSocket): {br(total_puladas)} "
          f"({br(100 * total_puladas / (total_puladas + len(pontos)) if pontos else float('nan'), 2)} % das montadas).",
          ""]
    if minutos:
        L += ["| Minuto | perdidos | enviados pelo A | % |", "|---|---|---|---|"]
        L += [f"| {m} | {br(p)} | {br(e)} | {br(pct, 2)} |" for m, p, e, pct in minutos]
        L.append("")
    if sess:
        L += ["## Conexões", "", "| Sessão | início (min) | duração (s) | como terminou |", "|---|---|---|---|"]
        for s in sess:
            fim = s["fim"] if s["fim"] is not None else t_fim
            L.append(f"| {s['sessao']} | {br((s['inicio'] - t0) / 60, 2)} | {br(fim - s['inicio'], 1)} | "
                     f"{s['motivo'] or 'aberta no fim da gravação'} |")
        L.append("")
    L += ["## Figuras", ""] + [f"- ![{f[:-4]}]({f})" for f in FIGURAS] + [""]
    with open(os.path.join(pasta, "resumo.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(L))
    return pasta


def main():
    for f in (sys.stdout, sys.stderr):
        try:
            f.reconfigure(errors="replace")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(description="Figuras e resumo de uma gravação do cliente")
    ap.add_argument("arquivo", help="o CSV de telemetria (…-tel.csv)")
    ap.add_argument("--saida", help="pasta de saída (padrão: logs/analise-<nome>)")
    a = ap.parse_args()
    pasta = analisar(a.arquivo, a.saida)
    print(f"[analisar] figuras e resumo em {pasta}")


if __name__ == "__main__":
    main()
