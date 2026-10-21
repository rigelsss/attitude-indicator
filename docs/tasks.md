# Tasks do ESP A (FreeRTOS)

Semana 3 do [plano.md](plano.md). O `main.cpp` só inicializa e cria as tasks (`src/tasks/`); a lógica fica em `lib/` e nos serviços. Antes, tudo rodava no `loop()` do `main.cpp` provisório ([refatoracao.md](refatoracao.md)).

## Tasks

| Task | Núcleo | Prioridade | Pilha | Função |
|---|---|---|---|---|
| `sensor_task` | 1 | 4 | 8 kB | Dona do barramento de sensores (`Wire`). Acordada pela INT do MPU a 200 Hz: lê o MPU6050 e, intercalados, o BMP280 e o HMC5883L; calibração e fusão. Executa os comandos (teclas) entre duas leituras |
| `serial_task` | 0 | 1 | 4 kB | Inicia o `Serial`, passa as mensagens do `lib/logger` para a UART a cada 2 ms e entrega as teclas à `sensor_task` |
| `radio_task` | 0 | 2 | — | Passo 4 (ESP-NOW) |
| `status_task` | 0 | 1 | — | Passo 6 (OLED de status no `Wire1` e LED) |

Os valores ficam no `config.h` (seção "Tasks"). A `loopTask` do Arduino se apaga no primeiro `loop()`.

**Por que cada coisa fica onde está:**

- **A `sensor_task` faz a própria inicialização** (`Wire.begin`, fila e ISR do INT, NVS, sensores). No ESP32, uma interrupção é alocada no núcleo de quem a registra. Assim, a ISR do I2C e a do INT ficam no núcleo 1, junto da task que elas acordam.
- **O `Serial.begin()` fica na `serial_task`** (decisão D1 da semana 3). A ISR da UART fica no núcleo 0, fora do núcleo da aquisição.
- **O Wi-Fi do IDF roda no núcleo 0.** Por isso, o rádio e o OLED também vão para lá.

## Comunicação entre as tasks

| De → para | Meio | Conteúdo |
|---|---|---|
| ISR do INT → `sensor_task` | fila de `MPU_INT_QUEUE_LEN` (16) instantes | carimbo do pulso (`esp_timer_get_time()`), ISR em IRAM no nível 2 |
| `serial_task` → `sensor_task` | fila de `CMD_QUEUE_LEN` (16) `SensorCmd` | tecla e origem (`CMD_SRC_CONSOLE`); na semana 5, também os `CMD_*` do B. Fila cheia: a tecla é descartada com aviso |
| `sensor_task` → `serial_task` | fila de `REPORT_QUEUE_LEN` (4) relatórios, do tamanho do maior (~0,6 kB) | cópias para o `c`, o `r` e o `a` (`src/console_report.h`); cheia, descarta e conta, e a `serial_task` avisa |
| qualquer task → `serial_task` | buffer do `lib/logger` (`esp_ringbuf`, 8 kB) | mensagens inteiras; cheio, descarta e conta ([hardware.md](hardware.md), "Logger com buffer…") |

- **Logger com um consumidor só.** O `logAttachSerial()`, chamado pela `serial_task` depois do `Serial.begin()`, registra a task que esvazia o buffer e escreve no `Serial`. Em qualquer outra task, o `logPump()` não faz nada, e o `logWaitRoom()` (dump do `d`) só espera. Até o registro, as mensagens do boot se acumulam no buffer.
- **`logFlush()` de outra task pede o flush à `serial_task` e espera a confirmação dela**, até `LOG_FLUSH_TIMEOUT_MS` (3 s). Ela retira um trecho do buffer antes de escrevê-lo, então, para as outras tasks, o buffer pode parecer vazio com a escrita ainda em curso. Só ela sabe quando tudo foi escrito; então ela faz o `Serial.flush()` e libera o semáforo. Usado antes de um reinício por erro e no fim do dump do `d`.
- **Inicialização sincronizada.** O `setup()` só imprime e cria a `sensor_task` depois que a `serial_task` sinaliza o `Serial` pronto (`logWaitSerial`, até `SERIAL_READY_TIMEOUT_MS`; sem o sinal, reinicia).
- **Modo degradado do logger, sem memória para o buffer no boot:** as mensagens anteriores ao `Serial` pronto são descartadas, e as seguintes são escritas direto no `Serial`. O `HardwareSerial` tem mutex, então isso é seguro entre tasks, mas bloqueia quem imprime, como antes da P2. O log do núcleo também não é redirecionado. O boot avisa com `[A] AVISO: sem memória para o buffer do logger…`. Os semáforos são estáticos e valem também nesse caso.
- **O console tem dois lados (passo 2).**
  - **Na `sensor_task` (`src/console.cpp`):** o despacho das teclas, que depende do estado das calibrações e por isso pode chamar os serviços direto, os liga/desliga do `r` e do `a`, e a montagem das cópias (`WindowReport`, `AttReport`, `StatusReport`). Os serviços entregam o resumo de uma linha por cópia (`BaroSummary`, `MagSummary`), com os textos como ponteiros para literais.
  - **Na `serial_task` (`src/console_view.cpp`):** a formatação, só a partir das cópias, sem acesso aos serviços. Cada bloco é montado inteiro num `TextBuf` (`lib/log_line`, `CONSOLE_VIEW_BUFFER_BYTES`) e entregue ao logger numa escrita só, que é atômica. Assim, linhas de outras tasks (o `p`, um `[cal]`) não caem no meio de um `c`.
  - **Ritmo:** a cada volta, a `serial_task` formata no máximo `SERIAL_REPORTS_PER_CYCLE` (2) relatórios e escoa o logger depois de cada um. Assim, uma rajada de `c` não acumula vários blocos de ~1,4 kB no buffer antes de escoar, e as teclas continuam sendo lidas a cada volta. O contador de relatórios descartados é `std::atomic`: a `sensor_task` escreve e a `serial_task` lê.
  - **Ordem:** o relatório é copiado no instante do comando ou do fim da janela e impresso alguns ms depois. Uma mensagem de evento da `sensor_task` desse intervalo pode sair antes do bloco, mas os valores são os do instante da cópia.

