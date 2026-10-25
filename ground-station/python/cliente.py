"""Cliente Python do Horizonte Artificial (docs/cliente-python.md): telemetria do ESP B pelo WebSocket JSON, com log
CSV, resumo no terminal e comandos digitados (? mostra a lista). Reconecta sozinho. Os gráficos entram no passo 5.

Uso (a partir de ground-station/python):
  python cliente.py                                  # o B real, ws://192.168.4.1:81/ (PC na rede Horizonte-ESP)
  python cliente.py --url ws://127.0.0.1:8765/       # o simulador (python sim_b.py)
Encerrar: q e Enter, ou Ctrl+C. Os CSV ficam em logs/ (AAMMDD-HHMMSS-tel.csv e -eventos.csv).
"""

import argparse
import os
import signal
import sys
import time

from horizonte.conexao import AvisoFalhas
from horizonte.operador import Operador
from horizonte.registro import Processador, texto_resumo
from horizonte.rede import URL_PADRAO, ClienteWS

EVENTOS_IMPRESSOS = ("desconectado", "telemetria", "invalida", "erro")   # "falha" e "conectado": abaixo


def imprimir(texto):
    print(texto, flush=True)


def main():
    # O console do Windows pode estar numa página de código sem algum caractere: troca em vez de falhar
    for f in (sys.stdout, sys.stderr):
        try:
            f.reconfigure(errors="replace")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(description="Cliente Python do Horizonte Artificial")
    ap.add_argument("--url", default=URL_PADRAO)
    ap.add_argument("--logs", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "logs"))
    ap.add_argument("--pin", default="2468", help="PIN dos comandos (WEB_CMD_PIN no config.h do B)")
    ap.add_argument("--resumo-s", type=int, default=5, help="de quantos em quantos s sai o resumo (0 = nunca)")
    a = ap.parse_args()

    rede = ClienteWS(a.url)
    op = Operador(rede.enviar, imprimir, pin=a.pin)
    op.resumo_ligado = a.resumo_s > 0
    aviso = AvisoFalhas()
    resumos = [0]

    def ao_evento(tipo, detalhe):
        # Sem conexão, o terminal mostra a falha só quando o motivo muda e um lembrete a cada 10 s (o CSV de
        # eventos guarda todas as tentativas)
        if tipo == "falha":
            texto = aviso.falha(detalhe, time.monotonic() * 1000)
            if texto is not None:
                imprimir(f"[falha] {texto}")
        elif tipo == "conectado":
            antes = aviso.conectou()
            imprimir(f"[conectado] {detalhe}" + (f" ({antes})" if antes else ""))
        elif tipo == "envio_falhou":
            op.ao_envio_falhou(detalhe)
        elif tipo in EVENTOS_IMPRESSOS:
            imprimir(f"[{tipo}] {detalhe}")

    def ao_resumo(r):
        op.tick(reg.relogio_ms())
        resumos[0] += 1
        # Só com o socket aberto (desconectado, não há o que resumir), e a cada --resumo-s
        if op.resumo_ligado and r["estado"] != "desconectado" and resumos[0] % max(1, a.resumo_s) == 0:
            imprimir(texto_resumo(r, rede.sessao))

    reg = Processador(rede.saida, a.logs, ao_resumo=ao_resumo, ao_evento=ao_evento, ao_cmd=op.ao_cmd)
    imprimir(f"[cliente] {a.url} | log: {reg.caminho_tel}")
    imprimir("[cliente] digite ? e Enter para ver os comandos; q e Enter (ou Ctrl+C) para sair")
    reg.iniciar()
    rede.iniciar()
    try:
        while True:
            try:
                linha = input()
            except EOFError:      # sem teclado (entrada redirecionada): só registra
                while True:
                    time.sleep(0.5)
            if not op.linha(linha):
                break
    except KeyboardInterrupt:
        pass
    finally:
        # Um segundo Ctrl+C aqui interromperia a gravação do que está na fila: ignorado até o fim
        try:
            signal.signal(signal.SIGINT, signal.SIG_IGN)
        except (ValueError, OSError):
            pass
        imprimir("[cliente] encerrando: gravando o que falta (Ctrl+C de novo é ignorado)...")
        rede.parar()
        if reg.parar(timeout=10.0):
            gravacao = "tudo gravado"
        elif not reg.encerrado:
            gravacao = f"ATENÇÃO: o registro não terminou em 10 s; {reg.pendentes} mensagens não gravadas"
        else:
            gravacao = (f"ATENÇÃO: falha na gravação ({'; '.join(reg.falhas_gravacao)}); {reg.pendentes} mensagens "
                        f"não gravadas, e as do último segundo podem ter se perdido")
        imprimir(f"[cliente] encerrado: {reg.linhas_tel} telemetrias, {reg.contador_n.puladas} puladas pelo B, "
                 f"{rede.sessao} conexões, {reg.invalidas} inválidas, {reg.erros} erros, {rede.descartadas} "
                 f"descartadas na fila | {gravacao}")


if __name__ == "__main__":
    main()
