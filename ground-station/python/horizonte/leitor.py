"""Leitura de um CSV que o cliente ainda está gravando (docs/cliente-python.md, passo 5). Lógica pura: só arquivos.

O cliente grava os CSV em pedaços (flush a cada 0,25 s), então a última linha pode estar pela metade: só as linhas
completas (terminadas em \\n) são lidas, e o resto fica para a próxima leitura. Cada chamada de ler_novas() devolve
só o que chegou desde a anterior.

Dois modos:
  - um arquivo fixo (LeitorCSV(caminho=...));
  - o mais recente de uma pasta (LeitorCSV(pasta=..., sufixo="-tel.csv")): se um arquivo mais novo aparece (o
    cliente foi reiniciado), troca para ele e marca self.trocou, para quem desenha limpar o que tinha.
"""

import csv
import glob
import io
import os


def caminho_eventos(caminho_tel):
    """O CSV de eventos que acompanha um CSV de telemetria (os dois têm o mesmo nome)."""
    if not caminho_tel.endswith("-tel.csv"):
        raise ValueError(f"não é um CSV de telemetria do cliente: {caminho_tel}")
    return caminho_tel[: -len("-tel.csv")] + "-eventos.csv"


def mais_recente(pasta, sufixo="-tel.csv"):
    """O arquivo mais novo da pasta com o sufixo (pela data de modificação), ou None."""
    candidatos = glob.glob(os.path.join(pasta, f"*{sufixo}"))
    if not candidatos:
        return None
    return max(candidatos, key=lambda c: (os.path.getmtime(c), c))


class LeitorCSV:
    def __init__(self, caminho=None, pasta=None, sufixo="-tel.csv"):
        if (caminho is None) == (pasta is None):
            raise ValueError("informe o caminho de um arquivo ou uma pasta, não os dois")
        self.pasta = pasta
        self.sufixo = sufixo
        self.caminho = caminho
        self.corrompidas = 0   # linhas com o número de colunas errado (ignoradas)
        self.trocou = False    # o arquivo mudou desde a última leitura (modo pasta, ou o mesmo nome recriado)
        self._recomecar()

    def _recomecar(self):
        self._pos = 0
        self._colunas = None

    def _escolher(self):
        if self.pasta is None:
            return
        novo = mais_recente(self.pasta, self.sufixo)
        if novo is not None and novo != self.caminho:
            self.caminho = novo
            self._recomecar()
            self.trocou = True

    def ler_novas(self):
        """As linhas completas que chegaram desde a última chamada, como dicts (coluna → texto)."""
        self.trocou = False
        self._escolher()
        if self.caminho is None or not os.path.exists(self.caminho):
            return []
        tamanho = os.path.getsize(self.caminho)
        if tamanho < self._pos:   # o arquivo diminuiu: foi recriado
            self._recomecar()
            self.trocou = True
        with open(self.caminho, "rb") as f:
            f.seek(self._pos)
            bloco = f.read()
        fim = bloco.rfind(b"\n")
        if fim < 0:
            return []   # nada completo ainda
        completo = bloco[: fim + 1]
        self._pos += len(completo)
        linhas = list(csv.reader(io.StringIO(completo.decode("utf-8", errors="replace"), newline="")))
        saida = []
        for linha in linhas:
            if self._colunas is None:
                self._colunas = linha
                continue
            if len(linha) != len(self._colunas):
                self.corrompidas += 1
                continue
            saida.append(dict(zip(self._colunas, linha)))
        return saida
