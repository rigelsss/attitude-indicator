"""Mensagens JSON do WebSocket do ESP B (contrato em docs/protocolo-websocket.md). Lógica pura, sem rede.

B → cliente: "hello" (na conexão), "tel" (telemetria a WS_RATE_HZ) e "cmd" (andamento de um comando ou recusa no
B). Cliente → B: {"cmd": ..., "arg": n, "pin": ..., "origin": "pc", "rid": n}.
"""

import json
import math

COMANDOS = ("calibrate_gyro", "calibrate_mag", "zero_attitude", "zero_altitude", "set_rate")
TAXAS_HZ = (10, 20, 25, 50)   # as que o A aceita; o "hello" repete a lista

# Colunas do CSV da telemetria, na ordem. pc_ns: chegada no PC (time.monotonic_ns); pc_unix: hora do PC (s);
# sessao: número da conexão desde o início do cliente
TEL_COLUNAS = (
    "pc_ns", "pc_unix", "sessao", "n", "tb", "cur",
    "t", "seq", "roll", "pitch", "hdg", "gx", "gy", "gz", "alt", "hpa", "temp", "status", "valid", "loop_us",
    "decl", "link_state", "age_ms", "delay_ms", "rx_hz", "lost", "loss_pct", "drops", "alarm", "alarm_acked",
)

_CAMPOS_PACOTE = ("t", "seq", "roll", "pitch", "hdg", "gx", "gy", "gz", "alt", "hpa", "temp", "status", "valid",
                  "loop_us")
_CAMPOS_LINK = ("age_ms", "delay_ms", "rx_hz", "lost", "loss_pct", "drops")


class MensagemInvalida(ValueError):
    """Texto que não é uma mensagem do contrato (JSON inválido, sem "type", campo faltando ou com tipo errado)."""


def _inteiro(v):
    return isinstance(v, int) and not isinstance(v, bool)


def _numero_ou_null(v):
    return v is None or (isinstance(v, (int, float)) and not isinstance(v, bool))


def _conferir_tel(d):
    if not isinstance(d.get("cur"), bool):
        raise MensagemInvalida("tel: cur não é booleano")
    for k in ("tb", "n"):
        if not _inteiro(d.get(k)) or d[k] < 0:
            raise MensagemInvalida(f"tel: {k} não é inteiro não negativo")
    link = d.get("link")
    if not isinstance(link, dict) or not isinstance(link.get("state"), str):
        raise MensagemInvalida("tel: link sem state em texto")
    for k in _CAMPOS_PACOTE + ("decl",):
        if not _numero_ou_null(d.get(k)):
            raise MensagemInvalida(f"tel: {k} não é número")
    for k in _CAMPOS_LINK:
        if not _numero_ou_null(link.get(k)):
            raise MensagemInvalida(f"tel: link.{k} não é número")
    alarme = d.get("alarm")
    if alarme is not None and not isinstance(alarme, dict):
        raise MensagemInvalida("tel: alarm não é objeto")


def _conferir_cmd(d):
    for k in ("cmd", "state"):
        if not isinstance(d.get(k), str):
            raise MensagemInvalida(f"cmd: {k} não é texto")
    for k in ("seq", "rid", "arg"):
        if k in d and not _inteiro(d[k]):
            raise MensagemInvalida(f"cmd: {k} não é inteiro")


def decodificar(texto):
    """Texto recebido → (tipo, dict). Tipos: "hello", "tel", "cmd". Outro tipo também volta, para o registro.

    Na "tel" e no "cmd", os campos usados pelas métricas e pelos comandos têm o tipo conferido: uma mensagem fora
    do contrato vira MensagemInvalida (contada e registrada), em vez de um erro mais adiante.
    """
    try:
        d = json.loads(texto)
    except (json.JSONDecodeError, TypeError) as e:
        raise MensagemInvalida(f"JSON inválido: {e}") from None
    if not isinstance(d, dict) or not isinstance(d.get("type"), str):
        raise MensagemInvalida("sem o campo type")
    tipo = d["type"]
    if tipo == "tel":
        _conferir_tel(d)
    elif tipo == "cmd":
        _conferir_cmd(d)
    return tipo, d


def _celula(v):
    """Valor para o CSV: ausente ou null → vazio; bool → 0/1; não finito → vazio."""
    if v is None:
        return ""
    if isinstance(v, bool):
        return 1 if v else 0
    if isinstance(v, float) and not math.isfinite(v):
        return ""
    return v


def linha_tel(d, pc_ns, pc_unix, sessao):
    """Telemetria decodificada → linha do CSV, na ordem de TEL_COLUNAS."""
    link = d.get("link") or {}
    alarme = d.get("alarm") or {}
    valores = {
        "pc_ns": pc_ns, "pc_unix": round(pc_unix, 3), "sessao": sessao,
        "n": d.get("n"), "tb": d.get("tb"), "cur": d.get("cur"),
        "decl": d.get("decl"), "link_state": link.get("state"),
        "alarm": alarme.get("kind"), "alarm_acked": alarme.get("acked"),
    }
    for k in _CAMPOS_PACOTE:
        valores[k] = d.get(k)
    for k in _CAMPOS_LINK:
        valores[k] = link.get(k)
    return [_celula(valores[c]) for c in TEL_COLUNAS]


def montar_comando(cmd, pin, arg=None, rid=None, conferir=True):
    """Comando para o B, com a origem "pc" e o identificador do pedido (rid, 1..65535; o B o repete em todos os
    eventos do comando). ValueError para um nome desconhecido, uma taxa fora da lista ou um rid inválido.

    A taxa é conferida aqui só para avisar antes; o A confere de novo e responde "rejected". conferir=False envia
    qualquer inteiro (o "t!" do terminal, para testar a recusa do A).
    """
    if cmd not in COMANDOS:
        raise ValueError(f"comando desconhecido: {cmd}")
    msg = {"cmd": cmd, "pin": pin, "origin": "pc"}
    if rid is not None:
        if not isinstance(rid, int) or isinstance(rid, bool) or not 1 <= rid <= 65535:
            raise ValueError(f"rid inválido: {rid}")
        msg["rid"] = rid
    if cmd == "set_rate":
        if not isinstance(arg, int) or isinstance(arg, bool) or (conferir and arg not in TAXAS_HZ):
            raise ValueError(f"taxa inválida: {arg} (aceitas: {', '.join(map(str, TAXAS_HZ))} Hz)")
        msg["arg"] = arg
    return json.dumps(msg, ensure_ascii=False)
