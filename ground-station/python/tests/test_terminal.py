"""Testes de horizonte.terminal: o que o usuário digita."""

import pytest

from horizonte.terminal import interpretar


@pytest.mark.parametrize("linha, esperado", [
    ("z", ("cmd", "zero_attitude", None, True)),
    ("0", ("cmd", "zero_altitude", None, True)),
    ("  g ", ("cmd", "calibrate_gyro", None, True)),
    ("M", ("cmd", "calibrate_mag", None, True)),
    ("t 25", ("cmd", "set_rate", 25, True)),
    ("t! 30", ("cmd", "set_rate", 30, False)),
    ("rajada", ("rajada", 5)),
    ("pin 1234", ("pin", "1234")),
    ("pin", ("pin", None)),
    ("s", ("lista",)),
    ("r", ("resumo",)),
    ("?", ("ajuda",)),
    ("q", ("sair",)),
    ("", ("vazio",)),
    ("   ", ("vazio",)),
])
def test_interpretar(linha, esperado):
    assert interpretar(linha) == esperado


@pytest.mark.parametrize("linha", ["t", "t vinte", "t 25 50", "z 1", "pin 1 2", "reboot", "q agora"])
def test_erros(linha):
    tipo, msg = interpretar(linha)
    assert tipo == "erro" and msg
