# Rádio: ESP-NOW do ESP A para o ESP B

Semana 3 do [plano.md](plano.md). O contrato é o `firmware/shared/include/telemetry.h` (v4: desde 21/10/2026, o `loop_us` é a latência do dado). O A envia o `TelemetryPacket` a `TX_RATE_HZ` (50 Hz); o B recebe a partir do passo 5. Comandos B → A (`CommandPacket`) e ACK ficam para a semana 5.

## ESP A (`src/tasks/radio_task.cpp`)

- **Task:** `radio_task`, no núcleo 0 (junto com o Wi-Fi do IDF), com prioridade 2. Liga o Wi-Fi `RADIO_START_DELAY_MS` (1 s) depois do boot, com a linha `[radio] ligando o Wi-Fi...` impressa antes, por causa do brownout de 21/10 ([hardware.md](hardware.md), "Brownout ao ligar o Wi-Fi").
- **Configuração:**
  - `WiFi.mode(WIFI_STA)`, sem se associar a nenhuma rede: o canal fica fixo, sem varredura;
  - `esp_wifi_set_ps(WIFI_PS_NONE)`, sem economia de energia;
  - `esp_wifi_set_channel(ESPNOW_CHANNEL, …)`, o canal 1, o mesmo do AP do B;
  - `esp_now_init` e o peer `PEER_MAC` (o MAC base do B, `14:33:5C:03:8A:B4`), no canal 1, interface STA, sem criptografia.
- **Envio:** a 50 Hz pelo próprio relógio (`vTaskDelayUntil`), **um em voo por vez** desde 23/10/2026: com o envio anterior ainda sem callback, o ciclo é pulado e contado, e o `seq` avança. Nunca liberado só pelo tempo; preso por 1 s, o ESP-NOW é reiniciado (`lib/send_gate`) ([solo.md](solo.md), "Passo 4"). A task lê a última cópia do estado publicada pela `sensor_task` (caixa de correio: fila de 1, com `xQueueOverwrite` e `xQueuePeek`), monta o pacote (`buildTelemetry`, `lib/telemetry_pack`) e chama `esp_now_send`. O `seq` é de 16 bits e dá a volta em 65535.
- **Callbacks** (task do Wi-Fi, núcleo 0): só contam, em contadores atômicos.
  - **Envio:** entregue (o B confirmou com o ACK da camada MAC) ou falha MAC (sem confirmação depois das retransmissões).
  - **Recepção:** só aceita o `PEER_MAC`. O `CommandPacket` é copiado com `memcpy` e conferido (tamanho, magic, versão, tipo; `decodeCommand`); os aceitos e os recusados são contados. A execução é da semana 5.
- **Console:**
  - `r`: `radio: N enviados/s | entregues N/s | falhas MAC N/s | pulados N/s | erros de envio N | comandos aceitos N, recusados N`;
  - `c`: `rádio: ESP-NOW ativo, canal 1, 50 Hz | desde o boot: …`.

### Campos do pacote

| Campo | Fonte |
|---|---|
| `t_ms` | `millis()` na montagem |
| `roll`, `pitch` | filtro complementar |
| `heading` | filtro da proa (magnética, 0–360°; a declinação é aplicada só no B) |
| `gx`, `gy`, `gz` | giroscópio da última amostra fundida, corrigido (bias e montagem), eixos do modelo |
| `altitude_m`, `pressure_hpa`, `temp_c` | última leitura do barômetro (`temp_c` é a dele, de ambiente; a do MPU é a do chip) |
| `status` | `ST_MPU_OK`, `ST_BARO_OK`, `ST_MAG_OK` (sensor respondendo); `ST_CALIBRATING` (giroscópio, `6`, `z` ou `m` em curso); `ST_GYRO_CAL` (bias deste boot); `ST_MAG_CAL` (calibração do magnetômetro carregada) |
| `valid` | `telemetryValidFlags`, com as idades calculadas na montagem (`validityAt`); a mesma regra do `c`. O instante da última amostra do MPU é guardado em 64 bits (relógio do `esp_timer`): com o MPU parado, a idade da atitude não dá a volta em ~71,6 min (o `micros()` de 32 bits daria), e um ângulo velho não volta a valer. O barômetro e o magnetômetro usam o `millis()` de 32 bits, que só dá a volta em ~49,7 dias |
| `loop_us` | pulso do INT → fim da fusão da última amostra (latência do dado), saturado em 65 535 |

