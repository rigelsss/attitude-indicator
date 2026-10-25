"""Thread de registro do cliente (docs/cliente-python.md, passo 3): consome a fila da thread de rede, decodifica,
grava os CSV, calcula as métricas e guarda os últimos segundos para os gráficos. Fica fora da thread de rede para
que o disco nunca atrase a leitura do socket.

Arquivos por execução, em `pasta`:
  AAMMDD-HHMMSS-tel.csv      uma linha por "tel" (mensagens.TEL_COLUNAS + colunas calculadas no PC)
  AAMMDD-HHMMSS-eventos.csv  conexões, quedas, estado da telemetria, hello, cmd, comandos enviados, inválidas, erros
Os dois são criados juntos e nunca sobrescrevem: com o nome já usado (duas execuções no mesmo segundo), vira
AAMMDD-HHMMSS-2, -3...

Uma mensagem fora do contrato é contada como inválida; qualquer outro erro no tratamento de uma mensagem vira um
evento "erro" e é contado. Em nenhum dos dois casos o registro para.

Uma falha de gravação (OSError ao escrever, no flush ou no fechamento: disco cheio, pendrive removido) é outra
coisa: o registro para de consumir, fecha o que puder e informa a falha e as mensagens que não foram gravadas. Por
isso "thread encerrada" (encerrado) e "gravação concluída" (o retorno de parar()) são separados.
"""

import csv
import os
import queue
import threading
import time
import traceback
from collections import deque

from .mensagens import TEL_COLUNAS, MensagemInvalida, decodificar, linha_tel
from .metricas import ContadorN, EstatIntervalo, EstimadorAtraso

COLUNAS_PC = ("atraso_bpc_ms", "puladas_ws")   # atraso relativo B → PC (pelo tb) e telemetrias puladas antes desta
EVENTOS_COLUNAS = ("pc_ns", "pc_unix", "sessao", "tipo", "detalhe")
FLUSH_S = 1.0
RESUMO_S = 1.0
MAX_NOMES = 1000   # tentativas de nome livre (-2 a -999)


def _abrir_exclusivo(pasta, nome):
    """Cria os dois arquivos de uma execução com o modo "x" (falha se já existir). Com o nome em uso, tenta -2, -3...
    Os dois ficam com o mesmo nome: se o de telemetria foi criado e o de eventos colidiu, o primeiro é apagado."""
    for k in range(1, MAX_NOMES):
        final = nome if k == 1 else f"{nome}-{k}"
        tel = os.path.join(pasta, f"{final}-tel.csv")
        ev = os.path.join(pasta, f"{final}-eventos.csv")
        try:
            f_tel = open(tel, "x", newline="", encoding="utf-8")
        except FileExistsError:
            continue
        try:
            f_ev = open(ev, "x", newline="", encoding="utf-8")
        except FileExistsError:
            f_tel.close()
            os.remove(tel)
            continue
        return final, tel, ev, f_tel, f_ev
    raise FileExistsError(f"nenhum nome livre para {nome} em {pasta}")


