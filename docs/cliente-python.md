# Cliente Python e métricas

Semana 6 do [plano.md](plano.md), adiantada (prevista para 12–18/11). O cliente roda no PC, ligado ao AP "Horizonte-ESP" do ESP B, e usa o WebSocket JSON ([protocolo-websocket.md](protocolo-websocket.md)): log CSV, gráficos ao vivo, comandos e métricas (jitter do loop, perda de pacotes e latência). Código em `ground-station/python`.

Plano aprovado em 25/10/2026.

## Decisões

- **Leitura sem bloqueio.**
  - Uma thread de rede (asyncio, biblioteca `websockets`) só lê o socket e põe cada mensagem numa fila, com o carimbo de chegada do PC.
  - Uma thread grava o CSV a partir da fila.
  - Os gráficos (matplotlib) ficam na thread principal e leem só os últimos segundos.
  - Assim, o B nunca vê o PC parado: ele derruba um cliente com 2 s sem progresso (`WS_STALL_MS`) e pula a telemetria de quem passa de 3 kB sem confirmação (`WS_MAX_INFLIGHT`).
- **Reconexão sozinha:** sem `tel` por 1 s, o dado não é atual (como na página). A conexão é dada como morta pelo ping a cada 1 s sem resposta por 5 s, ou pelo reset do B, e não pela falta de telemetria (revisão de 25/10/2026, abaixo). Tentativas a cada 2 s.
- **Simulador do B** (`sim_b.py`, em localhost): o desenvolvimento e os testes rodam sem o PC sair da rede de casa. A Horizonte-ESP fica para as bancadas, em blocos curtos combinados antes, porque o PC fica sem internet nelas (não há Ethernet nem ancoragem pelo USB).
- **Campos novos no JSON, só no B** (25/10/2026): `tb` (relógio do B na montagem), `n` (número da telemetria) e `loop_us` (latência da fusão no A), e o `"origin": "pc"` e o `"rid"` opcionais nos comandos. O `telemetry.h` não mudou: o `TELEMETRY_VERSION` continua 4, e a regra de atualizar os dois firmwares não se aplica. A página ignora os campos novos e mostra `· pc` no registro de comandos.
  - **O `n` é um contador só,** e não um por conexão: a telemetria é montada uma vez por volta e vai igual para todos. Para um cliente, um salto de `n` são telemetrias que o B pulou para ele.
- **Latência:** relativa em cada trecho, pelo menor valor visto, porque os relógios não estão sincronizados (`delay_ms` para o A → B; chegada no PC − `tb` para o B → PC). A absoluta é aproximada pelos RTTs: o ping do WebSocket (PC ↔ B) e um `zero_attitude` (`sent` → `done`, PC → B → A → B → PC). A medida exata, com um fio de GPIO entre o A e o B, fica para a semana 7, se fizer falta no relatório.
- **Gráficos:** ao vivo, mais um `analisar.py` que gera as figuras do relatório a partir do CSV.
- **Comandos pelo terminal,** com o PIN.
- **Testes:** pytest no PC e no CI (job `testes-python`).

## Passos

0. Wi-Fi do PC: diagnóstico (abaixo). A economia de energia foi descartada em 25/10/2026; faltam o roaming e o perfil WPA2.
1. **B:** campos `tb`, `n` e `loop_us` e o `origin` (**feito** em 25/10/2026; `tb` e `n` conferidos na bancada só com o B; o `loop_us` e o `rid` ficam para a bancada com o A).
2. **Base do cliente:** lógica pura e pytest (**feito** em 25/10/2026).
3. Simulador, leitura, CSV e reconexão (**feito** em 25/10/2026, no PC, contra o simulador).
4. Comandos no cliente (**feito** em 25/10/2026, contra o simulador).
5. Gráficos ao vivo e `analisar.py` (**feito** em 26/10/2026, no PC, contra o simulador e com os logs das bancadas).
6. Bancada 1: o PC na Horizonte-ESP (**feita** em 25/10/2026, em duas rodadas, abaixo; o teste do Wi-Fi do PC ficou inconclusivo).
7. Bancada 2: vários aparelhos, `WS_RATE_HZ` 30 contra 20 (rodadas A e B feitas em 26/10/2026: 30 Hz passa; a rodada C, com o PC, não completou por um conflito de IP do DHCP do B, abaixo).
8. Campanha de métricas (jitter do loop do A a partir dos logs existentes: feito em 27/10/2026, abaixo; falta a gravação longa com o firmware atual).
9. Item 5 da revisão de 15/10 (reinício da fusão).
10. Montagem no modelo: o modelo e o power bank do B ainda não estão disponíveis.

## Passo 0: o PC sai e volta da Horizonte-ESP

Sintomas da bancada de 24/10/2026 ([web.md](web.md), "Diagnóstico de rede…"): o PC saía e voltava do AP a cada ~33 s, pedia DHCP repetidamente e, no fim, o Windows voltou sozinho para a rede de casa.

**PC:** adaptador Qualcomm QCA9377 802.11ac (driver 12.0.0.1118), "Roaming aggressiveness" em Medium, sem MAC aleatório. O PC estava **sempre na bateria**, e na bateria o plano de energia deixa o Wi-Fi em **economia de energia média** (na tomada, desempenho máximo).

**O que os logs mostram** (log `WLAN-AutoConfig` do Windows, `wlanreport` e os logs do B `261024-185032` e `261024-192207`):

- **Na maior parte das vezes, o PC se reassocia em vez de cair.**
  - O B registra `saiu` e `entrou` em 60 a 130 ms, a cada **33,4 s** (305,4 → 338,8 → 372,2 s).
  - No Windows, a conexão ao perfil não cai; só a negociação de segurança é refeita ("Segurança sem fio interrompida/iniciada/êxito"), sem pedido do serviço WLAN. É o driver que faz isso.
  - Às vezes o handshake falha e é refeito ~4 s depois. No B, a volta leva ~4 s nesses casos (272,0 → 276,1 s).
- **Em 24/10, o driver reiniciou o adaptador 5 vezes** ("CDE relatou uma remoção de adaptador L2" e a chegada ~3 s depois) e desconectou a Horizonte-ESP 3 vezes ("desconectada pelo driver", às 17:30, 17:36 e 18:45). A volta levou ~8 s.
- **O DHCP repetido é consequência das reassociações:** o Windows reconfirma o endereço pelo cache do SSID e recebe o mesmo IP (192.168.4.4).
- **O B não informa o motivo da saída:** no IDF 4.4 do núcleo, o evento `WIFI_EVENT_AP_STADISCONNECTED` só traz o MAC e o AID (o campo do motivo é do IDF 5).
- **A volta para a rede de casa:** o perfil `brisa-1829237` está hoje em "Conectar manualmente".

**Hipóteses e contornos, a testar num bloco de ~15 min na Horizonte-ESP, com o monitor do B aberto:**

1. ~~Economia de energia do Wi-Fi na bateria~~: **descartada** na bancada de 25/10/2026 (abaixo). O carregador ficou ligado o tempo todo (Wi-Fi em desempenho máximo), e o ciclo de ~33,4 s continuou igual.
2. Negociação do handshake: o Windows começa a negociação com GCMP-128 e registra CCMP no fim; o perfil aceita WPA3 e WPA2. Teste: um perfil só WPA2/CCMP (muda uma configuração do PC; só com o OK do usuário).
3. Roaming do driver: "Roaming aggressiveness" no mínimo (exige administrador). **Melhora parcial** na bancada de 25/10/2026 (abaixo): as saídas de ~4,1 s a cada 33 s quase somem, mas as saídas longas continuam.
4. Handshake do AP: forçar WPA2 no `softAP` do B (mudança de firmware; só com o OK do usuário).

**Para o cliente, independente da causa:** uma reassociação de 60 a 130 ms com o mesmo IP não derruba o TCP. Uma de ~4 s ou ~8 s pode passar dos 2 s sem progresso do B. O cliente percebe a conexão morta pelo ping (ver "Revisão: telemetria e conexão separadas"); o simulador reproduz as duas.

## Passo 1: campos novos no B

- `lib/web_json`: `tb` e `n` sempre, e `loop_us` junto dos campos do pacote; `webParseCommand` lê o `origin` (ausente = página, `"pc"` = cliente Python, outro valor = `origem inválida`, conferido depois do PIN).
- `web_task`: o `tb` é o `millis()` da volta que monta a telemetria, e o `n` é o número dela (`telSent + 1`). Na trava de memória, nenhuma telemetria é montada, e o `n` não avança.
- `command_task`: origem `CMD_FROM_PC` (`pc` na serial e no JSON).
- Testes `native` (`test_web_json`): os três campos, os campos sem pacote do A e o `origin`. 138 casos passando; o `ground` compila (flash em 72,3 %).
- **Bancada pendente:** gravar o `ground` e conferir que a página continua igual, que um `tel` traz `tb`, `n` e `loop_us` e que os eventos de um pedido com `rid` o repetem.

### Identificador do pedido (`rid`; revisão de 25/10/2026)

A primeira versão do `comandos` casava um evento `"origin": "pc"` de `seq` desconhecido com o pedido pendente mais antigo do mesmo comando (e da mesma taxa). A revisão encontrou duas falhas, reproduzidas:

