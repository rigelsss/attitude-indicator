"""Testes de horizonte.leitor: o CSV lido enquanto o cliente grava."""

import os
import time

import pytest

from horizonte.leitor import LeitorCSV, caminho_eventos, mais_recente

CAB = "a,b,c\n"


def escrever(caminho, texto, modo="a"):
    with open(caminho, modo, encoding="utf-8", newline="") as f:
        f.write(texto)


def test_le_o_arquivo_inteiro(tmp_path):
    p = tmp_path / "x-tel.csv"
    escrever(p, CAB + "".join(f"{i},{i * 2},z\n" for i in range(100)), "w")
    linhas = LeitorCSV(caminho=str(p)).ler_novas()
    assert len(linhas) == 100 and linhas[7] == {"a": "7", "b": "14", "c": "z"}


def test_linha_pela_metade_espera_e_e_lida_uma_vez(tmp_path):
    p = tmp_path / "x-tel.csv"
    escrever(p, CAB + "1,2,3\n4,5", "w")            # o cliente no meio da escrita
    leitor = LeitorCSV(caminho=str(p))
    assert leitor.ler_novas() == [{"a": "1", "b": "2", "c": "3"}]
    assert leitor.ler_novas() == []                  # a metade não sai
    escrever(p, ",6\n")
    assert leitor.ler_novas() == [{"a": "4", "b": "5", "c": "6"}]
    assert leitor.ler_novas() == []                  # e não sai de novo


def test_arquivo_crescendo_devolve_so_as_novas(tmp_path):
    p = tmp_path / "x-tel.csv"
    escrever(p, CAB, "w")
    leitor = LeitorCSV(caminho=str(p))
    vistos = []
    for etapa in range(3):
        escrever(p, "".join(f"{etapa},{i},q\n" for i in range(5)))
        vistos.append(len(leitor.ler_novas()))
    assert vistos == [5, 5, 5]


@pytest.mark.parametrize("conteudo", [None, "", CAB])
def test_inexistente_vazio_ou_so_cabecalho(tmp_path, conteudo):
    p = tmp_path / "x-tel.csv"
    if conteudo is not None:
        escrever(p, conteudo, "w")
    leitor = LeitorCSV(caminho=str(p))
    assert leitor.ler_novas() == []
    escrever(p, (CAB if conteudo in (None, "") else "") + "1,2,3\n")   # depois aparece
    assert len(leitor.ler_novas()) == 1


def test_modo_pasta_troca_para_o_mais_recente(tmp_path):
    a = tmp_path / "261026-100000-tel.csv"
    escrever(a, CAB + "1,1,1\n", "w")
    leitor = LeitorCSV(pasta=str(tmp_path))
    assert len(leitor.ler_novas()) == 1 and leitor.trocou   # a primeira escolha também conta como troca
    b = tmp_path / "261026-100500-tel.csv"
    escrever(b, CAB + "2,2,2\n3,3,3\n", "w")
    os.utime(b, (time.time() + 5, time.time() + 5))   # mais novo
    linhas = leitor.ler_novas()
    assert leitor.trocou and leitor.caminho == str(b) and [l["a"] for l in linhas] == ["2", "3"]
    escrever(b, "4,4,4\n")
    assert [l["a"] for l in leitor.ler_novas()] == ["4"] and not leitor.trocou


def test_mesmo_nome_recriado_recomeca(tmp_path):
    p = tmp_path / "x-tel.csv"
    escrever(p, CAB + "1,1,1\n2,2,2\n", "w")
    leitor = LeitorCSV(caminho=str(p))
    leitor.ler_novas()
    escrever(p, CAB + "9,9,9\n", "w")               # menor que antes: recriado
    assert [l["a"] for l in leitor.ler_novas()] == ["9"] and leitor.trocou


def test_linha_corrompida_ignorada_e_contada(tmp_path):
    p = tmp_path / "x-tel.csv"
    escrever(p, CAB + "1,2,3\n1,2\n4,5,6,7\n8,9,10\n", "w")
    leitor = LeitorCSV(caminho=str(p))
    assert [l["a"] for l in leitor.ler_novas()] == ["1", "8"] and leitor.corrompidas == 2


def test_acentos(tmp_path):
    p = tmp_path / "x-eventos.csv"
    escrever(p, "tipo,detalhe\ncmd,\"{\"\"origin\"\":\"\"página\"\"}\"\ndesconectado,conexão perdida\n", "w")
    linhas = LeitorCSV(caminho=str(p)).ler_novas()
    assert linhas[0]["detalhe"] == '{"origin":"página"}' and linhas[1]["detalhe"] == "conexão perdida"


def test_auxiliares(tmp_path):
    assert caminho_eventos("logs/261026-1-tel.csv") == "logs/261026-1-eventos.csv"
    with pytest.raises(ValueError):
        caminho_eventos("logs/qualquer.csv")
    assert mais_recente(str(tmp_path)) is None
    with pytest.raises(ValueError):
        LeitorCSV()
