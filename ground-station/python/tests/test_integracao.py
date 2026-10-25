"""Integração: cliente (rede + registro) contra o simulador do B em localhost (docs/cliente-python.md, passo 3).
Usa tempo real (de 3 a 12 s por teste); exige a biblioteca websockets."""

import csv
import json
import time

import pytest

pytest.importorskip("websockets")

from horizonte.comandos import RastreadorComandos                  # noqa: E402
from horizonte.mensagens import montar_comando                     # noqa: E402
from horizonte.operador import Operador                            # noqa: E402
from horizonte.rede import ClienteWS                               # noqa: E402
from horizonte.registro import Processador                         # noqa: E402
from horizonte.simulador import Falhas, SimuladorB, SimuladorEmThread  # noqa: E402


class Sessao:
    """Simulador, rede e registro rodando juntos; guarda os eventos na ordem."""

    def __init__(self, tmp_path, falhas=None, sem_a=False):
        self.sim = SimuladorEmThread(SimuladorB(falhas or Falhas(), semente=1, sem_a=sem_a))
        self.eventos = []
        self.saida = []      # o que o operador imprimiria no terminal
        self.tmp = tmp_path

    def __enter__(self):
        self.sim.__enter__()
        self.rede = ClienteWS(self.sim.url)
        self.op = Operador(self.rede.enviar, self.saida.append, rastreador=RastreadorComandos(primeiro_rid=200))
        self.reg = Processador(self.rede.saida, str(self.tmp), ao_evento=lambda t, d: self.eventos.append((t, d)),
                               ao_cmd=self.op.ao_cmd)
        self.reg.iniciar()
        self.rede.iniciar()
        return self

    def __exit__(self, *exc):
        self.rede.parar()
        self.reg.parar()
        self.sim.__exit__(*exc)

    def esperar(self, cond, timeout):
        fim = time.monotonic() + timeout
        while time.monotonic() < fim:
            if cond():
                return True
            time.sleep(0.05)
        return False

    def estados(self):
        return [d for t, d in self.eventos if t == "telemetria"]

    def tel(self):
        with open(self.reg.caminho_tel, newline="", encoding="utf-8") as f:
            return list(csv.DictReader(f))


def test_b_saudavel(tmp_path):
    with Sessao(tmp_path) as s:
        time.sleep(3.0)
    linhas = s.tel()
    assert len(linhas) >= 80                              # 30 Hz por ~3 s
    assert s.rede.sessao == 1 and s.reg.invalidas == 0
    assert "ok" in s.estados() and "sem_telemetria" not in s.estados()
    assert s.reg.contador_n.puladas == 0
    assert all(r["tb"] and r["n"] and r["loop_us"] and r["roll"] for r in linhas)
    ns = [int(r["n"]) for r in linhas]
    assert ns == list(range(ns[0], ns[0] + len(ns)))     # nenhuma pulada
    assert any(t == "hello" for t, _ in s.eventos)


def test_queda_do_tcp_reconecta(tmp_path):
    with Sessao(tmp_path, Falhas(queda_s=1.5)) as s:
        assert s.esperar(lambda: s.rede.sessao >= 3, 8)
    quedas = [d for t, d in s.eventos if t == "desconectado"]
    assert any("sem quadro de fechamento" in d for d in quedas)
    sessoes = {int(r["sessao"]) for r in s.tel()}
    assert {1, 2} <= sessoes                              # telemetria gravada nas duas


def test_reassociacao_curta_nao_derruba(tmp_path):
    # O envio para 300 ms a cada 1 s (o PC da bancada de 24/10, em escala menor): nada cai nem fica sem telemetria
    with Sessao(tmp_path, Falhas(reassociar_s=1.0, reassociar_ms=300)) as s:
        time.sleep(4.0)
    assert s.rede.sessao == 1
    assert "sem_telemetria" not in s.estados()


def test_b_travado_por_2s_volta_sem_reconectar(tmp_path):
    with Sessao(tmp_path, Falhas(travar_apos_s=1.0, travar_por_s=2.0)) as s:
        time.sleep(4.5)
    assert s.estados()[-3:] == ["ok", "sem_telemetria", "ok"]
    assert s.rede.sessao == 1


def test_pausa_de_telemetria_com_ping_respondido_nao_derruba(tmp_path):
    # Revisão de 25/10/2026: como a trava de memória do B (telemetria pausada, o B respondendo o ping). Antes, o
    # cliente abandonava o socket aos 5 s sem tel, apesar dos pongs
    with Sessao(tmp_path, Falhas(travar_apos_s=1.0, travar_por_s=7.0)) as s:
        time.sleep(1.5)
        rtt_antes = sum(1 for t, _ in s.eventos if t == "rtt")
        assert s.esperar(lambda: s.estados()[-1:] == ["ok"] and len(s.estados()) >= 4, 10)
    assert s.rede.sessao == 1
    quedas = [d for t, d in s.eventos if t == "desconectado" and d != "cliente encerrado"]   # o fim do teste
    assert "abandonar" not in s.estados() and quedas == []
    assert s.estados()[-3:] == ["ok", "sem_telemetria", "ok"]
    assert sum(1 for t, _ in s.eventos if t == "rtt") >= rtt_antes + 5   # o ping seguiu respondido na pausa