1. **Um `busy` podia alterar um pedido antigo.** O `busy` sai com `seq` 0, e a busca pelo `seq` vinha antes do tratamento dele: se um comando anterior tinha recebido o `seq` válido 0, o `busy` novo caía nele, e o pedido novo terminava como `sem_eco`.
2. **Um resultado antigo podia concluir um pedido novo.** O pedido A vencia (`sem_eco`), o pedido B do mesmo comando era feito, e o `done` atrasado de A concluía B. O replay dos comandos recentes depois do `hello` podia fazer o mesmo. Comparar nome e taxa não resolve.

**Correção:** o cliente manda um `rid` (1..65535) em cada pedido, e o B o repete em todos os eventos do comando, no `busy` e na recusa ([protocolo-websocket.md](protocolo-websocket.md)). O cliente casa só pelo `rid`; o primeiro é aleatório a cada execução. Uma resposta depois do `sem_eco` atualiza o pedido certo e fica marcada como tardia.

No B, o `rid` acompanha o pedido do `commandSubmit` até os eventos (`CmdRequest`, o lugar do comando no `command_link`, `CmdSend`, `CmdEvent`). A mesma revisão achou o `seq` 0 do `busy` em mais dois lugares da `web_task`: ele entrava nos comandos recentes (`s_recent`, pela chave `seq`) e substituía o andamento de mesmo `seq` na fila do cliente, sobrescrevendo o de um comando real de `seq` 0. Agora o `busy` não entra nos recentes e não substitui nem é substituído; ele vai só na hora e não volta numa fila refeita.

Testes: `test_command_link` (o `rid` no envio, no reenvio, no ACK, na reconsulta e no tempo limite), `test_web_json` (leitura, faixa, tipo, eco no evento, no `busy` e na recusa) e `tests/test_comandos.py` (os dois casos da revisão, o replay de outra execução e da mesma depois de uma reconexão, a recusa pelo `rid` e a volta do `rid` sem o 0). 140 casos `native` e 33 do pytest passando; o `ground` e o `ground_diag` compilam (flash em 72,4 %).

## Passo 2: base do cliente

Pacote `ground-station/python/horizonte`, só com a biblioteca padrão (o pytest roda sem as outras dependências):

| Módulo | O que faz |
|---|---|
| `mensagens` | Confere e decodifica `hello`, `tel` e `cmd`; converte a `tel` numa linha do CSV (`TEL_COLUNAS`; `null` e ausente viram vazio); monta os comandos com `"origin": "pc"` e confere a taxa antes de enviar |
| `metricas` | `dif_seq16` (volta 65535 → 0), intervalo entre chegadas (mínimo, média, máximo, desvio, p50/p95/p99), telemetrias puladas pelo `n` (um `n` menor é B reiniciado) e atraso relativo pelo menor offset numa janela de 10 × 1 s (o `delay_estimator` do B, em Python) |
| `conexao` | Estado da telemetria (`aguardando`, `ok`, `sem_telemetria` com 1 s; `abandonar` com 5 s só sem o ping) e agenda das tentativas a cada 2 s |
| `comandos` | Casa o andamento com o pedido só pelo `rid` (seção acima); um estado intermediário depois do fim é ignorado; sem nenhum evento em 3 s, `sem_eco`, e uma resposta depois disso fica marcada como tardia; tempos até cada estado; lembra os últimos 64 pedidos |

33 testes (`python -m pytest`, a partir de `ground-station/python`). Dependências em `requirements.txt` (`websockets`, `matplotlib`, `numpy`, `pytest`; o `matplotlib` ainda falta no PC).

## Passo 3: simulador, leitura, CSV e reconexão

Uso, a partir de `ground-station/python`:

```
python sim_b.py                                   # B simulado em ws://127.0.0.1:8765/
python cliente.py --url ws://127.0.0.1:8765/      # contra o simulador
python cliente.py                                 # contra o B (ws://192.168.4.1:81/, PC na Horizonte-ESP)
```

O `sim_b.py` aceita as falhas: `--reassociar-s 33.4 --reassociar-ms 4000` (o envio para e volta em rajada, sem fechar o socket), `--queda-s` (TCP abortado sem o quadro de fechamento), `--travar-apos-s`/`--travar-por-s` (B travado com o socket aberto), `--pular` (telemetria pulada para o cliente, salto de `n`) e `--perda-a` (perdas no link A → B, no `lost`).

| Módulo | O que faz |
|---|---|
| `horizonte/rede.py` | Thread de rede: o laço asyncio só lê o socket e põe cada mensagem, com o carimbo de chegada (`time.monotonic_ns`), numa fila; confere a telemetria a cada 100 ms (`conexao.VigiaTelemetria`, só para dizer se o dado vale); a conexão morre pelo ping de 1 s sem resposta por 5 s ou pelo reset do B; tenta de novo a cada 2 s. A fila cheia descarta e conta, sem bloquear a leitura. Os comandos são enviados pelo laço dela (`enviar`, de qualquer thread) |
| `horizonte/registro.py` | Thread de registro: decodifica, grava os CSV (com flush a cada 1 s), calcula as métricas e guarda os últimos 30 s para os gráficos (`amostras()`, com trava). Um erro num callback (terminal, gráfico) é contado e não para o registro |
| `horizonte/simulador.py` | O B simulado: `hello` e os comandos recentes na conexão, `tel` a 30 Hz montada uma vez para todos (JSON compacto, como o ArduinoJson), comandos com `rid`, PIN, recusas e as falhas acima |
| `cliente.py` | Linha de comando: liga a rede e o registro, imprime os eventos de conexão e o resumo de 1 s |

**CSV** (`ground-station/python/logs`, fora do git):

- `AAMMDD-HHMMSS-tel.csv`: uma linha por `tel`, nas colunas de `mensagens.TEL_COLUNAS` (chegada no PC em ns e em hora Unix, sessão, `n`, `tb`, `cur`, os campos do pacote com o `loop_us`, `link.*`, alarme) mais `atraso_bpc_ms` (atraso relativo B → PC pelo `tb`) e `puladas_ws` (telemetrias puladas pelo B antes desta).
- `AAMMDD-HHMMSS-eventos.csv`: `tentando`, `conectado`, `falha`, `desconectado` (com o motivo: fechada pelo B, conexão perdida sem quadro de fechamento, abandonada), `telemetria` (`aguardando`, `ok`, `sem_telemetria`, `abandonar`), `hello` e `cmd` (o JSON inteiro), `enviado`, `envio_falhou` e `invalida`.

**Resumo de 1 s no terminal:** `[r] sessão N | estado | tel N/s | puladas N (total N) | intervalo mín/média/máx ms (p95) | atraso B->PC máx | A->B máx | perdidos A->B N (%)`. O console do Windows pode não ter algum caractere; o cliente troca em vez de falhar (as setas são `->`).

**Testes de integração** (`tests/test_integracao.py`, ~32 s, tempo real, o simulador numa thread do mesmo processo):

| Teste | Critério |
|---|---|
| B saudável, 3 s | ≥ 80 linhas, `n` sem saltos, `tb`, `n` e `loop_us` presentes, uma sessão, nenhuma inválida |
| Queda do TCP a cada 1,5 s | reconecta (≥ 3 sessões), motivo "sem quadro de fechamento", telemetria gravada nas sessões |
| Reassociação de 300 ms a cada 1 s | nenhuma reconexão, nenhum `sem_telemetria` |
| B travado 2 s | `ok` → `sem_telemetria` → `ok`, sem reconectar |
| B travado 7 s, respondendo o ping (revisão de 25/10/2026) | `ok` → `sem_telemetria` → `ok`, sem reconectar; o RTT continua chegando |
| B sumido 8 s, sem responder o ping | `sem resposta do ping por 5 s`, reconecta |
| 20 % das telemetrias puladas | `puladas_ws` soma o mesmo que o contador, e linhas + puladas = faixa de `n` |
| Thread principal presa em conta por 3 s | maior lacuna < 500 ms, nenhum `sem_telemetria`, nada descartado |
| Comando e recusa com `rid` | `sent` e `done` com o `rid` 5 e `origin` `pc`; PIN errado recusado com o `rid` 6 |

- **A thread principal ocupada:** com o simulador no mesmo processo, ele também espera o GIL, e parte da lacuna nasce no "B": o maior intervalo foi de ~33 ms (sem carga) a ~170 ms (com outro teste rodando ao lado). O primeiro limite, 150 ms, falhou 4 vezes em 5 rodadas, todas com o teste de duração rodando ao lado; o critério ficou no que importa, longe do 1 s de "sem telemetria" e dos 2 s em que o B derruba um cliente.
- **Intervalo do simulador:** ~30/33/47 ms (mín./média/máx.): o relógio do asyncio no Windows tem resolução de ~15,6 ms. No B real, o intervalo é o da `web_task`.
- **Encontrados nos testes:** um erro de codificação no terminal (o `→` no cp1252) matava a thread de registro, e o CSV parava; agora os callbacks são protegidos. O reconhecimento rápido da `tel` na thread de rede esperava o JSON compacto do B e falhava com espaços; agora tolera os dois.
- **CI:** o job `testes-python` instala o `websockets` e roda também a integração.
- **Duração, 30 min contra o simulador** (25/10/2026, com o código de antes da revisão do registro, abaixo): pausa de 4 s a cada 33,4 s, queda do TCP a cada 300 s, 0,5 % das telemetrias puladas e 0,2 % de perdas A → B. Resultado: 6 sessões (5 quedas, todas reconectadas); 48 pausas, todas `sem_telemetria` → `ok` sem reconectar e nenhum `abandonar`; maiores lacunas de 4 058 a 4 078 ms (só as pausas); 48 169 linhas (26,8/s, a conta esperada com as pausas), 5 826 puladas (~116 por pausa mais as sorteadas); 0 inválidas, 0 descartadas na fila, 0 erros nos callbacks.
- **Rodadas do usuário no terminal** (`sim_b.py --reassociar-s 33.4 --reassociar-ms 4000 --queda-s 60` e `cliente.py`; logs `261025-142337` e `261025-142712`): 3 pausas de ~4 s com 116 puladas cada, 3 quedas reconectadas em menos de 1 ms, 1 684 linhas por sessão de 60 s (a conta exata), intervalo com mediana de 31,4 ms, todos os campos presentes; o fechamento normal do simulador aparece como `fechada pelo B (código 1001)`. O relógio da reassociação do simulador recomeça a cada conexão: com a queda a cada 60 s, só houve uma pausa por sessão.

