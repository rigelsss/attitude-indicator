"""Leitura do fluxo de atitude ("a") do monitor do ESP A (firmware/aircraft/logs/device-monitor-*.log), para a
métrica de jitter do loop da semana 6 (docs/cliente-python.md). Lógica pura: linhas → números; sem matplotlib.

Cada linha "dt" do fluxo resume uma janela de ~100 ms (as estatísticas são zeradas a cada linha, src/console.cpp):

      dt 5.00/5.01/5.01 ms (20) | atraso máx 113 µs | filtro méd/máx 56/100 µs | ambíguos 0 | descartados 0 | reinícios 0

  - dt mín./médio/máx. entre amostras fundidas (ms, 2 casas: resolução de 10 µs) e quantas amostras (n);
  - atraso máx.: do pulso do INT até o início da leitura do MPU (µs), o maior da janela;
  - filtro: custo médio e máximo do update() da fusão (µs);
  - ambíguos (quadros fora da fusão), descartados (pulsos sem leitura), reinícios do filtro.
O jitter do loop de 200 Hz é o quanto o dt se afasta dos 5 ms: por janela, o pico a pico (máx. − mín.).
"""

import re
from dataclasses import dataclass, field

RE_DT = re.compile(r"^\s*dt ([\d.]+)/([\d.]+)/([\d.]+) ms \((\d+)\) \| atraso máx (\d+) µs \| filtro méd/máx "
                   r"(\d+)/(\d+) µs \| ambíguos (\d+) \| descartados (\d+) \| reinícios (\d+)")
RE_CARIMBO = re.compile(r"compilado em (\S+ \S+)")
PERIODO_MS = 5.0   # o MPU a 200 Hz (config.h do A)


@dataclass
class LogA:
    janelas: list = field(default_factory=list)   # dicts: t_s, dt_min, dt_med, dt_max, n, atraso, filtro_med,
    #                                               filtro_max, ambiguos, descartados, reinicios
    carimbo: str = None


def ler(linhas):
    """Linhas do log do A → LogA. O tempo de cada janela (t_s) é a soma das janelas anteriores (n × dt médio)."""
    log = LogA()
    t = 0.0
    for bruta in linhas:
        linha = bruta.rstrip("\r\n")
        m = RE_DT.match(linha)
        if m:
            dt_min, dt_med, dt_max = (float(m.group(i)) for i in (1, 2, 3))
            n = int(m.group(4))
            if n == 0:
                continue   # janela sem amostra fundida (atitude ainda sem referência)
            t += n * dt_med / 1000.0
            log.janelas.append({"t_s": t, "dt_min": dt_min, "dt_med": dt_med, "dt_max": dt_max, "n": n,
                                "atraso": int(m.group(5)), "filtro_med": int(m.group(6)),
                                "filtro_max": int(m.group(7)), "ambiguos": int(m.group(8)),
                                "descartados": int(m.group(9)), "reinicios": int(m.group(10))})
            continue
        c = RE_CARIMBO.search(linha)
        if c and log.carimbo is None:
            log.carimbo = c.group(1)
    return log


def resumo(log):
    """Os números do jitter do loop. As janelas com uma amostra só (a primeira depois de ligar o fluxo) não têm pico
    a pico e ficam fora dele."""
    from .series import estatisticas
    J = log.janelas
    if not J:
        return None
    amostras = sum(j["n"] for j in J)
    com_par = [j for j in J if j["n"] > 1]
    pico = [(j["dt_max"] - j["dt_min"]) * 1000.0 for j in com_par]          # µs
    desvio_max = [max(abs(j["dt_max"] - PERIODO_MS), abs(j["dt_min"] - PERIODO_MS)) * 1000.0 for j in J]
    return {
        "janelas": len(J), "amostras": amostras, "duracao_s": J[-1]["t_s"],
        "dt_medio": sum(j["n"] * j["dt_med"] for j in J) / amostras,
        "dt_min": min(j["dt_min"] for j in J), "dt_max": max(j["dt_max"] for j in J),
        "pico_us": estatisticas(pico),
        "desvio_us": estatisticas(desvio_max),
        "janelas_ate_10us": sum(1 for d in desvio_max if d <= 10.0 + 1e-6),
        "atraso_us": estatisticas([float(j["atraso"]) for j in J]),
        "filtro_med_us": sum(j["n"] * j["filtro_med"] for j in J) / amostras,
        "filtro_max_us": estatisticas([float(j["filtro_max"]) for j in J]),
        "ambiguos": sum(j["ambiguos"] for j in J), "descartados": sum(j["descartados"] for j in J),
        "reinicios": sum(j["reinicios"] for j in J),
    }
