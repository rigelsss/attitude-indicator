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
- `docs/fusao.md`: filtro complementar (roll/pitch), calibração de montagem (`z`), limites e roteiro de validação.
- `docs/barometro.md`: BME280 (perfil "indoor navigation"), altitude relativa, zero (`0`) e roteiro de validação.
- `docs/magnetometro.md`: HMC5883L, mapeamento de eixos, calibração (`m`), proa compensada e filtro da proa, roteiro de validação.
- `docs/refatoracao.md`: refatoração do `main.cpp` provisório (concluída em 20/10/2026): módulos, linha de base e critérios de tempo da bancada, resultados por passo e pendências para a migração às tasks.
- `docs/radio.md`: ESP-NOW (configuração do A e do B, campos do pacote, contadores do console) e a bancada do rádio.
- `docs/web.md`: semana 5: comandos do B para o A (camada única de comandos, ACK, reenvio), servidor web, WebSocket e página.
- `docs/solo.md`: semana 4 do ESP B: decisões (TFT só com o horizonte, páginas do OLED pelo encoder, LEDs, sprite de 8 bits, buzzer adiado), passos, ligação e diagnóstico (`ground_diag`).
- `docs/caixa-b.md`: montagem do B na caixa (power bank, layout e conectores da tampa, segundo OLED no `Wire1`, buzzer com BC337, anel de LEDs WS2812B, última prioridade); discutido em 29/10/2026, sem código.
- `docs/cliente-python.md`: semana 6: cliente Python (leitura sem bloqueio, CSV, reconexão, comandos), métricas, campos `tb`/`n`/`loop_us` e `origin` do JSON, diagnóstico do Wi-Fi do PC.
- `docs/tasks.md`: tasks do ESP A (núcleos, prioridades, filas, logger com um consumidor só) e as medições de tempo de cada passo da semana 3.
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
- Diagnóstico do hardware do B (LEDs, I2C, OLED, encoder, TFT, sprite): `pio run -e ground_diag -t upload`; depois, regravar o `ground`
- B com a telemetria do WebSocket a 20 Hz (bancada 2): `pio run -e ground_ws20 -t upload`; depois, regravar o `ground`
- Teste de pulsos falsos no INT (ESP A, tecla `F`): `pio run -e aircraft_pulsetest -t upload`; depois, regravar o `aircraft`
- Teste do reinício da fusão com a caixa em movimento (ESP A, tecla `R`): `pio run -e aircraft_resettest -t upload`; depois, regravar o `aircraft`
- A/B de tempo sem o Wi-Fi ligado (ESP A): `pio run -e aircraft_noradio -t upload`; depois, regravar o `aircraft`
- Monitor: `pio device monitor -b 115200` (no `ground`, o filtro `log2file` grava em `firmware/ground/logs`)
- Página web no B: `pio run -e ground -t uploadfs`
- Testes de lógica: `pio test -e native`
- Testes do cliente Python (a partir de `ground-station/python`): `python -m pytest`; dependências: `python -m pip install -r requirements.txt`
- Cliente Python (a partir de `ground-station/python`): `python cliente.py` (o B, com o PC na Horizonte-ESP) ou `python cliente.py --url ws://127.0.0.1:8765/` com o simulador `python sim_b.py` (`--sem-a`; as falhas em `-h`); `?` no cliente lista os comandos
- Gráficos ao vivo, num terminal ao lado do cliente (segue o CSV mais recente de `logs/`): `python graficos.py`; uma gravação inteira: `python graficos.py --arquivo logs\<nome>-tel.csv`
- Figuras e resumo de uma gravação, para o relatório: `python analisar.py logs\<nome>-tel.csv` (cria `logs/analise-<nome>/`)
- Jitter do loop do A (fluxo `a` do monitor do A; vários logs = comparação): `python analisar_a.py ..\..\firmware\aircraft\logs\device-monitor-<…>.log [outros]`
- Figuras e resumo do log do monitor do B (perdas, memória, aparelhos no AP; dois logs = comparação): `python analisar_b.py ..\..\firmware\ground\logs\device-monitor-<…>.log [outro log]`
- Conferência da calibração do magnetômetro (log com a tecla `d`): `python tools/mag_fit.py` (requer numpy)
- Comparar o texto das mensagens de um log novo com o de logs de base (números trocados por `#`; não confere a ordem): `python tools/log_templates.py logs/base.log [mais bases] [--novo logs/novo.log]`

## Placas
- Placa A (avião): MAC 1C:69:20:A4:E9:44, ESP32-D0WD-V3 rev 3.1, COM7
- Placa B (solo): MAC 14:33:5C:03:8A:B4, COM7
- As duas usam a COM7 (uma de cada vez), então a porta não identifica a placa. O `firmware/shared/scripts/check_board_mac.py` confere o MAC antes de `upload`/`uploadfs` e cancela se não bater com o `custom_board_mac` do ambiente.
- A linha de boot dos dois firmwares mostra `compilado em AAAA-MM-DD HH:MM:SS` (`firmware/shared/scripts/build_stamp.py` e `src/build_info.cpp`). Antes de analisar um log de bancada, conferir se o carimbo é da última compilação: um `upload` que falhou (por exemplo, com o monitor aberto na COM7) deixa o firmware antigo na placa.
