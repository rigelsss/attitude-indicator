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

> **Contrato v2 feito em 15/10/2026** (adiantado, sem o rádio): `TELEMETRY_VERSION` 2, cabeçalho de 8 bytes com todos os campos alinhados e conferidos por `static_assert`, campo `valid` com as flags `VAL_ATT`, `VAL_ATT_DEGRADED`, `VAL_ALT`, `VAL_HDG` e `VAL_HDG_GYRO_ONLY` (regras em `lib/telemetry_flags`, testadas no `native`), linha "validade" no `c` e protocolo WebSocket atualizado. O "dado antigo" entrou como idade máxima dentro de cada flag (`FUSION_MAX_DT_MS`, `BARO_STALE_MS`, `MAG_STALE_MS`). **Fica para a semana 3:** montar e enviar o pacote, e no B copiar o buffer com `memcpy` e descartar outras versões. Os dois itens abaixo ficam como registro da decisão.

> **Contrato v3, também em 15/10/2026:** `AckPacket` com `result` (`ACK_REJECTED`, `ACK_ACCEPTED`, `ACK_DONE`, só reservados; a lógica entra na semana 5) e `cmd_seq` (o `seq` do comando confirmado), sem mudar os 12 bytes; `VAL_HDG` passou a exigir o magnetômetro calibrado.
- **Flags de validade no `TelemetryPacket`** (revisão de 13/10/2026). Hoje o `status` só informa a saúde dos sensores (`ST_MPU_OK`, `ST_BARO_OK`…) e o estado da calibração. Faltam estados explícitos para o B saber quando um dado não pode ser exibido:
  - **atitude inicializada:** o filtro ainda espera a média inicial, ou reiniciou por uma lacuna;
  - **atitude degradada:** giroscópio saturado, recuperando a correção;
  - **altitude com zero válido:** sem p₀, a altitude não tem referência;
  - **dados antigos:** a última amostra válida tem mais de N ms, como na perda de pulsos do INT ou numa falha do BME280;
  - **proa inicializada:** o filtro da proa já partiu de uma proa válida do magnetômetro. Ele recomeça quando a atitude reinicia, numa lacuna de amostras e depois de uma calibração (docs/magnetometro.md, D6);
  - **correção da proa suspensa:** a proa segue só pelo giroscópio, por distúrbio magnético, inclinação alta ou atitude degradada. O `ST_MAG_CAL` diz só que existe uma calibração, não que a proa vale agora.

  O B usa essas flags para mostrar o dado como indisponível (horizonte riscado, altitude "---"), em vez de congelar o último valor. Isso muda o contrato: atualizar os dois firmwares, incrementar `TELEMETRY_VERSION` e espelhar no `docs/protocolo-websocket.md`.
- **Alinhamento do contrato** (junto com as flags, para a versão subir uma vez só). O `MsgHeader` tem 5 bytes, então `t_ms` e todos os floats ficam em offsets ímpares. Acessar o campo direto pelo struct funciona, mas um ponteiro `float *p = &pkt.roll` causa `LoadStoreAlignment` no Xtensa. Correção: cabeçalho de 8 bytes, com bytes de reserva. No B, o callback do ESP-NOW copia o `data` com `memcpy` para uma struct local antes de enfileirar, e nunca faz cast do ponteiro: o buffer recebido não tem alinhamento garantido.

## Cronograma da revisão de 15/10/2026

Itens de uma revisão externa (numeração dela), distribuídos pela etapa em que o assunto aparece:

- **Semana 2 (agora):**
  - 2 (ACK com `cmd_seq`) e 4 (`VAL_HDG` sem calibração): **feitos** no contrato v3;
  - 6 (`m` liberado durante a calibração do giroscópio do boot; `z` continua bloqueado), 9 (buffer da serial de 1 para 4 kB: o `c` tem ~1,4 kB), 10 (`MAG_DECLINATION_DEG` no `config.h` do B e `"decl"` no JSON, aplicada só na exibição) e 12 (comentário do TCOP e pinos `PIN_I2C1_SDA`/`SCL` 32 e 33 no `config.h` do A): **feitos** em 15/10;
  - 5 (reinício da fusão): tema da semana 2, mas não bloqueia nada. Pode ir até a semana 6, antes da montagem no modelo;
  - 17 (sincronizar o plano): esta seção.
