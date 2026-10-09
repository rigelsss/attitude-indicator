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

## Opcionais (só se sobrar tempo)
- Indicador de curva (gz) e variômetro (derivada da altitude) no PFD
- Gravação de "caixa-preta" na flash do A
- Replay de um log CSV na página web

## Riscos conhecidos
- Mesmo canal para o AP e o ESP-NOW (fixo em 1) — validar já na semana 3
- O GY-273 pode ser QMC5883L (0x0D) ou HMC5883L (0x1E), e cada um usa uma biblioteca diferente
- O magnetômetro sofre interferência de motores, ímãs e da própria protoboard: calibrar já montado no modelo
- Buzzer ativo direto no GPIO: medir a corrente; se passar de 20 mA, usar transistor