### Revisão do registro (25/10/2026)

Três falhas, reproduzidas antes da correção:

1. **Logs sobrescritos.** O nome tinha precisão de segundos e os arquivos eram abertos com `"w"`: duas execuções no mesmo segundo, na mesma pasta, apagavam a primeira (4 linhas viraram 1). Agora os dois arquivos são criados com o modo `"x"`; com o nome em uso, `-2`, `-3`… Os dois ficam com o mesmo nome: se só o de eventos colidir, o de telemetria criado é apagado e o próximo número é tentado.
2. **Uma mensagem malformada parava a gravação.** O `decodificar` conferia só a presença dos campos: depois de uma `tel` válida, uma com `"n": "invalid"` passava e dava `TypeError` na contagem das puladas; a thread de registro morria, o CSV parava e o contador de inválidas ficava em 0. É a mesma classe da falha do terminal do passo 3, que só tinha protegido os callbacks. Correção em duas camadas:
   - **tipos conferidos no `decodificar`:** na `tel`, `cur` booleano, `tb` e `n` inteiros não negativos (booleano não conta como inteiro), `link` objeto com `state` em texto, os campos do pacote e do `link` número ou `null`, `alarm` objeto; no `cmd`, `cmd` e `state` em texto, `seq`, `rid` e `arg` inteiros. Fora disso, a mensagem é inválida: contada e registrada no CSV de eventos;
   - **o tratamento de cada mensagem é protegido:** qualquer outra exceção vira o evento `erro` (tipo, mensagem e o começo do texto recebido) e o contador `erros`, e o registro segue.
3. **O encerramento podia deixar dados na fila sem avisar.** O `parar()` voltava depois do tempo limite com a thread ainda gravando, e o processo terminava (a thread é daemon) com o `cliente.py` anunciando o encerramento. Agora o `parar()` diz se esvaziou a fila e fechou os arquivos, e quantas mensagens ficaram; o `cliente.py` espera até 10 s e termina com `tudo gravado` ou `ATENÇÃO: o registro não terminou em 10 s; N mensagens não gravadas`. A thread continua daemon, para um disco travado não segurar o Ctrl+C.

Testes em `tests/test_registro.py`: as três execuções com o mesmo nome (`x`, `x-2`, `x-3`), o par com só o de eventos ocupado, dez mensagens com tipos errados entre duas válidas (as duas válidas gravadas, dez inválidas, nenhum erro), `null` continua válido, uma exceção forçada no tratamento (um evento `erro`, as duas telemetrias gravadas), o `parar()` com o registro preso (volta no tempo e informa as pendentes) e a fila de 500 mensagens esvaziada antes de fechar. 49 testes no total.

**Segunda revisão: thread encerrada não é gravação concluída** (25/10/2026). Uma falha do disco (`OSError` ao escrever, no `flush` ou no fechamento: disco cheio, pendrive removido) saía do laço; o `finally` marcava o fim e o `parar()` devolvia `True`. Reproduzido com o disco "cheio" em todas as escritas: `parar() = True`, 0 linhas gravadas, 0 pendentes, e o `cliente.py` diria `tudo gravado`. Agora:

- `encerrado` diz só que a thread terminou; `gravou_tudo` (o retorno do `parar()`) exige a fila esvaziada e nenhuma falha de gravação;
- uma falha de gravação para o consumo (as próximas também falhariam), fica em `falhas_gravacao` com onde aconteceu (`escrita`, `flush`, `flush final`, `fechamento`) e não conta como `erro` de mensagem; o `finally` tenta o flush e o fechamento de cada arquivo separadamente e registra cada falha;
- as pendentes são contadas no `parar()`, e não quando a thread parou: depois da falha, a rede continua pondo mensagens na fila até ser parada (o primeiro teste do flush mostrou 0 em vez de 3);
- o `cliente.py` termina com `tudo gravado`, com o tempo vencido, ou com `ATENÇÃO: falha na gravação (…); N mensagens não gravadas, e as do último segundo podem ter se perdido` (o que só passou pelo buffer pode não ter chegado ao disco).

Testes novos, com um arquivo que falha num ponto escolhido: escrita falhando depois de 2 telemetrias (2 gravadas, 8 pendentes, nenhum `erro`), flush falhando no meio da execução (as 3 que chegam depois contadas), fechamento falhando com tudo escrito (`False`, 0 pendentes, a falha do fechamento listada) e o mesmo embrulho sem falha (`True`). 53 testes no total.

## Bancada de 25/10/2026, 17:19: só o B, PC na Horizonte-ESP

O `ground` com os campos novos gravado; o A desligado; o PC no carregador o tempo todo; um celular com a página. Log do cliente `261025-171931` (o do monitor do B não foi gravado: o `pio device monitor` não tinha o filtro `log2file`, que agora está no `platformio.ini` do `ground`).

- **Campos novos no B real:** as 7 470 telemetrias trazem `tb` e `n`, com `link.state` `no_signal`, `cur` 0 e sem os campos do pacote (nem o `loop_us`), como esperado sem o A.
- **Comandos recentes:** a cada uma das 10 conexões, logo depois do `hello`, os 8 comandos do console (`seq` 12784 a 12791), do mais antigo ao mais novo, todos `no_response`.
- **Taxa real da telemetria no WebSocket:** mediana de 35 ms entre mensagens (~28,5/s), e não 33,3 ms: a `web_task` monta a próxima quando `now − última ≥ 33 ms`, conferido a cada volta.
- **O cliente antes da rede:** aberto às 17:19:31, antes de o PC entrar na Horizonte-ESP; 3 min de `timed out during opening handshake` e de `rede inalcançável`, sem travar; conectou às 17:23:13.
- **A conexão do PC morria a cada ~33,4 s.** Todas as sessões: ~28 s normais, ~3 s de chegadas aos trancos (lacunas de ~160 ms a cada ~280 ms, sem telemetria pulada pelo B), depois nada; o cliente abandonava o socket aos 5 s (`abandonada: 5 s sem telemetria`) e reconectava em ~0,1 s. 9 ciclos, ~5 s sem dados em cada (~15 % do tempo). O simulador não mostrava isso: nele, a pausa não mata a conexão. No B real, o mais provável é a derrubada por 2 s sem progresso (`WS_STALL_MS`), com o reset (`tcp_abort`) perdido enquanto o PC está fora do ar; sem nada a enviar, o PC não sabia. Os trancos regulares antes de cada queda parecem a placa Wi-Fi saindo do canal para varrer outras redes, o que reforça a hipótese do roaming do driver. Falta o log do B (`[ws] … derrubado`, `[ap] saiu/entrou`) para confirmar.
- **Correção no cliente: ping a cada 1 s** (antes, o da biblioteca, a cada 20 s). O primeiro ping que chega ao B depois da derrubada volta como reset, e o cliente reconecta ~1 s depois de o PC voltar, em vez de esperar 5 s. Numa conexão viva sem telemetria (a trava de memória do B pausa a telemetria de todos), o B responde (mas o cliente ainda abandonava o socket aos 5 s sem `tel`: corrigido na revisão abaixo). Foi descartado baixar o abandono para 2,5 s: na trava de memória, o cliente reconectaria em série com o B sem memória. A resposta do ping mede o RTT PC ↔ B (evento `rtt` a cada 1 s, no resumo); sem resposta por 5 s, a biblioteca fecha (`sem resposta do ping por 5 s`). A conferir na próxima bancada: as quedas passam a aparecer como `conexão perdida` ~1 s depois de o PC voltar.
- **Teste do `busy` pelo console do B: inconclusivo.** Os 5 comandos digitados no console (`seq` 12786 a 12790) terminaram todos em `no_response`: a digitação levou mais que os ~0,9 s de um comando sem o A. Com a `rajada` do cliente (passo 4), os 5 saem juntos. No roteiro, `z` estava descrito como zerar altitude: no console do B, `z` é zerar atitude e `0` é zerar altitude.
- **Encerramento:** o cliente foi encerrado com Ctrl+C, mas o log não tem o `cliente encerrado` final. Reproduzido no PC com uma interrupção da thread principal, o caminho grava o evento e termina com `tudo gravado`; a suspeita é um segundo Ctrl+C durante o encerramento. Agora o segundo Ctrl+C é ignorado até o fim, e o cliente avisa `encerrando: gravando o que falta`.

