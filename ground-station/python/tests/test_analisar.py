"""Testes do analisar.py: figuras e resumo de uma gravação sintética."""

import os

import pytest

pytest.importorskip("matplotlib")

import analisar  # noqa: E402
from conftest import linha_evento, linha_tel  # noqa: E402


def gravacao(n=600, com_a=True):
    """20 s a 30 Hz; com o A: loop_us 1000 + i%100, delay i%10, 1 perdido a cada 100 pacotes do A."""
    linhas = []
    for i in range(n):
        campos = dict(atraso_bpc_ms=str(i % 20), rx_hz="50")
        if com_a:
            campos.update(roll="1", pitch="2", hdg="90", alt="0.1", valid="13", loop_us=str(1000 + i % 100),
                          delay_ms=str(i % 10), seq=str((i * 5 // 3) % 65536), lost=str(i * 5 // 300))
        else:
            campos.update(cur="0", link_state="no_signal")
        linhas.append(linha_tel(i / 30.0, i + 1, **campos))
    eventos = [linha_evento(0, "conectado"), linha_evento(0.5, "rtt", "10"), linha_evento(1.5, "rtt", "20"),
               linha_evento(n / 30.0, "desconectado", "cliente encerrado")]
    return linhas, eventos


def test_gera_figuras_e_resumo(gravar_csv):
    linhas, eventos = gravacao()
    pasta = analisar.analisar(gravar_csv("g", linhas, eventos))
    assert os.path.basename(pasta) == "analise-g"
    for f in analisar.FIGURAS + ("resumo.md",):
        assert os.path.getsize(os.path.join(pasta, f)) > 1000, f
    r = open(os.path.join(pasta, "resumo.md"), encoding="utf-8").read()
    assert "| fusão no A (loop_us) | µs | 600 | 1 000 | 1 050 |" in r          # n, mín., mediana (vírgula: 1 049,5)
    assert "| RTT PC ↔ B (ping) | ms | 2 | 10,0 | 15,0 |" in r
    assert "Conexões: 1" in r and "cliente encerrado" in r
    # lost sobe 1 a cada 60 telemetrias e o seq, 5/3 por telemetria: 9 perdidos em 998 enviados (0,90 %)
    assert "9 pacotes perdidos de 998 enviados pelo A (0,90 %)" in r


def test_so_o_b_sem_dados_do_a(gravar_csv):
    linhas, eventos = gravacao(com_a=False)
    pasta = analisar.analisar(gravar_csv("b", linhas, eventos))
    r = open(os.path.join(pasta, "resumo.md"), encoding="utf-8").read()
    assert "| fusão no A (loop_us) | µs | sem dados |" in r
    assert "0 pacotes perdidos de 0 enviados" in r


def test_nao_sobrescreve_uma_analise(gravar_csv):
    linhas, eventos = gravacao(n=60)
    caminho = gravar_csv("d", linhas, eventos)
    a, b = analisar.analisar(caminho), analisar.analisar(caminho)
    assert os.path.basename(a) == "analise-d" and os.path.basename(b) == "analise-d-2"


def test_sem_o_csv_de_eventos(gravar_csv):
    linhas, _ = gravacao(n=60)
    pasta = analisar.analisar(gravar_csv("e", linhas))
    assert "(sem o de eventos)" in open(os.path.join(pasta, "resumo.md"), encoding="utf-8").read()


def test_recusa_o_que_nao_e_csv_do_cliente(tmp_path):
    with pytest.raises(ValueError):
        analisar.analisar(str(tmp_path / "qualquer.csv"))
