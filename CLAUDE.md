# attitude-indicator (Horizonte Artificial) — contexto para o Claude Code

Projeto final de Sistemas Embarcados. Dois ESP32 (NodeMCU-32S, Arduino via PlatformIO, Windows/VS Code).

## Arquitetura
- **ESP A — avião** (`firmware/aircraft`): MPU6050 + GY-273 + BME280 (I2C) → fusão → `TelemetryPacket` via ESP-NOW a 50 Hz. OLED 0,91" de status.
- **ESP B — solo** (`firmware/ground`): recebe ESP-NOW; horizonte no TFT ST7789 (SPI); dados no OLED 0,96"; encoder KY-040; buzzer/LEDs de alarme; AP Wi-Fi "Horizonte-ESP" com página (LittleFS) e WebSocket JSON :81.
- **PC** (`ground-station/`): `web/` (página servida pelo B) e `python/` (cliente WebSocket: log CSV, gráficos, comandos).
- **Contrato**: `firmware/shared/include/telemetry.h` (binário, ESP-NOW) e `docs/protocolo-websocket.md` (JSON). Mudou um, atualize os dois firmwares e incremente `TELEMETRY_VERSION`.

## Regras de código
- `main.cpp` só inicializa e cria tasks; lógica em `lib/` (drivers, serviços) e `src/tasks/`.
- Drivers não conhecem rede nem display. A lógica pura (fusão, calibração) não usa APIs Arduino e é testável com `pio test -e native`.
- Comunicação entre tasks via fila/mutex do FreeRTOS, sem globais compartilhadas sem proteção.
- ISRs e callbacks ESP-NOW curtos: só enfileiram (`xQueueSendFromISR`), com `IRAM_ATTR` nas ISRs.
- Pinos e constantes ficam só em `include/config.h`.
- AP e ESP-NOW no **mesmo canal** (1).
- Comentários em português.

## Comandos
- Compilar/gravar: `pio run -e aircraft -t upload` (em `firmware/aircraft`) / `pio run -e ground -t upload`
- Página web no B: `pio run -e ground -t uploadfs`
- Testes de lógica: `pio test -e native`

## Placas
- Placa A (avião): MAC 1C:69:20:A4:E9:44
- Placa B (solo): MAC 14:33:5C:03:8A:B4

Plano de etapas: `docs/plano.md`. Trabalhe só na etapa pedida.