## ESP B (`src/tasks/radio_rx.cpp`, `src/tasks/link_task.cpp`)

Passo 5, desde 23/10/2026.

- **Wi-Fi em `WIFI_AP_STA`:**
  - o AP "Horizonte-ESP" já fica ligado no canal 1, sem servidor HTTP (a página é da semana 5). Assim, o risco "mesmo canal para o AP e o ESP-NOW" é validado já na semana 3;
  - a STA nunca se associa (sem `WiFi.begin()`). O A envia para o MAC base (`…:8A:B4`, o da STA), que só recebe com a STA ligada. O MAC do AP é o base + 1 (`…:8A:B5`);
  - economia de energia desligada;
  - o canal é lido do rádio depois da configuração. Se não for o 1, o rádio fica desligado, com a linha `[radio] ERRO`.
- **Peer:** o A (`PEER_MAC` do B), na interface STA, para os comandos da semana 5. O B ainda não envia nada.
- **Callback de recepção** (task do Wi-Fi, núcleo 0):
  1. carimba a chegada (`esp_timer_get_time()`);
  2. descarta outro remetente (contado);
  3. confere e copia com `memcpy` (`decodeTelemetry`, `lib/telemetry_rx`), nesta ordem: tamanho do cabeçalho, magic, versão, tipo e tamanho do pacote. Assim, um ACK ou outra versão é contado pelo motivo certo;
  4. enfileira para a `link_task` (`xQueueSend` sem espera; fila de `RX_QUEUE_LEN` = 16). Com a fila cheia, conta e descarta.
- **`link_task`** (núcleo 1, prioridade 3; `lib/link_stats`, testada no `native`):
  - com `d = seq − último seq` em 16 bits (a volta 65535 → 0 sai certa): `d == 0` é duplicado; até `LINK_RESYNC_GAP` (500 pacotes, 10 s), conta `d − 1` perdidos; acima disso, ressincroniza sem contar perdas. Uma falta mais longa que 10 s também ressincroniza, e as perdas dela não entram na taxa;
  - **pacote atrasado** (revisão de 23/10/2026): com o `seq` até `LINK_LATE_MAX_PKTS` (100) atrás **e** o `t_ms` até `LINK_LATE_MAX_MS` (2 s) atrás, o pacote chegou depois de um mais novo. Ele é contado como atrasado e descartado; se tinha sido contado como perdido, continua contado. O `t_ms` voltando além disso é reinício do A: ressincroniza. Sem um identificador de boot no contrato, essa janela é o que separa os dois casos. No caso ambíguo (A reiniciado com até 100 pacotes enviados), o B descarta os primeiros pacotes do boot novo, ~2 s no máximo, e volta a aceitar o fluxo sem perda falsa (teste `native`). O identificador de boot pode entrar quando o contrato mudar na semana 5;
  - sem pacote por `LINK_TIMEOUT_MS` (500 ms): link perdido (pelo `poll`, a cada volta da task). O pacote seguinte o restabelece. Se duas chegadas estiverem separadas por mais de 500 ms sem o `poll` ter visto (task atrasada, com os pacotes esperando na fila), a queda é registrada no próprio pacote, junto com o restabelecimento;
  - publica o último pacote **aceito** (caixa de correio, para o TFT e o WebSocket das semanas 4 e 5): duplicados e atrasados não renovam o pacote nem o instante de chegada. Publica também o resumo de cada janela de 1 s e os eventos do link (fila).
