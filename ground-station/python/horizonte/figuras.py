"""Figuras do cliente (docs/cliente-python.md, passo 5): o painel ao vivo (graficos.py) e as figuras do relatório
(analisar.py). Só desenha: os números vêm de horizonte.series.

Estilo (paleta de referência do guia de visualização, validada em 26/10/2026 no modo claro sobre #fcfcfb: as três
primeiras cores passam em separação para daltonismo e visão normal; a terceira, água, tem contraste 2,7:1, por isso
as séries levam legenda e rótulo direto no último valor, e o resumo.md traz as tabelas):
  - uma escala por painel, nunca dois eixos y; painéis lado a lado em vez disso;
  - cores das séries na ordem fixa (azul, laranja, água), a mesma série sempre com a mesma cor;
  - linhas finas, grade em traço contínuo fino, texto em tinta neutra (nunca na cor da série);
  - números com vírgula decimal.
"""

import math

import matplotlib
from matplotlib import ticker

SERIE = ("#2a78d6", "#eb6834", "#1baf7a")        # azul, laranja, água
SUPERFICIE = "#fcfcfb"
TINTA, TINTA_2, TINTA_MUDA = "#0b0b0b", "#52514e", "#898781"
GRADE, EIXO = "#e1e0d9", "#c3c2b7"
AVISO = "#fab219"                                 # status "warning": só com rótulo
LINHA = 1.5


def _virgula(x, _pos=None):
    texto = f"{x:g}"
    return texto.replace(".", ",").replace("-", "−")


def estilo():
    matplotlib.rcParams.update({
        "figure.facecolor": SUPERFICIE, "axes.facecolor": SUPERFICIE, "savefig.facecolor": SUPERFICIE,
        "axes.edgecolor": EIXO, "axes.labelcolor": TINTA_2, "axes.titlecolor": TINTA,
        "axes.titlesize": 10, "axes.titleweight": "bold", "axes.titlelocation": "left", "axes.labelsize": 9,
        "axes.grid": True, "grid.color": GRADE, "grid.linewidth": 0.6, "grid.linestyle": "-",
        "axes.spines.top": False, "axes.spines.right": False,
        "xtick.color": TINTA_MUDA, "ytick.color": TINTA_MUDA, "xtick.labelsize": 8, "ytick.labelsize": 8,
        "legend.fontsize": 8, "legend.frameon": False, "font.family": "sans-serif",
        "lines.linewidth": LINHA, "axes.axisbelow": True,
    })


def _eixo(ax, titulo, unidade=None):
    ax.set_title(titulo)
    if unidade:
        ax.set_ylabel(unidade)
    ax.xaxis.set_major_formatter(ticker.FuncFormatter(_virgula))
    ax.yaxis.set_major_formatter(ticker.FuncFormatter(_virgula))


def _ultimo_valor(xs, ys):
    for x, y in zip(reversed(xs), reversed(ys)):
        if not math.isnan(y):
            return x, y
    return None


# ---------- painel ao vivo ----------

# Escala mínima de cada painel (o ruído pequeno não deve parecer movimento) e se começa em 0
ESCALA = {"atitude": (-10.0, 10.0, False), "alt": (-0.5, 0.5, False), "atrasos": (0.0, 20.0, True),
          "taxa": (0.0, 40.0, True), "perdas": (0.0, 5.0, True)}


def _escala_nova(atual, minimo, ys, zero):
    """A escala y que o painel deve ter, ou None para manter a atual. Só muda quando um dado sai da escala, ou quando
    os dados ocupam menos de 40 % dela: assim o fundo (eixos, marcas, rótulos) raramente precisa ser redesenhado."""
    v = [y for y in ys if not math.isnan(y)]
    if not v:
        return None
    lo, hi = min(v), max(v)
    a_lo, a_hi = atual
    cabe = a_lo <= lo and hi <= a_hi
    folgado = (hi - lo) < 0.4 * (a_hi - a_lo) and (a_hi - a_lo) > (minimo[1] - minimo[0]) * 1.01
    if cabe and not folgado:
        return None
    margem = 0.15 * max(hi - lo, 1e-9)
    novo_lo = 0.0 if zero else min(lo - margem, minimo[0])
    novo_hi = max(hi + margem, minimo[1])
    return (novo_lo, novo_hi)