- **19/10/2026, BMP280:**
  - o BMP280 substituiu o BME280 e passou no roteiro completo do [barometro.md](barometro.md) (resultados no [hardware.md](hardware.md), "BMP280 no firmware de voo");
  - o mau contato no VCC causou brownouts e um laço de reconfiguração, que revelou uma falha do driver. A falha foi corrigida no `lib/bme280`: a recuperação agora conta como falha;
  - correção conferida na bancada no mesmo dia (boot normal e SDA solto, sem mudança de comportamento).
  - **Depois disso:** decidir sobre a refatoração do `main.cpp` provisório (seção "Decisões para a semana 3"), que é pré-requisito das tasks da semana 3.
- **20/10/2026, refatoração retomada:** plano aprovado e andamento em [refatoracao.md](refatoracao.md) (passos 0–8, um módulo por vez). As tasks da semana 3 começam depois dela.
- **20/10/2026, refatoração concluída:** os passos 0–8 foram validados na bancada, e o `main.cpp` provisório tem 341 linhas: inicialização, ISR e laço de aquisição. A lógica está em `lib/`, e o console em `src/console.cpp`. Para a migração, as pendências 2, 5 e 7 do [refatoracao.md](refatoracao.md) tratam da serial fora do laço, da ISR do INT em IRAM e do console acessando os serviços entre tasks.
- **21/10/2026, pendências antes da semana 3** ([refatoracao.md](refatoracao.md), "Situação em 21/10/2026"):
  - feitas e conferidas na bancada: a ISR do INT pelo ESP-IDF em IRAM, no nível 2 (pendência 5), o logger com buffer próprio e o log do núcleo redirecionado (parte da pendência 2), o aviso de tecla ignorada no `m` (1), a cobertura do `m` com distância mínima (3) e a histerese do distúrbio magnético;
  - ficam para a semana 3, junto com a `sensor_task`: a formatação do `c`, do `r` e do `a` fora do laço (resto da 2), a fila de comandos e o estado publicado (7) e os pulsos falsos no INT (8);
  - ficam para o modelo final: o `s` do magnetômetro (4) e o das seis posições (6);
  - **item 5 da revisão de 15/10 (reinício da fusão): a definir.** O texto do item não está no repositório (só o título, no commit `9c12d4c`). Se não for recuperado até a semana 6, rever o reinício da fusão a partir dos registros do [fusao.md](fusao.md) antes da montagem no modelo.
- **Semana 3 (tasks e rádio):** 7 (perdas pelo `seq`), 8 (camada única de comandos), 11 (callback de envio do ESP-NOW) e a primeira parte do 3 (filtrar o remetente pelo `PEER_MAC` no callback de recepção do A).
  - **23/10/2026, passo 5** ([radio.md](radio.md)): 7, 11 e a primeira parte do 3 **feitos**. O 7 no B, pelo `lib/link_stats`, e o 11 e o 3 no A, no passo 4. O B também filtra o remetente. O 8 fica para a semana 5, com os comandos.
  - **Risco do canal validado:** o AP do B e o ESP-NOW funcionam juntos no canal 1.
