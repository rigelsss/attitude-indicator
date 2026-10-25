"""Simulador do ESP B para desenvolver e testar o cliente sem a rede Horizonte-ESP (docs/cliente-python.md, passo 3).

Fala o mesmo JSON do B (docs/protocolo-websocket.md): "hello" e os comandos recentes na conexão, "tel" a
WS_RATE_HZ com "tb" e "n" (montada uma vez e enviada a todos), e o andamento dos comandos com "rid". Injeta as
falhas vistas na bancada:
  - reassociação do PC: o envio para por alguns ms e volta em rajada, sem fechar o socket;
  - queda: o TCP é abortado sem o quadro de fechamento;
  - B travado: nenhuma telemetria por um tempo, com o socket aberto;
  - telemetria pulada para um cliente (salto de "n") e perdas no link A → B (salto de "seq").
"""

import asyncio
import json
import math
import random
import threading
import time
from dataclasses import dataclass

from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed

from .mensagens import COMANDOS, TAXAS_HZ

def _json(d):
    return json.dumps(d, ensure_ascii=False, separators=(",", ":"))   # compacto, como o ArduinoJson do B


COMANDOS_DEMORADOS = {"calibrate_gyro": 4.0, "calibrate_mag": 10.0}   # s até o done
RECENTES = 8
LUGARES = 4   # comandos em curso ao mesmo tempo no B (CommandLink::SLOTS)


@dataclass
class Falhas:
    reassociar_s: float = 0.0      # a cada quantos s o envio para (0 = nunca)
    reassociar_ms: float = 100.0   # por quanto tempo
    queda_s: float = 0.0           # a cada quantos s o TCP é abortado (0 = nunca)
    travar_apos_s: float = 0.0     # B travado: começa depois de tantos s do início (0 = nunca)
    travar_por_s: float = 0.0
    sumir_apos_s: float = 0.0      # B sumido (não lê o socket, então não responde o ping, e não manda nada): começa
    sumir_por_s: float = 0.0       #   depois de tantos s do início (0 = nunca), como o PC fora do alcance
    pular: float = 0.0             # probabilidade de pular uma telemetria para um cliente
    perda_a: float = 0.0           # probabilidade de perda de um pacote do A no link A → B


class _Cliente:
    def __init__(self, ws):
        self.ws = ws
        self.fila = asyncio.Queue(maxsize=4)   # cheia: a mais antiga sai (telemetria substituível)
        self.inicio = time.monotonic()


