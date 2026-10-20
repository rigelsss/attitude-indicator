# Refatoração do `main.cpp` provisório do ESP A

Pré-requisito das tasks FreeRTOS da semana 3 ([plano.md](plano.md), "Decisões para a semana 3"). Retomada em 20/10/2026, com o plano abaixo aprovado pelo autor.

## Objetivo

Tirar do `main.cpp` as máquinas de estado e os serviços, **sem mudar o comportamento**. Os módulos ficam com a forma que a semana 3 precisa: a `sensor_task`, dona do barramento, chama os serviços, e a serial vai para uma task de baixa prioridade.

## Decisões (20/10/2026)

- **R1. Duas camadas por módulo.**
  - **Núcleo puro** em `lib/`, sem `Arduino.h`: uma máquina de estado que recebe o instante como parâmetro e devolve um resultado ou evento, com getters para o estado. Não imprime, não lê o relógio e não acessa a NVS. É testado com `pio test -e native`.
  - **Serviço Arduino** em `lib/`: é dono do driver, chama o núcleo, grava na NVS e imprime.
- **R2. Mensagens por um ponto único, o `logPrintf()`/`logPrint()`/`logPrintln()` (`lib/logger`).** O nome não é `logf()` porque `logf()` é o logaritmo do `math.h`. Os serviços imprimem as mesmas mensagens de hoje. Na semana 3, só o `lib/logger` muda: passa a enfileirar para a task da serial, e os serviços ficam como estão.
- **R3. O laço de aquisição fica no `main.cpp` até a semana 3.** Ele faz: espera do pulso → leitura do MPU → barômetro e magnetômetro → contabilidade → calibração e fusão. É, quase literalmente, o corpo da futura `sensor_task`. O `main.cpp` continua marcado como provisório.
- **R4. Comandos como métodos dos serviços.** Exemplos: `startSix()`, `confirm()`, `cancel()`, `save()`, `zeroAltitude()`. O console só traduz teclas em chamadas. Isso prepara a camada única de comandos (item 8 da revisão de 15/10). Os `CMD_*` não são implementados agora.

## Módulos

| Módulo | Camada | Conteúdo |
|---|---|---|
| `lib/logger` | Arduino | `logPrintf()`, `logPrint()`, `logPrintln()` e `logFlush()`; hoje, um repasse para o `Serial` |
| `lib/sched` | puro | `Periodic`: próxima execução, período, ressincronização no atraso e período reduzido com o sensor falhando (hoje repetido no barômetro, no magnetômetro e nos fluxos) |
| `lib/baro` (amplia) | puro | `BaroZero`: estados Off/Settling/Measuring e recusas do zero |
| `lib/baro_service` | Arduino | `Bme280`, nova tentativa, recuperação, zero, aviso de ΔT, fluxo `b`, altitude atual |
| `lib/mag` (amplia) | puro | `MagCalibrator`: coleta, cobertura e progresso, ajuste e revisão (o buffer de pontos vem do serviço) |
| `lib/mag_service` | Arduino | `Hmc5883l`, agenda, correção da proa, borda do distúrbio, `m`/`s`/`x`/`d`, fluxo `p` |
| `lib/attitude` (amplia) | puro | `imuToBody()` (calibração → eixos do modelo → montagem); `FusionCore`: filtro, só-giroscópio, `HeadingFilter`, referência do dt, estatística do `a`, `reset()` |
| `lib/cal_flow` | puro | `GyroCalFlow` (boot e `g`, com nova tentativa), `SixPoseFlow`, `MountFlow` e o coordenador (modo, janela de calibração, regras de bloqueio: `m` liberado na calibração do boot, `z` com pré-requisitos) |
| `lib/cal_service` | Arduino | carga da NVS no boot, gravações (`imu_store`, `level_store`), mensagens `[cal]` e `[mont]` |
| `lib/acq` | puro | `PulseTracker` (pulsos, perdidas, descartadas, fechamento de janelas, virada do `micros()`); `SampleWindow` (médias, ruído, saturações, atitude); validade temporal do quadro; descarte de partida |
| `src/console.cpp` | Arduino | tecla → comando, `c`, `r`, validade, ajuda |

Os acoplamentos que hoje passam por globais viram parâmetros ou referências explícitas. Exemplos: o magnetômetro lê a atitude e o modo da calibração; o reinício da fusão zera a proa; a fusão consulta o modo da calibração magnética; o fechamento da janela zera a contagem de leituras do magnetômetro.

## O que não muda

- Teclas, texto das mensagens e ordem das linhas na serial.
- Constantes do `config.h`.
- Formatos e namespaces da NVS (sem recalibrar nada).
- Ordem do ciclo: MPU primeiro, depois barômetro e magnetômetro; espera depois do INT.
- Contrato (`telemetry.h`) e lógica das libs já existentes (só acréscimos).

## Ordem de extração

Cada passo termina com: compilar `aircraft`, rodar `pio test -e native`, fazer a conferência curta na bancada e o commit do autor.

