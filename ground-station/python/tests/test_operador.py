"""Testes de horizonte.operador sem rede: envio falso, relógio falso, saída capturada."""

import json

from horizonte.comandos import RastreadorComandos
from horizonte.operador import CONFIRMAR_MS, Operador


class Mesa:
    def __init__(self, conectado=True):
        self.agora = 0.0
        self.enviados = []
        self.saida = []
        self.conectado = conectado
        self.op = Operador(self.enviar, self.saida.append, pin="2468", relogio_ms=lambda: self.agora,
                           rastreador=RastreadorComandos(primeiro_rid=100))

    def enviar(self, texto):
        if not self.conectado:
            return False
        self.enviados.append(json.loads(texto))
        return True

    def ultima(self):
        return self.saida[-1]


def ev(cmd, state, rid=None, seq=None, origin="pc", **extra):
    d = {"type": "cmd", "cmd": cmd, "state": state, "origin": origin}
    if rid is not None:
        d["rid"] = rid
    if seq is not None:
        d["seq"] = seq
    d.update(extra)
    return d


def test_zerar_atitude_ate_o_fim():
    m = Mesa()
    assert m.op.linha("z")
    assert m.enviados == [{"cmd": "zero_attitude", "pin": "2468", "origin": "pc", "rid": 100}]
    assert m.ultima() == "[cmd] #100 zerar atitude: pedido"
    m.agora = 12
    m.op.ao_cmd(ev("zero_attitude", "sent", 100, 41850), False)
    assert m.ultima() == "[cmd] #100 zerar atitude: enviado ao A (12 ms, seq 41850)"
    m.agora = 41
    m.op.ao_cmd(ev("zero_attitude", "done", 100, 41850), False)
    assert m.ultima() == "[cmd] #100 zerar atitude: concluído (41 ms)"


def test_calibracao_pede_confirmacao():
    m = Mesa()
    m.op.linha("g")
    assert m.enviados == [] and "Digite g de novo" in m.ultima() and "caixa parada" in m.ultima()
    m.agora = CONFIRMAR_MS + 1          # passou do prazo: pede de novo
    m.op.linha("g")
    assert m.enviados == []
    m.agora += 1000
    m.op.linha("g")                     # confirmada
    assert [e["cmd"] for e in m.enviados] == ["calibrate_gyro"]
    m.op.linha("m")
    assert "gire a caixa" in m.ultima() and "parada" not in m.ultima()   # a coleta do magnetômetro é girando
    m.op.linha("g")                     # outra tecla no meio: não confirma o m
    assert [e["cmd"] for e in m.enviados] == ["calibrate_gyro"]


def test_taxa_conferida_e_forcada():
    m = Mesa()
    m.op.linha("t 30")
    assert m.enviados == [] and "não enviado: taxa inválida" in m.ultima()
    m.op.linha("t! 30")
    assert m.enviados[-1]["arg"] == 30
    m.op.linha("t 25")
    assert m.enviados[-1]["arg"] == 25


def test_rajada_com_um_busy():
    m = Mesa()
    m.op.linha("rajada")
    rids = [e["rid"] for e in m.enviados]
    assert rids == [100, 101, 102, 103, 104]
    m.op.ao_cmd(ev("zero_attitude", "busy", 104, 0), False)
    assert m.ultima() == "[cmd] #104 zerar atitude: ocupado: o B já tem 4 comandos em curso (0 ms)"
    assert m.op.rastreador.pedido(103).estado == "pedido"   # os outros não mudam


def test_sem_conexao():
    m = Mesa(conectado=False)
    m.op.linha("z")
    assert m.ultima() == "[cmd] #100 zerar atitude: não enviado: sem conexão"
    assert m.op.rastreador.pedido(100).estado == "nao_enviado"


def test_envio_que_falhou_na_rede():
    m = Mesa()
    m.op.linha("0")
    texto = json.dumps(m.enviados[0])
    m.op.ao_envio_falhou(f"{texto} (sent 1000 (going away))")
    assert m.op.rastreador.pedido(100).estado == "nao_enviado"
    assert m.ultima() == "[cmd] #100 zerar altitude: não enviado: a conexão caiu no envio"


def test_recusa_do_b_com_motivo():
    m = Mesa()
    m.op.linha("pin 0000")
    m.op.linha("z")
    m.op.ao_cmd(ev("zero_attitude", "refused", 100, origin=None, reason="PIN incorreto"), False)
    assert m.ultima() == "[cmd] #100 zerar atitude: recusado pelo B (0 ms): PIN incorreto"


def test_outras_origens_uma_vez_por_estado_e_sem_replay():
    m = Mesa()
    m.op.ao_cmd(ev("zero_attitude", "done", seq=7, origin="console"), True)      # recente, depois do hello
    assert m.saida == []
    m.op.ao_cmd(ev("zero_altitude", "sent", seq=8, origin="página"), False)
    m.op.ao_cmd(ev("zero_altitude", "sent", seq=8, origin="página"), False)      # reenvio do B
    m.op.ao_cmd(ev("zero_altitude", "no_response", seq=8, origin="página"), False)
    assert m.saida == ["[cmd] (página) zerar altitude, seq 8: enviado ao A",
                       "[cmd] (página) zerar altitude, seq 8: sem resposta do A (3 envios)"]
    m.op.ao_cmd(ev("zero_attitude", "done", rid=9999, seq=9), False)              # outro cliente Python
    assert len(m.saida) == 2


def test_sem_eco_no_tick_e_lista():
    m = Mesa()
    m.op.linha("z")
    m.agora = 3000
    m.op.tick()
    assert m.ultima() == "[cmd] #100 zerar atitude: sem resposta do B em 3 s"
    m.op.linha("s")
    assert m.ultima().startswith("[s] #100 zerar atitude: sem resposta do B em 3 s em 3000 ms")


def test_tempos_pela_chegada_e_nao_pelo_tratamento():
    # Revisão de 25/10/2026: um evento que chegou aos 20 ms e só foi tratado aos 5 000 ms (registro atrasado)
    m = Mesa()
    m.op.linha("z")
    m.agora = 5000
    m.op.ao_cmd(ev("zero_attitude", "done", 100, 7), False, chegada_ms=20)
    p = m.op.rastreador.pedido(100)
    assert p.estado == "done" and p.tempos["done"] == 20
    assert m.ultima() == "[cmd] #100 zerar atitude: concluído (20 ms)"


def test_sem_eco_pelo_relogio_do_registro():
    # Com a resposta ainda na fila, o tick usa o relógio do que já foi tratado: o pedido não vence
    m = Mesa()
    m.op.linha("z")
    m.agora = 9000
    m.op.tick(agora_ms=100)
    assert m.op.rastreador.pedido(100).estado == "pedido"
    m.op.tick(agora_ms=3000)
    assert m.op.rastreador.pedido(100).estado == "sem_eco"
