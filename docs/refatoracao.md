# Refatoração do `main.cpp` provisório do ESP A

Pré-requisito das tasks FreeRTOS da semana 3 ([plano.md](plano.md), "Decisões para a semana 3"). Retomada em 20/10/2026, com o plano abaixo aprovado pelo autor.

## Objetivo

Tirar do `main.cpp` as máquinas de estado e os serviços, **sem mudar o comportamento**. Os módulos ficam com a forma que a semana 3 precisa: a `sensor_task`, dona do barramento, chama os serviços, e a serial vai para uma task de baixa prioridade.

## Decisões (20/10/2026)

- **R1. Duas camadas por módulo.**
  - **Núcleo puro** em `lib/`, sem `Arduino.h`: uma máquina de estado que recebe o instante como parâmetro e devolve um resultado ou evento, com getters para o estado. Não imprime, não lê o relógio e não acessa a NVS. É testado com `pio test -e native`.
  - **Serviço Arduino** em `lib/`: é dono do driver, chama o núcleo, grava na NVS e imprime. Recebe os valores do `config.h` numa struct de configuração (ex.: `BaroServiceConfig`), montada no `main.cpp`, como os drivers já fazem. As libs não incluem o `config.h`: no build delas, um `config.h` do SDK (mbedtls, bt) vem antes do nosso no caminho de includes.
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
| 2 | Barômetro (`BaroZero` + `baro_service`) | zero do boot, `0`, `b`, SDA do BMP280 solto | feito em 20/10 (build: RAM 24 248 B, flash 363 941 B; 10 casos novos no `test_baro`; bancada abaixo) |
| 3+4 | Magnetômetro: `MagCalibrator` + `mag_service` (agenda, proa, distúrbio, calibração `m`). Juntos em 20/10: a leitura entrega os pontos à coleta, e separar exigiria um gancho provisório | roteiro base; `p` com ímã perto do módulo; `m` cancelado com `x`; `m` completo com `d` e `s` | feito em 20/10 (build: RAM 24 384 B, flash 364 769 B; 7 casos novos no `test_mag`; bancada abaixo; `s` de uma calibração aceita não conferido) |
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


**Passo 2, primeira sessão (log `261020-133830`, carimbo `2026-10-20 13:37:47`).** O roteiro base (1–11) foi completo; os testes do barômetro, só em parte.

- **Roteiro base:** as mensagens são as mesmas da linha de base.
  - `r`: 199–200 Hz, com perdidas, descartadas, falhas e fila cheia em 0 nas 82 linhas, inclusive com a caixa em movimento.
  - `a`: atraso de 113–120 µs; custo médio de 52–74 µs e máximo de 109–208 µs; ambíguos, descartados e reinícios em 0.
  - `b` e `p` a 10 Hz. Zero do boot aceito com 0,75 Pa, e `0` aceito com 0,43 Pa.
- **Zero com a caixa mexendo (passo 12):** de 8 pedidos de `0`, 2 foram recusados (1,58 e 1,77 Pa, "medindo de novo") e depois aceitos. Os outros foram aceitos com 0,65–1,40 Pa. A margem é pequena, como em 19/10: o filtro IIR do chip atenua o movimento. O `c` mostrou `(novo zero em andamento)`. Um zero foi aceito com 50 leituras, ainda acima do mínimo de 45.
- **"Terminal parou de responder":** depois do passo 12, chegaram as teclas `p`, `c` e `m`. O `m` iniciou a calibração do magnetômetro, e durante a coleta (até 180 s) todas as teclas, menos o `x`, são ignoradas. O mesmo vale na revisão que vem depois, que só aceita `s`, `x` e `d`. Esse comportamento é o de antes da refatoração, não uma regressão. Os passos 13–17 (SDA solto) ficaram para outra sessão.
- **Distúrbio magnético alternando:** com a caixa sendo manuseada, o |m| ficou em 28–31 µT, em torno do limite de 29,6 µT (raio de 25,7 µT + 15 %). As mensagens `DISTÚRBIO`/`distúrbio terminou` se alternaram várias vezes. Também é o comportamento de antes: o limite não tem histerese.

**Passo 2, segunda sessão (log `261020-134704`, mesmo carimbo).** SDA do BMP280 solto com o resumo `r` ligado. O SDA não foi recolocado durante o log.