- **Intervalo entre chegadas** (o jitter visto no B): medido só entre dois pacotes com o link OK. Inclui as lacunas de perdas isoladas (40 ms com um pacote perdido). Nos mesmos pares, o `r` mostra o intervalo pelo `t_ms` do A (`no A mín/máx ms`, resolução de 1 ms): se ele varia junto com o do B, o atraso está no envio do A; se fica em ~20 ms, está no ar ou na recepção. A latência A → B não é medida aqui: os relógios não estão sincronizados (semana 6).
- **Console do B** (`serial_task`, núcleo 0, prioridade 1, única dona do `Serial` depois do boot):
  - eventos, sempre impressos: `[link] estabelecido`, `[link] PERDIDO (N ms sem pacotes)`, `[link] restabelecido depois de N ms`, `[link] ressincronizado: seq X -> Y, t_ms do A …`;
  - `r` (começa ligado): `[r] T ms: link OK | rec N em N ms | perdidos | dup | atrasados | ressinc | recusados | outro remetente | fila cheia | intervalo mín/médio/máx ms | idade | seq | total: rec, perd (%), quedas`. O `T ms:` (o `millis()` do B, como nos eventos) entrou em 26/10/2026, para o `analisar_b.py` cruzar os resumos com as entradas e saídas dos aparelhos no AP;
  - `a`: fluxo de pacotes a 10 Hz (`seq`, `t_ms`, roll, pitch, proa, altitude, pressão, temperatura, `status`, `valid`, `loop_us`, idade). Fica em silêncio sem pacote novo;
  - `c`: firmware, MACs, AP, canal, peer, limites do link, totais desde o boot e recusados por motivo.

## Bancada

**Passo 4: o A sozinho** (o B sem receber; carimbo `17:28:37` e, com a fusão na IRAM, `17:53:21`):

| Rodada | Log | Resultado |
|---|---|---|
| Boot | `261021-173149` | `[radio] ESP-NOW ativo: MAC 1C:69:20:A4:E9:44, canal 1, peer 14:33:5C:03:8A:B4, 50 Hz \| heap livre 168 kB, maior bloco 107 kB`, depois de trocar o cabo USB (antes, brownout) |
| Roteiro base | `261021-173300` | `r`: ~40 enviados/s, 0 entregues, ~40 falhas MAC/s e ~10 erros de envio/s; MPU com 199–200 Hz e tudo em 0; `a` com dt 5,00–5,01 ms e atraso de 101–112 µs; `c` com `valid = 0x000D` |
| `r` e `a` juntos, ~83 s | `261021-173359` | atraso de 101–115 µs nas 835 linhas, dt 5,00–5,01 ms, 199–200 Hz |
| `m` com o Wi-Fi ativo | `261021-173557` | coleta iniciada: os ~54 kB cabem no maior bloco livre (107 kB) |

- **~40 enviados por segundo, e não 50:** sem o B confirmando, cada pacote é retransmitido, e o ESP-NOW recusa ~10 envios por segundo (fila cheia). A task tenta 50 por segundo. Conferir no passo 5, com o B recebendo.
- **O custo do filtro subiu com o binário maior:** ver [tasks.md](tasks.md), passo 4.

**Passo 5: o B recebendo** (23/10/2026; o A no power bank, firmware sem mudança desde o passo 6; o B no USB):

| Rodada | Log (`firmware/ground/logs`) | Resultado |
|---|---|---|
| Boot do B e ~90 s parado | `261023-060428` (carimbo `06:03:28`) | MAC STA `…:8A:B4`, MAC AP `…:8A:B5`, canal 1, sem `[radio] ERRO`; o AP aparece no celular. `estabelecido` ~650 ms depois do boot. 49–52 rec/s; **1 perdido em 4 616 (0,022 %)**, 0 duplicados, ressincronizações, quedas e recusados. Idade do último pacote de 0 a 20 ms. No A, o OLED passou a ~50 entregues/s |
| Intervalo no A, fluxo `a` e EN do A | `261023-061939` (carimbo `06:17:02`) | Intervalo pelo `t_ms` do A em **20/20 ms em todas as janelas**, com o das chegadas no B em ~9–15 / ~24–31 ms. `a`: roll até +35,6° (direita) e pitch até +24,7° (nariz para cima), de volta a −1,1° / −0,2°; proa entre 98,7° e 102,2° durante as inclinações; `valid = 0x000D` em todas as 234 linhas; `seq` +5 por linha (+4 e +6 em 13 delas, pela grade de 10 ms da serial). EN do A: `PERDIDO (509 ms sem pacotes)`, `restabelecido depois de 1 720 ms` e `ressincronizado: seq 12081 -> 0, t_ms do A 12039258 -> 1158`, com 0 perdidos contados e 1 queda; depois, 0 perdidos em ~1 000 pacotes |
| Distância: o A levado do quarto, no fim do apartamento, até a varanda (várias paredes) e de volta | `261023-062517` (carimbo `06:17:02`) | Perto: 50 rec/s, 0 perdidos. Afastando: perdas de 1 a 51 por janela, 11 quedas, a maior de **9 428 ms** (sem ressincronização: 486 perdidos contados pelo `seq`, abaixo do limite de 500). De volta: 0 perdidos. Total: **5 122 rec, 1 049 perdidos (17,0 %)**, 0 duplicados, ressincronizações e recusados. O "no A" máximo de cada janela é sempre múltiplo de 20 ms, coerente com os perdidos contados |
| Duração, tudo parado, perto | `261023-070627` (carimbo `06:17:02`) | **510 s, 25 475 rec, 0 perdidos, 0 quedas**, 0 duplicados, ressincronizações, recusados e fila cheia. "No A" em 20/20 ms em todas as 497 janelas; intervalo no B de 1,1 a 50,3 ms. O monitor parou aos ~8,5 min, no meio de uma linha, sem reinício do B no log (abaixo) |
| Correções da revisão (atrasados, duplicados, queda sem `poll`) | `261023-081424` (carimbo `08:13:29`) | `c` com `atrasado até 100 pacotes e 2000 ms`; 19 janelas com 50–51 rec, 0 perdidos, duplicados, atrasados, ressincronizações e quedas. Com o link bom, o comportamento não mudou |

