"""Testes de horizonte.mensagens. Rodar: python -m pytest (a partir de ground-station/python)."""

import json

import pytest

from horizonte.mensagens import TEL_COLUNAS, MensagemInvalida, decodificar, linha_tel, montar_comando

TEL = {
    "type": "tel", "cur": True, "tb": 98765, "n": 2950,
    "t": 123456, "seq": 4321,
    "roll": -3.2, "pitch": 1.8, "hdg": 274.5,
    "gx": 0.1, "gy": -0.3, "gz": 0.0,
    "alt": None, "hpa": 1013.2, "temp": 26.4,
    "status": 55, "valid": 9, "loop_us": 112, "decl": -20.7,
    "link": {"state": "ok", "age_ms": 12, "delay_ms": 4, "rx_hz": 50, "lost": 3, "loss_pct": 0.06, "drops": 0},
    "alarm": {"kind": "none", "acked": False},
}


def test_decodifica_tipos():
    assert decodificar(json.dumps(TEL))[0] == "tel"
    tipo, d = decodificar('{"type":"hello","decl":-20.7,"pin":true,"rates":[10,20,25,50]}')
    assert tipo == "hello" and d["rates"] == [10, 20, 25, 50]
    assert decodificar('{"type":"cmd","cmd":"zero_attitude","state":"refused","reason":"PIN incorreto"}')[0] == "cmd"
    assert decodificar('{"type":"outro"}')[0] == "outro"   # tipo novo não derruba o cliente


@pytest.mark.parametrize("texto", ["{cmd:", "[1,2]", '{"cur":true}', '{"type":"tel","cur":true}',
                                    '{"type":"tel","cur":true,"tb":1,"n":1,"link":{}}', '{"type":"cmd","cmd":"x"}'])
def test_recusa_mensagens_invalidas(texto):
    with pytest.raises(MensagemInvalida):
        decodificar(texto)


def test_linha_tel_na_ordem_das_colunas():
    _, d = decodificar(json.dumps(TEL))
    linha = dict(zip(TEL_COLUNAS, linha_tel(d, 5_000_000_000, 1761400000.12345, 3)))
    assert len(linha) == len(TEL_COLUNAS)
    assert linha["pc_ns"] == 5_000_000_000 and linha["pc_unix"] == 1761400000.123 and linha["sessao"] == 3
    assert linha["n"] == 2950 and linha["tb"] == 98765 and linha["cur"] == 1
    assert linha["roll"] == -3.2 and linha["loop_us"] == 112
    assert linha["alt"] == ""                      # null vira vazio
    assert linha["link_state"] == "ok" and linha["delay_ms"] == 4 and linha["loss_pct"] == 0.06
    assert linha["alarm"] == "none" and linha["alarm_acked"] == 0


def test_linha_tel_sem_pacote_do_a():
    # Antes do primeiro pacote do A: só cur, tb, n, decl, link.state e alarm
    d = {"type": "tel", "cur": False, "tb": 10, "n": 1, "decl": -20.7, "link": {"state": "no_signal"},
         "alarm": {"kind": "link_lost", "acked": False}}
    linha = dict(zip(TEL_COLUNAS, linha_tel(d, 1, 0.0, 1)))
    assert linha["roll"] == "" and linha["seq"] == "" and linha["age_ms"] == ""
    assert linha["link_state"] == "no_signal" and linha["cur"] == 0


def test_montar_comando():
    assert json.loads(montar_comando("zero_attitude", "2468")) == {"cmd": "zero_attitude", "pin": "2468",
                                                                    "origin": "pc"}
    assert json.loads(montar_comando("set_rate", "2468", 25))["arg"] == 25
    assert json.loads(montar_comando("zero_altitude", "2468", rid=65535))["rid"] == 65535
    for ruim in (0, 65536, "7", True):
        with pytest.raises(ValueError):
            montar_comando("zero_altitude", "2468", rid=ruim)
    with pytest.raises(ValueError):
        montar_comando("reboot", "2468")
    for ruim in (30, "25", None, True):
        with pytest.raises(ValueError):
            montar_comando("set_rate", "2468", ruim)