## Passo 4: comandos pelo terminal

`horizonte/terminal.py` (o que se digita; lógica pura) e `horizonte/operador.py` (pedido com `rid`, envio pela thread de rede, andamento impresso; o rastreador fica sob uma trava, porque a thread principal pede e a de registro casa os eventos). O `cliente.py` lê o teclado; o resumo sai a cada 5 s (`--resumo-s`; `r` liga e desliga).

| Digite | Faz |
|---|---|
| `z` / `0` | zerar atitude / zerar altitude (as teclas do console do B) |
| `g` / `m` | calibrar giroscópio / magnetômetro; pedem a mesma tecla de novo em 5 s para confirmar (como o segundo toque da página) |
| `t 25` | taxa do A (10, 20, 25 ou 50 Hz, conferida antes de enviar) |
| `t! 30` | envia sem conferir, para testar a recusa do A |
| `rajada` | 5 × zerar atitude seguidos, para testar o `busy` do B (4 lugares) |
| `pin 2468` / `pin` | troca / mostra o PIN (`--pin`, padrão 2468) |
| `s` | os últimos 10 pedidos desta execução, com o estado e o tempo |
| `r`, `?`, `q` | resumo, ajuda, sair |

O terminal mostra cada mudança de estado dos pedidos deste cliente (`[cmd] #rid zerar atitude: enviado ao A (12 ms, seq N)`, `concluído (41 ms)`, `recusado pelo B (…): PIN incorreto`, `ocupado: o B já tem 4 comandos em curso`, `sem resposta do A (3 envios)`, `sem resposta do B em 3 s`, `não enviado: sem conexão`), e os comandos das outras origens uma vez por estado (`[cmd] (página) zerar altitude, seq N: …`), sem os recentes que o B repete depois do `hello`.

**Simulador sem o A** (`sim_b.py --sem-a`): como o B sozinho, cada comando faz 3 envios a cada 300 ms e termina em `no_response`; o B simulado tem 4 lugares, e o 5º pedido vira `busy` com `seq` 0 e o `rid` dele.

Testes: `test_terminal.py` (as teclas e os erros), `test_operador.py` (o caminho completo com os tempos, a confirmação das calibrações, a taxa conferida e a forçada, a rajada com o `busy` só no 5º, sem conexão, envio que falhou na rede, recusa com o motivo, outras origens uma vez por estado e sem o replay, `sem_eco` e a lista) e a integração (o RTT pelo ping; com o A: zerar, taxa, PIN errado e a calibração confirmada até o `done`; sem o A: a rajada com 4 `no_response` e 1 `busy`, o 5º). 87 testes no total. Rodada do `cliente.py` com o teclado contra o simulador sem o A: `z` em `sem resposta do A` em ~950 ms, a rajada com o 5º pedido em `ocupado` e os outros 4 em `sem resposta`, `t 30` não enviado, `q` com `tudo gravado`, RTT de 0,4 ms.

## Bancada de 25/10/2026, 17:53: só o B, comandos pelo cliente e o ping de 1 s

Logs: B `firmware/ground/logs/device-monitor-261025-175254` (o primeiro com o `log2file`; carimbo `17:18:12`) e cliente `261025-175320`. Os instantes do B foram convertidos para a hora do PC pelo `tb`. PC no carregador; um celular com a página.

- **`rid` no B real:** o `z` (17:54:45) voltou com o `rid` 54131 nos 3 `sent` e no `no_response`, e o console do B mostrou `'zerar atitude' (pc, seq 12792) enviado`; o PIN errado (17:56:33) voltou `refused`, `PIN incorreto`, com o `rid` 54133. Comando do celular visto no PC (17:58:53, `origin` `página`). Comandos recentes depois de cada um dos 14 `hello`.
- **Rajada: não testada.** Foi enviada às 17:56:59 ainda com o PIN `0000`, no instante em que o PC ficou 35 s fora da rede: os 5 pedidos foram para um socket já morto (`sem resposta do B em 3 s`). O `busy` no B real continua pendente.
- **O PC sai da rede a cada ~33,4 s e fica ~4,1 s fora.** No B: `[ap] 18:47:3D:62:34:9F saiu` e `entrou` 4 094 a 4 141 ms depois (9 vezes), mais algumas saídas de 70 a 114 ms; ~2,5 s depois da saída, `[ws] cliente derrubado: 2000 ms sem espaço no envio`. Não é a reassociação curta de 24/10 (60 a 130 ms na maioria): com 4 s fora do ar, nenhum cliente mantém a conexão, e o ping de 1 s pouco muda (o PC volta perto dos 5 s do abandono). O problema é o Wi-Fi do PC, não o cliente. Os ~3 s de chegadas aos trancos antes de cada saída continuam.
- **Roaming no mínimo (`1.Lowest`, de ~17:59 ao fim; horário dado pelo usuário):** a saída de 7,35 s às 17:59:33 é a placa reiniciando na troca. Comparação, com as saídas do PC no log do B:

  | | `3.Medium` (17:53 a 17:59, ~6 min) | `1.Lowest` (17:59:33 a 18:04:50, ~5 min) |
  |---|---|---|
  | Saídas de ~4,1 s (derrubam a conexão) | 6, a cada ~33 s | 1 |
  | Saídas curtas, 70 a 114 ms (a conexão aguenta) | 2 | 4 |
  | Saídas longas | 7,4 s e 35 s | 10,8 s, 30 s e 5,8 s |
  | Sessões mais longas do cliente | 37 a 66 s | 94 e 105 s |

  O roaming do driver explica boa parte do ciclo de 33 s: no `1.Lowest`, a maioria das saídas de 4,1 s virou reassociação curta, como as de 24/10. As saídas longas (10 a 35 s, com `rede inalcançável` no cliente: o Windows perde a rede de vez) aparecem nos dois ajustes e são outro problema; suspeitas: a varredura e a troca automática de rede do Windows. O ajuste voltou a `3.Medium` depois do teste.
- **A página aberta também no navegador do PC** (provavelmente duas abas): às 17:57:39 o PC carregou a página duas vezes, e o B chegou a ter 3 WebSockets do PC além do cliente Python (no fim, duas abas com contagens idênticas). A cada saída do PC, 3 conexões penduradas e os buffers do Wi-Fi para um aparelho ausente: heap mínimo de 27 kB, **189 pausas da trava de memória** (29 s no total, a maior de 2,1 s), 57 clientes derrubados. A trava funcionou, sem travar o B, mas atrapalha o celular. Nos testes, a página fica fechada no PC.
- **RTT PC ↔ B (ping):** mediana de 39 ms, p95 de 165 ms, máximo de 397 ms (542 medidas): alto para dois aparelhos lado a lado, coerente com a placa Wi-Fi do PC saindo do canal.
- **Próximo teste do PC:** roaming em `1.Lowest` com a varredura periódica do Windows desligada (`netsh wlan set autoconfig enabled=no interface="Wi-Fi"`, administrador; o PC deixa de reconectar sozinho; volta com `enabled=yes`). Depois, se precisar, o perfil só WPA2 (hipótese 2).

## Revisão: telemetria e conexão separadas, tempos pela chegada, orientação do magnetômetro (25/10/2026)

Três pontos de uma revisão externa, confirmados no código:

1. **O cliente desconectava numa pausa de telemetria com o ping respondido.** O estado `abandonar` da `VigiaTelemetria` abortava o socket aos 5 s sem `tel`, apesar dos pongs: o contrário do que este documento dizia para a trava de memória do B (que chegou a 20–30 s com interferência no canal, em 24/10). Agora a validade da telemetria e a vitalidade da conexão são separadas: com o ping ligado, a falta de `tel` só deixa os dados indisponíveis (`sem_telemetria`); a conexão morre pelo ping sem resposta por 5 s (`sem resposta do ping por 5 s`) ou pelo reset do B. Quem responde o ping no B é o `loop()` da biblioteca na `web_task`: com a task travada, o ping fica sem resposta. Sem o ping (`ping_s=None`), vale a regra antiga. O simulador ganhou o B "sumido" (`--sumir-apos-s`/`--sumir-por-s`: não lê o socket, então não responde o ping, e não manda nada), e o "travado" (sem telemetria, respondendo o ping) virou o caso da trava de memória.
2. **Os tempos dos comandos incluíam o atraso do registro.** O operador usava a hora em que a thread de registro tratava o evento: um evento recebido aos 20 ms e tratado aos 5 000 ms virava uma conclusão aos 5 000 ms. Agora o registro passa o carimbo de chegada (`pc_ns`, o mesmo relógio monotônico do pedido), e os tempos são contados por ele. O mesmo atraso afetava o `sem_eco`: com a fila do registro atrasada, um pedido já respondido venceria antes de a resposta ser lida; o `tick` passa a usar `Processador.relogio_ms()` (a chegada da última mensagem tratada, enquanto há fila; o relógio, com a fila vazia).
3. **A confirmação do magnetômetro mandava deixar a caixa parada.** A coleta é o contrário ([magnetometro.md](magnetometro.md)): girar a caixa devagar no mesmo lugar, uma volta de 360° com cada face apoiada, longe de metais, por até 3 min. O giroscópio continua com "deixe a caixa parada".