class PainelVivo:
    """Seis painéis com os últimos `segundos`, atualizados por atualizar() e desenhados por desenhar(). Funciona sem
    janela (backend Agg).

    Desenho rápido (blitting): o fundo (eixos, grade, rótulos) é desenhado uma vez e guardado; a cada atualização, só
    as linhas, os rótulos diretos e a linha de estado são desenhados por cima. A figura inteira só é redesenhada quando
    a escala de algum painel muda (ver _escala_nova) ou a janela muda de tamanho: de ~170 ms para alguns ms por
    atualização (medido em 26/10/2026). animado=False desenha do jeito normal (a gravação inteira, parada).
    """

    PAINEIS = (
        ("atitude", "Atitude", "graus", (("roll", "roll"), ("pitch", "pitch"))),
        ("proa", "Proa magnética", "graus", (("hdg", "proa"),)),
        ("alt", "Altitude relativa", "m", (("alt", "altitude"),)),
        ("atrasos", "Atrasos", "ms", (("delay_ms", "A → B"), ("atraso_bpc_ms", "B → PC"), ("rtt", "RTT PC ↔ B"))),
        ("taxa", "Telemetria recebida no PC", "mensagens/s", (("taxa", "mensagens/s"),)),
        ("perdas", "Perdas no rádio A → B", "pacotes/s", (("perdas", "perdidos"),)),
    )

    def __init__(self, segundos=30.0, plt=None, animado=True):
        estilo()
        if plt is None:
            import matplotlib.pyplot as plt
        self.segundos = segundos
        self.fig, eixos = plt.subplots(3, 2, figsize=(12, 7.5), sharex=True)
        self.fig.subplots_adjust(left=0.07, right=0.93, top=0.90, bottom=0.08, hspace=0.45, wspace=0.22)
        self.eixos, self.linhas, self.rotulos = {}, {}, {}
        for ax, (chave, titulo, unidade, series) in zip(eixos.flat, self.PAINEIS):
            _eixo(ax, titulo, unidade)
            self.eixos[chave] = ax
            for i, (campo, nome) in enumerate(series):
                desenho = "steps-post" if chave in ("taxa", "perdas") else "default"
                (linha,) = ax.plot([], [], color=SERIE[i], label=nome, drawstyle=desenho)
                self.linhas[campo] = linha
                if len(series) > 1:   # rótulo direto no último valor (várias séries no painel)
                    self.rotulos[campo] = ax.annotate(nome, (0, 0), xytext=(4, 0), textcoords="offset points",
                                                      fontsize=8, color=TINTA_2, va="center", annotation_clip=False)
            if len(series) > 1:
                ax.legend(loc="upper left", ncols=len(series))
            ax.set_xlim(-segundos, 0)
        for ax in eixos[-1]:
            ax.set_xlabel("tempo (s, 0 = agora)")
        self.eixos["proa"].set_ylim(0, 360)
        self.eixos["proa"].set_yticks([0, 90, 180, 270, 360])
        self.estado = self.fig.text(0.07, 0.955, "aguardando dados…", fontsize=9, color=TINTA_2, ha="left")
        for chave, (lo, hi, _zero) in ESCALA.items():
            self.eixos[chave].set_ylim(lo, hi)
        self.animado = animado
        self._animados = list(self.linhas.values()) + list(self.rotulos.values()) + [self.estado]
        for a in self._animados:
            a.set_animated(animado)
        self._fundo = None
        self.redesenhos = 0   # quantas vezes o fundo foi redesenhado (escala nova ou janela redimensionada)
        self.fig.canvas.mpl_connect("resize_event", lambda _e: setattr(self, "_fundo", None))

    def atualizar(self, janela, agora, estado=""):
        """Redesenha com os pontos da janela. `agora` é o instante que vira o 0 do eixo x."""
        from . import series
        pts = list(janela.pontos)
        dados = {}
        for campo in ("roll", "pitch", "hdg", "alt", "delay_ms", "atraso_bpc_ms"):
            dados[campo] = series.com_buracos(pts, campo, origem=agora)
        rtt = [{"t": t, "rtt": ms, "sessao": ""} for t, ms in janela.rtt]
        dados["rtt"] = series.com_buracos(rtt, "rtt", origem=agora, lacuna_s=2.5)   # um RTT por segundo
        dados["taxa"] = series.por_segundo(pts, origem=agora)
        dados["perdas"] = series.perdas_por_intervalo(pts, 1.0, origem=agora)
        for campo, (xs, ys) in dados.items():
            self.linhas[campo].set_data(xs, ys)
            if campo in self.rotulos:
                fim = _ultimo_valor(xs, ys)
                self.rotulos[campo].set_visible(fim is not None)
                if fim is not None:
                    self.rotulos[campo].xy = fim
        campos = {"atitude": ("roll", "pitch"), "alt": ("alt",), "atrasos": ("delay_ms", "atraso_bpc_ms", "rtt"),
                  "taxa": ("taxa",), "perdas": ("perdas",)}
        for chave, (lo, hi, zero) in ESCALA.items():
            ys = [y for c in campos[chave] for y in dados[c][1]]
            nova = _escala_nova(self.eixos[chave].get_ylim(), (lo, hi), ys, zero)
            if nova is not None:
                self.eixos[chave].set_ylim(*nova)
                self._fundo = None
        self._afastar_rotulos()
        self.estado.set_text(estado)

    def _afastar_rotulos(self, distancia_pt=10.0):
        """Rótulos diretos do mesmo painel com pelo menos `distancia_pt` entre si na vertical (valores iguais ou
        próximos, como roll e pitch em 0, não se sobrepõem)."""
        pt_por_px = 72.0 / self.fig.dpi
        for ax in self.eixos.values():
            rot = [r for r in self.rotulos.values() if r.axes is ax and r.get_visible()]
            pos = sorted(((ax.transData.transform(r.xy)[1] * pt_por_px, r) for r in rot), key=lambda x: x[0])
            ultimo = None
            for y, r in pos:
                alvo = y if ultimo is None else max(y, ultimo + distancia_pt)
                r.set_position((4, alvo - y))   # deslocamento do texto em relação ao ponto (offset points)
                ultimo = alvo

    def desenhar(self):
        """Mostra o que o último atualizar() montou."""
        canvas = self.fig.canvas
        if not self.animado:
            canvas.draw_idle()
            return
        if self._fundo is None:
            canvas.draw()   # tudo, menos os animados
            self._fundo = canvas.copy_from_bbox(self.fig.bbox)
            self.redesenhos += 1
        else:
            canvas.restore_region(self._fundo)
        for a in self._animados:
            if a.get_visible():
                self.fig.draw_artist(a)
        canvas.blit(self.fig.bbox)