## Medições

**Passo 1: `sensor_task` e `serial_task`** (carimbo `2026-10-21 15:01:55`, caixa parada):

| Rodada | Log | Resultado |
|---|---|---|
| Roteiro base | `261021-150257` | `a`, 102 linhas: dt 5,00–5,01 ms, atraso de 104–115 µs, filtro méd. 24–54 / máx. 54–148 µs, ambíguos, descartados e reinícios em 0. `r` com 199–200 Hz e tudo em 0. `b`, `p`, `0` e `c` (`valid = 0x000D`) como antes; mensagens iguais às da base (`log_templates.py`), menos a linha de boot, que agora termina em `(tasks: serial e sensor)` |
| Teclas | `261021-150455` e `152530` | `h`, `g`, `6` com Enter e `x`, `z` com Enter e `x`, `m` e `x`: as mesmas respostas de antes, nenhuma tecla descartada |
| **`r` e `a` juntos, ~82 s** | `261021-150632` | **atraso de 104–118 µs nas 822 linhas** (antes: até 723 µs na P5 e 1 949 µs no passo 7), dt 5,00–5,01 ms, ambíguos e reinícios em 0 |

- **O atraso com o `r` e o `a` juntos não vinha da formatação.** O resumo `r` (~40 números em ponto flutuante) continua sendo formatado na `sensor_task`. O que atrasava o pulso seguinte era a escrita no `Serial`: a cópia para o driver da UART, com o mutex dele, e a ISR da UART no mesmo núcleo. Agora as duas ficam no núcleo 0. Com isso, o critério da pendência 2 (atraso abaixo de ~200 µs em todas as linhas) já foi atendido no passo 1.
- **Falta para o passo 2:** o atraso depois de um `c`, que formata ~1,4 kB (2 437 µs no logger da P2), e a leitura do estado por cópia (pendência 7).
- **Correções da revisão do logger (flush pela `serial_task`, inicialização sincronizada e modo degradado):** conferência curta no log `261021-153958` (carimbo `15:39:17`). Os dois boots saíram inteiros e na ordem de sempre, o `c` deu `valid = 0x000D` e o `r` (5 janelas) deu 199–200 Hz com tudo em 0. O flush pedido por outra task (reinício por erro e fim do `d`) e o modo degradado não foram exercitados na bancada.
- No primeiro boot do roteiro base, aos 26 s, dois `[E][Wire.cpp]` e um `HMC5883L perdeu a configuração` vieram logo antes de um `POWERON_RESET`. É o mesmo padrão de hoje à tarde, no reset com os fios mexendo; o roteiro rodou no boot seguinte.

**Passo 2: o `c`, o `r` e o `a` formatados na `serial_task`** (carimbo `2026-10-21 16:13:37`, caixa parada):

| Rodada | Log | Resultado |
|---|---|---|
| Roteiro base | `261021-161600` | `a`, 77 linhas: dt 5,00–5,01 ms, atraso de 106–113 µs, ambíguos, descartados e reinícios em 0. `r` com 199–200 Hz e tudo em 0. `b`, `p`, `0` e `c` (`valid = 0x000D`) normais; mensagens iguais às da base (`log_templates.py`) |
| `c` com o `p` ligado | `261021-161754` | 3 blocos do `c` inteiros (18 linhas cada), com 52 linhas `[mag] proa` em volta e nenhuma no meio |
| `a` com 15 `c` | `261021-161836` | **atraso de 105–119 µs nas 252 linhas** (na P2, 2 437 µs depois de um `c`), dt 5,00–5,01 ms, 15 × `valid = 0x000D` |
| `r` e `a` juntos, ~82 s | `261021-161934` | atraso de 105–120 µs nas 816 linhas, dt 5,00–5,01 ms, `r` com 199–200 Hz e tudo em 0 |

- Nenhum relatório descartado, nenhum texto cortado e nenhum `[log] … descartadas`.
- **Correções da revisão** (contador de descartes atômico; no máximo 2 relatórios por volta, com o logger escoado depois de cada um). Log `261021-162644`, carimbo `16:26:05`: rajada de `c` com o `a` ligado, com 9 blocos inteiros (18 linhas cada, sem linhas do `a` no meio), atraso de 102–109 µs nas 24 linhas do `a` e nenhum relatório ou mensagem descartados.
- Com isso, as pendências 2 e 7 do [refatoracao.md](refatoracao.md) estão fechadas.