Testes: a pausa de 7 s com o ping respondido (sem reconectar, o RTT seguindo), o B sumido (reconecta pelo ping), a `VigiaTelemetria` sem abandono, o evento tratado aos 5 000 ms com chegada aos 20 ms (conclusão em 20 ms), o `tick` pelo relógio do registro, o `relogio_ms()` com e sem fila e o texto das duas calibrações. 92 testes no total, duas rodadas seguidas.

## Bancada 1, primeira rodada (25/10/2026, 18:59 a 19:10): A no power bank, B no USB do PC

Logs: B `device-monitor-261025-185907`, cliente `261025-185923`. Referência com o roaming em `3.Medium` a partir das 19:03; teste do Wi-Fi às 19:09; o A desligado às 19:10:17 (`[link] PERDIDO`).

**Comandos com os ACKs reais do A:**

| Pedido | Resultado |
|---|---|
| `z` (`rid` 65312) | `sent` → `done` em 66 ms; o B mostrou `'zerar atitude' (pc, seq 12794) … concluído` |
| `t 50` (`rid` 65314) | `done` em 52 ms (o primeiro `t 50`, às 18:59:57, saiu um segundo antes de uma queda e se perdeu: `sem resposta do B`) |
| `t! 30` (×2) | `rejected` pelo A em ~50 ms: a recusa do A pelo `rid` funciona |
| `pin 0000` + `0` | `refused`, `PIN incorreto`, com o `rid` |
| zerar altitude pela página (19:02:42 e 19:03:12) | `accepted` → `done` em 2 s; o cliente viu o segundo com `origin` `página` |
| depois do teste do PIN errado | o PIN foi trocado para `2462`, e não `2468`: zerar altitude (×2), calibrar giroscópio (×2) e a rajada (×5) voltaram `PIN incorreto` |

Não testados por isso: `t 25`, zerar altitude e calibrar o giroscópio pelo PC, e o `busy` (a rajada saiu com o A já desligado, mas foi recusada pelo PIN).

**Link A → B e campos do pacote** (14 371 telemetrias com o A ligado):

- `loop_us`: mediana de 974 µs, p99 1 702 µs, máximo 1 845 µs. É a primeira medida do ciclo completo (pulso do INT → fim da fusão), que inclui a leitura do MPU pelo I2C e as leituras intercaladas do barômetro e do magnetômetro; o `a` do A só mede o atraso do pulso (~110 µs) e o filtro (~60 µs). A conferir com a serial do A na campanha de métricas.
- `delay_ms` (atraso relativo A → B): mediana 1 ms, p99 20 ms, máximo 42 ms. `age_ms`: mediana 9, p99 30, máximo 487 ms. `rx_hz` de 43 a 52. 7 telemetrias em `late`.
- **Perdas A → B: 573 em 39 103 (1,47 %)**, em 257 das 666 janelas de 1 s, sem quedas até o A ser desligado. Bem acima dos 0,02–0,04 % com o B sem clientes, e na faixa dos 0,6–1 % com dois celulares (24/10): aqui o B tinha 3 WebSockets do PC (abaixo) e o PC saindo da rede.
- **`valid` = 5** (`VAL_ATT` + `VAL_ALT`) em todas as linhas, com `status` 55 (`ST_MAG_CAL` ligado): **sem `VAL_HDG`**, o filtro da proa não inicializou no boot. Em 23/10, com o A também no power bank, era 13 (`0x0D`). A investigar na serial do A (`c`, `p`): a suspeita é o magnetômetro recusando as leituras por distúrbio (o power bank perto do sensor).
- Atraso relativo B → PC: mediana 4,9 ms, p99 201 ms, máximo 1,9 s.

**Conexões:**

- **O navegador do PC abriu a página de novo:** todos os WebSockets da bancada vieram do PC (192.168.4.3), em geral 3 ao mesmo tempo (o cliente e duas abas); o celular estava na rede, mas não conectou o WebSocket. Os dois "zerar altitude" da página saíram do navegador do PC. Com o PC saindo da rede, cada saída deixava 3 conexões penduradas: heap mínimo de 22 kB, **327 pausas da trava de memória (52,7 s no total, a maior de 2,1 s)**.
- **O ping como vitalidade funcionou:** duas quedas detectadas como `sem resposta do ping por 5 s` (19:02:43 e 19:04:55), e várias saídas curtas do PC atravessadas sem reconectar (`sem_telemetria` → `ok` em 0,1 a 0,9 s).
- **O Wi-Fi do PC continua saindo**, com o período de ~33 s: entre 19:03 e 19:09 (`3.Medium`), a maioria das saídas foi curta (no mesmo segundo), e as longas foram de 4, 6, 6 e 15 s; o cliente ficou sem rede (`rede inalcançável`) em cada uma das longas. As sessões duraram 48 a 134 s (na bancada das 17:53, ~33 s). O teste do roaming no mínimo com o `autoconfig` desligado (19:09) durou ~1 min, com a saída de 22 s da troca de ajuste no começo: inconclusivo.

## Bancada 1, segunda rodada (25/10/2026, 19:35 a 19:46): a repetição

Logs: B `device-monitor-261025-193520`, cliente `261025-193532`. O navegador do PC fechado; a página só no celular. Referência com o roaming em `3.Medium` a partir das 19:38; teste do Wi-Fi às 19:44. A parte da serial do A (a proa) não gerou log.

**Comandos, todos pelo PC com o PIN certo e os ACKs reais do A:**

| Pedido | Resultado (tempos desde o `sent`) |
|---|---|
| `z` | `done` em 17 ms |
| `t 25` / `t 50` | `done` em 40 ms / no mesmo instante do `sent` |
| `t! 30` | `rejected` pelo A em 39 ms |
| `0` | `accepted` em 47 ms, `done` em 2,0 s |
| `g` (confirmado) | `accepted` em 41 ms, `done` em 4,1 s (`status` 63, com `ST_CALIBRATING`, nessas 111 telemetrias) |
| zerar atitude pela página do celular | `done` em 35 ms, visto no PC com `origin` `página` |
| `rajada`, com o A desligado | **`busy` no 5º (`rid` 40130, `seq` 0)**; os outros 4 (`seq` 12807 a 12810) com 3 envios cada e `no_response` em ~1,1 s. É a correção do `seq` 0 da revisão do `rid` funcionando no B real |

**O que mudou com o navegador do PC fechado:** só 2 WebSockets (o cliente e o celular); **28 pausas da trava de memória (4 s no total)**, contra 327 (52,7 s) na primeira rodada; heap mínimo de 30 kB (era 22 kB). As abas do navegador do PC eram a maior parte da pressão de memória.

**Proa:** `valid` = 13 (`VAL_ATT` + `VAL_ALT` + `VAL_HDG`) em todas as 11 538 telemetrias com o A: a proa válida voltou sem mudança nenhuma. O `valid` = 5 da primeira rodada (o filtro da proa sem inicializar desde o boot, com o power bank longe da caixa, como sempre) continua sem explicação; se voltar, olhar o `c` e o `p` na serial do A.

**Link A → B e latência:**

- `loop_us`: mediana 1 024 µs, p99 1 114 µs; o máximo, 10 479 µs, às 19:38:05, no fim da calibração do giroscópio (a gravação do bias na NVS desliga o cache dos dois núcleos; previsto em `docs/plano.md`, "Gravações na NVS").
- `delay_ms`: mediana 1 ms, p99 10 ms, máximo 28 ms.
- **Perdas A → B: ~1 %** (812 desde o boot do B, 0,99 %; 0,75 % antes das 19:38, 0,99 % na referência), com só o PC e um celular conectados. 62 % das perdas caíram nas janelas das saídas do PC da rede (3 s antes até 3 s depois da volta), que cobrem 45 % do tempo: uma concentração fraca. O resto acompanha o tráfego do WebSocket no canal 1 (0,02–0,04 % sem clientes): a pendência dos vários aparelhos (`WS_RATE_HZ` 30 contra 20) é o próximo teste disso.

**Wi-Fi do PC:** na referência (`3.Medium`), as saídas de ~4,1 s a cada ~33 s voltaram a ser a maioria (19:37:18, 19:37:52, 19:39:00, 19:41:45, 19:42:04, 19:42:34, 19:43:07), com algumas de 60 a 100 ms e duas de 7,3 s; o cliente reconectou ~1 s depois de cada volta. O teste com o roaming no mínimo e o `autoconfig` desligado começou com a saída de 28 s da troca de ajuste (19:43:27) e durou ~2 min até o fim do bloco, com saídas de 99 ms, 7,3 s, 83 ms e 6 s: **inconclusivo de novo**, e sem sinal de que o `autoconfig` desligado resolva.

## Um aparelho fora do ar e os outros (análise de 26/10/2026, logs da bancada 1)

