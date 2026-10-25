"""Simulador do ESP B em localhost, para testar o cliente sem a rede Horizonte-ESP (docs/cliente-python.md).

Exemplos (a partir de ground-station/python):
  python sim_b.py                                   # B saudável em ws://127.0.0.1:8765/
  python sim_b.py --reassociar-s 33.4 --reassociar-ms 4000   # o PC da bancada de 24/10
  python sim_b.py --queda-s 20 --pular 0.01 --perda-a 0.005
  python sim_b.py --travar-apos-s 15 --travar-por-s 7         # B travado com o socket aberto
Depois: python cliente.py --url ws://127.0.0.1:8765/
"""

import argparse
import asyncio

from horizonte.simulador import Falhas, SimuladorB


def main():
    ap = argparse.ArgumentParser(description="Simulador do ESP B (WebSocket JSON)")
    ap.add_argument("--porta", type=int, default=8765)
    ap.add_argument("--hz", type=float, default=30)
    ap.add_argument("--pin", default="2468")
    ap.add_argument("--reassociar-s", type=float, default=0, help="a cada quantos s o envio para (0 = nunca)")
    ap.add_argument("--reassociar-ms", type=float, default=100, help="por quanto tempo o envio para")
    ap.add_argument("--queda-s", type=float, default=0, help="a cada quantos s o TCP é abortado (0 = nunca)")
    ap.add_argument("--travar-apos-s", type=float, default=0, help="B travado depois de tantos s (0 = nunca)")
    ap.add_argument("--travar-por-s", type=float, default=0)
    ap.add_argument("--sumir-apos-s", type=float, default=0,
                    help="B sumido (sem telemetria e sem responder o ping) depois de tantos s (0 = nunca)")
    ap.add_argument("--sumir-por-s", type=float, default=0)
    ap.add_argument("--pular", type=float, default=0, help="probabilidade de pular uma tel para um cliente")
    ap.add_argument("--perda-a", type=float, default=0, help="probabilidade de perda no link A → B")
    ap.add_argument("--sem-a", action="store_true", help="como o B sem o A: comandos terminam em no_response")
    a = ap.parse_args()
    falhas = Falhas(reassociar_s=a.reassociar_s, reassociar_ms=a.reassociar_ms, queda_s=a.queda_s,
                    travar_apos_s=a.travar_apos_s, travar_por_s=a.travar_por_s, sumir_apos_s=a.sumir_apos_s,
                    sumir_por_s=a.sumir_por_s, pular=a.pular, perda_a=a.perda_a)
    sim = SimuladorB(falhas, hz=a.hz, pin=a.pin, sem_a=a.sem_a)
    print(f"[sim] B simulado em ws://127.0.0.1:{a.porta}/ ({a.hz:g} Hz{', sem o A' if a.sem_a else ''}) | {falhas}")
    try:
        asyncio.run(sim.servir(porta=a.porta))
    except KeyboardInterrupt:
        print("[sim] encerrado")


if __name__ == "__main__":
    main()
