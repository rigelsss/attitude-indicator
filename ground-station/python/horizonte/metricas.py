"""Métricas do cliente (semana 6): intervalo entre chegadas, telemetrias puladas pelo B e atraso relativo B → PC.
Lógica pura, sem rede; os instantes entram em ms, de um relógio monotônico.
"""

import math


def dif_seq16(novo, velho):
    """Diferença novo − velho de um contador de 16 bits, em 0..65535 (a volta 65535 → 0 dá 1)."""
    return (novo - velho) & 0xFFFF


def percentil(ordenados, p):
    """Percentil p (0..100) de uma lista já ordenada, por interpolação linear. None numa lista vazia."""
    if not ordenados:
        return None
    pos = (len(ordenados) - 1) * p / 100.0
    i = int(math.floor(pos))
    j = min(i + 1, len(ordenados) - 1)
    return ordenados[i] + (ordenados[j] - ordenados[i]) * (pos - i)


class EstatIntervalo:
    """Intervalos entre chegadas numa janela: mínimo, média, máximo, desvio e percentis. resumo() fecha a janela."""

    def __init__(self):
        self._ultimo = None
        self._amostras = []

    def chegada(self, agora_ms):
        if self._ultimo is not None:
            self._amostras.append(agora_ms - self._ultimo)
        self._ultimo = agora_ms

    def recomecar(self):
        """Nova conexão: o intervalo até a primeira chegada dela não conta."""
        self._ultimo = None

    def resumo(self):
        """Resumo da janela (None sem intervalos) e começo de uma nova. A última chegada continua valendo."""
        a = sorted(self._amostras)
        self._amostras = []
        if not a:
            return None
        n = len(a)
        media = sum(a) / n
        desvio = math.sqrt(sum((x - media) ** 2 for x in a) / n)
        return {
            "n": n, "min": a[0], "media": media, "max": a[-1], "desvio": desvio,
            "p50": percentil(a, 50), "p95": percentil(a, 95), "p99": percentil(a, 99),
        }


class ContadorN:
    """Telemetrias que o B pulou para este cliente, pelo campo "n" (contador do B, o mesmo para todos).

    Um salto de n (de 5 para 8) são 2 puladas. n repetido ou menor é um B reiniciado (ou uma conexão nova a um B
    reiniciado): recomeça sem contar perdas. Entre conexões, chame recomecar(): o que o B montou enquanto o cliente
    estava fora não é perda do WebSocket.
    """

    def __init__(self):
        self._ultimo = None
        self.recebidas = 0
        self.puladas = 0
        self.reinicios = 0

    def recomecar(self):
        self._ultimo = None

    def chegada(self, n):
        """Devolve as puladas antes desta (0 na primeira e num reinício)."""
        self.recebidas += 1
        puladas = 0
        if self._ultimo is not None:
            if n > self._ultimo:
                puladas = n - self._ultimo - 1
            else:
                self.reinicios += 1
        self._ultimo = n
        self.puladas += puladas
        return puladas


class EstimadorAtraso:
    """Atraso relativo de cada mensagem: (chegada − carimbo da origem) menos o menor valor recente. A mesma ideia
    do lib/delay_estimator do B, para o par B → PC (carimbo "tb").

    Os relógios não estão sincronizados, então o offset sozinho não diz nada; o menor offset recente é o de uma
    mensagem que chegou "na hora". O mínimo é tomado numa janela de `faixas` faixas de `faixa_ms` (10 × 1 s),
    para acompanhar a deriva entre os cristais. Um carimbo que volta (B reiniciado) recomeça a janela.
    Limitação: se todas as mensagens da janela atrasarem, o mínimo sobe e o atraso fica subestimado.
    """

    def __init__(self, faixa_ms=1000, faixas=10):
        if faixa_ms <= 0 or faixas <= 0:
            raise ValueError("faixa_ms e faixas precisam ser positivos")
        self.faixa_ms = faixa_ms
        self.faixas = faixas
        self.recomecar()

    def recomecar(self):
        self._min = {}            # número da faixa → menor offset nela
        self._ultimo_carimbo = None

    def chegada(self, carimbo_ms, chegada_ms):
        """Devolve o atraso estimado desta mensagem (ms, ≥ 0)."""
        if self._ultimo_carimbo is not None and carimbo_ms < self._ultimo_carimbo:
            self.recomecar()
        self._ultimo_carimbo = carimbo_ms
        offset = chegada_ms - carimbo_ms
        faixa = int(chegada_ms // self.faixa_ms)
        if faixa not in self._min or offset < self._min[faixa]:
            self._min[faixa] = offset
        for f in [f for f in self._min if f <= faixa - self.faixas]:
            del self._min[f]
        return offset - min(self._min.values())
