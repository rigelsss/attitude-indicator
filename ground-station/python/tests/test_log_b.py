"""Testes de horizonte.log_b e do analisar_b.py: o log do monitor do ESP B."""

import os

import pytest

from horizonte import log_b as B

PC, CEL = "18:47:3D:62:34:9F", "A6:E5:E4:C0:F6:20"


def r(ms, perdidos=0, rec=50, heap=60, estado="OK", com_ms=True):
    inst = f"{ms} ms: " if com_ms else ""
    return (f"[r] {inst}link {estado} | rec {rec} em 1000 ms | perdidos {perdidos} | dup 0 | fora de ordem 0 | "
            f"ressinc 0 | recusados 0 | outro remetente 0 | fila cheia 0 | intervalo 20.0/20.0/21.0 ms | "
            f"tft 25 q/s, maior 30 ms | heap {heap} kB (bloco 50 kB)")


LOG = [
    "firmware: compilado em 2026-10-26 15:00:00 | contrato v4, TelemetryPacket 56 bytes",
    r(1000),
    f"[ap] 1500 ms: celular {CEL} entrou | no AP agora: 1",
    "[ap] 1510 ms: IP 192.168.4.2 entregue pelo DHCP",
    "[ws] 1800 ms: cliente 0 conectado (192.168.4.2)",
    r(2000),
    f"[ap] 2500 ms: celular {PC} entrou | no AP agora: 2",
    "[ap] 2520 ms: IP 192.168.4.3 entregue pelo DHCP",
    "[ws] 2700 ms: cliente 1 conectado (192.168.4.3)",
    r(3000), r(4000), r(5000),
    f"[ap] 5500 ms: celular {PC} saiu | no AP agora: 1",
    r(6000, perdidos=4, heap=30),
    "[web] 6100 ms: telemetria PAUSADA para todos: heap livre 30 kB (abaixo de 32 kB)",
    "[web] 6400 ms: telemetria retomada: heap livre 45 kB, pausada por 300 ms",
    r(7000, perdidos=2, heap=45),
    "[ws] 7500 ms: cliente 1 derrubado: 2000 ms sem espaço no envio",
    "[ws] 7500 ms: cliente 1 desconectado",
    "[ws] 7600 ms: cliente 0 derrubado: trava de memória (heap livre 28 kB, 640 ms sem progresso)",
    r(8000, perdidos=1),
    f"[ap] 9600 ms: celular {PC} entrou | no AP agora: 2",
    r(10000), r(11000), r(12000, perdidos=1), r(13000),
    "página: gravada | http://192.168.4.1 (WebSocket :81) | clientes 1 | telemetria 20 Hz, 500 enviadas | comandos",
    "ws cliente 0: 192.168.4.2 | telemetrias enviadas 300, puladas 3 | último progresso há 12 ms | "
    "sem confirmação 0 B (máx 2100, limite 3072)",
    "[link] 13500 ms: PERDIDO (500 ms sem pacotes)",
    r(14000, rec=0, estado="PERDIDO"),
]


def test_le_resumos_eventos_e_c():
    log = B.ler(LOG)
    assert log.carimbo == "2026-10-26 15:00:00" and log.taxa_hz == 20
    assert len(log.resumos) == 13 and log.resumos[5] == {"ms": 6000, "estimado": False, "estado": "OK", "rec": 50,
                                                         "janela_ms": 1000, "perdidos": 4, "heap": 30, "bloco": 50}
    assert log.pausas == [(6100, 300)]
    assert [a[2] for a in log.ap] == ["entrou", "entrou", "saiu", "entrou"]
    assert log.clientes_c == {"192.168.4.2": (300, 3, 2100)}
    assert log.link == [(13500, "PERDIDO (500 ms sem pacotes)")]


def test_mac_ip_saidas_e_perdas():
    log = B.ler(LOG)
    assert B.mac_para_ip(log) == {CEL: "192.168.4.2", PC: "192.168.4.3"}
    s = B.saidas(log)
    assert len(s) == 1 and s[0]["mac"] == PC and s[0]["duracao"] == 4100
    perd, rec, pct = B.perdas_link(log)          # só os resumos com o link OK
    assert (perd, rec) == (8, 600) and pct == pytest.approx(100 * 8 / 608)   # 12 resumos OK


def test_efeito_da_saida_nos_outros():
    e = B.efeito_das_saidas(B.ler(LOG))[0]
    assert e["ip"] == "192.168.4.3"
    assert e["janela_ms"] == (9600 + 2000) - (5500 - 3000)
    assert e["pausa_ms"] == 300                   # a pausa para todos dentro da janela
    assert e["perdidos"] == 4 + 2 + 1             # resumos de 6, 7 e 8 s (o de 12 s fica fora)
    assert e["derrubados_outros"] == ["192.168.4.2"]   # o celular derrubado na janela; o próprio PC não conta


