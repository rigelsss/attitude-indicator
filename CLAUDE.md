# attitude-indicator (Horizonte Artificial) — contexto para o Claude Code

Projeto final de Sistemas Embarcados (entrega: fim de novembro/2026). Dois ESP32 NodeMCU-32S, Arduino via PlatformIO, Windows (PowerShell) + VS Code. Abrir pelo `attitude-indicator.code-workspace`.

## Arquitetura
- **ESP A — avião** (`firmware/aircraft`): MPU6050 + GY-273 (HMC5883L, 0x1E) + BME280 (I2C) → calibração → fusão → `TelemetryPacket` via ESP-NOW a 50 Hz. OLED 0,91" de status.
- **ESP B — solo** (`firmware/ground`): recebe ESP-NOW; horizonte no TFT ST7789 (SPI); dados no OLED 0,96"; encoder KY-040; buzzer/LEDs de alarme; AP Wi-Fi "Horizonte-ESP" com página (LittleFS) e WebSocket JSON :81.
- **PC** (`ground-station/`): `web/` (página servida pelo B) e `python/` (cliente WebSocket: log CSV, gráficos, comandos).
- **Contrato**: `firmware/shared/include/telemetry.h` (binário, ESP-NOW) e `docs/protocolo-websocket.md` (JSON). Se um mudar, atualize os dois firmwares e incremente `TELEMETRY_VERSION`.

## Documentos de referência (fonte da verdade)
- `docs/plano.md`: etapas. Trabalhe **só na etapa pedida**; não adiante fusão, rádio ou RTOS por conta própria.
- `docs/hardware.md`: evidência de bancada (endereços, medidas, testes). Se um teste contradisser o `config.h`, corrija o `config.h` e registre o teste aqui.
- `docs/calibracao.md`: estratégia de calibração do MPU6050. Siga-a; se precisar divergir, pergunte antes.
- Antes de concluir uma etapa, atualize o doc afetado (procedimento de uso, resultados, decisões).

## Regras de código
- Firmware final: `main.cpp` só inicializa e cria tasks; lógica em `lib/` (drivers, serviços) e `src/tasks/`. Em etapas de bancada anteriores ao RTOS, um `main.cpp` provisório com `loop()` é aceito; marque-o como provisório no topo.
- Drivers não conhecem rede nem display.
- A lógica pura (calibração, fusão, estatística) não inclui `Arduino.h` e é testável com `pio test -e native`.
- Comunicação entre tasks via fila/mutex do FreeRTOS, sem globais compartilhadas sem proteção.
- ISRs e callbacks ESP-NOW curtos: só enfileiram (`xQueueSendFromISR`), com `IRAM_ATTR` nas ISRs.
- Pinos, faixas, taxas e limiares ficam só em `include/config.h`.
- **Grandezas em unidades físicas** (g, °/s, °C, hPa) fora do driver; limiares nunca em LSB, porque o LSB muda com a faixa configurada (±4 g, ±500 °/s no firmware de voo).
- Correções de calibração aplicadas no software, não nos registradores de offset do MPU.
- Dados persistentes na NVS via `Preferences`, com namespace por módulo (ex.: `imu_cal`).
- AP e ESP-NOW no **mesmo canal** (1).
- Diagnóstico: o ambiente `i2cscan` (`src/diag/`) é separado e acessa registradores sem bibliotecas; não o misture com o firmware de voo.
- Comentários e mensagens de serial em português.

## Fluxo de trabalho
- Ao terminar: compilar (`pio run -e <env>`) e rodar `pio test -e native` quando houver lógica pura. Não declare pronto sem compilar.
- Gravar a placa e abrir o monitor serial são feitos pelo usuário (a placa e o monitor interativo estão com ele): informe os comandos exatos.
- Commits em português, no padrão `tipo: descrição` (`feat`, `fix`, `docs`, `chore`, `test`, `refactor`). Só commite quando o usuário pedir.

## Comandos (PowerShell, a partir de `firmware/aircraft` ou `firmware/ground`)
- Compilar/gravar: `pio run -e aircraft -t upload` / `pio run -e ground -t upload`
- Diagnóstico I2C (ESP A): `pio run -e i2cscan -t upload`
- Monitor: `pio device monitor -b 115200`
- Página web no B: `pio run -e ground -t uploadfs`
- Testes de lógica: `pio test -e native`

## Placas
- Placa A (avião): MAC 1C:69:20:A4:E9:44, ESP32-D0WD-V3 rev 3.1, COM7
- Placa B (solo): MAC 14:33:5C:03:8A:B4
