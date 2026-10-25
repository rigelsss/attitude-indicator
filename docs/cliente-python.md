# Cliente Python e métricas

Semana 6 do [plano.md](plano.md), adiantada (prevista para 12–18/11). O cliente roda no PC, ligado ao AP "Horizonte-ESP" do ESP B, e usa o WebSocket JSON ([protocolo-websocket.md](protocolo-websocket.md)): log CSV, gráficos ao vivo, comandos e métricas (jitter do loop, perda de pacotes e latência). Código em `ground-station/python`.

Plano aprovado em 25/10/2026.

## Decisões

- **Leitura sem bloqueio.**
  - Uma thread de rede (asyncio, biblioteca `websockets`) só lê o socket e põe cada mensagem numa fila, com o carimbo de chegada do PC.
  - Uma thread grava o CSV a partir da fila.
  - Os gráficos (matplotlib) ficam na thread principal e leem só os últimos segundos.
  - Assim, o B nunca vê o PC parado: ele derruba um cliente com 2 s sem progresso (`WS_STALL_MS`) e pula a telemetria de quem passa de 3 kB sem confirmação (`WS_MAX_INFLIGHT`).
- **Reconexão sozinha,** com as regras da página: sem `tel` por 1 s, o dado não é atual; por 5 s, o cliente abandona o socket. Tentativas a cada 2 s. O ping de 20 s da biblioteca fica ligado, como reserva.
- **Simulador do B** (`sim_b.py`, em localhost): o desenvolvimento e os testes rodam sem o PC sair da rede de casa. A Horizonte-ESP fica para as bancadas, em blocos curtos combinados antes, porque o PC fica sem internet nelas (não há Ethernet nem ancoragem pelo USB).
- **Campos novos no JSON, só no B** (25/10/2026): `tb` (relógio do B na montagem), `n` (número da telemetria) e `loop_us` (latência da fusão no A), e o `"origin": "pc"` e o `"rid"` opcionais nos comandos. O `telemetry.h` não mudou: o `TELEMETRY_VERSION` continua 4, e a regra de atualizar os dois firmwares não se aplica. A página ignora os campos novos e mostra `· pc` no registro de comandos.
  - **O `n` é um contador só,** e não um por conexão: a telemetria é montada uma vez por volta e vai igual para todos. Para um cliente, um salto de `n` são telemetrias que o B pulou para ele.
- **Latência:** relativa em cada trecho, pelo menor valor visto, porque os relógios não estão sincronizados (`delay_ms` para o A → B; chegada no PC − `tb` para o B → PC). A absoluta é aproximada pelos RTTs: o ping do WebSocket (PC ↔ B) e um `zero_attitude` (`sent` → `done`, PC → B → A → B → PC). A medida exata, com um fio de GPIO entre o A e o B, fica para a semana 7, se fizer falta no relatório.
- **Gráficos:** ao vivo, mais um `analisar.py` que gera as figuras do relatório a partir do CSV.
- **Comandos pelo terminal,** com o PIN.
- **Testes:** pytest no PC e no CI (job `testes-python`).

## Passos

0. Wi-Fi do PC: diagnóstico (abaixo). Falta a bancada dos contornos.
1. **B:** campos `tb`, `n` e `loop_us` e o `origin` (**feito** em 25/10/2026; falta a conferência na bancada).
2. **Base do cliente:** lógica pura e pytest (**feito** em 25/10/2026).
3. Simulador, leitura, CSV e reconexão (**feito** em 25/10/2026, no PC, contra o simulador).
4. Comandos no cliente.
5. Gráficos ao vivo e `analisar.py`.
6. Bancada 1: o PC na Horizonte-ESP.
7. Bancada 2: vários aparelhos, `WS_RATE_HZ` 30 contra 20.
8. Campanha de métricas.
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

1. Economia de energia do Wi-Fi na bateria: comparar 5 min na tomada (ou com o Wi-Fi em desempenho máximo na bateria) com 5 min na bateria.
2. Negociação do handshake: o Windows começa a negociação com GCMP-128 e registra CCMP no fim; o perfil aceita WPA3 e WPA2. Teste: um perfil só WPA2/CCMP (muda uma configuração do PC; só com o OK do usuário).
3. Roaming do driver: "Roaming aggressiveness" no mínimo (exige administrador).
4. Handshake do AP: forçar WPA2 no `softAP` do B (mudança de firmware; só com o OK do usuário).

**Para o cliente, independente da causa:** uma reassociação de 60 a 130 ms com o mesmo IP não derruba o TCP. Uma de ~4 s ou ~8 s pode passar dos 2 s sem progresso do B. O cliente tolera lacunas abaixo de 5 s sem largar o socket e reconecta depois disso; o simulador reproduz as duas.

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
| `conexao` | Estado da telemetria (`aguardando`, `ok`, `sem_telemetria` com 1 s, `abandonar` com 5 s) e agenda das tentativas a cada 2 s |
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
| `horizonte/rede.py` | Thread de rede: o laço asyncio só lê o socket e põe cada mensagem, com o carimbo de chegada (`time.monotonic_ns`), numa fila; confere a telemetria a cada 100 ms (`conexao.VigiaTelemetria`) e abandona o socket com 5 s sem `tel` (abortando o TCP, como o B); tenta de novo a cada 2 s. A fila cheia descarta e conta, sem bloquear a leitura. Os comandos são enviados pelo laço dela (`enviar`, de qualquer thread) |
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
| B travado 6 s | `abandonar`, motivo "abandonada", reconecta e volta a `ok` |
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

