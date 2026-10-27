"""Gráficos ao vivo do cliente (docs/cliente-python.md, passo 5). Roda num terminal próprio, ao lado do cliente.py:
lê o CSV que o cliente está gravando (sem tocar no cliente) e atualiza seis painéis com os últimos 30 s.

Uso (a partir de ground-station/python):
  python graficos.py                                  # segue o CSV mais recente de logs\\
  python graficos.py --arquivo logs\\261025-193532-tel.csv   # mostra uma gravação inteira, parada (com zoom)
  python graficos.py --sem-janela --segundos 600      # teste: sem janela, mede quantas atualizações por segundo
Fechar a janela não mexe no cliente.
"""

import argparse
import math
import os
import sys
import time

from horizonte import series as S
from horizonte.leitor import LeitorCSV, caminho_eventos

PASTA_PADRAO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "logs")
ESTADOS = {"ok": "telemetria ok", "sem_telemetria": "SEM TELEMETRIA", "aguardando": "aguardando telemetria",
           "abandonar": "conexão abandonada"}


class Fonte:
    """Lê o CSV de telemetria e o de eventos que o acompanha, e mantém o estado da linha do topo."""

    def __init__(self, arquivo=None, pasta=None):
        self.tel = LeitorCSV(caminho=arquivo) if arquivo else LeitorCSV(pasta=pasta)
        self.ev = None
        self._limpar_estado()

    def _limpar_estado(self):
        self.sessao, self.conexao, self.estado_tel, self.rtt = "—", "aguardando o cliente", None, None
        self.lost, self.loss_pct, self.ultimo_t = None, None, None

    def ler(self):
        """(pontos novos, rtt novos, trocou de arquivo)."""
        linhas = self.tel.ler_novas()
        trocou = self.tel.trocou
        if self.tel.caminho and (self.ev is None or trocou):
            self.ev = LeitorCSV(caminho=caminho_eventos(self.tel.caminho))
            self._limpar_estado()
        pontos = [S.ponto_tel(l) for l in linhas]
        eventos = self.ev.ler_novas() if self.ev else []
        for e in eventos:
            tipo = e.get("tipo")
            if tipo == "conectado":
                self.sessao, self.conexao = e.get("sessao"), "conectado"
            elif tipo in ("desconectado", "falha"):
                self.conexao, self.estado_tel = "SEM CONEXÃO", None
            elif tipo == "telemetria":
                self.estado_tel = e.get("detalhe")
            elif tipo == "rtt":
                self.rtt = S.num(e.get("detalhe"))
        if pontos:
            ultimo = pontos[-1]
            self.ultimo_t = ultimo["t"]
            if ultimo["lost"] is not None:
                self.lost, self.loss_pct = ultimo["lost"], ultimo["loss_pct"]
        return pontos, S.pontos_rtt(eventos), trocou

    def texto(self, agora):
        arquivo = os.path.basename(self.tel.caminho) if self.tel.caminho else "nenhum CSV ainda"
        partes = [arquivo, f"sessão {self.sessao}", self.conexao]
        if self.estado_tel and self.conexao == "conectado":
            partes.append(ESTADOS.get(self.estado_tel, self.estado_tel))
        if self.lost is not None:
            pct = "—" if math.isnan(self.loss_pct) else f"{self.loss_pct:.2f}".replace(".", ",")
            partes.append(f"perdidos A → B {self.lost} ({pct} %)")
        if self.rtt is not None and not math.isnan(self.rtt):
            partes.append(f"RTT {self.rtt:.1f} ms".replace(".", ","))
        if self.ultimo_t is not None:
            partes.append(f"último dado há {max(0.0, agora - self.ultimo_t):.1f} s".replace(".", ","))
        return "  |  ".join(partes)


