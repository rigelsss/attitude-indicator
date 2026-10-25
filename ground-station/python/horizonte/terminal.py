"""O que o usuário digita no terminal do cliente (docs/cliente-python.md, passo 4). Lógica pura: só interpreta.

As teclas dos comandos são as mesmas do console do B: g (calibrar giroscópio), m (calibrar magnetômetro),
z (zerar atitude), 0 (zerar altitude). A taxa é "t 25"; "t! 30" envia sem a conferência local, para testar a recusa
do A.
"""

AJUDA = """Comandos (Enter depois de cada um):
  z          zerar atitude              0          zerar altitude
  g          calibrar giroscópio        m          calibrar magnetômetro   (os dois pedem confirmação: g/m de novo)
  t 25       taxa do A (10, 20, 25 ou 50 Hz)       t! 30      envia sem conferir (testa a recusa do A)
  rajada     5 x zerar atitude seguidos (testa o "ocupado" do B, que tem 4 lugares)
  pin 2468   troca o PIN                pin        mostra o PIN em uso
  s          pedidos desta execução     r          liga/desliga o resumo de telemetria
  ?          esta ajuda                 q          sair (ou Ctrl+C)"""

TECLAS = {"z": "zero_attitude", "0": "zero_altitude", "g": "calibrate_gyro", "m": "calibrate_mag"}
RAJADA = 5


def interpretar(linha):
    """Linha digitada → tupla com a ação:
    ("cmd", nome, arg, conferir) | ("rajada", n) | ("pin", valor ou None) | ("lista",) | ("resumo",) |
    ("ajuda",) | ("sair",) | ("vazio",) | ("erro", mensagem)."""
    partes = linha.strip().split()
    if not partes:
        return ("vazio",)
    chave, args = partes[0].lower(), partes[1:]
    if chave in TECLAS and not args:
        return ("cmd", TECLAS[chave], None, True)
    if chave in ("t", "t!"):
        if len(args) != 1:
            return ("erro", "use: t 25 (taxa em Hz)")
        try:
            hz = int(args[0])
        except ValueError:
            return ("erro", f"taxa não é um número inteiro: {args[0]}")
        return ("cmd", "set_rate", hz, chave == "t")
    if chave == "rajada" and not args:
        return ("rajada", RAJADA)
    if chave == "pin":
        if len(args) > 1:
            return ("erro", "use: pin 2468")
        return ("pin", args[0] if args else None)
    simples = {"s": "lista", "r": "resumo", "?": "ajuda", "ajuda": "ajuda", "q": "sair", "sair": "sair"}
    if chave in simples and not args:
        return (simples[chave],)
    return ("erro", f"não entendi: {linha.strip()!r} (? para a ajuda)")