# ---------- figuras do relatório ----------

def _figura(plt, linhas, colunas, altura):
    estilo()
    fig, eixos = plt.subplots(linhas, colunas, figsize=(10, altura), squeeze=False)
    fig.subplots_adjust(hspace=0.5, wspace=0.25)
    return fig, eixos


def fig_telemetria(plt, pontos):
    """Atitude, proa e altitude ao longo da gravação (minutos desde o primeiro ponto)."""
    from . import series as S
    fig, eixos = _figura(plt, 3, 1, 7.5)
    t0 = pontos[0]["t"] if pontos else 0.0
    minutos = [dict(p, t=(p["t"] - t0) / 60.0) for p in pontos]
    lac = S.LACUNA_S / 60.0
    ax = eixos[0][0]
    _eixo(ax, "Atitude", "graus")
    for i, (campo, nome) in enumerate((("roll", "roll"), ("pitch", "pitch"))):
        ax.plot(*S.com_buracos(minutos, campo, lacuna_s=lac), color=SERIE[i], label=nome)
    ax.legend(loc="upper left", ncols=2)
    ax = eixos[1][0]
    _eixo(ax, "Proa magnética", "graus")
    ax.plot(*S.com_buracos(minutos, "hdg", lacuna_s=lac), color=SERIE[0])
    ax.set_ylim(0, 360)
    ax.set_yticks([0, 90, 180, 270, 360])
    ax = eixos[2][0]
    _eixo(ax, "Altitude relativa", "m")
    ax.plot(*S.com_buracos(minutos, "alt", lacuna_s=lac), color=SERIE[0])
    ax.set_xlabel("tempo (min)")
    return fig


def _histograma(ax, valores, titulo, unidade, limite=None, passo=None):
    from . import series as S
    v = [x for x in valores if not math.isnan(x)]
    _eixo(ax, titulo, None)
    ax.set_xlabel(unidade)
    ax.set_ylabel("ocorrências")
    st = S.estatisticas(v)
    if not st:
        ax.text(0.5, 0.5, "sem dados", transform=ax.transAxes, ha="center", va="center", color=TINTA_MUDA)
        return
    topo = limite if limite is not None else st["max"]
    # O eixo começa em 0, a não ser que os dados fiquem todos longe dele (o loop_us, perto de 1 000 µs)
    base = 0.0 if st["min"] < 0.3 * topo else st["min"] - 0.1 * (topo - st["min"])
    fora = sum(1 for x in v if x > topo)
    dentro = [x for x in v if x <= topo]
    caixas = max(10, min(120, int((topo - base) / passo))) if passo else 60
    ax.hist(dentro, bins=caixas, range=(base, topo), color=SERIE[0], edgecolor=SUPERFICIE, linewidth=0.8)
    ax.set_xlim(base, topo)
    # Rótulos das linhas em alturas diferentes, para não se sobreporem quando a mediana e o p99 estão perto
    for (nome, chave), altura in zip((("mediana", "mediana"), ("p99", "p99")), (-10, -24)):
        x = st[chave]
        if x <= topo:
            ax.axvline(x, color=TINTA_2, linewidth=0.8)
            ax.annotate(f"{nome} {_virgula(round(x, 1))}", (x, 1), xycoords=("data", "axes fraction"),
                        xytext=(3, altura), textcoords="offset points", fontsize=8, color=TINTA_2)
    if fora:   # acima da área do gráfico, alinhado à direita do título
        ax.annotate(f"{fora} acima de {_virgula(round(topo))} {unidade}", (1, 1), xycoords="axes fraction",
                    xytext=(0, 6), textcoords="offset points", ha="right", fontsize=8, color=TINTA_MUDA)


