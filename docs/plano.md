# Plano de desenvolvimento — Horizonte Artificial (entrega: fim de novembro/2026)

Cada etapa termina com algo **demonstrável**. Tópico da disciplina entre colchetes.

| Sem. | Período | Entrega | Tópicos |
|---|---|---|---|
| 1 | 08–14/10 | Repo criado; I2C scan no A (confirmar endereços e chip do GY-273); leitura bruta do MPU na serial; solda dos headers | I2C |
| 2 | 15–21/10 | Calibração do giroscópio; filtro complementar (roll/pitch) com teste `native`; magnetômetro com compensação de inclinação (proa); BME280 (altitude relativa) | I2C, arquitetura |
| 3 | 22–28/10 | Tasks FreeRTOS no A acionadas pela INT do MPU; ESP-NOW A→B com `seq`; B imprime pacotes e perdas na serial; OLED de status no A | Interrupções, timers, RTOS |
| 4 | 29/10–04/11 | B: horizonte no TFT (sprite sem flicker) + OLED secundário; encoder via interrupção (troca de telas); alarmes com buzzer/LEDs; timeout de link | SPI, GPIO, interrupções |
| 5 | 05–11/11 | B: AP Wi-Fi + página web (LittleFS) + WebSocket JSON; comandos web → B → A com ACK | IoT |
| 6 | 12–18/11 | Cliente Python (log CSV, gráficos, comandos); métricas: jitter do loop, perda de pacotes, latência; montagem no modelo | IoT, RTOS |
| 7 | 19–30/11 | **Folga** para atrasos; testes de duração; relatório, slides, diagrama e ensaio da apresentação | — |

## Pendências para a semana 3 (rádio)
- **Flags de validade no `TelemetryPacket`** (revisão de 13/10/2026). Hoje o `status` só informa a saúde dos sensores (`ST_MPU_OK`, `ST_BARO_OK`…) e o estado da calibração. Faltam estados explícitos para o B saber quando um dado não pode ser exibido:
  - **atitude inicializada:** o filtro ainda espera a média inicial, ou reiniciou por uma lacuna;
  - **atitude degradada:** giroscópio saturado, recuperando a correção;
  - **altitude com zero válido:** sem p₀, a altitude não tem referência;
  - **dados antigos:** a última amostra válida tem mais de N ms, como na perda de pulsos do INT ou numa falha do BME280;
  - **proa inicializada:** o filtro da proa já partiu de uma proa válida do magnetômetro. Ele recomeça quando a atitude reinicia, numa lacuna de amostras e depois de uma calibração (docs/magnetometro.md, D6);
  - **correção da proa suspensa:** a proa segue só pelo giroscópio, por distúrbio magnético, inclinação alta ou atitude degradada. O `ST_MAG_CAL` diz só que existe uma calibração, não que a proa vale agora.

  O B usa essas flags para mostrar o dado como indisponível (horizonte riscado, altitude "---"), em vez de congelar o último valor. Isso muda o contrato: atualizar os dois firmwares, incrementar `TELEMETRY_VERSION` e espelhar no `docs/protocolo-websocket.md`.
- **Alinhamento do contrato** (junto com as flags, para a versão subir uma vez só). O `MsgHeader` tem 5 bytes, então `t_ms` e todos os floats ficam em offsets ímpares. Acessar o campo direto pelo struct funciona, mas um ponteiro `float *p = &pkt.roll` causa `LoadStoreAlignment` no Xtensa. Correção: cabeçalho de 8 bytes, com bytes de reserva. No B, o callback do ESP-NOW copia o `data` com `memcpy` para uma struct local antes de enfileirar, e nunca faz cast do ponteiro: o buffer recebido não tem alinhamento garantido.

