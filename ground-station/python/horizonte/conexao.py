"""Regras da conexão do cliente (as mesmas da página, docs/web.md, passo 4). Lógica pura, sem rede; tempos em ms.

- Sem "tel" por SEM_TEL_MS (1 s), o dado não é atual: os gráficos mostram a lacuna.
- Validade da telemetria e vitalidade da conexão são coisas separadas (revisão de 25/10/2026). Com o ping ligado
  (horizonte.rede), a conexão é dada como morta só quando o ping fica sem resposta por ABANDONO_MS (5 s): uma pausa
  de telemetria com o B respondendo (a trava de memória do B, que chegou a 20–30 s com interferência no canal) deixa
  os dados indisponíveis, mas não derruba o socket. Sem o ping, a falta de "tel" por ABANDONO_MS abandona o socket
  (VigiaTelemetria com abandono_ms; None desliga).
- Tentativas de conexão a cada TENTATIVA_MS (2 s).
"""

SEM_TEL_MS = 1000
ABANDONO_MS = 5000
TENTATIVA_MS = 2000


class VigiaTelemetria:
    """Estado da telemetria de uma conexão aberta: "aguardando" (nenhuma tel ainda), "ok", "sem_telemetria" ou
    "abandonar" (só com abandono_ms; None nunca abandona). A contagem começa na abertura: um socket que conecta e
    nunca manda tel também é abandonado."""

    def __init__(self, agora_ms, sem_tel_ms=SEM_TEL_MS, abandono_ms=ABANDONO_MS):
        self.sem_tel_ms = sem_tel_ms
        self.abandono_ms = abandono_ms
        self._aberto_ms = agora_ms
        self._ultima_ms = None

    def tel(self, agora_ms):
        self._ultima_ms = agora_ms

    def estado(self, agora_ms):
        ref = self._ultima_ms if self._ultima_ms is not None else self._aberto_ms
        parado = agora_ms - ref
        if self.abandono_ms is not None and parado >= self.abandono_ms:
            return "abandonar"
        if self._ultima_ms is None:
            return "aguardando"
        return "sem_telemetria" if parado >= self.sem_tel_ms else "ok"


class AgendaReconexao:
    """Quando tentar de novo: TENTATIVA_MS depois do início da tentativa anterior (uma tentativa que demorou mais
    que isso é seguida logo por outra)."""

    def __init__(self, intervalo_ms=TENTATIVA_MS):
        self.intervalo_ms = intervalo_ms
        self._inicio_ms = None
        self.tentativas = 0

    def tentar_agora(self, agora_ms):
        return self._inicio_ms is None or agora_ms - self._inicio_ms >= self.intervalo_ms

    def iniciou(self, agora_ms):
        self._inicio_ms = agora_ms
        self.tentativas += 1

    def espera_ms(self, agora_ms):
        """Quanto falta para a próxima tentativa (0 se já pode)."""
        if self._inicio_ms is None:
            return 0
        return max(0, self.intervalo_ms - (agora_ms - self._inicio_ms))


class AvisoFalhas:
    """O que imprimir no terminal enquanto não há conexão (o CSV de eventos guarda todas as tentativas): a primeira
    falha e cada mudança de motivo na hora; o mesmo motivo de novo, só um lembrete a cada `lembrete_ms`."""

    def __init__(self, lembrete_ms=10_000):
        self.lembrete_ms = lembrete_ms
        self.recomecar()

    def recomecar(self):
        self.tentativas = 0
        self._inicio_ms = None
        self._motivo = None
        self._avisado_ms = None

    def falha(self, motivo, agora_ms):
        """Devolve o texto a imprimir, ou None."""
        self.tentativas += 1
        if self._inicio_ms is None:
            self._inicio_ms = agora_ms
        if motivo != self._motivo:
            self._motivo = motivo
            self._avisado_ms = agora_ms
            return motivo
        if agora_ms - self._avisado_ms >= self.lembrete_ms:
            self._avisado_ms = agora_ms
            return (f"ainda sem conexão: {self.tentativas} tentativas em "
                    f"{(agora_ms - self._inicio_ms) / 1000:.0f} s ({motivo})")
        return None

    def conectou(self):
        """Texto do fim da sequência de falhas (None se não houve), e recomeça."""
        texto = None
        if self.tentativas:
            texto = f"depois de {self.tentativas} tentativa(s) sem conexão"
        self.recomecar()
        return texto