def fig_jitter(plt, intervalos):
    fig, eixos = _figura(plt, 1, 1, 3.6)
    from . import series as S
    st = S.estatisticas(intervalos)
    topo = max(120.0, st["p99"] * 1.1) if st else 120.0   # o p99 sempre dentro do gráfico
    _histograma(eixos[0][0], intervalos, "Intervalo entre telemetrias no PC", "ms", limite=topo, passo=1)
    return fig


def fig_latencia(plt, loop_us, delay_ms, atraso_bpc_ms, rtt_ms):
    from . import series as S
    fig, eixos = _figura(plt, 2, 2, 6.5)
    def limite(v, minimo):
        st = S.estatisticas(v)
        return max(minimo, st["p99"] * 1.5) if st else minimo
    _histograma(eixos[0][0], loop_us, "Fusão no A (loop_us)", "µs", limite=limite(loop_us, 0), passo=10)
    _histograma(eixos[0][1], delay_ms, "Atraso relativo A → B", "ms", limite=limite(delay_ms, 20), passo=1)
    _histograma(eixos[1][0], atraso_bpc_ms, "Atraso relativo B → PC", "ms", limite=limite(atraso_bpc_ms, 20),
                passo=2)
    _histograma(eixos[1][1], rtt_ms, "RTT PC ↔ B (ping)", "ms", limite=limite(rtt_ms, 20), passo=2)
    return fig


def fig_perdas(plt, minutos, puladas_por_minuto):
    fig, eixos = _figura(plt, 2, 1, 5.5)
    ax = eixos[0][0]
    _eixo(ax, "Perdas no rádio A → B por minuto", "% dos pacotes do A")
    if minutos:
        ax.bar([m for m, *_ in minutos], [0 if math.isnan(p) else p for *_, p in minutos], width=0.8,
               color=SERIE[0], edgecolor=SUPERFICIE, linewidth=1.5)
    else:
        ax.text(0.5, 0.5, "sem dados do A", transform=ax.transAxes, ha="center", color=TINTA_MUDA)
    ax = eixos[1][0]
    _eixo(ax, "Telemetrias puladas pelo B para o PC, por minuto", "mensagens")
    if puladas_por_minuto:
        ax.bar([m for m, _ in puladas_por_minuto], [q for _, q in puladas_por_minuto], width=0.8, color=SERIE[0],
               edgecolor=SUPERFICIE, linewidth=1.5)
    ax.set_xlabel("minuto desde o início")
    for linha in eixos:
        linha[0].xaxis.set_major_locator(ticker.MaxNLocator(integer=True))
    ax.yaxis.set_major_locator(ticker.MaxNLocator(integer=True))
    return fig


def fig_conexoes(plt, sessoes, sem_tel, t0, t_fim):
    from matplotlib.patches import Patch
    fig, eixos = _figura(plt, 1, 1, 2.4)
    ax = eixos[0][0]
    _eixo(ax, "Conexões do cliente", None)
    m = lambda t: (t - t0) / 60.0
    for s in sessoes:
        fim = s["fim"] if s["fim"] is not None else t_fim
        ax.broken_barh([(m(s["inicio"]), m(fim) - m(s["inicio"]))], (0.55, 0.4), facecolor=SERIE[0])
    for a, b in sem_tel:
        ax.broken_barh([(m(a), m(b) - m(a))], (0.05, 0.4), facecolor=AVISO)
    ax.set_yticks([0.25, 0.75], ["sem telemetria", "conectado"])
    ax.set_ylim(0, 1)
    ax.set_xlim(0, max(m(t_fim), 0.1))
    ax.grid(axis="y", visible=False)
    ax.set_xlabel("tempo (min); os vazios na linha \"conectado\" são o cliente sem conexão")
    ax.legend(handles=[Patch(color=SERIE[0], label="conectado"), Patch(color=AVISO, label="sem telemetria")],
              loc="upper right", ncols=2, bbox_to_anchor=(1, 1.35))
    return fig