| Passo | Conteúdo | Conferência na bancada | Situação |
|---|---|---|---|
| 0 | Linha de base: log de referência, RAM/flash e métricas do `a` | roteiro base (abaixo) | feito em 20/10 |
| 1 | `lib/logger` + `lib/sched` | roteiro base | feito em 20/10 (build: RAM 24 136 B, flash 363 485 B; `test_sched` com 7 casos; bancada abaixo) |
| 2 | Barômetro (`BaroZero` + `baro_service`) | zero do boot, `0`, `b`, SDA do BMP280 solto | — |
| 3 | Magnetômetro sem calibração (agenda, proa, distúrbio) | `p`, ímã perto do módulo | — |
| 4 | Calibração `m` (`MagCalibrator`) | `m` completo, `d`, `x` e `s` | — |
| 5 | Fusão (`imuToBody` + `FusionCore`) | `a`, inclinações conhecidas, métricas iguais às da base | — |
| 6a | Calibração do giroscópio + coordenador | `g`, calibração do boot, `m` durante a calibração do boot | — |
| 6b | Seis posições + montagem | `z` completo; `6`: entrada, uma posição, cancelamento | — |
| 7 | Aquisição (`lib/acq`) | `r`, `c`, INT solto, métricas iguais às da base | — |
| 8 | Console (`src/console.cpp`) | roteiro base inteiro, mensagem a mensagem | — |

Os passos 1–7 são os essenciais. Se o prazo apertar, o 8 é quase só mover código. O risco principal é uma regressão de tempo no passo 7, e a mitigação é comparar as métricas com a linha de base.

**Testes novos no `native`:**

- máquinas de estado das calibrações: aceita, recusa, inconsistente, cancelamento, nova tentativa no boot;
- `BaroZero`;
- `MagCalibrator`, com fim por cobertura e por tempo;
- `Periodic`;
- `PulseTracker`, com virada do `micros()`, pulsos atrasados e janelas consecutivas.

## Linha de base (passo 0)

Firmware do commit `af389ba`, compilado em 20/10/2026: RAM 24 136 B (7,4 %), flash 363 701 B (27,7 %).

**Roteiro base.** Caixa parada na mesa, BMP280 e HMC5883L ligados. A mesma sequência é repetida depois de cada passo:

1. Boot. Esperar a calibração do giroscópio do boot e o zero da altitude, cerca de 15 s.
2. `c`.
3. `r`, deixar 10 s e `r` de novo.
4. `a`, deixar 10 s e `a` de novo.
5. `b`, deixar 5 s e `b` de novo.
6. `p`, deixar 5 s e `p` de novo.
7. `0` e esperar o zero ser aceito.
8. `c`.

**Métricas de referência** (log `261020-130702`, carimbo `2026-10-20 13:06:22`, segundo boot do log). Um passo da refatoração só fecha se ficar dentro destas faixas:

| Métrica | Linha de base |
|---|---|
| Taxa (`r`) | 199–200 Hz |
| Perdidas, descartadas, falhas, fila cheia (`r`, 15 s) | 0 |
| dt da fusão (`a`, mín/méd/máx) | 5,00/5,01/5,01 ms |
| Atraso máximo pulso → leitura (`a`) | 110–125 µs |
| Custo do filtro (`a`, méd/máx) | 60–73 / 85–193 µs. O máximo alterna entre ~85 e ~185 µs de uma linha para a outra |
| Ambíguos, descartados, reinícios (`a`, ~20 s) | 0 |
| Leituras do magnetômetro | 24–26 por segundo |
| Ruído parado | a 2,4–4,5 mg, g 0,038–0,098 °/s |
| Boot | giroscópio calibrado, zero aceito (0,56 Pa), `valid = 0x000D` |
| `0` | zero aceito (0,53 Pa) |

O último `c` do roteiro (passo 8) ficou fora do log. O formato do `c` está registrado no primeiro.

## Conferências na bancada

**Passo 1 (log `261020-131802`, carimbo `2026-10-20 13:17:16`).** Roteiro base completo, com o `c` final.

- **Mensagens:** as mesmas da linha de base. Tirando os números, a única linha que muda é a recusa do giroscópio por movimento. Ela apareceu no primeiro boot da linha de base e não neste.
- **`r`:**
  - 199–200 Hz;
  - perdidas, descartadas, falhas e fila cheia em 0;
  - magnetômetro com 24–26 leituras por segundo.
- **`a`, 87 linhas:**
  - dt 5,00–5,01/5,01/5,01 ms;
  - atraso máximo de 112–130 µs (típico 113, contra 117 na linha de base);
  - ambíguos, descartados e reinícios em 0.
- **Cadência dos fluxos:** `b` e `p` a 10 Hz, como antes.
- **Custo do filtro:** médio de 51–55 µs, contra 60–73 µs na linha de base. O máximo ficou em 172–200 µs em todas as linhas, sem a alternância de antes entre ~85 e ~185 µs.
  - O código do filtro não mudou. O que mudou foi a posição do código na flash, e o custo medido inclui as faltas no cache da flash, principalmente na primeira amostra depois de uma impressão.
  - **Critério a partir deste passo:** custo médio de até ~75 µs e máximo de até ~200 µs. Esses valores ficam muito abaixo do período de 5 ms. O padrão de alternância não conta como critério.