- **Semana 4 (TFT):** plano em [solo.md](solo.md). 1 (sprite e heap) e 13 (GPIO 5 como CS do TFT). **23/10/2026, passo 0:** o 13 está validado, e o sprite de 8 bits foi medido (57,6 kB, 28,3 ms por quadro). **Passo 5:** o 1 está fechado: sprite de 8 bits alocado antes do Wi-Fi, 25 quadros/s com o maior quadro em 30–32 ms e a recepção sem mudança. **Buzzer adiado** (23/10/2026): o buzzer é ativo, e não há transistor para ligá-lo se a corrente passar do que o GPIO aguenta (lista de riscos). Os alarmes da semana 4 usam só os LEDs. A `alarm_task` deixa o buzzer previsto (GPIO 26, ritmos já definidos), para entrar quando houver transistor ou quando a corrente medida permitir ligá-lo direto. Também o **atraso dos pacotes com o link ruim**, decidido em 23/10/2026 na bancada do passo 5 ([radio.md](radio.md)). Os pacotes ficam retidos no A e chegam em rajadas, com até ~1 s de atraso, e a `idade` do B não mostra isso. Duas medidas: o B estima o atraso relativo (chegada − `t_ms`, comparado com o menor valor visto) e trata o dado atrasado como indisponível no TFT, e o A não enfileira um envio com o anterior pendente (descarta o dado velho em vez de atrasá-lo).
- **Semana 5 (web e comandos):** a segunda parte do 3 (validar o `arg` do `CMD_SET_RATE_HZ`), 15 (senha do AP) e a lógica dos estados do ACK (aceito, concluído, recusado). Se o contrato mudar nessa etapa, avaliar um **identificador de boot do A** no cabeçalho (por exemplo, no `reserved1`). Ele substituiria a janela de "pacote atrasado" do B ([radio.md](radio.md), `link_task`), que hoje separa um pacote velho de um A reiniciado.
- **Folga (semana 7):** 14 (`upload_port` fixo; o risco de gravar a placa errada já é coberto pelo `check_board_mac.py`) e 16 (CI, **já feito** em 15/10; na folga, só ampliações).

## Decisões para a semana 3 (revisão de 13/10/2026)
- **B em `WIFI_AP_STA`.** No ESP32, a interface STA usa o MAC base e o softAP usa o MAC base + 1. O `PEER_MAC` do A é o MAC base do B (`…:8A:B4`). Em `WIFI_AP` puro, o B só receberia em `…:8A:B5`, e os pacotes não chegariam. Com `WIFI_AP_STA`, o MAC base continua valendo. O B nunca chama `WiFi.begin()`, para o STA não varrer canais e tirar o rádio do canal 1.
- **Canal fixo no A.** O A fica em `WIFI_STA` sem se associar a nenhum AP, então o canal é fixado com `esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE)` antes de registrar o peer.
- **OLED de status no segundo controlador I2C (`Wire1`), SDA no GPIO 32 e SCL no GPIO 33.** Um quadro do OLED 128×32 tem 512 bytes, o que leva ~12–14 ms a 400 kHz. Isso é mais que o período de 5 ms do MPU: no mesmo barramento, cada atualização faria perder 2 ou 3 amostras. Os GPIO 32 e 33 são de uso geral e não interferem no boot. Ficaram de fora os pinos de strapping (0, 2, 5, 12, 15), os da flash (6–11), os só de entrada (34–39), os já usados (19, 21, 22) e os reservados para um eventual SPI (18, 23). Ao ligar o OLED: conferir se o módulo tem pull-ups e estender o `i2cscan` ao segundo barramento.
- **Uma task dona do barramento de sensores.** O MPU6050, o BME280 e o HMC5883L são lidos pela mesma task: o MPU a cada pulso do INT, e o BME e o HMC intercalados em taxa menor, logo depois de uma leitura do MPU (como o `main.cpp` provisório já faz com o BME280). Assim não há mutex de barramento, e o `recoverBus()`, que dá `Wire.end()` no barramento inteiro, continua seguro, porque nenhuma outra task usa o `Wire`. **Ao migrar, medir de novo o impacto do barômetro e do magnetômetro no MPU** (atraso, ambíguos, descartados e fila cheia no `r` e no `a`), com os sensores bons e com um deles falhando. No `main.cpp` provisório: ~0,3 ms por leitura, logo depois do MPU, e no máximo ~1 descarte por segundo com um sensor falhando (revisão de 15/10/2026). **Serial:** os logs de erro do núcleo (`[E][Wire.cpp]`, com `CORE_DEBUG_LEVEL=3`) não passam pelo buffer do `Serial` e podem parar o laço por dezenas de ms quando há uma impressão longa em curso (hardware.md, 15/10/2026). Na migração, a serial vai para uma task de baixa prioridade, e convém rever o nível de log do núcleo.
- **Refatorar antes das tasks.** Primeiro extrair a lógica do `main.cpp` provisório (máquina de estados da calibração, zero do barômetro) para serviços em `lib/`, sem rede nem display e sem mudar o comportamento, e validar na bancada. Só depois migrar para tasks, para não refatorar e migrar ao mesmo tempo.
  - **Adiada em 15/10/2026, por decisão do autor,** para outro momento. Ela continua sendo **pré-requisito das tasks FreeRTOS**: antes de migrar para tasks, ou se faz a refatoração, ou se revê esta decisão. Situação em 15/10: `main.cpp` provisório com ~1 450 linhas, que misturam aquisição pelo INT, janelas de estatística (`r` e `c`), máquinas de estado das calibrações (`g`, `6`, `z`, `m`), zero do barômetro, filtro da proa, fluxos (`a`, `b`, `p`) e console serial. Na retomada, o primeiro passo é um plano, sem código: módulos e responsabilidades, o que vira lógica pura testável no `native`, o que sobra no `main.cpp`, ordem de extração (um módulo por vez, com compilação, testes e conferência curta na bancada) e o que não muda (comandos, mensagens e constantes).
  - **Retomada em 20/10/2026:** plano aprovado em [refatoracao.md](refatoracao.md).
