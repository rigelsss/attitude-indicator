"""Testes de horizonte.figuras: o painel ao vivo, sem janela (backend Agg)."""

import math
import time

import pytest

pytest.importorskip("matplotlib")
import matplotlib.pyplot as plt  # noqa: E402

from conftest import linha_tel  # noqa: E402
from horizonte import series as S  # noqa: E402
from horizonte.figuras import PainelVivo  # noqa: E402


def lote(t0, t1, passo=0.035, n0=1):
    return [S.ponto_tel(linha_tel(t0 + i * passo, n0 + i, roll=str(math.sin(i / 10)), pitch="1", hdg="90",
                                  alt="0.2", valid="13", delay_ms="2", atraso_bpc_ms="4", lost=str(i // 50)))
            for i in range(int((t1 - t0) / passo))]


@pytest.fixture
def painel():
    p = PainelVivo(30, plt)
    yield p
    plt.close(p.fig)


def test_atualizacoes_acompanham_o_csv_crescendo(painel):
    janela = S.Janela(30)
    tamanhos = []
    for etapa in range(3):
        janela.acrescentar(lote(etapa * 2.0, etapa * 2.0 + 2.0, n0=etapa * 100))
        janela.cortar(etapa * 2.0 + 2.0)
        painel.atualizar(janela, etapa * 2.0 + 2.0, "teste")
        tamanhos.append(len(painel.linhas["roll"].get_xdata()))
    assert tamanhos[0] < tamanhos[1] < tamanhos[2]
    for campo in ("roll", "pitch", "hdg", "alt", "delay_ms", "atraso_bpc_ms", "taxa", "perdas"):
        assert len(painel.linhas[campo].get_xdata()) > 0, campo
    assert painel.estado.get_text() == "teste"


def test_pausa_vira_buraco(painel):
    janela = S.Janela(30)
    janela.acrescentar(lote(0, 5) + lote(9, 14, n0=500))   # 4 s sem telemetria no meio
    painel.atualizar(janela, 14.0)
    ys = painel.linhas["roll"].get_ydata()
    assert sum(1 for y in ys if math.isnan(y)) == 1
    taxa = list(painel.linhas["taxa"].get_ydata())
    assert taxa.count(0) >= 3                    # os segundos da pausa com 0 mensagens


def test_uma_atualizacao_cabe_em_100_ms(painel):
    # 30 s de dados (~860 pontos), chegando aos poucos como ao vivo: atualizar e desenhar abaixo de 100 ms garante
    # pelo menos 4 atualizações por segundo. Sem o blitting, cada uma levava ~170 ms (26/10/2026)
    janela = S.Janela(30)
    janela.acrescentar(lote(0, 30), rtt=[(t * 1.0, 5.0) for t in range(30)])
    painel.atualizar(janela, 30.0)
    painel.desenhar()                            # o primeiro desenho monta o fundo: fora da medida
    tempos = []
    for i in range(10):
        agora = 30.25 + 0.25 * i
        janela.acrescentar(lote(agora - 0.25, agora, n0=2000 + 10 * i))
        janela.cortar(agora)
        t0 = time.perf_counter()
        painel.atualizar(janela, agora, "medida")
        painel.desenhar()
        tempos.append(time.perf_counter() - t0)
    assert sorted(tempos)[5] < 0.100, tempos
    assert painel.redesenhos == 1                # a escala não mudou: o fundo não foi redesenhado


@pytest.mark.parametrize("atual, ys, zero, esperado", [
    ((-10, 10), [1, -2, 3], False, None),                 # cabe: mantém
    ((-10, 10), [1, 25], False, (-10, 28.6)),             # saiu por cima: amplia com margem
    ((0, 40), [30, 31], True, None),                      # cabe e ocupa pouco, mas já é a escala mínima
    ((0, 200), [10, 20], True, (0.0, 40)),                # ocupa menos de 40 % de uma escala ampliada: volta
    ((-10, 10), [math.nan], False, None),                 # só buracos: mantém
])
def test_escala_so_muda_quando_precisa(atual, ys, zero, esperado):
    from horizonte.figuras import _escala_nova
    minimo = (0, 40) if zero else (-10, 10)
    nova = _escala_nova(atual, minimo, ys, zero)
    assert nova == (pytest.approx(esperado) if esperado else None)


def test_escala_nova_redesenha_o_fundo(painel):
    janela = S.Janela(30)
    janela.acrescentar(lote(0, 5))
    painel.atualizar(janela, 5.0)
    painel.desenhar()
    janela.acrescentar([S.ponto_tel(linha_tel(5.1, 9999, roll="40", pitch="1", valid="13"))])   # roll de 40°
    painel.atualizar(janela, 5.2)
    painel.desenhar()
    assert painel.redesenhos == 2 and painel.eixos["atitude"].get_ylim()[1] > 40


def test_modo_parado_desenha_as_linhas(tmp_path):
    # animado=False (a gravação inteira): as linhas entram no desenho normal e no arquivo salvo
    painel = PainelVivo(30, plt, animado=False)
    janela = S.Janela(30)
    janela.acrescentar(lote(0, 30))
    painel.atualizar(janela, 30.0, "parado")
    assert not painel.linhas["roll"].get_animated()
    painel.fig.savefig(tmp_path / "parado.png")
    plt.close(painel.fig)
    assert (tmp_path / "parado.png").stat().st_size > 20_000