- **Isolado:** o B derruba só a conexão do aparelho parado (o PC), e as outras seguem: na 2ª rodada, as conexões do celular receberam ~4 500 telemetrias com 59 puladas (1,5 %) e uma derrubada (dele mesmo) enquanto o PC entrava e saía da rede. O TFT e o OLED do B ficaram a 25 quadros/s nas duas rodadas.
- **Critério "o B nunca derruba o PC":** não passou (27 e 24 derrubadas), mas pelo Wi-Fi do PC: na 2ª rodada, todas as 24 derrubadas vieram até 4 s depois de um `[ap] … saiu` do PC, menos a do celular; na 1ª, 24 de 27, e as outras 3 foram as três conexões do PC juntas, sem saída do AP antes (o PC fora do canal, ainda associado).
- **O que vaza: memória e ar.** Com o PC fora do canal, os buffers do Wi-Fi para ele levam o heap abaixo de 32 kB, e a trava de memória pausa a telemetria de todos:

  | | Pausas para todos | Perto das saídas do PC (3 s antes até 2 s depois da volta) | Fora delas |
  |---|---|---|---|
  | 2ª rodada (PC + celular) | 28, 4,0 s em ~11 min | 16, 2,9 s | 12, 1,2 s |
  | 1ª rodada (PC com 2 abas a mais) | 327, 52,7 s | 115, 18,9 s | 212, 33,8 s |

  Na 2ª rodada, ~0,14 s por saída do PC para o celular, abaixo do 1 s de "sem telemetria" da página; o número de conexões pesou mais que as saídas (13 vezes mais pausas com as duas abas). As retransmissões para o ausente dividem o canal com o ESP-NOW: 62 % das perdas A → B nas janelas das saídas do PC, que cobrem 45 % do tempo.
- **Por que importa:** com até 8 celulares e alguns sumindo juntos (a interferência de 24/10 pausou a telemetria por 20–30 s), o efeito cresce. Medir na bancada 2 e decidir as melhorias do B registradas em [plano.md](plano.md), "Pendências da semana 6".
- **Outros critérios da bancada 1, conferidos nos logs:** nenhum erro nem mensagem inválida; reconexão ~1 s depois da volta do PC; 99,74 % e 99,96 % das telemetrias montadas pelo B no tempo conectado chegaram ao CSV; 0 quadros pela metade (2ª rodada); perdas do CSV iguais às do `[r]` do B (573 e 812).

## Passo 5: gráficos ao vivo e `analisar.py` (26/10/2026)

**Decisões** (plano aprovado em 26/10/2026):

- **Gráficos num processo separado** (`graficos.py`), lendo o CSV que o cliente grava: o desenho não disputa a CPU nem o GIL com a leitura do socket, e abrir ou fechar a janela não mexe no cliente. Para isso, o registro passou a gravar os CSV em disco a cada 0,25 s (era 1 s): ~0,3 s de atraso no desenho.
- **Seis painéis, uma escala por painel** (o guia de visualização proíbe dois eixos y; o plano falava em quatro, com proa e altitude juntas e taxa e perdas juntas): atitude (roll e pitch), proa magnética, altitude relativa, atrasos (A → B, B → PC e RTT), telemetria recebida por segundo e perdas A → B por segundo, com os últimos 30 s e a linha de estado no topo (arquivo, sessão, conexão, perdas, RTT, idade do último dado).
- **Buracos, nunca o último valor repetido:** campo vazio, `cur` = 0, flag de validade desligada (`VAL_ATT`, `VAL_HDG`, `VAL_ALT`, cada uma no seu dado), mais de 1 s entre telemetrias, troca de sessão; o RTT, com mais de 2,5 s entre medidas.
- **Cores e estilo:** a paleta de referência do guia, validada com o script dele (azul, laranja, água, no modo claro sobre `#fcfcfb`: passam em separação para daltonismo e visão normal; a água tem contraste 2,7:1, compensado com legenda, rótulo direto no último valor de cada série e as tabelas do `resumo.md`); linhas finas, grade em traço contínuo fino, texto em tinta neutra, números com vírgula decimal.
- **`analisar.py`** gera, numa pasta nova `logs/analise-<nome>/` (nunca sobrescreve: `-2`, `-3`…), `telemetria.png`, `jitter.png`, `latencia.png`, `perdas.png`, `conexoes.png` e o `resumo.md` (entrega no PC, latência, orçamento de latência, perdas A → B por minuto pelo avanço do `seq`, telemetrias puladas, conexões).
- **O jitter do loop do A** (o `dt` do fluxo `a` na serial do A) fica para a campanha de métricas (passo 8), quando houver um log do A.

**Arquivos:** `horizonte/leitor.py` (CSV lido enquanto cresce: só linhas completas, troca para o mais recente da pasta), `horizonte/series.py` (séries, buracos, contagens, perdas, estatísticas, sessões, orçamento; lógica pura), `horizonte/figuras.py` (o painel ao vivo e as figuras do relatório), `graficos.py` e `analisar.py`.

**Desenho rápido (blitting).** A primeira versão redesenhava a figura inteira a cada atualização: ~170 ms sem concorrência, e no teste de 10 min, com o simulador e o cliente ao lado, 3,9 atualizações/s (mediana de 208 ms), abaixo do critério de 4/s. Agora o fundo (eixos, grade, rótulos) é desenhado uma vez e guardado; a cada atualização, só as linhas, os rótulos diretos e a linha de estado vão por cima. A figura inteira só é redesenhada quando a escala de um painel muda (um dado sai dela, ou os dados ocupam menos de 40 % dela) ou a janela muda de tamanho: **12 ms por atualização**, com o fundo redesenhado 1 vez em 20 atualizações. A gravação inteira (`--arquivo`) desenha do jeito normal, com o zoom do matplotlib.

**Teste de 10 min** (simulador com a pausa de 4 s a cada 33,4 s, a queda a cada 60 s e 0,5 % das telemetrias puladas; o cliente; o `graficos.py --sem-janela`; três processos):

| | Sem o blitting | Com o blitting |
|---|---|---|
| Atualizações por segundo | 3,9 (abaixo do critério de 4) | **4,0** (o intervalo configurado, 0,25 s) |
| Tempo de uma atualização, mediana / máx. | 208 / 1 020 ms | 44 / 380 ms (com o simulador, o cliente e, no 1º minuto, a suíte de testes ao lado) |
| Fundo redesenhado | sempre | 22 vezes (escalas mudando nas pausas e quedas) |
| Cliente | 16 829 telemetrias, 0 descartadas, `tudo gravado`; maior lacuna fora das pausas, 94 ms | 16 832 telemetrias, 0 descartadas, `tudo gravado`; 110 ms |

O cliente não muda com o gráfico aberto: nos dois, nada descartado, e a maior lacuna fora das pausas fica na faixa do relógio do simulador no Windows (até ~110 ms).

**Conferências com os logs reais das bancadas** (`python analisar.py logs\<nome>-tel.csv`), números iguais aos calculados nas bancadas:

| Log | Resumo |
|---|---|
| `261025-193532` (bancada 1, 2ª rodada) | `loop_us` com mediana 1 024 µs, p99 1 114 µs, máximo 10 479 µs; atraso A → B com p99 de 10 ms; 217 perdidos em 23 781 pacotes do A (0,91 %, desde o início do cliente; o `[r]` do B dá 0,98 % desde o boot dele); 14 conexões |
| `261025-185923` (bancada 1, 1ª rodada) | `loop_us` com mediana 974 µs; 1,51 % de perdas; 9 conexões |
| `261025-175320` (só o B) | as seções do A com "sem dados"; 14 conexões |
| `261025-142712` (simulador) | 3 trechos sem telemetria, 348 telemetrias puladas, 4 conexões |

Olhando as figuras (o guia pede olhar o resultado, não só os números): os rótulos da mediana e do p99 se sobrepunham quando próximos (agora em alturas diferentes), o histograma do `loop_us` começava em 0 com os dados perto de 1 000 µs (agora começa perto dos dados), e no painel ao vivo os rótulos diretos de séries com o mesmo valor (roll e pitch em 0) se cobriam (agora afastados 10 pt) e a linha do RTT atravessava os buracos (agora com buraco acima de 2,5 s).

**Testes** (pytest; o CI instala o `matplotlib` e desenha sem janela):

- `test_leitor.py`: arquivo inteiro; linha pela metade lida uma vez só, quando completa; arquivo crescendo; inexistente, vazio ou só com o cabeçalho; troca para o mais recente da pasta; o mesmo nome recriado; linha corrompida contada; acentos.
- `test_series.py`: campo vazio vira buraco; dado não atual; cada flag só no seu dado; lacuna e troca de sessão; janela de 30 s; RTT dos eventos; telemetria por segundo com zero nos vazios; perdas por intervalo sem negativo num B reiniciado; perdas por minuto pelo `seq` com a volta de 16 bits e o salto de um A reiniciado; intervalos só dentro da sessão; estatísticas; sessões e trechos sem telemetria; orçamento de latência.
- `test_figuras.py`: o painel acompanha o CSV crescendo; a pausa vira buraco e a taxa vai a 0; uma atualização abaixo de 100 ms com 30 s de dados, sem redesenhar o fundo; a regra da escala; uma escala nova redesenha o fundo; o modo parado desenha as linhas.
- `test_analisar.py`: figuras e resumo com números conhecidos; só o B ("sem dados"); não sobrescreve; sem o CSV de eventos; recusa o que não é CSV do cliente.

135 testes no total.

## Bancada 2: preparação (26/10/2026)

**Objetivo:** com vários aparelhos ao mesmo tempo, medir as perdas no rádio A → B, as pausas da trava de memória do B e o efeito de um aparelho que some nos outros, a 30 e a 20 Hz de telemetria no WebSocket. Critério do plano: perdas A → B ≤ 0,5 % com todos conectados; se 30 Hz não passar e 20 Hz passar, fica 20. Os números também decidem a melhoria da trava de memória ([plano.md](plano.md), "Pendências da semana 6").

