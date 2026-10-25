"""Thread de rede do cliente (docs/cliente-python.md, passo 3): só lê o WebSocket do B e põe cada mensagem numa
fila, com o carimbo de chegada do PC. Nada aqui grava em disco nem desenha: o B derruba um cliente com 2 s sem
progresso no envio e pula a telemetria de quem passa de 3 kB sem confirmação.

Itens da fila (tuplas):
  ("msg", pc_ns, pc_unix, sessao, texto)      mensagem recebida, sem decodificar
  ("evento", pc_ns, pc_unix, sessao, (nome, detalhe))
Eventos: "tentando", "conectado", "falha", "desconectado", "telemetria" (mudança de estado: aguardando, ok,
sem_telemetria, abandonar), "enviado", "envio_falhou".

Reconexão sozinha (horizonte.conexao): tentativa a cada 2 s; sem "tel" por 5 s, o socket é abandonado. O ping da
biblioteca (20 s) fica ligado, como reserva.
"""

import asyncio
import queue
import threading
import time

from websockets.asyncio.client import connect
from websockets.exceptions import ConnectionClosed, InvalidHandshake, InvalidURI

from .conexao import ABANDONO_MS, SEM_TEL_MS, AgendaReconexao, VigiaTelemetria

URL_PADRAO = "ws://192.168.4.1:81/"
VOLTA_S = 0.1   # de quanto em quanto tempo a leitura confere o estado da telemetria e o pedido de parar


def _agora_ms():
    return time.monotonic_ns() / 1e6


def _e_tel(texto):
    # O B escreve o "type" primeiro (ArduinoJson mantém a ordem); a decodificação completa é do registro. Espaços
    # (outro gerador de JSON) não atrapalham
    return texto[:24].replace(" ", "").startswith('{"type":"tel"')


class ClienteWS:
    def __init__(self, url=URL_PADRAO, saida=None, ping_s=20.0, sem_tel_ms=SEM_TEL_MS, abandono_ms=ABANDONO_MS):
        self.url = url
        self.saida = saida if saida is not None else queue.Queue(maxsize=100_000)
        self.ping_s = ping_s
        self.sem_tel_ms = sem_tel_ms
        self.abandono_ms = abandono_ms
        self.sessao = 0
        self.descartadas = 0          # fila cheia (o registro parou?): contadas, nunca bloqueiam a leitura
        self._parar = threading.Event()
        self._thread = threading.Thread(target=self._rodar, name="rede", daemon=True)
        self._loop = None
        self._ws = None

    # ---------- de qualquer thread ----------
    def iniciar(self):
        self._thread.start()

    def parar(self, timeout=5.0):
        self._parar.set()
        self._thread.join(timeout)

    @property
    def conectado(self):
        return self._ws is not None

    def enviar(self, texto):
        """Envia um comando. False sem conexão (o pedido não saiu)."""
        loop, ws = self._loop, self._ws
        if loop is None or ws is None:
            return False
        asyncio.run_coroutine_threadsafe(self._enviar(ws, texto), loop)
        return True

    # ---------- thread de rede ----------
    def _por(self, tipo, dado, ns=None):
        item = (tipo, ns if ns is not None else time.monotonic_ns(), time.time(), self.sessao, dado)
        try:
            self.saida.put_nowait(item)
        except queue.Full:
            self.descartadas += 1

    def _evento(self, nome, detalhe=""):
        self._por("evento", (nome, detalhe))

    async def _enviar(self, ws, texto):
        try:
            await ws.send(texto)
            self._evento("enviado", texto)
        except ConnectionClosed as e:
            self._evento("envio_falhou", f"{texto} ({e})")

    def _rodar(self):
        asyncio.run(self._principal())

    async def _principal(self):
        self._loop = asyncio.get_running_loop()
        agenda = AgendaReconexao()
        while not self._parar.is_set():
            espera = agenda.espera_ms(_agora_ms())
            if espera > 0:
                await asyncio.sleep(min(espera / 1000.0, VOLTA_S))
                continue
            agenda.iniciou(_agora_ms())
            self._evento("tentando", self.url)
            try:
                async with connect(self.url, open_timeout=3, ping_interval=self.ping_s, ping_timeout=self.ping_s,
                                   close_timeout=1, max_size=1 << 16) as ws:
                    self.sessao += 1
                    self._ws = ws
                    self._evento("conectado", self.url)
                    motivo = await self._ler(ws)
                    self._ws = None
                    self._evento("desconectado", motivo)
            except (OSError, TimeoutError, InvalidHandshake, InvalidURI) as e:
                self._evento("falha", f"{type(e).__name__}: {e}")
            finally:
                self._ws = None

    async def _ler(self, ws):
        vigia = VigiaTelemetria(_agora_ms(), self.sem_tel_ms, self.abandono_ms)
        anterior = None
        while not self._parar.is_set():
            try:
                texto = await asyncio.wait_for(ws.recv(), timeout=VOLTA_S)   # cancelar o recv não perde dados
            except TimeoutError:
                texto = None
            except ConnectionClosed as e:
                self._ws = None
                if e.rcvd is not None:
                    return f"fechada pelo B (código {e.rcvd.code})"
                return "conexão perdida (sem quadro de fechamento)"
            if texto is not None:
                ns = time.monotonic_ns()
                if isinstance(texto, bytes):
                    texto = texto.decode("utf-8", "replace")
                self._por("msg", texto, ns)
                if _e_tel(texto):
                    vigia.tel(ns / 1e6)
            estado = vigia.estado(_agora_ms())
            if estado != anterior:
                self._evento("telemetria", estado)
                anterior = estado
            if estado == "abandonar":
                self._ws = None
                ws.transport.abort()
                return f"abandonada: {self.abandono_ms / 1000:.0f} s sem telemetria"
        return "cliente encerrado"