def test_derrubadas_da_trava():
    assert B.derrubadas_trava(B.ler(LOG)) == 1


def test_clientes_ws_por_ip():
    c = B.clientes_ws(B.ler(LOG))
    assert c["192.168.4.3"] == {"conectado": 1, "desconectado": 1, "derrubado": 1}
    assert c["192.168.4.2"] == {"conectado": 1, "desconectado": 0, "derrubado": 1}


def test_formato_antigo_sem_ms_estimado_pelas_janelas():
    log = B.ler(["[ap] 5000 ms: celular X entrou | no AP agora: 1", r(0, com_ms=False), r(0, com_ms=False),
                 "[ws] 7300 ms: cliente 0 conectado (1.2.3.4)", r(0, com_ms=False)])
    assert [(x["ms"], x["estimado"]) for x in log.resumos] == [(6000, True), (7000, True), (8300, True)]
    assert B.ler([r(0, com_ms=False)]).resumos == []   # sem nenhum evento antes: sem como situar


def test_analisar_b_gera_figuras_resumo_e_comparacao(tmp_path):
    pytest.importorskip("matplotlib")
    import analisar_b
    caminhos = []
    for nome, taxa in (("device-monitor-261026-150000", "30"), ("device-monitor-261026-152000", "20")):
        p = tmp_path / f"{nome}.log"
        p.write_text("\n".join(l.replace("telemetria 20 Hz", f"telemetria {taxa} Hz") for l in LOG), encoding="utf-8")
        caminhos.append(str(p))
    pastas = [analisar_b.analisar_log(c)[0] for c in caminhos]
    for pasta in pastas:
        for f in analisar_b.FIGURAS + ("resumo.md",):
            assert os.path.getsize(os.path.join(pasta, f)) > 1000, f
    texto = open(os.path.join(pastas[0], "resumo.md"), encoding="utf-8").read()
    assert "telemetria no WebSocket: 30 Hz" in texto and "1 pausas da telemetria para todos" in texto
    assert "| 192.168.4.2 | 1 | 0 | 1 | 300 | 3 | 2 100 |" in texto
    resumos = [analisar_b.resumo(B.ler(open(c, encoding="utf-8")), os.path.basename(c)) for c in caminhos]
    destino = tmp_path / "comparacao.md"
    analisar_b.comparar(resumos, str(destino))
    comp = destino.read_text(encoding="utf-8")
    assert "| telemetria no WebSocket | 30 Hz | 20 Hz |" in comp
    assert "| WebSockets derrubados (pela trava) | 2 (1) | 2 (1) |" in comp
    assert "1 clientes derrubados pela trava" in texto
    with pytest.raises(ValueError):
        vazio = tmp_path / "vazio.log"
        vazio.write_text("nada aqui\n", encoding="utf-8")
        analisar_b.analisar_log(str(vazio))


def test_reinicio_mantem_o_tempo_continuo_e_conta_a_corrupcao():
    log = B.ler([r(5000), "[ap] 5500 ms: celular X saiu | no AP agora: 1",
                 r(6000) + " CORRUPT HEAP: Bad head at 0x3fff47c4. Expected 0xabba1234 got 0x3ffe4364",
                 "assert failed: multi_heap_free multi_heap_poisoning.c:259 (head != NULL)", "Rebooting...",
                 "rst:0xc (SW_CPU_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)",
                 r(2300), "[ap] 2400 ms: celular X entrou | no AP agora: 1",
                 "[heap] 3000 ms: CORRUPÇÃO DO HEAP detectada (última verificação ok em 2900 ms); detalhes:"])
    assert [x["ms"] for x in log.resumos] == [5000, 6000, 8300]     # depois do reinício: 6 000 + 2 300
    assert log.reinicios == [(6000, "SW_CPU_RESET")]
    # o CORRUPT HEAP no meio do resumo fica com o último instante; a verificação do B, com o próprio (3 000 + 6 000)
    assert [c[0] for c in log.corrupcoes] == [5500, 9000]
    assert [a[0] for a in log.ap] == [5500, 8400]
    assert B.saidas(log)[0]["duracao"] == 2900



def test_corrupcao_com_o_instante_proprio():
    # Revisão de 27/10/2026: uma corrupção aos 8 000 ms ficava registrada no último instante visto (1 000 ms)
    log = B.ler([r(1000), "[heap] 8000 ms: CORRUPÇÃO DO HEAP detectada (última verificação ok em 7900 ms); detalhes:"])
    assert log.corrupcoes[0][0] == 8000
