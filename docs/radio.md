# Rádio: ESP-NOW do ESP A para o ESP B

Semana 3 do [plano.md](plano.md). O contrato é o `firmware/shared/include/telemetry.h` (v4: desde 21/10/2026, o `loop_us` é a latência do dado). O A envia o `TelemetryPacket` a `TX_RATE_HZ` (50 Hz); o B recebe a partir do passo 5. Comandos B → A (`CommandPacket`) e ACK ficam para a semana 5.

## ESP A (`src/tasks/radio_task.cpp`)

- **Task:** `radio_task`, no núcleo 0 (junto com o Wi-Fi do IDF), com prioridade 2. Liga o Wi-Fi `RADIO_START_DELAY_MS` (1 s) depois do boot, com a linha `[radio] ligando o Wi-Fi...` impressa antes, por causa do brownout de 21/10 ([hardware.md](hardware.md), "Brownout ao ligar o Wi-Fi").
- **Configuração:**
  - `WiFi.mode(WIFI_STA)`, sem se associar a nenhuma rede: o canal fica fixo, sem varredura;
  - `esp_wifi_set_ps(WIFI_PS_NONE)`, sem economia de energia;
  - `esp_wifi_set_channel(ESPNOW_CHANNEL, …)`, o canal 1, o mesmo do AP do B;
  - `esp_now_init` e o peer `PEER_MAC` (o MAC base do B, `14:33:5C:03:8A:B4`), no canal 1, interface STA, sem criptografia.
- **Envio:** a 50 Hz pelo próprio relógio (`vTaskDelayUntil`). A task lê a última cópia do estado publicada pela `sensor_task` (caixa de correio: fila de 1, com `xQueueOverwrite` e `xQueuePeek`), monta o pacote (`buildTelemetry`, `lib/telemetry_pack`) e chama `esp_now_send`. O `seq` é de 16 bits e dá a volta em 65535.
- **Callbacks** (task do Wi-Fi, núcleo 0): só contam, em contadores atômicos.
  - **Envio:** entregue (o B confirmou com o ACK da camada MAC) ou falha MAC (sem confirmação depois das retransmissões).
  - **Recepção:** só aceita o `PEER_MAC`. O `CommandPacket` é copiado com `memcpy` e conferido (tamanho, magic, versão, tipo; `decodeCommand`); os aceitos e os recusados são contados. A execução é da semana 5.
- **Console:**
  - `r`: `radio: N enviados/s | entregues N/s | falhas MAC N/s | erros de envio N | comandos aceitos N, recusados N`;
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
