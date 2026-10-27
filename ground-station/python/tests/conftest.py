"""Configuração comum dos testes: o matplotlib desenha sem janela (backend Agg), e o auxiliar `gravar_csv` escreve
CSV sintéticos no mesmo formato do cliente (horizonte.registro)."""

import csv
import os

import pytest

try:
    import matplotlib

    matplotlib.use("Agg")
except ImportError:   # os testes que precisam dele são pulados
    pass

from horizonte.mensagens import TEL_COLUNAS
from horizonte.registro import COLUNAS_PC, EVENTOS_COLUNAS

COLUNAS_TEL = TEL_COLUNAS + COLUNAS_PC


def linha_tel(t_s, n, sessao=1, **campos):
    """Uma linha do CSV de telemetria: t_s (s) vira pc_ns; o resto, campos do CSV (texto ou número)."""
    base = {c: "" for c in COLUNAS_TEL}
    base.update({"pc_ns": str(int(t_s * 1e9)), "pc_unix": f"{1761400000 + t_s:.3f}", "sessao": str(sessao),
                 "n": str(n), "tb": str(int(t_s * 1000)), "cur": "1", "link_state": "ok", "puladas_ws": "0"})
    base.update({k: str(v) for k, v in campos.items()})
    return base


def linha_evento(t_s, tipo, detalhe="", sessao=1):
    return {"pc_ns": str(int(t_s * 1e9)), "pc_unix": f"{1761400000 + t_s:.3f}", "sessao": str(sessao),
            "tipo": tipo, "detalhe": detalhe}


@pytest.fixture
def gravar_csv(tmp_path):
    """gravar_csv(nome, linhas_tel, eventos=None) → caminho do -tel.csv em tmp_path."""
    def gravar(nome, linhas_tel, eventos=None):
        caminho = os.path.join(tmp_path, f"{nome}-tel.csv")
        with open(caminho, "w", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, COLUNAS_TEL)
            w.writeheader()
            w.writerows(linhas_tel)
        if eventos is not None:
            with open(os.path.join(tmp_path, f"{nome}-eventos.csv"), "w", newline="", encoding="utf-8") as f:
                w = csv.DictWriter(f, EVENTOS_COLUNAS)
                w.writeheader()
                w.writerows(eventos)
        return caminho
    return gravar
