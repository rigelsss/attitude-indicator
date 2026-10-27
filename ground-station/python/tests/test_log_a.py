"""Testes de horizonte.log_a e do analisar_a.py: o fluxo de atitude do ESP A."""

import os

import pytest

from horizonte import log_a as A


def dt(dmin, dmed, dmax, n=20, atraso=110, fmed=55, fmax=100, amb=0, desc=0, rein=0):
    return (f"      dt {dmin:.2f}/{dmed:.2f}/{dmax:.2f} ms ({n}) | atraso máx {atraso} µs | filtro méd/máx "
            f"{fmed}/{fmax} µs | ambíguos {amb} | descartados {desc} | reinícios {rein}")


LOG = ["[A] firmware: compilado em 2026-10-22 10:23:29 (tasks)",
       "[att] roll   -1.46 pitch   +0.06 | acc   -1.36   -0.06 | giro   -1.46   +0.06 | |a| 1.001 w 1.00 | 27.74 °C",
       dt(5.01, 5.01, 5.01, n=1, atraso=112, fmed=82, fmax=82),
       dt(5.00, 5.01, 5.01, atraso=114, fmed=52, fmax=80),
       dt(5.01, 5.01, 5.01, atraso=113, fmed=56, fmax=100),
       dt(4.99, 5.01, 5.02, atraso=124, fmed=60, fmax=161, amb=1),
       "      dt 0.00/0.00/0.00 ms (0) | atraso máx 0 µs | filtro méd/máx 0/0 µs | ambíguos 0 | descartados 0 | reinícios 0",
       "[r] qualquer outra linha"]


def test_le_as_janelas_e_o_carimbo():
    log = A.ler(LOG)
    assert log.carimbo == "2026-10-22 10:23:29"
    assert len(log.janelas) == 4                      # a janela sem amostra (n = 0) fica fora
    j = log.janelas[3]
    assert (j["dt_min"], j["dt_max"], j["n"], j["atraso"], j["filtro_max"], j["ambiguos"]) == (4.99, 5.02, 20, 124, 161, 1)
    assert log.janelas[0]["t_s"] == pytest.approx(0.00501)
    assert log.janelas[-1]["t_s"] == pytest.approx((1 + 60) * 0.00501)


def test_resumo_do_jitter():
    r = A.resumo(A.ler(LOG))
    assert r["janelas"] == 4 and r["amostras"] == 61
    assert r["dt_medio"] == pytest.approx(5.01) and (r["dt_min"], r["dt_max"]) == (4.99, 5.02)
    # pico a pico só das janelas com mais de uma amostra: 10, 0 e 30 µs
    assert r["pico_us"]["n"] == 3 and r["pico_us"]["max"] == pytest.approx(30.0)
    assert r["pico_us"]["mediana"] == pytest.approx(10.0)
    # desvio de 5 ms: 10, 10, 10 e 20 µs; até 10 µs em 3 das 4 janelas
    assert r["desvio_us"]["max"] == pytest.approx(20.0) and r["janelas_ate_10us"] == 3
    assert r["atraso_us"]["max"] == 124 and r["filtro_max_us"]["max"] == 161
    assert r["filtro_med_us"] == pytest.approx((82 + 52 * 20 + 56 * 20 + 60 * 20) / 61)
    assert (r["ambiguos"], r["descartados"], r["reinicios"]) == (1, 0, 0)
    assert A.resumo(A.ler(["nada"])) is None


def test_analisar_a_figuras_resumo_e_comparacao(tmp_path):
    pytest.importorskip("matplotlib")
    import analisar_a
    caminhos = []
    for nome in ("device-monitor-261022-103255", "device-monitor-261021-173359"):
        p = tmp_path / f"{nome}.log"
        p.write_text("\n".join(LOG), encoding="utf-8")
        caminhos.append(str(p))
    resumos = []
    for c in caminhos:
        pasta, r = analisar_a.analisar_log(c)
        resumos.append(r)
        for f in analisar_a.FIGURAS + ("resumo.md",):
            assert os.path.getsize(os.path.join(pasta, f)) > 1000, f
    texto = open(os.path.join(pasta, "resumo.md"), encoding="utf-8").read()
    assert "dt médio: **5,01 ms**" in texto and "| pico a pico do dt (máx. − mín.) | µs | 0 | 10 |" in texto
    destino = tmp_path / "comparacao.md"
    analisar_a.comparar(resumos, str(destino))
    comp = destino.read_text(encoding="utf-8")
    assert "| pico a pico do dt, mediana / máx. (µs) | 10 / 30 | 10 / 30 |" in comp
    vazio = tmp_path / "vazio.log"
    vazio.write_text("sem fluxo a\n", encoding="utf-8")
    with pytest.raises(ValueError):
        analisar_a.analisar_log(str(vazio))