class Processador:
    def __init__(self, entrada, pasta, janela_s=30, ao_resumo=None, ao_evento=None, ao_cmd=None, nome=None):
        os.makedirs(pasta, exist_ok=True)
        (self.nome, self.caminho_tel, self.caminho_eventos, self._arq_tel,
         self._arq_ev) = _abrir_exclusivo(pasta, nome or time.strftime("%y%m%d-%H%M%S"))
        self.entrada = entrada
        self.ao_resumo = ao_resumo
        self.ao_evento = ao_evento
        self.ao_cmd = ao_cmd       # (dict do "cmd", replay, chegada_ms): replay = um dos recentes logo depois do hello
        self._chegada_ms = None    # carimbo de chegada (ms, monotônico) da última mensagem tratada
        self._apos_hello = False   # do hello até a primeira tel: os "cmd" são os recentes que o B repete
        self._tel = csv.writer(self._arq_tel)
        self._ev = csv.writer(self._arq_ev)
        self._tel.writerow(TEL_COLUNAS + COLUNAS_PC)
        self._ev.writerow(EVENTOS_COLUNAS)
        self.intervalo = EstatIntervalo()
        self.contador_n = ContadorN()
        self.atraso = EstimadorAtraso()
        self.linhas_tel = 0
        self.invalidas = 0
        self.erros = 0             # exceções no tratamento de uma mensagem (evento "erro")
        self.erros_callback = 0
        self.encerrado = False     # a thread de registro terminou (com ou sem sucesso)
        self.falhas_gravacao = []  # OSError ao escrever, no flush ou no fechamento, na ordem
        self.pendentes = 0         # mensagens não gravadas: na fila (tempo vencido ou falha) e a que falhou
        self._perdida = 0          # a mensagem em tratamento quando a gravação falhou
        self.estado_tel = "desconectado"
        self.rtt_ms = None         # último RTT PC ↔ B (resposta do ping), por conexão
        self.ultimo_resumo = None
        self._janela = deque(maxlen=int(janela_s * 40))   # 30 Hz com folga
        self._trava = threading.Lock()
        self._parar = threading.Event()
        self._thread = threading.Thread(target=self._rodar, name="registro", daemon=True)
        self._resumo_tel = 0
        self._resumo_puladas = 0
        self._resumo_atraso_max = None
        self._resumo_delay_max = None
        self._ultima_tel = None

    # ---------- de qualquer thread ----------
    def iniciar(self):
        self._thread.start()

    @property
    def gravou_tudo(self):
        """A fila foi esvaziada e os arquivos foram gravados e fechados sem nenhuma falha."""
        return self.encerrado and not self.falhas_gravacao

    def parar(self, timeout=10.0):
        """Pede o fim e espera esvaziar a fila e fechar os arquivos. True só se a gravação foi concluída sem falha.
        Senão: self.encerrado diz se a thread terminou (False = tempo vencido; a thread é daemon, para um disco
        travado não segurar o processo), self.falhas_gravacao traz as falhas e self.pendentes, as mensagens que não
        foram gravadas."""
        self._parar.set()
        self._thread.join(timeout)
        # Contadas agora, e não quando a thread parou: depois de uma falha de gravação, a rede continua pondo
        # mensagens na fila até ser parada (o cliente para a rede antes do registro)
        if not self.gravou_tudo:
            self.pendentes = self._perdida + self.entrada.qsize()
        return self.gravou_tudo

    def amostras(self):
        """Cópia dos últimos segundos, para os gráficos: dicts com pc_s, n, roll, pitch, hdg, alt, cur, delay_ms,
        atraso_bpc_ms, loss_pct e puladas."""
        with self._trava:
            return list(self._janela)

    # ---------- thread de registro ----------
    def _falha_gravacao(self, onde, e):
        self.falhas_gravacao.append(f"{onde}: {type(e).__name__}: {e}")

    def _rodar(self):
        ultimo_flush = ultimo_resumo = time.monotonic()
        try:
            while not self.falhas_gravacao and (not self._parar.is_set() or not self.entrada.empty()):
                try:
                    item = self.entrada.get(timeout=0.2)
                except queue.Empty:
                    item = None
                if item is not None:
                    try:
                        self._tratar(*item)
                    except OSError as e:
                        self._falha_gravacao("escrita", e)
                        self._perdida = 1
                        break
                    except Exception as e:
                        self._erro(item, e)
                agora = time.monotonic()
                if agora - ultimo_resumo >= RESUMO_S:
                    ultimo_resumo = agora
                    try:
                        self._resumir()
                    except OSError as e:
                        self._falha_gravacao("escrita", e)
                        break
                    except Exception as e:
                        self._erro(None, e)
                if agora - ultimo_flush >= FLUSH_S:
                    ultimo_flush = agora
                    try:
                        self._arq_tel.flush()
                        self._arq_ev.flush()
                    except OSError as e:
                        self._falha_gravacao("flush", e)
                        break
        finally:
            for nome, arq in (("tel", self._arq_tel), ("eventos", self._arq_ev)):
                try:
                    arq.flush()
                except (OSError, ValueError) as e:
                    self._falha_gravacao(f"flush final ({nome})", e)
                try:
                    arq.close()
                except OSError as e:
                    self._falha_gravacao(f"fechamento ({nome})", e)
            self.encerrado = True

    def _erro(self, item, e):
        # O registro segue para a próxima mensagem; o erro fica no CSV de eventos e no contador
        self.erros += 1
        if self.erros <= 3:
            traceback.print_exc()
        ns, unix, sessao = (item[1], item[2], item[3]) if item else (time.monotonic_ns(), time.time(), "")
        amostra = repr(item[4])[:200] if item else "resumo"
        try:
            self._registrar(ns, unix, sessao, "erro", f"{type(e).__name__}: {e} | {amostra}")
        except OSError as e2:
            self._falha_gravacao("escrita", e2)   # o laço para na próxima volta
        except Exception:
            pass

    def _chamar(self, f, *args):
        # Um erro no callback (terminal, gráfico) não pode parar o registro: o CSV continua
        try:
            f(*args)
        except Exception:
            self.erros_callback += 1
            if self.erros_callback <= 3:
                traceback.print_exc()

    def _registrar(self, pc_ns, pc_unix, sessao, tipo, detalhe):
        self._ev.writerow((pc_ns, round(pc_unix, 3), sessao, tipo, detalhe))
        if self.ao_evento is not None:
            self._chamar(self.ao_evento, tipo, detalhe)

    def relogio_ms(self):
        """O "agora" do que já foi tratado (ms, monotônico): com a fila vazia, o relógio; com mensagens esperando,
        a chegada da última tratada. Assim, um prazo (o sem_eco dos comandos) não vence por atraso do registro
        enquanto a resposta ainda está na fila."""
        if self.entrada.empty() or self._chegada_ms is None:
            return time.monotonic_ns() / 1e6
        return self._chegada_ms

    def _tratar(self, tipo, pc_ns, pc_unix, sessao, dado):
        self._chegada_ms = pc_ns / 1e6
        if tipo == "evento":
            nome, detalhe = dado
            if nome == "conectado":
                self.intervalo.recomecar()
                self.contador_n.recomecar()
                self.rtt_ms = None
            elif nome == "rtt":
                self.rtt_ms = float(detalhe)
            if nome == "telemetria":
                self.estado_tel = detalhe
            elif nome in ("desconectado", "falha"):
                self.estado_tel = "desconectado"
            self._registrar(pc_ns, pc_unix, sessao, nome, detalhe)
            return
        try:
            tipo_msg, d = decodificar(dado)
        except MensagemInvalida as e:
            self.invalidas += 1
            self._registrar(pc_ns, pc_unix, sessao, "invalida", f"{e}: {dado[:200]}")
            return
        if tipo_msg != "tel":
            self._registrar(pc_ns, pc_unix, sessao, tipo_msg, dado)
            if tipo_msg == "hello":
                self._apos_hello = True
            elif tipo_msg == "cmd" and self.ao_cmd is not None:
                self._chamar(self.ao_cmd, d, self._apos_hello, pc_ns / 1e6)
            return
        self._apos_hello = False
        pc_ms = pc_ns / 1e6
        self.intervalo.chegada(pc_ms)
        puladas = self.contador_n.chegada(d["n"])
        atraso = round(self.atraso.chegada(d["tb"], pc_ms), 1) if isinstance(d.get("tb"), int) else None
        self._tel.writerow(linha_tel(d, pc_ns, pc_unix, sessao) + [atraso if atraso is not None else "", puladas])
        self.linhas_tel += 1
        link = d.get("link") or {}
        self._resumo_tel += 1
        self._resumo_puladas += puladas
        if atraso is not None:
            self._resumo_atraso_max = atraso if self._resumo_atraso_max is None else max(self._resumo_atraso_max, atraso)
        delay = link.get("delay_ms")
        if isinstance(delay, (int, float)):
            self._resumo_delay_max = delay if self._resumo_delay_max is None else max(self._resumo_delay_max, delay)
        self._ultima_tel = d
        amostra = {
            "pc_s": pc_ns / 1e9, "n": d["n"], "cur": bool(d.get("cur")),
            "roll": d.get("roll"), "pitch": d.get("pitch"), "hdg": d.get("hdg"), "alt": d.get("alt"),
            "delay_ms": delay, "atraso_bpc_ms": atraso, "loss_pct": link.get("loss_pct"), "puladas": puladas,
        }
        with self._trava:
            self._janela.append(amostra)

    def _resumir(self):
        iv = self.intervalo.resumo()
        d = self._ultima_tel or {}
        link = d.get("link") or {}
        r = {
            "estado": self.estado_tel, "tel_s": self._resumo_tel, "puladas": self._resumo_puladas,
            "puladas_total": self.contador_n.puladas, "intervalo": iv,
            "atraso_bpc_max": self._resumo_atraso_max, "delay_ab_max": self._resumo_delay_max,
            "lost": link.get("lost"), "loss_pct": link.get("loss_pct"), "linhas": self.linhas_tel,
            "rtt_ms": self.rtt_ms,
        }
        self._resumo_tel = self._resumo_puladas = 0
        self._resumo_atraso_max = self._resumo_delay_max = None
        self.ultimo_resumo = r
        if self.ao_resumo is not None:
            self._chamar(self.ao_resumo, r)


def texto_resumo(r, sessao):
    """Linha do resumo de 1 s para o terminal."""
    iv = r["intervalo"]
    iv_txt = (f"intervalo {iv['min']:.0f}/{iv['media']:.1f}/{iv['max']:.0f} ms (p95 {iv['p95']:.0f})"
              if iv else "intervalo ---")
    fmt = lambda v, suf="": "---" if v is None else f"{v:g}{suf}"
    return (f"[r] sessão {sessao} | {r['estado']} | tel {r['tel_s']}/s | puladas {r['puladas']} "
            f"(total {r['puladas_total']}) | {iv_txt} | atraso B->PC máx {fmt(r['atraso_bpc_max'], ' ms')} | "
            f"A->B máx {fmt(r['delay_ab_max'], ' ms')} | perdidos A->B {fmt(r['lost'])} ({fmt(r['loss_pct'], ' %')}) | "
            f"RTT PC<->B {fmt(r.get('rtt_ms'), ' ms')}")