**Aparelhos:** 4 celulares, mais o PC como 5º. O PC entra só numa rodada: o Wi-Fi dele sai da rede a cada ~33 s e atrapalharia a comparação das taxas. Nas outras, ele grava o log do B pelo USB, na rede de casa, com internet.

**Firmware do B:**

- **Ambiente `ground_ws20`:** o mesmo firmware, com `WS_RATE_HZ` 20 em vez de 30 (o ambiente liga `BANCADA_WS_RATE_HZ_20`, e o `config.h` escolhe; a taxa continua só no `config.h`). Os dois ambientes compilam, e o CI compila os dois.
- **`[r] T ms: link …`:** o resumo de 1 s ganhou o instante (o `millis()` do B), como os eventos.
- **O `c` mostra a taxa:** `página: … | telemetria 30 Hz, N enviadas | …`, para o log dizer qual firmware estava gravado.

**`analisar_b.py`** (ground-station/python; leitura em `horizonte/log_b.py`, lógica pura): para cada log do monitor do B, `perdas_ab.png` (perdas por resumo de 1 s, com as saídas dos aparelhos do AP sombreadas), `memoria.png` (heap, com as pausas da trava e os limites de 32 e 40 kB), `aparelhos.png` (cada MAC no AP ao longo do tempo, com o IP) e `resumo.md` (perdas A → B, heap, pausas, WebSocket por IP com as derrubadas e o último `c`, e cada saída do AP com a pausa para todos, as perdas A → B e as derrubadas de outros IPs na janela de 3 s antes da saída até 2 s depois da volta). Com dois logs, também `comparacao-<nome>.md`, lado a lado. Os logs anteriores a 26/10, sem o instante no `[r]`, têm o tempo de cada resumo estimado pela soma das janelas (até ~1 s de erro).

Nos dois logs do B da bancada 1, os números saíram iguais aos calculados à mão: 327 pausas (52,7 s) contra 28 (4,0 s), 27 e 24 WebSockets derrubados; perdas A → B de 1,54 % e 0,89 % pelos resumos do B (1,51 % e 0,91 % pelo CSV do cliente). A figura das perdas da 2ª rodada mostra os picos de perda dentro das faixas em que o PC estava fora do AP.

Testes: `test_log_b.py` (resumos, eventos e o `c`; MAC → IP, saídas e perdas só com o link ativo; o efeito de uma saída: a pausa para todos, as perdas e o outro aparelho derrubado na janela; WebSocket por IP; o formato antigo do `[r]`, estimado pelas janelas; o `analisar_b.py` com as figuras, o resumo e a comparação). 141 testes no total.

## Bancada 2: resultados (26/10/2026, 20:11 a 20:50)

Logs do B: `device-monitor-261026-201139` (rodada A, 30 Hz), `-202847` (rodada B, 20 Hz), `-204146` (rodada C, 20 Hz com o PC); cliente da rodada C: `261026-204057`. Horários dados pelo usuário: A de 20:15 a 20:25 (modo avião às 20:20), B de 20:28 a 20:38 (modo avião às 20:32). A no power bank, B no USB do PC, 4 celulares com a página.

| | Rodada A: 30 Hz, 4 celulares | Rodada B: 20 Hz, 4 celulares |
|---|---|---|
| Perdas A → B | **0,19 %** nos 553 s com os 4 WebSockets abertos (0,17 % no log inteiro) | **0,38 %** no log (os 4 conectados o tempo todo, pelo `c`) |
| Pausas da trava de memória | 98, 14,5 s em 13,7 min (~1,1 s por minuto), a maior de 1,7 s | 54, 16,3 s em 9,6 min (~1,7 s por minuto), a maior de 2,1 s |
| Heap | mínimo 24 kB, mediana 52 kB | mínimo 23 kB, mediana 52 kB |
| Modo avião num celular (~31 s fora) | 0 ms de pausa, 0 perdas, nenhum outro derrubado | 51 ms de pausa, 10 perdas na janela, nenhum outro derrubado |
| Máx. sem confirmação por celular (último `c`) | 1 578 a 3 153 B | 1 599 a 3 168 B |

**Conclusões:**

- **O critério da bancada 2 passa a 30 Hz** (perdas ≤ 0,5 % com todos conectados). A 20 Hz não melhorou: as perdas e as pausas por minuto saíram até maiores (a diferença está na variação entre rodadas). **Fica 30 Hz**; o B volta ao `ground`.
- **As perdas de ~1 % da bancada 1 vinham do PC**, e não do número de aparelhos: com 4 celulares e sem o PC, 0,17–0,38 %. É o Wi-Fi do PC saindo do canal (o caso que o modo avião não reproduz: o celular em modo avião se despede do AP, e o efeito nos outros foi quase nulo).
- **A trava de memória pausa a telemetria com frequência com 4 celulares,** sem ligação com saídas do AP: o heap fica em ~52 kB e cai abaixo de 32 kB em picos espalhados (~1 a 1,7 s de telemetria pausada para todos por minuto). Cada celular chega a ~3 kB sem confirmar (o limite por cliente), e os buffers do Wi-Fi somam por cima. Com 8 celulares, a folga diminui. A decisão da melhoria da trava continua em [plano.md](plano.md), agora com estes números.
- **Rodada C (o PC como 5º): não completou.** O PC ficou ~5 min sem rede (`rede inalcançável` de 20:41:19 a 20:46:21; nem o cliente nem o navegador funcionaram). Causa, pelos logs: **conflito de IP criado pelo DHCP do B.** O B ofereceu ao PC o 192.168.4.2, a cada 3 a 5 s, e o Windows registrou às 20:42:05 (`Tcpip 4199`) um conflito desse endereço com o celular `3A:C0:40:0E:BF:E0`, que já o usava; depois, o B recusou o PC (`DHCPNACK`). A gravação da rodada B reiniciou o B, o que zera a tabela do DHCP dele; os celulares voltaram com os IPs que já tinham, e o servidor DHCP do ESP-IDF 4.4 não confere se o endereço está em uso antes de oferecer, nem trata a recusa (`DHCPDECLINE`): oferece o mesmo IP de novo. Afeta qualquer aparelho que entrar depois de um reinício do B com outros ainda conectados, não só o PC.

## Correções no B depois da bancada 2 (26/10/2026)

**1. Faixa do DHCP alternada a cada boot.** Contra o conflito de IP da rodada C: um contador de boots na NVS (namespace `ap`) escolhe o início da faixa, 192.168.4.2 nos boots pares e 192.168.4.100 nos ímpares (11 IPs cada, `AP_DHCP_INICIO_PAR`/`IMPAR` no `config.h`; `WiFi.softAPConfig` com o início do DHCP, logo depois do `softAP`). Os aparelhos que voltam de um reinício do B com os IPs do boot anterior não coincidem com os oferecidos agora. O boot imprime `[ap] DHCP: 192.168.4.100 a .110 (boot N)`, e o `c` mostra a faixa na linha do rádio. **É um contorno, não uma correção** (revisão de 27/10/2026): as duas faixas se repetem, e um aparelho ausente durante o boot intermediário pode voltar com um IP da faixa atual, já entregue a outro; nada garante que ele tenha pedido um IP novo no meio. O cenário entra numa bancada (ver [plano.md](plano.md)).

**2. Trava de memória mirando o culpado** (`lib/heap_guard`, 6 testes `native`). Com o heap abaixo de 32 kB, o B derruba o cliente com algo pendente (quadro por sair ou bytes sem confirmação) e sem progresso no envio há pelo menos 500 ms, o mais parado deles; uma derrubada por vez, a cada 250 ms, para a memória voltar. Sem culpado (todos progredindo, como os celulares saudáveis da bancada 2, que confirmam em lotes), ou ainda com pouca memória depois dos 250 ms, a telemetria pausa para todos como antes; durante a pausa, um culpado que apareça também cai. A consulta do TCP de cada cliente só acontece com o heap baixo. Na serial: `[ws] … cliente N derrubado: trava de memória (heap livre N kB, N ms sem progresso)`; no `c`: `culpados derrubados N`. O `analisar_b.py` separa essas derrubadas das por 2 s parado (`WebSockets derrubados (pela trava)`).

Compilados: `ground` (flash 72,6 %), `ground_ws20` e `ground_diag`; 146 testes `native`. **Falta a bancada:** a repetição da rodada C (4 celulares e o PC, a 30 Hz, logo depois de um reinício do B com os celulares conectados).

## Repetição da rodada C (26/10/2026, 21:11 a 21:24): o B travou

Log do B `device-monitor-261026-211236`; cliente `261026-211400`. Início às 21:13 (5 aparelhos), modo avião às 21:18, fim às 21:24.

