"""Comandos do cliente (docs/cliente-python.md, passo 4): o que o usuário digita vira um pedido com rid, enviado pela
thread de rede; o andamento chega pela thread de registro e é impresso. O rastreador é usado pelas duas threads (a
principal pede, a de registro casa os eventos), então fica sob uma trava.

Imprime:
  - cada mudança de estado dos pedidos deste cliente, com o tempo desde o pedido;
  - os comandos de outras origens (página, console, encoder, outro cliente), uma vez por estado, exceto os comandos
    recentes que o B repete depois do hello (já aconteceram).
"""

import json
import threading
import time

from .comandos import RastreadorComandos
from .mensagens import montar_comando
from .terminal import AJUDA, interpretar

CONFIRMAR_MS = 5000   # g/m: a segunda tecla, dentro deste prazo, confirma a calibração
MAX_LISTA = 10
# O que fazer com a caixa em cada calibração (docs/calibracao.md e docs/magnetometro.md)
ORIENTACAO = {
    "calibrate_gyro": "deixe a caixa parada, sem tocar na mesa, por uns 5 s",
    "calibrate_mag": ("a coleta leva até 3 min: gire a caixa devagar no mesmo lugar, uma volta de 360° com cada face "
                      "apoiada, longe de metais e de fios com corrente (ver docs/magnetometro.md)"),
}

NOMES = {"calibrate_gyro": "calibrar giroscópio", "calibrate_mag": "calibrar magnetômetro",
         "zero_attitude": "zerar atitude", "zero_altitude": "zerar altitude", "set_rate": "taxa"}
ESTADOS = {
    "pedido": "pedido, aguardando o B", "sent": "enviado ao A", "accepted": "aceito pelo A, em curso",
    "done": "concluído", "rejected": "recusado pelo A (o motivo fica na serial do A)",
    "no_response": "sem resposta do A (3 envios)", "no_completion": "aceito, sem conclusão em 200 s",
    "busy": "ocupado: o B já tem 4 comandos em curso", "refused": "recusado pelo B",
    "sem_eco": "sem resposta do B em 3 s", "nao_enviado": "não enviado",
}


def _nome(cmd, arg):
    base = NOMES.get(cmd, cmd)
    return f"{base} {arg} Hz" if cmd == "set_rate" and arg is not None else base


def _relogio_ms():
    return time.monotonic_ns() / 1e6