def test_b_sumido_sem_ping_reconecta(tmp_path):
    # O B some (não lê o socket: ping sem resposta, nada chega): a conexão é dada como morta pelo ping, em ~5 s
    with Sessao(tmp_path, Falhas(sumir_apos_s=1.0, sumir_por_s=8.0)) as s:
        assert s.esperar(lambda: any(t == "desconectado" for t, _ in s.eventos), 9)
        assert s.esperar(lambda: s.rede.sessao >= 2, 4)
    motivo = [d for t, d in s.eventos if t == "desconectado"][0]
    assert motivo == "sem resposta do ping por 5 s"


def test_puladas_pelo_b_contadas_pelo_n(tmp_path):
    with Sessao(tmp_path, Falhas(pular=0.2)) as s:
        time.sleep(3.0)
    linhas = s.tel()
    ns = [int(r["n"]) for r in linhas]
    puladas = sum(int(r["puladas_ws"]) for r in linhas)
    assert puladas > 0 and puladas == s.reg.contador_n.puladas
    assert len(linhas) + puladas == ns[-1] - ns[0] + 1


def test_leitura_nao_para_com_a_thread_principal_ocupada(tmp_path):
    # A thread principal (onde ficam os gráficos) presa em conta por 3 s, segurando o GIL o quanto pode
    with Sessao(tmp_path) as s:
        time.sleep(1.0)
        fim = time.monotonic() + 3.0
        x = 0
        while time.monotonic() < fim:
            x += 1
        time.sleep(0.5)
    pc = [int(r["pc_ns"]) / 1e6 for r in s.tel()]
    maior = max(b - a for a, b in zip(pc, pc[1:]))
    # O que importa é ficar longe dos 2 s em que o B derruba um cliente parado e do 1 s de "sem telemetria". Aqui o
    # simulador roda no mesmo processo e também espera o GIL, então parte da lacuna nasce no "B": no PC, em
    # 25/10/2026, de 33 ms (sem carga) a ~170 ms (com outro teste rodando ao lado)
    assert maior < 500
    assert "sem_telemetria" not in s.estados()
    assert s.rede.descartadas == 0


def test_comando_e_recusa_com_rid(tmp_path):
    with Sessao(tmp_path) as s:
        assert s.esperar(lambda: s.rede.conectado, 3)
        assert s.rede.enviar(montar_comando("zero_attitude", "2468", rid=5))
        assert s.rede.enviar(montar_comando("zero_altitude", "0000", rid=6))
        assert s.esperar(lambda: sum(1 for t, _ in s.eventos if t == "cmd") >= 3, 3)
    cmds = [json.loads(d) for t, d in s.eventos if t == "cmd"]
    assert [c["state"] for c in cmds if c.get("rid") == 5] == ["sent", "done"]
    recusa = [c for c in cmds if c.get("rid") == 6]
    assert recusa and recusa[0]["state"] == "refused" and recusa[0]["reason"] == "PIN incorreto"
    assert all(c["origin"] == "pc" for c in cmds if c.get("rid") == 5)


def test_rtt_pelo_ping(tmp_path):
    with Sessao(tmp_path) as s:
        assert s.esperar(lambda: any(t == "rtt" for t, _ in s.eventos), 4)
    rtts = [float(d) for t, d in s.eventos if t == "rtt"]
    assert 0 < min(rtts) < 100               # localhost


def test_operador_com_o_a(tmp_path):
    with Sessao(tmp_path) as s:
        assert s.esperar(lambda: s.rede.conectado, 3)
        s.op.linha("z")
        s.op.linha("t 25")
        s.op.linha("pin 0000")
        s.op.linha("0")
        s.op.linha("pin 2468")
        s.op.linha("g")
        s.op.linha("g")                      # confirmada
        # o primeiro g só pede a confirmação: a calibração é o rid 203
        assert s.esperar(lambda: s.op.rastreador.pedido(203) and s.op.rastreador.pedido(203).final, 8)
    estado = {p.rid: p.estado for p in s.op.rastreador.todos()}
    assert estado == {200: "done", 201: "done", 202: "refused", 203: "done"}
    assert any("#203 calibrar giroscópio: aceito pelo A" in l for l in s.saida)
    assert any("#202 zerar altitude: recusado pelo B" in l and "PIN incorreto" in l for l in s.saida)


def test_rajada_sem_o_a_um_busy(tmp_path):
    # Como o B sem o A: cada comando faz 3 envios e termina em no_response em ~0,9 s; o B tem 4 lugares
    with Sessao(tmp_path, sem_a=True) as s:
        assert s.esperar(lambda: s.rede.conectado, 3)
        s.op.linha("rajada")
        assert s.esperar(lambda: all(p.final for p in s.op.rastreador.todos()), 4)
    estado = {p.rid: p.estado for p in s.op.rastreador.todos()}
    assert sorted(estado.values()) == ["busy"] + ["no_response"] * 4
    ocupado = [rid for rid, e in estado.items() if e == "busy"]
    assert ocupado == [204]                  # o 5º; o busy (seq 0) não mexeu nos outros
    assert all(p.seq is not None for p in s.op.rastreador.todos() if p.estado == "no_response")
