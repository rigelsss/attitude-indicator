"""Testes de horizonte.series: buracos, janelas, contagens, perdas, estatísticas, sessões e orçamento."""

import math

import pytest

from conftest import linha_evento, linha_tel
from horizonte import series as S


def pts(*linhas):
    return [S.ponto_tel(l) for l in linhas]


def test_campo_vazio_vira_buraco_e_nao_zero():
    p = S.ponto_tel(linha_tel(1.0, 1, roll="", pitch="2.5", valid="13"))
    assert math.isnan(p["roll"]) and p["pitch"] == 2.5


def test_dado_nao_atual_vira_buraco():
    p = S.ponto_tel(linha_tel(1.0, 1, cur="0", roll="1", pitch="2", hdg="90", alt="0.5", valid="13", delay_ms="3"))
    assert all(math.isnan(p[c]) for c in ("roll", "pitch", "hdg", "alt"))
    assert p["delay_ms"] == 3.0                  # o atraso do link continua valendo


@pytest.mark.parametrize("valid, buracos", [
    (13, set()), (12, {"roll", "pitch"}), (5, {"hdg"}), (9, {"alt"}), (0, {"roll", "pitch", "hdg", "alt"}),
])
def test_cada_flag_so_no_seu_dado(valid, buracos):
    p = S.ponto_tel(linha_tel(1.0, 1, roll="1", pitch="2", hdg="90", alt="0.5", valid=str(valid)))
    assert {c for c in ("roll", "pitch", "hdg", "alt") if math.isnan(p[c])} == buracos


def test_lacuna_e_troca_de_sessao_inserem_buraco():
    p = pts(linha_tel(0.0, 1, roll="1", valid="1"), linha_tel(0.03, 2, roll="2", valid="1"),
            linha_tel(2.0, 3, roll="3", valid="1"),                     # 1,97 s depois: lacuna
            linha_tel(2.03, 4, sessao=2, roll="4", valid="1"))          # outra sessão
    xs, ys = S.com_buracos(p, "roll")
    assert len(xs) == 6
    assert [math.isnan(y) for y in ys] == [False, False, True, False, True, False]
    assert xs[2] == pytest.approx(1.015)        # o buraco fica no meio da lacuna


def test_janela_corta_pelos_ultimos_segundos():
    j = S.Janela(30)
    j.acrescentar(pts(*(linha_tel(t, t) for t in range(0, 60))), rtt=[(t, 5.0) for t in range(0, 60)])
    j.cortar(59.0)
    assert j.pontos[0]["t"] == 29.0 and len(j.rtt) == 31
    j.limpar()
    assert not j.pontos and not j.rtt


def test_rtt_do_csv_de_eventos():
    ev = [linha_evento(1.0, "rtt", "12.5"), linha_evento(1.5, "telemetria", "ok"), linha_evento(2.0, "rtt", "x")]
    assert S.pontos_rtt(ev) == [(1.0, 12.5)]


def test_por_segundo_com_zero_nos_vazios():
    p = pts(*(linha_tel(0.1 + 0.25 * i, i) for i in range(4)), linha_tel(3.2, 9))
    xs, ys = S.por_segundo(p)
    assert xs == [0, 1, 2, 3] and ys == [4, 0, 0, 1]


def test_perdas_por_intervalo_sem_negativo_no_reinicio():
    p = pts(linha_tel(0.0, 1, lost="10"), linha_tel(0.5, 2, lost="12"), linha_tel(1.5, 3, lost="15"),
            linha_tel(2.5, 4, lost="0"), linha_tel(3.5, 5, lost="1"))   # o B reiniciou entre 1,5 e 2,5 s
    xs, ys = S.perdas_por_intervalo(p, 1.0)
    assert xs == [0.0, 1.0, 2.0, 3.0] and ys == [2, 3, 0, 1]


def test_perdas_por_minuto_pelo_seq_com_volta():
    p = pts(linha_tel(0.0, 1, seq="65530", lost="0"), linha_tel(30.0, 2, seq="1000", lost="5"),
            linha_tel(70.0, 3, seq="3000", lost="5"),
            linha_tel(80.0, 4, seq="50000", lost="7"))   # salto de 47 000: A reiniciado, não conta como enviados
    m = S.perdas_por_minuto(p)
    assert m[0][:3] == (0, 5, 1006)            # 65530 → 1000 são 1006 pacotes
    assert m[1][:3] == (1, 2, 2000)            # 1000 → 3000; o salto do reinício fica de fora
    assert m[0][3] == pytest.approx(100 * 5 / 1006)