## Decisões para a semana 3 (revisão de 13/10/2026)
- **B em `WIFI_AP_STA`.** No ESP32, a interface STA usa o MAC base e o softAP usa o MAC base + 1. O `PEER_MAC` do A é o MAC base do B (`…:8A:B4`). Em `WIFI_AP` puro, o B só receberia em `…:8A:B5`, e os pacotes não chegariam. Com `WIFI_AP_STA`, o MAC base continua valendo. O B nunca chama `WiFi.begin()`, para o STA não varrer canais e tirar o rádio do canal 1.
- **Canal fixo no A.** O A fica em `WIFI_STA` sem se associar a nenhum AP, então o canal é fixado com `esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE)` antes de registrar o peer.
- **OLED de status no segundo controlador I2C (`Wire1`), SDA no GPIO 32 e SCL no GPIO 33.** Um quadro do OLED 128×32 tem 512 bytes, o que leva ~12–14 ms a 400 kHz. Isso é mais que o período de 5 ms do MPU: no mesmo barramento, cada atualização faria perder 2 ou 3 amostras. Os GPIO 32 e 33 são de uso geral e não interferem no boot. Ficaram de fora os pinos de strapping (0, 2, 5, 12, 15), os da flash (6–11), os só de entrada (34–39), os já usados (19, 21, 22) e os reservados para um eventual SPI (18, 23). Ao ligar o OLED: conferir se o módulo tem pull-ups e estender o `i2cscan` ao segundo barramento.
- **Uma task dona do barramento de sensores.** O MPU6050, o BME280 e o HMC5883L são lidos pela mesma task: o MPU a cada pulso do INT, e o BME e o HMC intercalados em taxa menor, logo depois de uma leitura do MPU (como o `main.cpp` provisório já faz com o BME280). Assim não há mutex de barramento, e o `recoverBus()`, que dá `Wire.end()` no barramento inteiro, continua seguro, porque nenhuma outra task usa o `Wire`.
- **Refatorar antes das tasks.** Primeiro extrair a lógica do `main.cpp` provisório (máquina de estados da calibração, zero do barômetro) para serviços em `lib/`, sem rede nem display e sem mudar o comportamento, e validar na bancada. Só depois migrar para tasks, para não refatorar e migrar ao mesmo tempo.
- **Gravações na NVS.** Acontecem só em eventos de calibração (`g`, `6`, `z` e o bias do boot), não periodicamente. Durante uma escrita na flash, o cache fica desligado nos dois núcleos, então mover a gravação para uma task de baixa prioridade não evita a pausa da amostragem. A lacuna é aceita: se passar de `FUSION_MAX_DT_MS`, a fusão reinicia como em qualquer lacuna.
- **`CMD_ZERO_ATTITUDE` continua no contrato.** Ele é o zero operacional, de um passo, e não substitui a calibração de montagem (`z`), feita em duas posições ([fusao.md](fusao.md)). Implementação na etapa dos comandos (semana 5).

## Opcionais (só se sobrar tempo)
- Indicador de curva (gz) e variômetro (derivada da altitude) no PFD
- Gravação de "caixa-preta" na flash do A
- Replay de um log CSV na página web
- ~~CI no GitHub Actions rodando `pio test -e native` (semana 7, folga)~~ adiantado em 15/10/2026, depois que um commit parcial deixou o `main` sem compilar: `.github/workflows/ci.yml` roda os testes `native` do aircraft e compila `aircraft`, `i2cscan` e `ground` a cada push e pull request

## Riscos conhecidos
- Mesmo canal para o AP e o ESP-NOW (fixo em 1) — validar já na semana 3
- O GY-273 é HMC5883L (0x1E), mas alguns módulos vendidos assim trazem um clone QMC5883L (0x0D), que usa outra biblioteca: confirmar no diagnóstico `i2cscan`
- O magnetômetro sofre interferência de ímãs, de correntes nos fios de alimentação e da própria protoboard: calibrar já montado no modelo
- Buzzer ativo direto no GPIO: medir a corrente; se passar de 20 mA, usar transistor
