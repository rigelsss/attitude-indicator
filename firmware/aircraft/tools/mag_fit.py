"""Conferência da calibração do magnetômetro fora do firmware (docs/magnetometro.md, D8).

Lê os pontos impressos pela tecla d (revisão da calibração, comando m) num log do monitor serial,
refaz o ajuste do elipsoide alinhado aos eixos (6 parâmetros, o mesmo do firmware) e ajusta também o
elipsoide completo (9 parâmetros, com acoplamentos entre eixos). Mostra:
  - se o ajuste do firmware bate com o de 6 parâmetros feito aqui;
  - quanto o elipsoide completo reduz o resíduo (critério para a evolução do D4).

Uso (a partir de firmware/aircraft):
    python tools/mag_fit.py                 # último log em logs/
    python tools/mag_fit.py logs/arquivo.log
Usa a última coleta do arquivo. Requer numpy.
"""
import glob
import os
import re
import sys

import numpy as np

INICIO = "# mag_cal inicio"
FIM = "# mag_cal fim"


def ler_coleta(caminho):
    """Devolve (pontos Nx3, dicionário do ajuste do firmware) da última coleta do arquivo."""
    with open(caminho, encoding="utf-8", errors="replace") as f:
        linhas = f.read().splitlines()
    inicios = [i for i, l in enumerate(linhas) if INICIO in l]
    if not inicios:
        sys.exit(f"nenhuma coleta ({INICIO!r}) em {caminho}")
    i0 = inicios[-1]
    pontos, firmware = [], {}
    for l in linhas[i0 + 1:]:
        if FIM in l:
            break
        if l.startswith("# firmware"):
            for chave, valor in re.findall(r"(\w+)=(\S+)", l):
                firmware[chave] = valor
            continue
        partes = l.strip().split(",")
        if len(partes) == 3:
            try:
                pontos.append([float(p) for p in partes])
            except ValueError:
                pass   # cabeçalho "x,y,z" ou linha corrompida
    if len(pontos) < 30:
        sys.exit(f"coleta com só {len(pontos)} pontos")
    return np.array(pontos), firmware


def residuo_pct(pts, b, w, r):
    m = (pts - b) @ w.T
    return float(np.sqrt(np.mean((np.linalg.norm(m, axis=1) - r) ** 2)) / r * 100)


def ajuste_6(pts):
    """A·x² + B·y² + C·z² + D·x + E·y + F·z = 1, com normalização (igual ao firmware)."""
    c0 = pts.mean(axis=0)
    s0 = np.sqrt(np.mean(np.sum((pts - c0) ** 2, axis=1)))
    u = (pts - c0) / s0
    phi = np.column_stack([u ** 2, u])
    th, *_ = np.linalg.lstsq(phi, np.ones(len(u)), rcond=None)
    a, l = th[:3], th[3:]
    if np.any(a <= 0):
        raise ValueError("coeficiente quadrático não positivo: não é um elipsoide")
    g = 1 + np.sum(l ** 2 / (4 * a))
    centro = c0 + s0 * (-l / (2 * a))
    semi = s0 * np.sqrt(g / a)
    r = float(np.cbrt(np.prod(semi)))
    w = np.diag(r / semi)
    return centro, w, r


def ajuste_9(pts):
    """Elipsoide completo: x·M·x + v·x = 1 (M simétrica). W = raiz simétrica de M normalizada."""
    c0 = pts.mean(axis=0)
    s0 = np.sqrt(np.mean(np.sum((pts - c0) ** 2, axis=1)))
    u = (pts - c0) / s0
    x, y, z = u.T
    phi = np.column_stack([x * x, y * y, z * z, 2 * x * y, 2 * x * z, 2 * y * z, x, y, z])
    th, *_ = np.linalg.lstsq(phi, np.ones(len(u)), rcond=None)
    a, b, c, d, e, f = th[:6]
    m = np.array([[a, d, e], [d, b, f], [e, f, c]])
    v = th[6:]
    cu = -0.5 * np.linalg.solve(m, v)           # centro (normalizado)
    g = 1 + cu @ m @ cu                          # (u − cu)·M·(u − cu) = g
    autoval, autovet = np.linalg.eigh(m / g)
    if np.any(autoval <= 0):
        raise ValueError("matriz não positiva definida: não é um elipsoide")
    semi = s0 / np.sqrt(autoval)                 # semieixos em µT
    r = float(np.cbrt(np.prod(semi)))
    w = autovet @ np.diag(r / semi) @ autovet.T  # leva o elipsoide à esfera de raio r
    return c0 + s0 * cu, w, r


def main():
    sys.stdout.reconfigure(encoding="utf-8")   # acentos e µ no console do Windows
    if len(sys.argv) > 1:
        caminho = sys.argv[1]
    else:
        logs = sorted(glob.glob(os.path.join("logs", "*.log")), key=os.path.getmtime)
        if not logs:
            sys.exit("nenhum log em logs/; passe o caminho do arquivo")
        caminho = logs[-1]
    pts, fw = ler_coleta(caminho)
    print(f"arquivo: {caminho}\npontos: {len(pts)}")
    np.set_printoptions(precision=3, suppress=True)

    b6, w6, r6 = ajuste_6(pts)
    res6 = residuo_pct(pts, b6, w6, r6)
    print("\n6 parâmetros (alinhado aos eixos, como no firmware)")
    print(f"  offset {b6} µT | W diag {np.diag(w6)} | raio {r6:.3f} µT | resíduo {res6:.3f} %")

    if fw:
        bf = np.array([float(t) for t in fw["b"].split(",")])
        wf = np.array([float(t) for t in fw["w"].split(",")])
        rf = float(fw["R"])
        print(f"  firmware: status {fw.get('status')} | offset {bf} | W diag {wf} | raio {rf:.3f} | "
              f"resíduo {float(fw['resid']):.3f} %")
        d_off = np.max(np.abs(bf - b6))
        d_w = np.max(np.abs(wf - np.diag(w6)))
        ok = d_off < 0.05 and d_w < 1e-3 and abs(rf - r6) < 0.05
        print(f"  diferença: offset {d_off:.4f} µT, W {d_w:.5f}, raio {abs(rf - r6):.4f} µT -> "
              f"{'BATE' if ok else 'NÃO BATE'}")

    try:
        b9, w9, r9 = ajuste_9(pts)
    except (ValueError, np.linalg.LinAlgError) as e:
        print(f"\n9 parâmetros: {e}")
        return
    res9 = residuo_pct(pts, b9, w9, r9)
    print("\n9 parâmetros (elipsoide completo)")
    print(f"  offset {b9} µT | raio {r9:.3f} µT | resíduo {res9:.3f} %")
    print(f"  W =\n{w9}")
    fora = np.max(np.abs(w9 - np.diag(np.diag(w9))))
    print(f"  maior termo fora da diagonal: {fora:.4f}")
    print(f"\nresíduo: 6 parâmetros {res6:.3f} % -> 9 parâmetros {res9:.3f} % "
          f"(redução de {(1 - res9 / res6) * 100 if res6 > 0 else 0:.0f} %)")


if __name__ == "__main__":
    main()
