"""Testes de horizonte.registro sem rede: nomes exclusivos, mensagens fora do contrato, erros no tratamento,
encerramento e falhas de gravação (revisões de 25/10/2026)."""

import csv
import json
import queue
import time

from horizonte.registro import Processador


def tel(num, **extra):
    d = {"type": "tel", "cur": True, "tb": 1000 + num, "n": num, "link": {"state": "ok"}}
    d.update(extra)
    return json.dumps(d)


def por(q, texto, sessao=1):
    q.put(("msg", time.monotonic_ns(), time.time(), sessao, texto))


def rodar(tmp_path, textos, nome="t"):
    q = queue.Queue()
    reg = Processador(q, str(tmp_path), nome=nome)
    reg.iniciar()
    for t in textos:
        por(q, t)
    assert reg.parar()
    return reg


def linhas(caminho):
    with open(caminho, newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def test_mesmo_nome_nao_sobrescreve(tmp_path):
    a = rodar(tmp_path, [tel(1), tel(2)], nome="x")
    b = rodar(tmp_path, [tel(1)], nome="x")
    c = rodar(tmp_path, [], nome="x")
    assert (a.nome, b.nome, c.nome) == ("x", "x-2", "x-3")
    assert len(linhas(a.caminho_tel)) == 2 and len(linhas(b.caminho_tel)) == 1


def test_nome_com_so_um_dos_arquivos_ocupado(tmp_path):
    # Sobrou só o de eventos de uma execução antiga: o par novo pula para -2, e o de telemetria não fica órfão
    (tmp_path / "y-eventos.csv").write_text("antigo", encoding="utf-8")
    reg = rodar(tmp_path, [], nome="y")
    assert reg.nome == "y-2"
    assert not (tmp_path / "y-tel.csv").exists()
    assert (tmp_path / "y-eventos.csv").read_text(encoding="utf-8") == "antigo"


def test_tipos_errados_sao_invalidas_e_o_registro_segue(tmp_path):
    ruins = [
        tel(2, n="invalid"),                                   # o caso da revisão
        tel(3, tb=-5),
        tel(4, n=True),                                        # booleano não é inteiro
        tel(5, cur="sim"),
        json.dumps({"type": "tel", "cur": True, "tb": 1, "n": 6, "link": "ok"}),
        tel(7, roll="10"),
        tel(8, link={"state": "ok", "delay_ms": "4"}),
        tel(9, alarm=[]),
        json.dumps({"type": "cmd", "cmd": "zero_attitude", "state": "done", "rid": "5"}),
        json.dumps({"type": "cmd", "cmd": 1, "state": "done"}),
    ]
    reg = rodar(tmp_path, [tel(1)] + ruins + [tel(10)])
    assert reg.invalidas == len(ruins) and reg.erros == 0
    assert [int(r["n"]) for r in linhas(reg.caminho_tel)] == [1, 10]
    tipos = [r["tipo"] for r in linhas(reg.caminho_eventos)]
    assert tipos.count("invalida") == len(ruins)


def test_null_continua_valido(tmp_path):
    reg = rodar(tmp_path, [tel(1, roll=None, alt=None, link={"state": "lost", "delay_ms": None})])
    assert reg.invalidas == 0 and reg.linhas_tel == 1


def test_erro_no_tratamento_vira_evento_e_o_registro_segue(tmp_path):
    q = queue.Queue()
    reg = Processador(q, str(tmp_path), nome="e")
    original = reg._tratar

    def tratar(*item):
        if item[4] == "explode":
            raise RuntimeError("falha forçada")
        return original(*item)

    reg._tratar = tratar
    reg.iniciar()
    por(q, tel(1))
    por(q, "explode")
    por(q, tel(2))
    assert reg.parar()
    assert reg.erros == 1 and reg.linhas_tel == 2
    erros = [r for r in linhas(reg.caminho_eventos) if r["tipo"] == "erro"]
    assert len(erros) == 1 and "RuntimeError: falha forçada" in erros[0]["detalhe"]


def test_parar_informa_o_que_ficou_pendente(tmp_path):
    q = queue.Queue()
    reg = Processador(q, str(tmp_path), nome="p")
    original = reg._tratar
    reg._tratar = lambda *item: (time.sleep(2), original(*item))
    reg.iniciar()
    for n in range(1, 4):
        por(q, tel(n))
    t0 = time.monotonic()
    assert reg.parar(timeout=0.5) is False
    assert time.monotonic() - t0 < 1.5
    assert reg.pendentes >= 1 and not reg.encerrado and not reg.falhas_gravacao


def test_parar_esvazia_a_fila_antes_de_fechar(tmp_path):
    q = queue.Queue()
    reg = Processador(q, str(tmp_path), nome="d")
    for n in range(1, 501):
        por(q, tel(n))
    reg.iniciar()
    assert reg.parar() and reg.encerrado and reg.gravou_tudo and reg.pendentes == 0
    assert len(linhas(reg.caminho_tel)) == 500


# ---------- falhas de gravação: thread encerrada não é gravação concluída ----------

class ArquivoFalho:
    """Embrulha o arquivo real e falha (disco cheio) na escrita, no flush ou no fechamento, a partir de um ponto."""

    def __init__(self, f, escrita_apos=None, flush=False, fechamento=False):
        self.f = f
        self.escritas = 0
        self.escrita_apos = escrita_apos
        self.falha_flush = flush
        self.falha_fechamento = fechamento

    def write(self, x):
        self.escritas += 1
        if self.escrita_apos is not None and self.escritas > self.escrita_apos:
            raise OSError(28, "No space left on device")
        return self.f.write(x)

    def flush(self):
        if self.falha_flush:
            raise OSError(28, "No space left on device")
        self.f.flush()

    def close(self):
        self.f.close()
        if self.falha_fechamento:
            raise OSError(5, "Input/output error")


def com_arquivo_falho(tmp_path, nome, **falhas):
    q = queue.Queue()
    reg = Processador(q, str(tmp_path), nome=nome)
    reg._arq_tel = ArquivoFalho(reg._arq_tel, **falhas)
    reg._tel = csv.writer(reg._arq_tel)
    return q, reg


def test_escrita_falhando_para_e_conta_as_pendentes(tmp_path):
    # O caso da revisão: antes, parar() devolvia True ("tudo gravado") com 0 linhas gravadas
    q, reg = com_arquivo_falho(tmp_path, "w", escrita_apos=2)   # 2 telemetrias gravadas, a 3ª falha
    for n in range(1, 11):
        por(q, tel(n))
    reg.iniciar()
    assert reg.parar() is False
    assert reg.encerrado and not reg.gravou_tudo
    assert reg.linhas_tel == 2
    assert reg.pendentes == 8                     # a que falhou e as 7 que ficaram na fila
    assert reg.falhas_gravacao[0].startswith("escrita: OSError")
    assert reg.erros == 0                         # falha de gravação não é "erro" de uma mensagem


def test_flush_falhando_durante_a_execucao(tmp_path):
    q, reg = com_arquivo_falho(tmp_path, "fl", flush=True)
    reg.iniciar()
    for n in range(1, 4):
        por(q, tel(n))
    time.sleep(1.5)                               # passa do flush de 1 s
    for n in range(4, 7):
        por(q, tel(n))                            # chegam depois da falha: não são gravadas
    assert reg.parar() is False
    assert reg.encerrado
    assert any(f.startswith("flush: OSError") for f in reg.falhas_gravacao)
    assert reg.pendentes == 3


def test_fechamento_falhando_com_tudo_escrito(tmp_path):
    # Tudo passou pelo writer, mas o fechamento falhou: os dados podem não estar no disco, e o retorno diz isso
    q, reg = com_arquivo_falho(tmp_path, "fc", fechamento=True)
    for n in range(1, 4):
        por(q, tel(n))
    reg.iniciar()
    assert reg.parar() is False
    assert reg.encerrado and reg.linhas_tel == 3 and reg.pendentes == 0
    assert reg.falhas_gravacao == ["fechamento (tel): OSError: [Errno 5] Input/output error"]


def test_sem_falha_gravou_tudo(tmp_path):
    q, reg = com_arquivo_falho(tmp_path, "ok")   # o embrulho sem nenhuma falha ligada
    for n in range(1, 4):
        por(q, tel(n))
    reg.iniciar()
    assert reg.parar() is True and reg.gravou_tudo and not reg.falhas_gravacao


def test_relogio_do_registro_com_fila():
    q = queue.Queue()
    reg = Processador(q, str(tmp_dir()), nome="rel")
    antes = time.monotonic_ns() / 1e6
    assert reg.relogio_ms() >= antes                 # nada tratado: o relógio
    reg._tratar("msg", 5_000_000, time.time(), 1, tel(1))   # tratada uma que chegou aos 5 ms
    q.put(("msg", time.monotonic_ns(), time.time(), 1, tel(2)))
    assert reg.relogio_ms() == 5.0                   # com fila: a chegada da última tratada
    q.get_nowait()
    assert reg.relogio_ms() >= antes                 # fila vazia: o relógio de novo
    reg.iniciar()
    assert reg.parar()


def tmp_dir():
    import tempfile
    return tempfile.mkdtemp()