- **DHCP alternado: funcionou.** `DHCP .100 a .110 (boot 1)`; o PC recebeu o 192.168.4.106. A cada reinício do B, o PC pediu o IP antigo (o .2, depois o .106, depois o .2), o B recusou (`DHCPNACK`, evento 1002 do Windows) e ele pegou um da faixa nova. Nenhum conflito de endereço no Windows (sem `Tcpip 4199`).
- **Trava nova: derrubou 3 culpados** (por exemplo, `heap livre 30 kB, 1005 ms sem progresso`). Perdas A → B de 0,14 % com os 5 aparelhos.
- **O B travou duas vezes**, às ~21:17:33 e ~21:20:39 (as duas quedas do cliente no PC), com `CORRUPT HEAP: Bad tail at 0x3fff52fa` e `Bad head at 0x3fff47c4`, `assert failed: multi_heap_free`, e reiniciou sozinho (boots 2 e 3). Backtrace idêntico, decodificado com o binário gravado: `ppTask` → `ppProcTxDone` → `esf_buf_recycle` → `free` → `multi_heap_free`. É a task do driver Wi-Fi liberando um buffer de transmissão confirmado e encontrando o bloco vizinho sobrescrito: o backtrace mostra onde a corrupção foi descoberta, não quem a causou. O segundo veio logo depois de o PC sair do AP, com o heap a 23 kB; as 3 derrubadas da trava foram longe dos travamentos.
- **Inédito:** em todos os logs do B, só aparece neste, o primeiro com a trava nova e o DHCP alternado. A bancada 1 (o PC entrando e saindo, 3 conexões dele, heap a 22 kB, 2 × 11 min) e as rodadas A e B da bancada 2 (4 celulares, sem o PC) não travaram. Depois do segundo travamento, o PC não voltou mais à rede até o fim.

**Investigação** (26–27/10/2026):

- **Consulta do TCP limitada:** a trava nova consultava o TCP de todos os clientes (na thread do TCP/IP) a cada volta da task enquanto o heap estava baixo, centenas de vezes por segundo; agora, no máximo a cada `WEB_PAUSE_CHECK_MS` (100 ms), como na pausa.
- **Chaves de bancada no `config.h`:** `BANCADA_SEM_TRAVA_NOVA` (a trava antiga: só pausa) e `BANCADA_SEM_DHCP_ALTERNADO` (a faixa padrão). Ambientes: `ground_estavel` (as duas desligadas: o comportamento das rodadas A e B, sem travamento), `ground_sem_trava` e `ground_sem_dhcp`. O `c` mostra qual trava e qual DHCP estão em uso.
- **Verificação de integridade do heap** a cada `WEB_HEAP_CHECK_MS` (100 ms) na `serial_task` (`heap_caps_check_integrity_all`): na primeira falha, `[heap] T ms: CORRUPÇÃO DO HEAP detectada (última verificação ok em T0 ms)` e os detalhes; o `c` mostra `heap: íntegro | N verificações ok`. Diz quando a corrupção aconteceu, para cruzar com os eventos em volta.
- **`analisar_b.py`:** conta os reinícios (`rst:`) e as corrupções (`CORRUPT HEAP` e a verificação do B), mantém o tempo contínuo através de um reinício (antes, a duração deste log saía 2,4 min, e não 11,9) e marca os reinícios na figura das perdas; a comparação ganhou a linha `reinícios do B / corrupções do heap`.

Compilados: `ground`, `ground_estavel`, `ground_sem_trava` e `ground_sem_dhcp`; 146 testes `native` e 143 do Python.

### Revisão de 27/10/2026 (antes da bancada da investigação)

1. **A trava podia derrubar um cliente que ainda progredia** (afeta as versões com a trava nova, `ground` e `ground_sem_dhcp`). Fora da pausa, o `s_wsOkMs` só andava com uma escrita nova; um cliente recebendo confirmações, mas acima de `WS_MAX_INFLIGHT` (sem escrita), parecia parado. Na pausa, a atualização pelas confirmações vinha depois da decisão. Agora, na consulta do TCP de cada cliente, as confirmações (bytes sem confirmação zerados ou menores que na consulta anterior) contam como progresso antes da escolha do culpado. A regra virou `heapProgressed` (`lib/heap_guard`, teste `native`), a mesma da pausa.
2. **A alternância do DHCP é um contorno:** texto do `config.h`, do código e deste documento corrigido; o cenário do aparelho ausente no boot intermediário entra nas pendências de bancada ([plano.md](plano.md)).
3. **Um A reiniciado distorcia as perdas por minuto:** o limiar do `seq` (30 000) não separa todo reinício da volta do contador (50000 → 5 dá 15 541 pela volta, contados como enviados). Agora o carimbo `t` do A, já no CSV, também marca o reinício (o `t` voltou).
4. **A corrupção do heap ficava no instante errado no `analisar_b.py`:** a linha `[heap] T ms: CORRUPÇÃO …` traz o próprio instante, e era registrada no último instante visto. Agora usa o dela (mais a base do boot); o `CORRUPT HEAP` do núcleo, sem instante, continua com o último visto.
5. **O orçamento de latência supunha 30 Hz:** a espera da `web_task` agora é o período medido na gravação, a mediana do avanço do `tb` entre telemetrias consecutivas (`periodo_ws_ms`); sem `tb`, supõe 30 Hz e diz isso no texto.

6. **O `wsCanLoad()` apagava a evidência das confirmações** (complemento do ponto 1): ele roda antes da trava, a cada envio, e sobrescrevia o valor guardado dos bytes sem confirmação sem marcar o progresso. Uma queda ainda acima de `WS_MAX_INFLIGHT` (3 200 → 3 150: confirmações chegaram, mas sem envio liberado) sumia ali, e a trava, comparando 3 150 com 3 150, via o cliente parado. Agora o `wsCanLoad()` aplica a mesma `heapProgressed` antes de sobrescrever. Só com a trava nova ligada, para o `ground_estavel` continuar igual ao firmware sem travamentos (lá, o progresso pelas confirmações também mudaria a derrubada por `WS_STALL_MS`).

**No teste do travamento:** os pontos 1, 4 e 6 mudam a investigação (a trava nova derruba menos, e o instante da corrupção sai certo); os pontos 2, 3 e 5 não (o DHCP não muda, o A não deve reiniciar, e as rodadas são a 30 Hz).

## Passo 8 (parte): jitter do loop do A (27/10/2026)

`analisar_a.py` (leitura em `horizonte/log_a.py`, lógica pura): a partir do fluxo de atitude (`a`) do monitor do A. Cada linha `dt` resume uma janela de ~100 ms, com as estatísticas zeradas a cada linha (`src/console.cpp`): dt mínimo, médio e máximo entre as amostras fundidas (2 casas: resolução de 10 µs), o atraso máximo do pulso do INT até a leitura do MPU, o custo médio e máximo do filtro e os ambíguos, descartados e reinícios da janela. O jitter do loop de 200 Hz é o quanto o dt se afasta dos 5 ms: por janela, o pico a pico (máximo − mínimo) e o maior desvio. Gera `dt.png` (o maior e o menor dt de cada janela, em dois painéis com a mesma escala: numa só, as linhas coincidem quase sempre em 5,01 ms), `jitter.png` (histogramas do pico a pico e do atraso do pulso), `filtro.png` e `resumo.md`; com vários logs, `comparacao-a-<nome>.md`.

Nos logs que já existiam (as rodadas "r e a juntos" do [tasks.md](tasks.md), ~80 s cada), números iguais aos registrados lá:

| | Tasks, sem rádio | Com o rádio (filtro na flash) | Rádio e OLED (filtro na IRAM) |
|---|---|---|---|
| Log (firmware) | `261021-150632` (21/10 15:01) | `261021-173359` (21/10 17:28) | `261022-103255` (22/10 10:23) |
| Amostras | 16 421 | 16 681 | 15 941 |
| dt médio | 5,01 ms | 5,01 ms | 5,01 ms |
| Menor / maior dt | 5,00 / 5,01 ms | 5,00 / 5,01 ms | 4,99 / 5,02 ms |
| Pico a pico do dt por janela, mediana / máx. | 0 / 10 µs | 0 / 10 µs | 0 / 30 µs |
| Janelas com todo dt a até 10 µs dos 5 ms | 100 % | 100 % | 99,9 % |
| Atraso do pulso até a leitura, mediana / máx. | 109 / 118 µs | 102 / 115 µs | 110 / 124 µs |
| Filtro, médio / máx. | 28 / 161 µs | 145 / 277 µs | 53 / 161 µs |
| Ambíguos / descartados / reinícios | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 |

- **O jitter do loop fica abaixo da resolução do log** (10 µs) em quase todas as janelas: o pico a pico mediano é 0, e o maior foi 30 µs, uma única janela (4,99/5,02 ms, o mesmo caso de ~10 µs no carimbo de um pulso já visto antes das tasks, [tasks.md](tasks.md)). O período médio sai em 5,01 ms (~200 Hz; a média também só vale até a 2ª casa).
- **O atraso do pulso até a leitura** fica em ~110 µs, com o máximo abaixo de 125 µs, com e sem o rádio e o OLED.
- **O custo do filtro** mostra o efeito do layout da flash e da correção pela IRAM (`HOT_PATH`), já registrado no [tasks.md](tasks.md): 28 → 145 → 53 µs de média.
- O `loop_us` da telemetria (pulso → fim da fusão, ~1 ms, bancada 1) inclui a leitura do MPU pelo I2C e as leituras intercaladas do barômetro e do magnetômetro; o atraso daqui vai só até o início da leitura.
- **Falta:** uma gravação longa (10 min) do fluxo `a` com o firmware atual do A (23/10), com o rádio ativo, na campanha de métricas.

Testes: `test_log_a.py` (as janelas, a de n = 0 fora, o carimbo e o tempo acumulado; o resumo com o pico a pico só das janelas com mais de uma amostra, o desvio, o filtro médio ponderado e os contadores; o `analisar_a.py` com as figuras, o resumo, a comparação e a recusa de um log sem o fluxo).