def passo(fonte, painel, janela):
    """Uma atualização: lê o que chegou, corta a janela, atualiza e desenha."""
    pontos, rtt, trocou = fonte.ler()
    if trocou:
        janela.limpar()
    janela.acrescentar(pontos, rtt)
    agora = time.monotonic_ns() / 1e9   # o mesmo relógio do pc_ns do cliente (monotônico do sistema)
    janela.cortar(agora)
    painel.atualizar(janela, agora, fonte.texto(agora))
    painel.desenhar()
    return len(pontos)


def rodar_vivo(fonte, painel, janela, plt, intervalo_s):
    timer = painel.fig.canvas.new_timer(interval=int(intervalo_s * 1000))
    timer.add_callback(passo, fonte, painel, janela)
    painel.fig.canvas.manager.set_window_title("Horizonte Artificial: gráficos ao vivo")
    timer.start()
    plt.show()


def rodar_sem_janela(fonte, painel, janela, segundos, intervalo_s):
    """Teste: o mesmo laço, sem janela, por `segundos`. Imprime atualizações por segundo e o tempo de cada uma."""
    tempos, linhas = [], 0
    fim = time.monotonic() + segundos
    while time.monotonic() < fim:
        t0 = time.perf_counter()
        linhas += passo(fonte, painel, janela)
        tempos.append(time.perf_counter() - t0)
        time.sleep(max(0.0, intervalo_s - tempos[-1]))
    tempos.sort()
    print(f"[graficos] {len(tempos)} atualizações em {segundos:g} s ({len(tempos) / segundos:.1f}/s) | "
          f"tempo de uma: mediana {1000 * tempos[len(tempos) // 2]:.1f} ms, máx. {1000 * tempos[-1]:.1f} ms | "
          f"{linhas} linhas lidas, {fonte.tel.corrompidas} corrompidas | fundo redesenhado {painel.redesenhos} vezes | "
          f"arquivo {fonte.tel.caminho}")


def main():
    for f in (sys.stdout, sys.stderr):
        try:
            f.reconfigure(errors="replace")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(description="Gráficos ao vivo do cliente Python")
    ap.add_argument("--pasta", default=PASTA_PADRAO, help="pasta dos CSV do cliente (segue o mais recente)")
    ap.add_argument("--arquivo", help="mostra esta gravação inteira, parada")
    ap.add_argument("--janela", type=float, default=30.0, help="segundos mostrados ao vivo")
    ap.add_argument("--intervalo", type=float, default=0.25, help="segundos entre atualizações")
    ap.add_argument("--sem-janela", action="store_true", help="teste sem janela (com --segundos)")
    ap.add_argument("--segundos", type=float, default=60.0)
    a = ap.parse_args()

    import matplotlib
    if a.sem_janela:
        matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from horizonte.figuras import PainelVivo

    if a.arquivo:   # gravação inteira, parada
        fonte = Fonte(arquivo=a.arquivo)
        pontos, rtt, _ = fonte.ler()
        if not pontos:
            print(f"[graficos] sem telemetria em {a.arquivo}")
            return
        duracao = max(1.0, pontos[-1]["t"] - pontos[0]["t"])
        janela = S.Janela(duracao + 1)
        janela.acrescentar(pontos, rtt)
        painel = PainelVivo(duracao + 1, plt, animado=False)
        agora = pontos[-1]["t"]
        painel.atualizar(janela, agora, fonte.texto(agora).replace("último dado há 0,0 s", "gravação inteira"))
        painel.fig.canvas.manager.set_window_title(f"Horizonte Artificial: {os.path.basename(a.arquivo)}")
        plt.show()
        return

    fonte = Fonte(pasta=a.pasta)
    janela = S.Janela(a.janela)
    painel = PainelVivo(a.janela, plt)
    if a.sem_janela:
        rodar_sem_janela(fonte, painel, janela, a.segundos, a.intervalo)
    else:
        print(f"[graficos] seguindo o CSV mais recente de {a.pasta}; feche a janela para sair")
        rodar_vivo(fonte, painel, janela, plt, a.intervalo)


if __name__ == "__main__":
    main()
