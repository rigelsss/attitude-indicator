"""Testes de horizonte.metricas."""

import pytest

from horizonte.metricas import ContadorN, EstatIntervalo, EstimadorAtraso, dif_seq16, percentil


def test_dif_seq16_com_volta():
    assert dif_seq16(5, 4) == 1
    assert dif_seq16(0, 65535) == 1
    assert dif_seq16(2, 65534) == 4
    assert dif_seq16(7, 7) == 0
    assert dif_seq16(4, 5) == 65535   # para trás: o chamador decide (atrasado, reinício)


def test_percentil():
    assert percentil([], 50) is None
    assert percentil([10], 99) == 10
    assert percentil([0, 10, 20, 30, 40], 50) == 20
    assert percentil([0, 10], 25) == pytest.approx(2.5)


def test_estat_intervalo():
    e = EstatIntervalo()
    assert e.resumo() is None
    for t in (0, 33, 66, 100, 133, 200):
        e.chegada(t)
    r = e.resumo()
    assert r["n"] == 5 and r["min"] == 33 and r["max"] == 67
    assert r["media"] == pytest.approx(40.0)
    assert e.resumo() is None              # a janela fechou
    e.chegada(233)                         # a última chegada continua valendo
    assert e.resumo()["min"] == 33


def test_estat_intervalo_recomeca_na_conexao_nova():
    e = EstatIntervalo()
    e.chegada(0)
    e.chegada(33)
    e.recomecar()
    e.chegada(9000)                        # a lacuna da reconexão não entra
    e.chegada(9033)
    r = e.resumo()
    assert r["n"] == 2 and r["max"] == 33


def test_contador_n():
    c = ContadorN()
    assert c.chegada(10) == 0              # a primeira não conta
    assert c.chegada(11) == 0
    assert c.chegada(14) == 2              # 12 e 13 pulados pelo B
    assert (c.recebidas, c.puladas, c.reinicios) == (3, 2, 0)
    assert c.chegada(1) == 0               # B reiniciado: sem perda falsa
    assert c.reinicios == 1
    c.recomecar()                          # conexão nova: o que o B montou no meio não é perda
    assert c.chegada(500) == 0
    assert c.puladas == 2


def test_estimador_atraso_pelo_minimo():
    e = EstimadorAtraso(faixa_ms=1000, faixas=10)
    # Offset base 1000 ms (relógios diferentes); a segunda chega 7 ms atrasada
    assert e.chegada(0, 1000) == 0
    assert e.chegada(33, 1040) == 7
    assert e.chegada(66, 1066) == 0
    # Um atraso grande não desloca o mínimo
    assert e.chegada(100, 1350) == 250


def test_estimador_atraso_acompanha_a_deriva():
    e = EstimadorAtraso(faixa_ms=1000, faixas=3)
    e.chegada(0, 1000)                     # offset 1000 na faixa 1
    # A deriva leva o offset a 1005; depois de 3 faixas o mínimo antigo sai da janela
    for s in range(1, 6):
        atraso = e.chegada(s * 1000, s * 1000 + 1005)
    assert atraso == 0


def test_estimador_recomeca_com_carimbo_para_tras():
    e = EstimadorAtraso()
    e.chegada(50_000, 60_000)              # offset 10 000
    assert e.chegada(10, 61_000) == 0      # B reiniciado: offset novo, sem atraso falso


def test_estimador_parametros_invalidos():
    with pytest.raises(ValueError):
        EstimadorAtraso(faixa_ms=0)
