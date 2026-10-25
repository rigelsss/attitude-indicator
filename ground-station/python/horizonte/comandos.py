"""Andamento dos comandos pedidos por este cliente (contrato em docs/protocolo-websocket.md). Lógica pura; ms.

Cada pedido leva um "rid" (1..65535), e o B o repete em todos os eventos daquele comando ("sent" a "done", o
"busy" e a recusa no próprio B). O casamento é só pelo rid: nome, taxa e seq não entram. Assim, um evento atrasado
de um pedido antigo, o "busy" (que sai com seq 0) e os comandos recentes que o B manda depois do hello caem no
pedido certo ou em nenhum.

O primeiro rid é aleatório a cada execução: o replay de uma execução anterior quase nunca coincide com um pedido
desta (e, se coincidir com um rid que esta ainda não usou, é ignorado). Um pedido sem nenhum evento em SEM_ECO_MS
vira "sem_eco"; se a resposta chegar depois, ele é atualizado e marcado como tardio.
"""

import random

FINAIS = ("done", "rejected", "no_response", "no_completion", "busy", "refused", "sem_eco")
SEM_ECO_MS = 3000
MAX_PEDIDOS = 64   # pedidos lembrados (os terminados mais antigos saem primeiro)


class Pedido:
    def __init__(self, rid, cmd, arg, agora_ms):
        self.rid = rid
        self.cmd = cmd
        self.arg = arg
        self.pedido_ms = agora_ms
        self.seq = None
        self.estado = "pedido"
        self.motivo = None
        self.tardio = False    # resposta chegou depois do sem_eco
        self.tempos = {}       # estado → ms desde o pedido (o primeiro de cada)

    @property
    def final(self):
        return self.estado in FINAIS

    def _mudar(self, estado, agora_ms):
        self.estado = estado
        self.tempos.setdefault(estado, agora_ms - self.pedido_ms)


class RastreadorComandos:
    def __init__(self, primeiro_rid=None, sem_eco_ms=SEM_ECO_MS):
        if primeiro_rid is None:
            primeiro_rid = random.randint(1, 65535)
        if not 1 <= primeiro_rid <= 65535:
            raise ValueError("rid fora de 1..65535")
        self._proximo = primeiro_rid
        self.sem_eco_ms = sem_eco_ms
        self._pedidos = {}   # rid → Pedido, na ordem dos pedidos

    def _novo_rid(self):
        rid = self._proximo
        self._proximo = 1 if rid == 65535 else rid + 1   # o 0 é "sem rid"
        return rid

    def pedir(self, cmd, arg, agora_ms):
        """Novo pedido; o rid dele vai na mensagem (mensagens.montar_comando)."""
        p = Pedido(self._novo_rid(), cmd, arg if cmd == "set_rate" else None, agora_ms)
        self._pedidos.pop(p.rid, None)   # um rid reaproveitado depois da volta substitui o antigo
        self._pedidos[p.rid] = p
        self._podar()
        return p

    def _podar(self):
        if len(self._pedidos) <= MAX_PEDIDOS:
            return
        for rid in [r for r, p in self._pedidos.items() if p.final]:
            del self._pedidos[rid]
            if len(self._pedidos) <= MAX_PEDIDOS:
                return

    def evento(self, d, agora_ms):
        """Mensagem "cmd" decodificada. Devolve o Pedido deste cliente que mudou, ou None (outro cliente, outra
        origem, execução anterior, ou um evento que não muda nada)."""
        p = self._pedidos.get(d.get("rid"))
        if p is None:
            return None
        estado = d.get("state")
        if p.final and p.estado != "sem_eco" and estado not in FINAIS:
            return None   # um estado intermediário depois do fim (fora de ordem): ignorado
        if p.estado == "sem_eco":
            p.tardio = True
        if estado == "refused":
            p.motivo = d.get("reason")
        elif estado != "busy" and d.get("seq") is not None:   # o busy sai com seq 0: não identifica o comando
            p.seq = d["seq"]
        if estado == p.estado:
            return None
        p._mudar(estado, agora_ms)
        return p

    def vencer(self, agora_ms):
        """Pedidos sem nenhum evento há SEM_ECO_MS viram "sem_eco". Devolve os que venceram."""
        vencidos = [p for p in self._pedidos.values()
                    if p.estado == "pedido" and agora_ms - p.pedido_ms >= self.sem_eco_ms]
        for p in vencidos:
            p._mudar("sem_eco", agora_ms)
        return vencidos

    def pendentes(self):
        """Pedidos ainda sem resposta."""
        return [p for p in self._pedidos.values() if p.estado == "pedido"]

    def pedido(self, rid):
        return self._pedidos.get(rid)