class Operador:
    def __init__(self, enviar, imprimir=print, pin="2468", relogio_ms=_relogio_ms, rastreador=None):
        self.enviar = enviar          # texto -> bool (False sem conexão)
        self.imprimir = imprimir
        self.pin = pin
        self.relogio_ms = relogio_ms
        self.rastreador = rastreador or RastreadorComandos()
        self.resumo_ligado = True
        self._trava = threading.Lock()
        self._confirmar = None        # (cmd, prazo_ms) da calibração aguardando a segunda tecla
        self._outros = {}             # seq → último estado impresso, dos comandos de outras origens

    # ---------- thread principal ----------
    def linha(self, texto):
        """Trata uma linha digitada. Devolve False para sair."""
        acao = interpretar(texto)
        tipo = acao[0]
        if tipo == "sair":
            return False
        if tipo == "vazio":
            return True
        if tipo == "erro":
            self.imprimir(f"[?] {acao[1]}")
        elif tipo == "ajuda":
            self.imprimir(AJUDA)
        elif tipo == "pin":
            if acao[1] is None:
                self.imprimir(f"[pin] em uso: {self.pin}")
            else:
                self.pin = acao[1]
                self.imprimir(f"[pin] trocado para {self.pin}")
        elif tipo == "resumo":
            self.resumo_ligado = not self.resumo_ligado
            self.imprimir(f"[r] resumo {'ligado' if self.resumo_ligado else 'desligado'}")
        elif tipo == "lista":
            self._listar()
        elif tipo == "rajada":
            self.imprimir(f"[cmd] rajada: {acao[1]} x zerar atitude")
            for _ in range(acao[1]):
                self._pedir("zero_attitude", None, True)
        elif tipo == "cmd":
            _, cmd, arg, conferir = acao
            if cmd in ("calibrate_gyro", "calibrate_mag") and not self._confirmado(cmd):
                return True
            self._pedir(cmd, arg, conferir)
        return True

    def _confirmado(self, cmd):
        agora = self.relogio_ms()
        if self._confirmar is not None and self._confirmar[0] == cmd and agora <= self._confirmar[1]:
            self._confirmar = None
            return True
        self._confirmar = (cmd, agora + CONFIRMAR_MS)
        tecla = "g" if cmd == "calibrate_gyro" else "m"
        self.imprimir(f"[cmd] {_nome(cmd, None)}: {ORIENTACAO[cmd]}. Digite {tecla} de novo em "
                      f"{CONFIRMAR_MS // 1000} s para confirmar")
        return False

    def _pedir(self, cmd, arg, conferir):
        with self._trava:
            p = self.rastreador.pedir(cmd, arg, self.relogio_ms())
        try:
            texto = montar_comando(cmd, self.pin, arg, rid=p.rid, conferir=conferir)
        except ValueError as e:
            self._nao_enviado(p, str(e))
            return
        if self.enviar(texto):
            self.imprimir(f"[cmd] #{p.rid} {_nome(cmd, arg)}: pedido")
        else:
            self._nao_enviado(p, "sem conexão")

    def _nao_enviado(self, p, motivo):
        with self._trava:
            self.rastreador.nao_enviado(p, self.relogio_ms(), motivo)
        self.imprimir(f"[cmd] #{p.rid} {_nome(p.cmd, p.arg)}: {ESTADOS['nao_enviado']}: {motivo}")

    def _listar(self):
        with self._trava:
            pedidos = self.rastreador.todos()[-MAX_LISTA:]
        if not pedidos:
            self.imprimir("[s] nenhum pedido nesta execução")
            return
        for p in pedidos:
            ms = p.tempos.get(p.estado)
            quando = f" em {ms:.0f} ms" if ms is not None else ""
            seq = f", seq {p.seq}" if p.seq is not None else ""
            self.imprimir(f"[s] #{p.rid} {_nome(p.cmd, p.arg)}: {ESTADOS.get(p.estado, p.estado)}{quando}{seq}"
                          f"{' (tardio)' if p.tardio else ''}{': ' + p.motivo if p.motivo else ''}")

    # ---------- thread de registro ----------
    def ao_cmd(self, d, replay, chegada_ms=None):
        """Mensagem "cmd" decodificada. replay: um dos comandos recentes que o B repete depois do hello. chegada_ms:
        o carimbo de chegada no PC (o mesmo relógio monotônico do pedido); os tempos dos comandos são contados por
        ele, e não pela hora em que a thread de registro tratou a mensagem."""
        agora = chegada_ms if chegada_ms is not None else self.relogio_ms()
        with self._trava:
            p = self.rastreador.evento(d, agora)
        if p is not None:
            ms = p.tempos.get(p.estado, 0)
            seq = f", seq {p.seq}" if p.estado == "sent" and p.seq is not None else ""
            motivo = f": {p.motivo}" if p.motivo else ""
            tardio = " (tardio)" if p.tardio else ""
            self.imprimir(f"[cmd] #{p.rid} {_nome(p.cmd, p.arg)}: {ESTADOS.get(p.estado, p.estado)} "
                          f"({ms:.0f} ms{seq}){motivo}{tardio}")
            return
        if replay or d.get("origin") == "pc" and d.get("rid") is not None:
            return   # recentes, ou de outro cliente Python (ou de uma execução anterior deste)
        seq, estado = d.get("seq"), d.get("state")
        if estado == "refused" or seq is None or self._outros.get(seq) == estado:
            return   # recusa de outro cliente (só ele recebe) ou estado repetido (reenvio do B)
        self._outros[seq] = estado
        if len(self._outros) > 64:
            self._outros.pop(next(iter(self._outros)))
        self.imprimir(f"[cmd] ({d.get('origin', '?')}) {_nome(d.get('cmd'), d.get('arg'))}, seq {seq}: "
                      f"{ESTADOS.get(estado, estado)}")

    def ao_envio_falhou(self, texto):
        """O envio de um comando falhou na thread de rede (a conexão caiu no meio)."""
        try:
            rid = json.loads(texto.split(" (", 1)[0]).get("rid")
        except (ValueError, AttributeError):
            return
        with self._trava:
            p = self.rastreador.pedido(rid)
            if p is None or p.final:
                return
        self._nao_enviado(p, "a conexão caiu no envio")

    def tick(self, agora_ms=None):
        """A cada resumo (1 s): pedidos sem resposta do B viram sem_eco. agora_ms: o relógio do que o registro já
        tratou (Processador.relogio_ms), para uma resposta ainda na fila não vencer o prazo."""
        with self._trava:
            vencidos = self.rastreador.vencer(agora_ms if agora_ms is not None else self.relogio_ms())
        for p in vencidos:
            self.imprimir(f"[cmd] #{p.rid} {_nome(p.cmd, p.arg)}: {ESTADOS['sem_eco']}")
