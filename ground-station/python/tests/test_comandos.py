"""Testes de horizonte.comandos: casamento pelo rid."""

import pytest

from horizonte.comandos import MAX_PEDIDOS, RastreadorComandos


def ev(cmd, state, rid=None, seq=None, origin="pc", reason=None):
    d = {"type": "cmd", "cmd": cmd, "state": state}
    for k, v in (("rid", rid), ("seq", seq), ("origin", origin), ("reason", reason)):
        if v is not None:
            d[k] = v
    return d


def test_caminho_completo_com_tempos():
    r = RastreadorComandos(primeiro_rid=10)
    p = r.pedir("calibrate_gyro", None, 1000)
    assert p.rid == 10
    assert r.evento(ev("calibrate_gyro", "sent", 10, 41850), 1012) is p
    assert p.seq == 41850 and not r.pendentes()
    assert r.evento(ev("calibrate_gyro", "sent", 10, 41850), 1312) is None   # reenvio do B: nada muda
    r.evento(ev("calibrate_gyro", "accepted", 10, 41850), 1350)
    r.evento(ev("calibrate_gyro", "done", 10, 41850), 5350)
    assert p.final and p.estado == "done" and not p.tardio
    assert p.tempos == {"sent": 12, "accepted": 350, "done": 4350}


def test_rid_da_volta_pula_o_zero():
    r = RastreadorComandos(primeiro_rid=65535)
    assert [r.pedir("zero_attitude", None, 0).rid for _ in range(3)] == [65535, 1, 2]
    with pytest.raises(ValueError):
        RastreadorComandos(primeiro_rid=0)


def test_busy_nao_cai_no_comando_antigo_de_seq_zero():
    # Revisão: um comando anterior com o seq válido 0 e um busy novo (também seq 0)
    r = RastreadorComandos(primeiro_rid=1)
    a = r.pedir("zero_attitude", None, 0)
    r.evento(ev("zero_attitude", "sent", a.rid, 0), 5)
    r.evento(ev("zero_attitude", "done", a.rid, 0), 40)
    b = r.pedir("zero_attitude", None, 100)
    assert r.evento(ev("zero_attitude", "busy", b.rid, 0), 105) is b
    assert a.estado == "done" and b.estado == "busy" and b.seq is None
    assert not r.pendentes()


def test_resultado_tardio_nao_conclui_o_pedido_novo():
    # Revisão: A vence (sem_eco), B do mesmo comando é pedido, chega o done atrasado de A
    r = RastreadorComandos(primeiro_rid=1)
    a = r.pedir("zero_attitude", None, 0)
    assert r.vencer(3000) == [a]
    b = r.pedir("zero_attitude", None, 3100)
    assert r.evento(ev("zero_attitude", "done", a.rid, 77), 3200) is a
    assert a.estado == "done" and a.tardio
    assert b.estado == "pedido"


def test_replay_de_outra_execucao_e_ignorado():
    # Comandos recentes depois do hello: rids que esta execução não usou (ou de outra origem, sem rid)
    r = RastreadorComandos(primeiro_rid=500)
    p = r.pedir("zero_attitude", None, 0)
    assert r.evento(ev("zero_attitude", "done", 12, 9), 1) is None          # outra execução
    assert r.evento(ev("zero_attitude", "done", 501, 9), 1) is None         # rid ainda não usado
    assert r.evento(ev("zero_attitude", "sent", None, 9, origin="página"), 1) is None
    assert p.estado == "pedido"


def test_replay_da_mesma_execucao_depois_da_reconexao():
    r = RastreadorComandos(primeiro_rid=7)
    p = r.pedir("calibrate_mag", None, 0)
    r.evento(ev("calibrate_mag", "accepted", 7, 300), 20)
    # Conexão caiu; o done saiu enquanto o cliente estava fora e volta no replay depois do hello
    assert r.evento(ev("calibrate_mag", "done", 7, 300), 90_000) is p
    assert p.estado == "done"
    assert r.evento(ev("calibrate_mag", "done", 7, 300), 92_000) is None   # replay repetido: nada muda


def test_estado_intermediario_depois_do_fim_e_ignorado():
    r = RastreadorComandos(primeiro_rid=3)
    p = r.pedir("zero_altitude", None, 0)
    r.evento(ev("zero_altitude", "done", 3, 1), 10)
    assert r.evento(ev("zero_altitude", "accepted", 3, 1), 20) is None
    assert p.estado == "done"


def test_recusa_no_b_pelo_rid():
    r = RastreadorComandos(primeiro_rid=40)
    a = r.pedir("zero_altitude", None, 0)
    b = r.pedir("zero_altitude", None, 1)
    q = r.evento(ev("zero_altitude", "refused", b.rid, origin=None, reason="PIN incorreto"), 15)
    assert q is b and b.motivo == "PIN incorreto"
    assert a.estado == "pedido"   # a recusa não cai no mais antigo do mesmo comando


def test_sem_eco():
    r = RastreadorComandos(primeiro_rid=1, sem_eco_ms=3000)
    p = r.pedir("zero_attitude", None, 0)
    assert r.vencer(2999) == []
    assert r.vencer(3000) == [p] and p.estado == "sem_eco"
    assert r.vencer(9000) == []   # vence uma vez só


def test_lembra_um_numero_limitado_de_pedidos():
    r = RastreadorComandos(primeiro_rid=1)
    primeiro = r.pedir("zero_attitude", None, 0)
    r.evento(ev("zero_attitude", "done", primeiro.rid, 1), 1)
    for i in range(MAX_PEDIDOS):
        r.pedir("zero_attitude", None, 2 + i)
    assert r.pedido(primeiro.rid) is None          # o terminado mais antigo saiu
    assert len(r.pendentes()) == MAX_PEDIDOS       # os sem resposta ficam
