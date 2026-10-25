"""Testes de horizonte.conexao."""

from horizonte.conexao import AgendaReconexao, VigiaTelemetria


def test_vigia_estados():
    v = VigiaTelemetria(agora_ms=0)
    assert v.estado(500) == "aguardando"
    v.tel(600)
    assert v.estado(1599) == "ok"
    assert v.estado(1600) == "sem_telemetria"     # 1 s sem tel
    assert v.estado(5599) == "sem_telemetria"
    assert v.estado(5600) == "abandonar"          # 5 s sem tel
    v.tel(5700)
    assert v.estado(5800) == "ok"


def test_vigia_reassociacao_curta_nao_abandona():
    # Reassociação do PC de ~4 s (docs/cliente-python.md): sem telemetria, mas o socket fica
    v = VigiaTelemetria(agora_ms=0)
    v.tel(100)
    assert v.estado(4200) == "sem_telemetria"


def test_vigia_socket_sem_nenhuma_tel_e_abandonado():
    v = VigiaTelemetria(agora_ms=1000)
    assert v.estado(5999) == "aguardando"
    assert v.estado(6000) == "abandonar"


def test_agenda_reconexao():
    a = AgendaReconexao(intervalo_ms=2000)
    assert a.tentar_agora(0) and a.espera_ms(0) == 0
    a.iniciou(0)
    assert not a.tentar_agora(1999) and a.espera_ms(1500) == 500
    assert a.tentar_agora(2000)
    a.iniciou(2000)
    assert a.tentativas == 2
    assert a.tentar_agora(9000) and a.espera_ms(9000) == 0   # tentativa demorada: a próxima vai logo


def test_aviso_falhas():
    from horizonte.conexao import AvisoFalhas
    a = AvisoFalhas(lembrete_ms=10_000)
    assert a.falha("recusada", 0) == "recusada"            # a primeira, na hora
    assert a.falha("recusada", 2000) is None               # o mesmo motivo: silêncio
    assert a.falha("recusada", 10_000) == "ainda sem conexão: 3 tentativas em 10 s (recusada)"
    assert a.falha("recusada", 12_000) is None
    assert a.falha("tempo esgotado", 14_000) == "tempo esgotado"   # motivo novo, na hora
    assert a.conectou() == "depois de 5 tentativa(s) sem conexão"
    assert a.conectou() is None                            # sem falhas antes