- **Gravações na NVS.** Acontecem só em eventos de calibração (`g`, `6`, `z` e o bias do boot), não periodicamente. Durante uma escrita na flash, o cache fica desligado nos dois núcleos, então mover a gravação para uma task de baixa prioridade não evita a pausa da amostragem. A lacuna é aceita: se passar de `FUSION_MAX_DT_MS`, a fusão reinicia como em qualquer lacuna.
- **`CMD_ZERO_ATTITUDE` continua no contrato.** Ele é o zero operacional, de um passo, e não substitui a calibração de montagem (`z`), feita em duas posições ([fusao.md](fusao.md)). Implementação na etapa dos comandos (semana 5).

## Antes de fechar a caixa do ESP A

Conferências a fazer com a fiação já presa, antes de fechar a caixa (depois dela, mexer no módulo exige abrir tudo de novo):

- **Montagem do MPU depois de prender a fiação.** Em 23/10/2026, depois de refazer e prender os fios, a atitude parada ficou em roll +0,65° e pitch −0,59°. Na véspera, tinha sido −1,42° e +0,07° ([hardware.md](hardware.md), "SCL aberto depois de prender a fiação"). Ainda não se sabe se a caixa só mudou de lugar na mesa ou se o módulo se mexeu ao prender os fios. Conferência, pelo mesmo método do `z` ([fusao.md](fusao.md)):
  1. Encoste o nariz da caixa na referência do `z` e anote o roll e o pitch do `a`.
  2. Gire a caixa 180° e encoste a cauda na mesma referência. Anote o roll e o pitch de novo.
  3. Com a correção de montagem certa, as duas medidas ficam opostas (± a inclinação da mesa), e a média delas fica perto de 0°.
  4. Se a média passar de ~0,5° em roll ou em pitch (o dobro da repetibilidade do assento, 0,26°, medida em 13/10), o módulo se mexeu: refaça o `z`. Se ficar abaixo, a montagem gravada continua valendo.

  **Feito em 23/10/2026: a montagem gravada continua valendo** ([hardware.md](hardware.md), "Conferência da montagem depois de prender a fiação"). A média nariz/cauda deu roll −0,31° e pitch +0,18°. A caixa pode ser fechada.

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