- **Barômetro:** se comportou como esperado. Saíram as 5 falhas a 25 Hz, `FALHANDO` e depois 1 tentativa por segundo (falhas de 4 a 87). O `c` deu `ALT não (sem barômetro)` e `valid = 0x0009`, e não houve recuperação.
- **Magnetômetro:** perdeu a configuração uma vez, provavelmente pelo fio solto mexendo no barramento. Ficou `FALHANDO` por ~1 s e voltou a `ok` com `recuperações 1`. É o mesmo mecanismo de 19/10.
- **MPU: `descartadas 10` em quase toda janela de 1 s**, e `1` nas outras. A atitude ficou com 170 de 200 amostras por janela: a fusão reiniciou a cada segundo, porque a lacuna de ~55 ms passa do `FUSION_MAX_DT_MS` (50) e o filtro espera 20 amostras para recomeçar.
  - **Causa:** a tentativa do barômetro gera o log de erro do núcleo (`[E][Wire.cpp:499]`, com `CORE_DEBUG_LEVEL=3`). Esse log não passa pelo buffer do `Serial` e espera a transmissão do resumo em curso (~600 bytes, ~52 ms). É o mecanismo registrado em 15/10 (hardware.md, "Buffer da serial").
  - **Por que não é regressão do passo 2:**
    - Os dois períodos são de 1 s: o da tentativa (fase contínua, igual à de antes) e o da janela do resumo. Por isso a fase entre eles quase não muda, e o resultado depende de onde a falha começou.
    - Neste log, a tentativa cai logo depois da primeira linha do resumo, em `xx676–xx681` ms.
    - Em 19/10 (log `261019-210112`, firmware antigo, resumo ligado e barômetro falhando), ela caía logo antes do resumo, e houve 0 descartes.
    - Os testes de 19/10 com o SDA solto (`213533`) usaram só o `c`, sem o `r`.
- **Proa:** foi de ~102° para ~73°, porque a caixa foi girada ao mexer no fio.
- **Falta:** recolocar o SDA (`baro: ok` sem reconfiguração) e o `c` final.

**Passo 2, terceira sessão (log `261020-135408`, mesmo carimbo).** Boot com o SDA do BMP280 solto e o resumo `r` ligado. O comportamento foi igual ao de 19/10:

- `sem resposta no endereço I2C`, com nova tentativa a cada 5 s (11 tentativas, de 0,7 a 60,8 s);
- giroscópio calibrado;
- `altitude: sem zero` e `valid = 0x0009`;
- `r` com perdidas, falhas e fila cheia em 0, e 1 descarte isolado em 30 janelas. Com uma tentativa a cada 5 s, a colisão com o resumo é rara.

O barômetro não voltou até o fim do log: o SDA não foi recolocado, ou ficou sem contato.

**Passo 2, fechamento (logs `140055` a `141053`).**

- **De ~13:48 a 14:05, o BMP280 não respondeu em nenhum boot**, nem com o SDA encaixado (log `140247`). Aos 25,1 s houve o `Error 263` do encaixe, e depois 16 tentativas com `Error -1`.
- **O diagnóstico das 14:08 (log `140827`) encontrou o 0x76 (BMP280) de novo**, com o 0x1E e o 0x68.
- **Firmware de voo regravado (carimbo `2026-10-20 14:08:56`, log `141053`):** dois boots com `BMP280 configurado` e `zero aceito` (0,32 Pa). O `c` deu `ALT ok` e `valid = 0x000D`.
- **Conclusão:** a volta do chip não foi vista ao vivo, mas o caminho que falta (nova tentativa a cada 5 s → configuração → zero) usa a mesma rotina do boot, conferida aqui. O ritmo das tentativas foi conferido nos logs anteriores. **Passo 2 fechado.**

**Passo 3+4, primeira sessão (log `261020-143421`, carimbo `2026-10-20 14:33:39`).** Passos 1–7 do roteiro: base, distúrbio e `m` cancelado.

- **Roteiro base:** as mensagens são as mesmas da linha de base.
  - `r`: 199–200 Hz, com perdidas, descartadas, falhas e fila cheia em 0, e o magnetômetro com 24–26 leituras/s.
  - `a`, 87 linhas:
    - custo do filtro médio de 29–35 µs e máximo de 53–137 µs, mais baixo que na linha de base;
    - ambíguos, descartados e reinícios em 0;
    - **atraso pulso → leitura bimodal:** ~105 µs em 28 linhas e 153–173 µs em 59, alternando de uma linha para a outra. Na linha de base, ficava em 110–125 µs.
  - **Sobre o atraso:** o caminho da leitura do MPU não mudou neste passo. O padrão de alternância é o mesmo que o custo do filtro mostrou na linha de base. A hipótese é falta no cache da flash depois das impressões, com a posição do código mudando a cada build. O efeito é desprezível: o limite de segurança é de 4 000 µs (`FUSION_MAX_INT_LAG_US`), e não houve ambíguos. **Critério daqui em diante:** atraso máximo abaixo de ~200 µs, com 0 ambíguos.
  - `b` e `p` a 10 Hz. Zero do boot com 0,32 Pa, e `0` com 0,56 Pa.
- **Distúrbio com a chave de fenda:** `DISTÚRBIO` com o |m| em 21,7 µT (abaixo do raio) e depois em 31,9 µT (acima), e `distúrbio terminou` ao afastar. Enquanto durou, a proa filtrada ficou parada em 114,7°, com a `mag` indo até 46°, e o estado no fluxo era `DISTURBIO`.
  - Depois do "terminou", o campo ainda estava distorcido, mas dentro dos 15 %, e a proa filtrada foi puxada (de 115° para ~88°). Isso já tinha sido visto em 15/10 (magnetometro.md: ajuste do `MAG_DISTURB_PCT` ou do τ a decidir) e não é regressão.