class SimuladorB:
    def __init__(self, falhas=None, hz=30, pin="2468", decl=-20.7, semente=None, sem_a=False):
        self.falhas = falhas or Falhas()
        self.hz = hz
        self.pin = pin
        self.decl = decl
        self._rnd = random.Random(semente)
        self._t0 = time.monotonic()
        self._offset_a_ms = 12_345          # relógio do A adiantado em relação ao do B
        self._clientes = set()
        self._n = 0
        self._lost = 0
        self._seq_cmd = self._rnd.randint(0, 65535)
        self.sem_a = sem_a           # como o B sem o A: 3 envios a cada 300 ms e no_response
        self._em_curso = 0           # o B tem 4 lugares; o 5º pedido é "busy" (seq 0)
        self._recentes = {}                 # seq → último evento (ordem de inserção = do mais antigo ao mais novo)
        self.porta = None
        self.conexoes = 0

    # ---------- relógios ----------
    def _b_ms(self):
        return int((time.monotonic() - self._t0) * 1000)

    def _janela(self, apos_s, por_s):
        if apos_s <= 0:
            return False
        dt = time.monotonic() - self._t0
        return apos_s <= dt < apos_s + por_s

    def _travado(self):
        # B travado: sem telemetria, mas o loop() da biblioteca responde o ping
        return self._janela(self.falhas.travar_apos_s, self.falhas.travar_por_s)

    def _sumido(self):
        return self._janela(self.falhas.sumir_apos_s, self.falhas.sumir_por_s)

    # ---------- mensagens ----------
    def _hello(self):
        return _json({"type": "hello", "decl": self.decl, "pin": bool(self.pin), "rates": list(TAXAS_HZ)})

    def _tel(self):
        self._n += 1
        tb = self._b_ms()
        t_a = tb + self._offset_a_ms
        s = t_a / 1000.0
        if self.falhas.perda_a > 0 and self._rnd.random() < self.falhas.perda_a:
            self._lost += 1
        recebidos = max(1, int(s * 50))
        d = {
            "type": "tel", "cur": True, "tb": tb, "n": self._n,
            "t": t_a, "seq": int(s * 50) & 0xFFFF,
            "roll": round(20 * math.sin(2 * math.pi * s / 10), 2),
            "pitch": round(10 * math.sin(2 * math.pi * s / 7), 2),
            "hdg": round((s * 3) % 360, 1),
            "gx": 0.1, "gy": -0.2, "gz": 0.0,
            "alt": round(0.5 * math.sin(2 * math.pi * s / 30), 2), "hpa": 1009.5, "temp": 26.4,
            "status": 55, "valid": 13, "loop_us": 110 + self._rnd.randint(0, 15), "decl": self.decl,
            "link": {"state": "ok", "age_ms": self._rnd.randint(0, 20), "delay_ms": self._rnd.randint(0, 4),
                     "rx_hz": 50, "lost": self._lost,
                     "loss_pct": round(100.0 * self._lost / (recebidos + self._lost), 3), "drops": 0},
            "alarm": {"kind": "none", "acked": False},
        }
        return _json(d)

    def _cmd_evento(self, cmd, arg, seq, state, rid, origin="pc"):
        d = {"type": "cmd", "cmd": cmd}
        if cmd == "set_rate":
            d["arg"] = arg
        d.update({"seq": seq, "state": state, "origin": origin})
        if rid:
            d["rid"] = rid
        return d

    async def _difundir_cmd(self, d):
        if d["state"] != "busy":
            self._recentes.pop(d["seq"], None)
            self._recentes[d["seq"]] = d
            while len(self._recentes) > RECENTES:
                self._recentes.pop(next(iter(self._recentes)))
        texto = _json(d)
        for c in list(self._clientes):
            try:
                await c.ws.send(texto)
            except ConnectionClosed:
                pass

    # ---------- comandos ----------
    async def _comando(self, c, texto):
        try:
            d = json.loads(texto)
        except json.JSONDecodeError:
            return await self._recusar(c, "unknown", "mensagem inválida", 0)
        if not isinstance(d, dict):
            return await self._recusar(c, "unknown", "mensagem inválida", 0)
        rid = d.get("rid", 0)
        if "rid" in d and (not isinstance(rid, int) or isinstance(rid, bool) or not 1 <= rid <= 65535):
            return await self._recusar(c, d.get("cmd", "unknown"), "mensagem inválida", 0)
        cmd = d.get("cmd")
        if cmd not in COMANDOS:
            return await self._recusar(c, cmd or "unknown", "comando desconhecido", rid)
        if self.pin and d.get("pin") != self.pin:
            return await self._recusar(c, cmd, "PIN incorreto", rid)
        arg = d.get("arg")
        if cmd == "set_rate" and (not isinstance(arg, int) or isinstance(arg, bool)):
            return await self._recusar(c, cmd, "argumento inválido", rid)
        origin = d.get("origin")
        if origin is not None and origin != "pc":
            return await self._recusar(c, cmd, "origem inválida", rid)
        origem = "pc" if origin == "pc" else "página"
        if self._em_curso >= LUGARES:
            return await self._difundir_cmd(self._cmd_evento(cmd, arg, 0, "busy", rid, origem))
        seq = self._seq_cmd
        self._seq_cmd = (self._seq_cmd + 1) & 0xFFFF
        self._em_curso += 1
        asyncio.create_task(self._andamento(cmd, arg, seq, rid, origem))

    async def _recusar(self, c, cmd, motivo, rid):
        d = {"type": "cmd", "cmd": cmd, "state": "refused", "reason": motivo}
        if rid:
            d["rid"] = rid
        try:
            await c.ws.send(_json(d))
        except ConnectionClosed:
            pass

    async def _andamento(self, cmd, arg, seq, rid, origin):
        try:
            await self._andamento_em_curso(cmd, arg, seq, rid, origin)
        finally:
            self._em_curso -= 1

    async def _andamento_em_curso(self, cmd, arg, seq, rid, origin):
        ev = lambda st: self._cmd_evento(cmd, arg, seq, st, rid, origin)
        await asyncio.sleep(0.005)
        await self._difundir_cmd(ev("sent"))
        if self.sem_a:
            for _ in range(2):   # reenvios do B a cada 300 ms
                await asyncio.sleep(0.3)
                await self._difundir_cmd(ev("sent"))
            await asyncio.sleep(0.3)
            await self._difundir_cmd(ev("no_response"))
        elif cmd == "set_rate":
            await asyncio.sleep(0.035)
            await self._difundir_cmd(ev("done" if arg in TAXAS_HZ else "rejected"))
        elif cmd in COMANDOS_DEMORADOS:
            await asyncio.sleep(0.035)
            await self._difundir_cmd(ev("accepted"))
            await asyncio.sleep(COMANDOS_DEMORADOS[cmd])
            await self._difundir_cmd(ev("done"))
        else:
            await asyncio.sleep(0.035)
            await self._difundir_cmd(ev("done"))

    # ---------- conexões ----------
    async def _enviar(self, c):
        f = self.falhas
        proxima_reassoc = c.inicio + f.reassociar_s if f.reassociar_s > 0 else None
        proxima_queda = c.inicio + f.queda_s if f.queda_s > 0 else None
        while True:
            texto = await c.fila.get()
            agora = time.monotonic()
            if proxima_queda is not None and agora >= proxima_queda:
                c.ws.transport.abort()   # sem o quadro de fechamento, como o reset do B
                return
            if proxima_reassoc is not None and agora >= proxima_reassoc:
                await asyncio.sleep(f.reassociar_ms / 1000.0)   # o que chegou no meio sai em rajada depois
                proxima_reassoc = time.monotonic() + f.reassociar_s
            await c.ws.send(texto)

    async def _atender(self, ws):
        c = _Cliente(ws)
        self.conexoes += 1
        await ws.send(self._hello())
        for d in list(self._recentes.values()):
            await ws.send(_json(d))
        self._clientes.add(c)
        envio = asyncio.create_task(self._enviar(c))
        try:
            async for texto in ws:
                await self._comando(c, texto)
        except ConnectionClosed:
            pass
        finally:
            self._clientes.discard(c)
            envio.cancel()

    async def _produzir(self):
        periodo = 1.0 / self.hz
        proxima = time.monotonic()
        sumido = False
        while True:
            proxima += periodo
            await asyncio.sleep(max(0.0, proxima - time.monotonic()))
            if self._sumido() != sumido:
                sumido = not sumido
                for c in list(self._clientes):   # sem ler o socket, os pings ficam sem resposta
                    (c.ws.transport.pause_reading if sumido else c.ws.transport.resume_reading)()
            if not self._clientes or self._travado() or sumido:
                continue
            texto = self._tel()
            for c in list(self._clientes):
                if self.falhas.pular > 0 and self._rnd.random() < self.falhas.pular:
                    continue
                if c.fila.full():
                    c.fila.get_nowait()
                c.fila.put_nowait(texto)

    async def servir(self, host="127.0.0.1", porta=8765, pronto=None, parar=None):
        """Atende até `parar` (asyncio.Event) ser ligado, ou para sempre. `pronto` (threading.Event) é ligado com
        a porta em self.porta (porta 0: o sistema escolhe)."""
        async with serve(self._atender, host, porta, ping_interval=None) as servidor:
            self.porta = servidor.sockets[0].getsockname()[1]
            if pronto is not None:
                pronto.set()
            produtor = asyncio.create_task(self._produzir())
            try:
                if parar is None:
                    await asyncio.Future()
                else:
                    await parar.wait()
            finally:
                produtor.cancel()


class SimuladorEmThread:
    """O simulador numa thread com o próprio laço asyncio (para os testes)."""

    def __init__(self, sim):
        self.sim = sim
        self._pronto = threading.Event()
        self._loop = None
        self._parar = None
        self._thread = threading.Thread(target=self._rodar, name="sim_b", daemon=True)

    def _rodar(self):
        async def principal():
            self._loop = asyncio.get_running_loop()
            self._parar = asyncio.Event()
            await self.sim.servir(porta=0, pronto=self._pronto, parar=self._parar)
        asyncio.run(principal())

    def __enter__(self):
        self._thread.start()
        if not self._pronto.wait(5):
            raise RuntimeError("simulador não subiu")
        return self

    @property
    def url(self):
        return f"ws://127.0.0.1:{self.sim.porta}/"

    def __exit__(self, *exc):
        self._loop.call_soon_threadsafe(self._parar.set)
        self._thread.join(5)