def test_intervalos_so_dentro_da_sessao():
    p = pts(linha_tel(0.0, 1), linha_tel(0.035, 2), linha_tel(5.0, 3, sessao=2), linha_tel(5.03, 4, sessao=2))
    assert S.intervalos_ms(p) == pytest.approx([35.0, 30.0])


def test_estatisticas():
    st = S.estatisticas([float(x) for x in range(1, 101)] + [math.nan])
    assert st["n"] == 100 and st["min"] == 1 and st["max"] == 100
    assert st["mediana"] == pytest.approx(50.5) and st["p95"] == pytest.approx(95.05)
    assert st["p99"] == pytest.approx(99.01)
    assert S.estatisticas([math.nan]) is None


def test_sessoes_e_periodos_sem_telemetria():
    ev = [linha_evento(0, "conectado"), linha_evento(10, "telemetria", "sem_telemetria"),
          linha_evento(11, "telemetria", "ok"), linha_evento(20, "telemetria", "sem_telemetria"),
          linha_evento(24, "desconectado", "conexão perdida (sem quadro de fechamento)"),
          linha_evento(25, "conectado", sessao=2)]
    s = S.sessoes(ev)
    assert [(x["inicio"], x["fim"]) for x in s] == [(0, 24), (25, None)]
    assert s[0]["motivo"].startswith("conexão perdida")
    assert S.periodos_sem_telemetria(ev) == [(10, 11), (20, 24)]


def test_orcamento_de_latencia():
    st = lambda med, p99: {"mediana": med, "p99": p99}
    partes = S.orcamento_latencia(st(1000, 1100), st(1, 10), st(4, 140), st(26, 220))
    valores = [(round(m, 2), round(p, 2)) for _, m, p, _ in partes]
    assert valores == [(1.0, 1.1), (10.0, 20.0), (1, 10), (17.5, 35.0), (4, 140), (13.0, 110.0)]
    sem = S.orcamento_latencia(None, None, None, None)
    assert math.isnan(sem[0][1]) and sem[1][1] == 10.0   # as estimadas continuam



def test_perdas_por_minuto_reconhece_o_reinicio_do_a_pelo_t():
    # Revisão de 27/10/2026: 50000 → 5 dá 15 541 pela volta de 16 bits, abaixo do limiar; o t do A voltou
    p = pts(linha_tel(0.0, 1, seq="50000", t="900000", lost="0"), linha_tel(10.0, 2, seq="5", t="200", lost="0"),
            linha_tel(20.0, 3, seq="55", t="1200", lost="1"))
    m = S.perdas_por_minuto(p)
    assert m[0][:3] == (0, 1, 50)               # só os 50 depois do reinício
    # a volta legítima do contador continua contando (o t avança)
    p = pts(linha_tel(0.0, 1, seq="65530", t="1000", lost="0"), linha_tel(1.0, 2, seq="20", t="2000", lost="0"))
    assert S.perdas_por_minuto(p)[0][2] == 26


def test_periodo_ws_pelo_tb():
    p = pts(*(linha_tel(i * 0.05, i + 1, tb=str(1000 + i * 50)) for i in range(10)))
    assert S.periodo_ws_ms(p) == 50             # 20 Hz
    p = pts(linha_tel(0, 1, tb="1000"), linha_tel(0.04, 3, tb="1070"), linha_tel(0.08, 4, tb="1105"))
    assert S.periodo_ws_ms(p) == 35             # o salto de n (telemetria pulada) não entra
    assert S.periodo_ws_ms(pts(linha_tel(0, 1, tb=""))) is None


def test_orcamento_com_o_periodo_medido():
    partes = S.orcamento_latencia(None, None, None, None, periodo_ws=50.0)
    assert partes[3][1:3] == (25.0, 50.0) and "medido" in partes[3][3]
    assert "supondo 30 Hz" in S.orcamento_latencia(None, None, None, None)[3][3]