- **`m` cancelado:** linhas de progresso a cada 2 s (51, 101… 351 pontos) e `x` com `calibração cancelada; calibração anterior mantida`. O `c` seguinte mostrou a mesma calibração (offset +8,1 +6,2 +10,6 µT, raio de 25,7 µT).
  - Com a caixa parada, a cobertura do progresso chegou a 16/24 em 14 s. O centro provisório (mín./máx.) coincide com a nuvem de pontos, e o ruído espalha as direções pelos setores. Esse comportamento já existia. No fim da coleta, a cobertura é recontada com o centro ajustado, e a recusa vem daí. O guia `faltam:` só fica confiável depois que a caixa já girou.

**Passo 3+4, segunda sessão (log `261020-144205`, mesmo carimbo).** Calibração curta, com a caixa parada na mesa o tempo todo.

- **Coleta:** progresso a cada 2 s. **A cobertura chegou a 24/24 aos 46 s só com ruído**, e a coleta terminou pela cobertura (1 151 pontos), não pelo tempo.
- **Ajuste:** recusado por `raio fora da faixa` (0,19 µT, resíduo de 34,7 %, 22/24 setores recontados com o centro ajustado). A mensagem trouxe os limites.
- **Revisão:**
  - `s` deu `calibração recusada não pode ser gravada`;
  - `d` imprimiu os 1 151 pontos entre `# mag_cal inicio` e `# mag_cal fim`, com a linha `# firmware`;
  - `x` deu `resultado descartado; calibração anterior mantida`.
- **Depois:**
  - `p` com 53 linhas no estado `ok`;
  - `c` com a mesma calibração (offset +8,1 +6,2 +10,6 µT), `ATT ok`, `HDG ok` e `valid = 0x000D`.
- **`fila cheia 432 desde o boot`:** veio do `d`, que parou o laço por ~3 s (1 151 linhas a 115200). É o esperado: o D5 do magnetometro.md já registra que a impressão bloqueia o laço, e a fusão é reiniciada depois.
- **Não conferido na bancada:** o `s` de uma calibração aceita (gravação, uso imediato da calibração nova e reinício da proa). São ~10 linhas movidas sem mudança, e a gravação (`magStoreSave`) não mudou. Fica para a próxima calibração real, no modelo final (pendência 4). A carga no boot (`loadCalibration`) está conferida em todos os boots.
- **Passo 3+4 fechado.**

## Pendências (fora da refatoração)

1. **Teclas ignoradas na calibração do magnetômetro.** Na coleta do `m` (até 180 s), o firmware ignora as teclas sem dizer nada, e o terminal parece travado. A revisão que vem depois, que só aceita `s`, `x` e `d`, também. Proposta: ao ignorar uma tecla, imprimir uma linha curta, por exemplo `[mag] calibração em andamento: x cancela`, no máximo uma vez por segundo. Fica para depois do passo 4, num commit à parte, porque muda o comportamento. Vista em 20/10, log `261020-133830`.
2. **Log de erro do núcleo parando o laço.** Com um sensor falhando e uma impressão longa em curso (o resumo `r`, o `c`), cada `[E][Wire.cpp]` pode parar o laço por ~50 ms. Isso descarta 10 amostras e reinicia a fusão a cada tentativa, quando a fase coincide. Já estava previsto para a semana 3 ([plano.md](plano.md): serial numa task de baixa prioridade e revisão do nível de log do núcleo). Agora há a medida do efeito na fusão. Decidir na migração: baixar o `CORE_DEBUG_LEVEL` ou redirecionar o log do núcleo para a fila do `lib/logger`. Vista em 20/10, log `261020-134704`.
3. **Cobertura da calibração completada pelo ruído.** Com a caixa parada, o centro provisório (mín./máx.) fica no meio da nuvem de pontos, e o ruído espalha as direções pelos setores. Em 20/10, a coleta chegou a 24/24 em 46 s sem girar a caixa. O resultado é recusado no fim, mas o guia `faltam:` e o fim antecipado confundem. Proposta: só contar a cobertura quando a amplitude (máx. − mín.) passar de uma fração do raio esperado em pelo menos dois eixos, por exemplo `MAG_CAL_MIN_SPAN_UT` no `config.h`. Muda o comportamento, então fica fora da refatoração. Vista no log `261020-144205`.
4. **`s` de uma calibração aceita do magnetômetro, sem conferência na bancada.** A gravação na NVS, o uso imediato da calibração nova, o reinício da proa e a **persistência depois de um reinício** (boot com `NVS: … magnetômetro calibrado` e o `c` com o offset e a W novos) ficaram sem conferir na refatoração. Os testes novos cobrem só o núcleo puro (`MagCalibrator`), não essa integração com a NVS e o filtro da proa. Conferir na próxima calibração real (modelo final), com os passos 8–12 do roteiro completo do passo 3+4. Registrada em 20/10/2026.