- **O risco "mesmo canal para o AP e o ESP-NOW" está validado:** o AP e o ESP-NOW funcionam juntos no canal 1.
- **Os ~40 enviados/s do passo 4 eram as retransmissões sem o B:** com o B confirmando, o A envia e entrega ~50 por segundo.
- **A variação das chegadas não vem da `radio_task` do A:** o `t_ms` é lido na montagem do pacote, e o intervalo dele fica em 20 ms (resolução de 1 ms). O atraso entra depois disso: na pilha do Wi-Fi do A (`esp_now_send` até a transmissão), no ar (retransmissões, espera do canal livre) ou na recepção do B, até o callback. Um pacote atrasado faz o seguinte chegar adiantado (mínimo e máximo simétricos em torno de 20 ms, média 20,0 ms); o maior caso foi 1,4 / 55,3 ms, sem perda. Para o TFT, é irrelevante. A latência A → B fica para a semana 6.
- **Com o link ruim, os pacotes chegam em rajadas e atrasados.** Nas janelas com perdas, o intervalo mínimo no B cai para ~1 ms, enquanto o `t_ms` do A continua em 20 ms. Os pacotes ficam retidos no A (fila de transmissão do ESP-NOW, retransmissões) e saem juntos depois. No pior caso, uma janela de 1 007 ms teve 81 recebidos + 16 perdidos (97 `seq`): ~47 pacotes, ~1 s de dados, chegaram atrasados. A `idade` do B mede o tempo desde a chegada, e não desde a geração, então não mostra esse atraso. **Pendência:** decidir se o B estima o atraso relativo (chegada − `t_ms`, comparado com o menor valor visto) e/ou se o A deixa de enfileirar envios com o anterior ainda pendente, descartando o dado velho em vez de atrasá-lo. Isso entra na semana 4 (o TFT não deve mostrar um dado atrasado como atual) ou na 6 (latência).
- **Texto embaralhado no monitor:**
  - **Na rodada 2:** uma vez, 64 caracteres inválidos no lugar do `[B` de uma linha, logo depois de duas teclas.
  - **Na rodada de duração:** 11 vezes em 8,5 min, sem tecla nenhuma, e depois o monitor parou no meio de uma linha. **Sempre exatamente 64 caracteres inválidos** (o tamanho de um pacote USB), no lugar de só 1 a 4 caracteres do começo da linha (`[`, `[r`, `[r] `). O `seq` e os totais continuam sem lacuna depois de cada ocorrência.
  - **Origem:** uma falha no firmware trocaria bytes, e não acrescentaria 64. A origem está no caminho USB-serial (cabo, porta, conversor CP2102 ou driver), e não no firmware.
  - **Antecedente:** o mesmo padrão aparece em logs do A de 11/10.
  - **Próxima vez:** trocar o cabo ou a porta USB do B. Se o monitor parar de novo, olhar o OLED do A antes de mexer: com ~50 entregues/s, o B continua recebendo.
