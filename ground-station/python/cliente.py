"""Cliente Python do Horizonte Artificial (docs/cliente-python.md): telemetria do ESP B pelo WebSocket JSON, com log
CSV e resumo de 1 s no terminal. Reconecta sozinho. Comandos e gráficos entram nos passos 4 e 5.

Uso (a partir de ground-station/python):
  python cliente.py                                  # o B real, ws://192.168.4.1:81/ (PC na rede Horizonte-ESP)
  python cliente.py --url ws://127.0.0.1:8765/       # o simulador (python sim_b.py)
Encerrar: Ctrl+C. Os CSV ficam em logs/ (AAMMDD-HHMMSS-tel.csv e -eventos.csv).
"""

import argparse
import os
import sys
import time

from horizonte.conexao import AvisoFalhas
from horizonte.registro import Processador, texto_resumo
from horizonte.rede import URL_PADRAO, ClienteWS

EVENTOS_IMPRESSOS = ("desconectado", "telemetria", "invalida", "erro", "envio_falhou")   # "falha" e "conectado": abaixo


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
    ap.add_argument("--sem-resumo", action="store_true", help="não imprime o resumo de 1 s")
    a = ap.parse_args()

    rede = ClienteWS(a.url)

    aviso = AvisoFalhas()

    def ao_evento(tipo, detalhe):
        # Sem conexão, o terminal mostra a falha só quando o motivo muda e um lembrete a cada 10 s (o CSV de
        # eventos guarda todas as tentativas)
        if tipo == "falha":
            texto = aviso.falha(detalhe, time.monotonic() * 1000)
            if texto is not None:
                print(f"[falha] {texto}", flush=True)
        elif tipo == "conectado":
            antes = aviso.conectou()
            print(f"[conectado] {detalhe}" + (f" ({antes})" if antes else ""), flush=True)
        elif tipo in EVENTOS_IMPRESSOS:
            print(f"[{tipo}] {detalhe}", flush=True)

    def ao_resumo(r):
        # Só com o socket aberto: desconectado, não há o que resumir
        if not a.sem_resumo and r["estado"] != "desconectado":
            print(texto_resumo(r, rede.sessao), flush=True)

    reg = Processador(rede.saida, a.logs, ao_resumo=ao_resumo, ao_evento=ao_evento)
    print(f"[cliente] {a.url} | log: {reg.caminho_tel}", flush=True)
    reg.iniciar()
    rede.iniciar()
    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        pass
    finally:
        rede.parar()
        if reg.parar(timeout=10.0):
            gravacao = "tudo gravado"
        elif not reg.encerrado:
            gravacao = f"ATENÇÃO: o registro não terminou em 10 s; {reg.pendentes} mensagens não gravadas"
        else:
            gravacao = (f"ATENÇÃO: falha na gravação ({'; '.join(reg.falhas_gravacao)}); {reg.pendentes} mensagens "
                        f"não gravadas, e as do último segundo podem ter se perdido")
        print(f"[cliente] encerrado: {reg.linhas_tel} telemetrias, {reg.contador_n.puladas} puladas pelo B, "
              f"{rede.sessao} conexões, {reg.invalidas} inválidas, {reg.erros} erros, {rede.descartadas} "
              f"descartadas na fila | {gravacao}", flush=True)


if __name__ == "__main__":
    main()
