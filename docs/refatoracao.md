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
| `lib/attitude` (amplia) | puro | `imuToBody()` (calibração → eixos do modelo → montagem) |
| `lib/fusion` | puro | `FusionCore`: filtro, só-giroscópio, `HeadingFilter`, referência do dt, estatística do filtro para o `a` (o custo pelo relógio dado por quem usa), `reset()`. Fica fora do `lib/attitude` porque o `lib/mag` (com o `HeadingFilter`) já depende dele. A estatística da aquisição (atraso, ambíguos, descartados) fica no `main.cpp` até o passo 7 |
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
| 5 | Fusão (`imuToBody` + `FusionCore`) | `a`, inclinações conhecidas, métricas iguais às da base | feito em 20/10 (build: RAM 24 408 B, flash 365 229 B; `test_fusion` com 10 casos; bancada e A/B abaixo; variação de ±50 µs no dt, pendência 5) |
| 6a | Calibração do giroscópio + coordenador (`GyroCalFlow`, `CalCoordinator`, `cal_service` com a carga da NVS, inclusive a montagem) | `g` parado e balançando, boot balançando, `m` durante a calibração do boot, `z` durante um `g` | feito em 20/10 (build: RAM 24 608 B, flash 366 137 B; `test_cal_flow` com 18 casos; `main.cpp` com 891 linhas; bancada abaixo) |
| 6b | Seis posições + montagem (`SixPoseFlow`, `MountFlow`, comandos do coordenador, `CalService::handleKey`) | `z` completo com `s` e persistência; `z` com medida recusada; `6`: entrada, uma posição, cancelamento | código em 20/10 (build: RAM 24 832 B, flash 366 665 B; `test_cal_flow` com 23 casos; `main.cpp` com 630 linhas; bancada abaixo) |
| 7 | Aquisição (`lib/acq`: `SampleWindow`, `PulseTracker`, `checkFrame`, `AcqStats`, `SettleGate`) | `r`, `c`, INT solto, métricas iguais às da base | feito em 20/10 (build: RAM 24 840 B, flash 367 013 B; `test_acq` com 13 casos; `main.cpp` com 556 linhas; bancada e A/B abaixo) |
| 8 | Console (`src/console.cpp`: teclas, `c`, `r`, `a`, ajuda; `validityInputs()` fica no `main.cpp`) | roteiro base inteiro, mensagem a mensagem (texto pelo `tools/log_templates.py`, ordem lendo o log) | código em 20/10 (build: RAM 24 880 B, flash 367 413 B; sem lógica pura nova; `main.cpp` com 341 linhas); feito em 20/10 (bancada abaixo) |

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

**Passo 5, primeira sessão (log `261020-150426`, carimbo `2026-10-20 15:03:32`).** Roteiro base e calço novo, mais alto que o de 2,5 cm. Pelo inclinômetro do celular, ele dá ~9–10° no nariz e na cauda e ~20–21° nas asas.

- **Roteiro base:** as mensagens são as mesmas da linha de base.
  - `r`: 199–200 Hz, com perdidas, descartadas, falhas e fila cheia em 0 nas 118 janelas.
  - `a`, 147 linhas: atraso de 102–117 µs; custo do filtro médio de 27–36 µs e máximo de 93–208 µs; ambíguos, descartados e reinícios em 0.
  - `b` e `p` a 10 Hz. Zero com 0,65 Pa (boot) e 0,73 Pa (`0`).
- **Calço** (média de 1 s no `r`, desvio de 0,00–0,01° parado):

  | Posição | Roll | Pitch |
  |---|---|---|
  | Nivelada (antes, entre e depois) | −0,45 a −0,54° | −0,79 a −0,84° |
  | Asa esquerda | +20,97° | −0,99° |
  | Asa direita | −22,96° | −0,56° |
  | Nariz | −0,35° | +8,79° |
  | Cauda | +1,43° | −10,27° |

  - **Meia diferença entre os lados opostos** (o ângulo do calço, sem a inclinação da mesa): roll de 21,97° e pitch de 9,53°, dentro da faixa do celular, que tem resolução de 1°.
  - **O ponto médio entre os lados opostos repete a mesa nivelada:** roll de −1,00° (nivelada: −0,5°) e pitch de −0,74° (nivelada: −0,80°).
  - A razão entre os senos (2,26) passa ~5 % da razão das bases da caixa (28/13 = 2,15). Em 14/10 também passou (~3,5 %). O apoio do calço muda a distância efetiva, então não é erro do filtro.
  - O outro eixo mudou ao apoiar o calço (até 1,9° de roll na cauda), como em 14/10: é o apoio da caixa.
- **O dt da fusão passou a ter ±50 µs de variação em janelas alternadas.** Em ~55 % das linhas do `a`, o dt mínimo/máximo ficou em 4,94–4,98 / 5,03–5,07 ms. Nos logs anteriores (passos 0 a 3+4), era sempre 5,00–5,01 ms. A média observada permanece em 5,01 ms; não foi identificado impacto nos testes realizados. A estimativa por intervalo é pequena (a 20 °/s, 50 µs dão 0,001°), mas o efeito durante movimentos variáveis ainda não foi quantificado: a soma dos dt se mantém, e o erro integrado depende também da velocidade angular de cada amostra. O comportamento é novo, e a causa ainda não foi identificada: o carimbo do pulso vem da ISR, que este passo não mudou.

**Passo 5, teste A/B (logs `261020-151621` e `261020-151801`).** Mesma sessão, caixa parada, só o `a` por 10 s:

| Firmware | Carimbo | dt no `a` |
|---|---|---|
| Passo 3+4 (`ae27361`, compilado num worktree à parte) | `15:15:32` | 83 linhas, todas 5,00/5,01/5,01 ms |
| Passo 5 | `15:17:12` | 75 de 85 linhas entre 4,95 e 5,06 ms |

- **A variação vem do build, não da sessão.** Atraso, custo do filtro, ambíguos, descartados e reinícios ficaram iguais nas duas rodadas.
- **Hipótese, não confirmada:** o caminho da interrupção não mudou, mas o carimbo do pulso depende de duas coisas que variam com o build.
  - A ISR do GPIO tem a mesma prioridade (nível 1) da ISR de transmissão da UART, que copia dezenas de bytes para a FIFO. Um pulso que chega durante essa cópia espera.
  - O despachante de GPIO do Arduino (`__onPinInterrupt`) roda da flash, porque o `CONFIG_ARDUINO_ISR_IRAM` está desligado no núcleo, e fica sujeito ao cache.
  - O passo 5 mudou o tempo entre o pulso e a impressão do `a` e a posição do código. Com isso, a cópia da UART passou a coincidir com os pulsos. Isso explicaria também o atraso bimodal do passo 3+4 e a alternância do custo do filtro na linha de base.
- **Decisão (autor, 20/10):** fechar o passo 5 como está e corrigir o caminho da INT na semana 3 (pendência 5). A média observada do dt permanece em 5,01 ms, e não foi identificado impacto nos testes realizados. O efeito durante movimentos variáveis não foi quantificado (pendência 5).
- **Passo 5 fechado.**

**Passo 6a (log `261020-181722`, carimbo `2026-10-20 18:16:11`, três boots).**

- **Roteiro base, parte:** boot, `r`, `a` e `c`. O `b`, o `p` e o `0` não foram rodados; o barômetro e o magnetômetro não mudaram neste passo.
  - Boot: linha `[A] NVS: …` igual à de antes, giroscópio calibrado e zero aceito (0,45 Pa).
  - `r`, 6 janelas: 199–200 Hz, com perdidas, descartadas, falhas e fila cheia em 0, e o magnetômetro com 25 leituras/s.
  - `a`, 36 linhas:
    - atraso de 112–117 µs;
    - custo do filtro médio de 54–68 µs e máximo de 172–192 µs. A primeira linha, com 1 amostra, deu 105/105 µs;
    - ambíguos, descartados e reinícios em 0.
  - `c`: `valid = 0x000D` e giroscópio corrigido em +0,007 +0,002 −0,005 °/s.
- **dt da fusão:** 5,00/5,01/5,01 ms em todas as linhas do `a`. A variação de ±50 µs do passo 5 não apareceu neste build. Isso reforça a hipótese de que ela depende da posição do código (pendência 5). Nada foi corrigido, então a pendência continua.
- **Calibração do giroscópio:**

  | Teste | Linhas | Resultado |
  |---|---|---|
  | `g` parado | 128–129 | aceito: bias −4,862 +2,597 +1,141 °/s, diferença entre janelas de 0,011 °/s e 0,009° |
  | `g` balançando | 130–137 | `recusado: placa em movimento` (41,98 °/s) e `mantido o bias anterior`, sem nova tentativa. O `c` seguinte mostrou o mesmo bias do `g` parado, como `DEFINITIVO (deste boot)` |
  | Boot balançando | 169–175 | duas recusas (29,2 e 16,7 °/s), cada uma seguida de um novo `mantenha a placa parada…` e sem `mantido`; depois, aceito |
  | `m` durante a calibração do boot | 194–204 | a coleta começou, sem `espere a calibração em andamento terminar`. O giroscópio foi aceito no meio da coleta (linha 199), e o `x` cancelou com `calibração anterior mantida` |
  | `z` durante um `g` | 205–207 | `[mont] espere a calibração em andamento terminar`, e depois o `g` foi aceito |

  - Nas cinco calibrações aceitas (três boots e dois `g`), o bias variou até 0,033 °/s (de −4,843 a −4,876 °/s no x), com a temperatura entre 29,2 e 29,5 °C.
- **Mensagens:** com o mesmo texto e a mesma ordem de antes.

**Passo 6a, complemento (log `261020-182139`, mesmo carimbo).** O resto do roteiro base (`b`, `p`, `0` e `c`), com a caixa parada.

- **Boot:** giroscópio calibrado e zero aceito (0,57 Pa).
- **`b`:** 57 linhas a 10 Hz, com a altitude entre −0,09 e +0,07 m.
- **`p`:** 45 linhas a 10 Hz, todas no estado `ok`, com a proa em ~91°.
- **`0`:** zero aceito (0,41 Pa).
- **`c`:**
  - `giroscópio: DEFINITIVO (deste boot)`;
  - `altitude: zero p0 = 1009.78 hPa`;
  - `valid = 0x000D`;
  - perdidos, descartados e fila cheia em 0.
- **Captura do boot com falha:** nas linhas 14–15, sumiu um trecho, do `DLPF` do MPU até o fim da ajuda, e ~30 bytes do `mantenha a placa parada` saíram duplicados.
  - Com o mesmo binário, os três boots do log anterior saíram inteiros.
  - O firmware não descarta bytes: o `Serial.write` espera quando o buffer de 4 kB enche, e o boot imprime ~1 kB.
  - **Conclusão provável:** é uma falha na captura do lado do PC, na abertura do monitor junto com o boot, e não uma regressão. Se aparecer de novo com o monitor já aberto antes do boot, investigar.

**Passo 6a, correção da revisão (20/10, antes do commit).**

- **Início da próxima janela:** no código conferido acima, a próxima janela do giroscópio abria antes das mensagens `[cal]`. O instante era lido antes do `feed()`.
  - Antes da refatoração, ela abria depois das mensagens: na inconsistência, na recusa do boot, na volta ao boot depois do `g` e no `endSix()`.
  - Com o buffer da serial cheio (`r` ou `d` em curso), o tempo de bloqueio sairia da janela, que poderia ficar abaixo das 360 amostras.
  - **Correção:** o coordenador deixa a próxima janela pendente, e o serviço a abre com `startPendingWindow(micros())` depois de imprimir.
- **Testes novos:**
  - média de duas janelas diferentes dentro do limite: bias de 0,50 e 0,52 °/s dá 0,51 °/s, e 24 e 26 °C dão 25 °C;
  - temperatura da candidata depois de uma inconsistência;
  - janela pendente até ser aberta.
- **Bancada (log `261020-183137`, carimbo `2026-10-20 18:30:38`):**
  - **Boot:** aceito na segunda janela, com bias −4,665 +2,555 +1,180 °/s a 27,1 °C e diferença entre as janelas de 0,003 °/s e 0,012°. A candidata passou pela janela pendente, e a janela seguinte veio completa.
  - **`g` balançando:** `recusado: placa em movimento` (19,8 °/s), com 400 amostras, e `mantido o bias anterior`.
  - **`c`:**
    - o mesmo bias, como `DEFINITIVO (deste boot)`;
    - `valid = 0x000D`;
    - perdidos, descartados e fila cheia em 0.
  - **`g` parado:** não foi rodado à parte. A calibração do boot passa pelo mesmo caminho do coordenador (candidata → janela pendente → aceite); só muda o tipo.
- **Passo 6a fechado.**

**Passo 6b (logs `261020-185412` e `261020-185947`, carimbo `2026-10-20 18:53:13`).**

- **Roteiro base** (log `185947`): as mensagens são as mesmas da linha de base.
  - `r`, 11 janelas: 199–200 Hz, com perdidas, descartadas, falhas e fila cheia em 0, e o magnetômetro com 25 leituras/s.
  - `a`, 82 linhas:
    - dt de 5,00–5,01/5,01/5,01 ms, de novo sem a variação de ±50 µs;
    - atraso de 108–117 µs;
    - custo do filtro médio de 41–44 µs e máximo de 120–136 µs. A primeira linha, com 1 amostra, deu 81/81 µs;
    - ambíguos, descartados e reinícios em 0.
  - `b`: 60 linhas a 10 Hz. `p`: 71 linhas, todas no estado `ok`.
  - Zero com 1,17 Pa no boot e 0,28 Pa no `0`. `c` com `valid = 0x000D`.
- **`6`, entrada e uma posição:**
  - Log `185947`: a posição 1 foi aceita (`[cal] ok: +0.0806 +0.0329 +1.1068 g`) e veio o pedido da posição 2. O `x` deu `seis posições canceladas`, sem recomeçar o giroscópio, porque o bias deste boot já existia. O `c` seguinte mostrou o acelerômetro sem mudança.
  - Log `185412`, uma tentativa anterior com a caixa deitada de lado (a bruto em y = +1,0 g): `a média (-0.014 +1.005 +0.090 g) não corresponde a "nivelada, face para cima" (outro eixo): confira e repita`, e a mesma posição pedida de novo.
- **`z` completo:**
  - duas medidas recusadas por movimento na posição 2, cada uma seguida do mesmo pedido;
  - o resultado: módulo em −2,66° / +1,94° e mesa em −0,18° / +0,88°;
  - `s` com `calibração de montagem gravada na NVS`.
  - Os números e a conferência estão no [hardware.md](hardware.md), "`z` refeito na montagem final".
- **Persistência:** no boot seguinte, `NVS: … montagem calibrada`, e o `c` com os mesmos ângulos.
- **`x` na espera do `z`:** `calibração de montagem cancelada`.
- **Fora do passo** (log `185412`): o `DISTÚRBIO` alternou cinco vezes com o |m| em 21,8–21,9 µT, em torno do limite de baixo (25,7 µT − 15 % = 21,8 µT). O limite não tem histerese, como já registrado no passo 2.
- **Não conferido:** o `s` das seis posições. Completar o `6` obrigaria a refazer o `z`. Ficou como a pendência 6.
- **Testes da montagem com sinal** (ajuste da revisão, antes do commit): os ângulos esperados passaram a ter sinal, sem `fabsf()`. Os casos novos:
  - módulo em roll −3° e pitch +2°;
  - mesa em roll +1,5° e pitch −2°;
  - inclinada demais em roll +12°.
- **Passo 6b fechado.**

**Passo 7 (log `261020-193349`, carimbo `2026-10-20 19:32:03`).**

- **Primeiro boot do log:** aos 11 s, um `[E][Wire.cpp:499] … Error -1`. Logo depois, o BMP280 e o HMC5883L perderam a configuração ao mesmo tempo (`queda de alimentação?`) e foram reconfigurados. Em seguida houve um `POWERON_RESET`. A causa provável é o mau contato na alimentação dos módulos, como em 19/10, ou o cabo USB mexido. O caminho dessas mensagens não é deste passo. O roteiro foi feito no segundo boot.
- **Roteiro base** (`r` e `a` separados): as mensagens são as mesmas da linha de base.
  - `r`, 25 janelas: 199–200 Hz, com perdidas, descartadas, falhas e fila cheia em 0.
  - `a`, 119 linhas:
    - dt de 5,00–5,01/5,01/5,01 ms;
    - atraso de 108–121 µs;
    - custo do filtro médio de 52–59 µs e máximo de 141–184 µs;
    - ambíguos, descartados e reinícios em 0.
  - `b`, `p`, `0` e os dois `c` normais, com `pulsos do INT: … perdidos 0 e descartados 0 | fila cheia 0` e `valid = 0x000D`.
- **`r` e `a` ligados juntos** (antes e depois do INT solto, 189 linhas do `a`): o atraso máximo passou de 200 µs em 4 linhas (201, 222, 247 e 248 µs). As outras ficaram em 108–185 µs, com ambíguos em 0 e dt de 5,00–5,01 ms.
  - É a carga das duas impressões juntas. A ISR do GPIO divide o nível 1 com a da UART (pendência 5), e o limite de segurança é de 4 000 µs.
  - Os passos anteriores não registraram o `r` e o `a` ligados juntos, então não há base para comparar. **Decisão pendente:** aceitar, ou fazer um A/B com o build do passo 6b (`10e3332`) nas mesmas condições.
- **INT solto (GPIO 19), com `r` e `a`:**
  - **Ao soltar:** três janelas de mau contato (9, 34 e 47 Hz, com descartadas 21 e 33, 12 ambíguos e atraso de até 2 065 µs). Uma lacuna acima de 50 ms reiniciou a fusão (`reinícios 1`).
  - **Solto:** 9 janelas com `0 Hz | nenhuma leitura válida | falhas 0 (seguidas 0, recuperações 0) | sem pulsos do INT`. O barômetro e o magnetômetro seguiram normais (24–25 leituras/s).
  - **`c` com o fio solto:** `ATT não (filtro não inicializado)`, `HDG não (sem atitude)`, `valid = 0x0004` e `último 1 s: nenhuma leitura válida`. O guia previa `MPU sem amostra há … ms`, mas a fusão já tinha reiniciado no mau contato, e a validade dá prioridade ao filtro não inicializado. O código está certo; a previsão estava errada.
  - **Na volta:** `perdidas 2183` na janela do primeiro pulso, ~10,9 s × 200 Hz, como previsto. Depois, uma janela de encaixe (151 Hz, perdidas 51) e, a partir dela, 199–200 Hz com tudo em 0. A fusão voltou em ~0,3 s. Na volta, `reinícios` ficou em 0, porque o filtro já estava não inicializado.
  - `falhas 0` e `recuperações 0` o tempo todo. O `c` final voltou a `valid = 0x000D`.
  - **Glitch do primeiro boot:** segundo o autor, ele provavelmente tocou num fio de alimentação ao mexer nos fios.

**Passo 7, teste A/B do atraso com `r` e `a` juntos, rodada A** (logs `261020-195007` e `261020-195242`). Build do passo 6b (`10e3332`), compilado num worktree à parte (carimbo `19:48:27`), caixa parada. Os dois logs são do mesmo build: o upload da rodada B não chegou a rodar.

| Build | Linhas do `a` | ≤ 125 µs | 126–150 µs | 151–200 µs | > 200 µs | Picos |
|---|---|---|---|---|---|---|
| 6b, rodada 1 | 835 | 99,5 % | 0 | 0,4 % | 0,1 % | 574 µs |
| 6b, rodada 2 | 599 | 99,7 % | 0,2 % | 0 | 0,2 % | 390 µs |
| Passo 7 (log `193349`, fora do mau contato) | 189 | 96,8 % | 0 | 1,1 % | 2,1 % | 201–248 µs |
| Passo 7, só o `a` (log `193349`) | 119 | 100 % | 0 | 0 | 0 | 121 µs |

- **O 6b também passa de 200 µs** com o `r` e o `a` juntos, e com picos maiores (390 e 574 µs). Então os picos não são uma novidade do passo 7.
- **A cauda acima de 150 µs foi mais frequente no passo 7** (3,2 % contra 0,4 %). Mas a amostra é pequena (189 linhas), e a sessão foi diferente: o autor estava preparando o teste do INT. **Falta a rodada B** (passo 7, 60 s nas mesmas condições) para decidir.
- **Rodada B** (log `261020-200224`, build do passo 7, carimbo `20:00:44`, 817 linhas do `a` em ~82 s):
  - 96,0 % das linhas com atraso até 125 µs, 0,5 % em 126–150 µs, 0,2 % em 151–200 µs e **3,3 % (27 linhas) acima de 200 µs**;
  - picos de 1 077 a 1 949 µs em 5 segundos seguidos;
  - ambíguos, descartados e reinícios em 0; o `r` com 199–200 Hz e tudo em 0;
  - dt com ±50 µs em 14 linhas;
  - custo do filtro médio de 50–66 µs e máximo de 89–193 µs.
- **Mecanismo, comum aos dois builds:** todas as linhas acima de 150 µs, nas três rodadas, são a primeira ou a segunda linha do `a` depois de um resumo `r`.
  - O resumo é formatado no próprio laço (4 linhas do MPU, mais o barômetro e o magnetômetro, ~40 números em ponto flutuante). Isso leva perto de um período de 5 ms, e o pulso seguinte espera o fim da impressão.
  - Quando a linha do `a` cai no mesmo ciclo, ou perto dele, a espera cresce. A fase entre os dois depende do instante em que o `a` foi ligado e da deriva entre a janela de 1 s, medida pelo relógio, e a cadência do `a`, contada em amostras. Por isso os picos vêm em grupos.
  - O carimbo do pulso vem da ISR, então a espera da leitura não entra no dt da fusão. A variação de ±50 µs do dt, vista em alguns builds, é outro efeito (pendência 5). O teto de 4 000 µs não foi atingido.
- **Comparação:** no passo 7, as linhas acima de 200 µs foram mais frequentes (3,3 % contra 0,1–0,2 %). A diferença é grande para ser só acaso, mas o código extraído não tem laços nem esperas. As explicações prováveis são a fase do `a` em relação ao `r` e a posição do código, que já mudou o custo do filtro e o dt de um build para o outro.
- **dt com ±50 µs de novo, no build do 6b feito no worktree:** 14 de 835 linhas e 8 de 599 entre 4,96 e 5,05 ms. O mesmo código, compilado na pasta principal, deu 5,00–5,01 ms em todas as linhas na bancada do 6b. O custo do filtro também mudou (médio de 38–54 µs, contra 41–44 µs). O caminho de compilação muda as strings de arquivo e a posição do código. Isso reforça a hipótese da pendência 5: o efeito depende da posição do código, e não do código em si.
- **Decisão (autor, 20/10):**
  - Fechar o passo 7. O roteiro base, com o `r` e o `a` separados, passou em todos os critérios.
  - O atraso com o `r` e o `a` juntos fica como observação. O mecanismo é comum aos builds, e não houve ambíguos, descartes nem reinícios nas condições testadas (caixa parada). O efeito com a caixa em movimento não foi medido.
  - A medida de novo na semana 3 entrou na pendência 2.
  - O worktree do A/B foi removido.
- **Correção da revisão (antes do commit): pulso em 0 µs.** O `PulseTracker` usava `lastIntUs_ == 0` para dizer "sem pulso anterior". O defeito já existia no `main.cpp`, não veio da extração.
  - Como 0 µs é um instante válido na virada do `micros()`, um pulso exatamente em 0 deixaria de servir de referência, e as perdidas até o pulso seguinte não seriam contadas.
  - Agora há um booleano explícito (`havePrev_`), e um teste novo cobre esse caso.
  - É a única mudança de comportamento do passo, e só nesse caso: a chance é de cerca de 1 em 5 000 por virada (~71 min). A bancada acima não precisa ser refeita.
- **Passo 7 fechado.**

**Passo 8 (log `261020-202749`, carimbo `2026-10-20 20:26:53`).** Roteiro base e as teclas que ele não cobre, numa sessão só, com a caixa parada.

- **Mensagens** (`python tools/log_templates.py` com os logs `181722`, `185947` e `193349` como base):
  - Só uma linha aparece só no novo: uma de progresso do `m` cuja lista `faltam:` não tinha aparecido antes. O texto é o mesmo; só o conteúdo varia.
  - As 20 linhas só na base são de situações que não se repetiram: INT solto, recusas, o glitch de alimentação do passo 7, a gravação do `z` e o `DISTÚRBIO`.
  - **Alcance:** o script compara conjuntos de modelos de linha. Ele confirma o texto, mas não a ordem das linhas nem a frequência de um modelo que existe dos dois lados. A ordem foi conferida lendo, no log, a sequência de cada tecla abaixo e a do boot, que é a mesma da linha de base.
- **Teclas:**
  - `h` e `?`: a ajuda;
  - `q`: nada;
  - `g`: calibrado;
  - `6`: Enter, `[cal] ok` e `x` com `seis posições canceladas`;
  - `z`: uma medida aceita e `x` com `calibração de montagem cancelada`;
  - `m` e `x`, duas vezes: `calibração cancelada; calibração anterior mantida`.
- **`r`, 19 janelas:** 199–200 Hz, com perdidas, descartadas, falhas e fila cheia em 0, e o magnetômetro com 24–26 leituras/s.
- **`a`, 181 linhas:**
  - dt de 5,00–5,01/5,01/5,01 ms, sem a variação de ±50 µs;
  - custo do filtro médio de 42–51 µs e máximo de 93–161 µs;
  - ambíguos, descartados e reinícios em 0.
- **`b`, `p` e `0`:** 49 linhas `[baro]` e 74 `[mag]` a 10 Hz, todas no estado `ok`. Zero aceito com 0,38 Pa. Os dois `c` com `valid = 0x000D`.
- **Atraso com o `a` sozinho: 102–197 µs, dentro do critério (~200 µs), mas mais alto que nos passos 6b e 7.**
  - Neste build, 29 % das linhas ficaram até 125 µs, 23 % em 126–150 µs e 48 % acima de 150 µs.
  - Nos passos 6b e 7, nas mesmas condições, 1 de 232 linhas passou de 125 µs.
  - É o mesmo padrão do passo 3+4, que teve ~105 µs ou 153–173 µs conforme a linha, atribuído à falta no cache da flash depois das impressões, com a posição do código mudando a cada build. Foi esse padrão que deu origem ao critério de ~200 µs.
  - Neste passo, a impressão do `a` foi para o `console.cpp`. Isso muda a posição do código do laço e da impressão.
  - **A margem ficou pequena** (pico de 197 µs). Na semana 3, com a ISR do INT em IRAM (pendência 5) e a serial fora do laço (pendência 2), o atraso deve ser medido de novo. Se continuar perto de 200 µs, investigar.
- **Passo 8 fechado. Refatoração concluída:** os passos 0–8 estão feitos, e o `main.cpp` provisório tem 341 linhas (eram ~1 450 em 15/10 e 1 010 no início do passo 6a).

## Pendências (fora da refatoração)

**Situação em 21/10/2026, antes da semana 3:**

| # | Pendência | Situação |
|---|---|---|
| 1 | Teclas ignoradas no `m` | feita em 21/10 |
| 2 | Log do núcleo e formatação no laço | **feita em 21/10**: log do núcleo pelo logger; com as tasks, atraso de 104–120 µs com o `r` e o `a` juntos e de 105–119 µs depois de um `c` ([tasks.md](tasks.md), passos 1 e 2) |
| 3 | Cobertura do `m` pelo ruído | feita em 21/10 |
| 4 | `s` de uma calibração aceita do magnetômetro | modelo final (calibração real no lugar) |
| 5 | Carimbo do pulso do INT | feita em 21/10 (ISR em IRAM, nível 2); causa de 20/10 não confirmada |
| 6 | `s` das seis posições | modelo final |
| 7 | Console chamando os serviços direto | **feita em 21/10**: teclas pela fila de comandos para a `sensor_task`; `c`, `r` e `a` formatados na `serial_task` a partir de cópias ([tasks.md](tasks.md), passos 1 e 2) |
| 8 | Pulsos falsos no INT | **feita em 21/10**: filtro de intervalo mínimo, conferência do `INT_STATUS` e quarentena; teste por injeção no ambiente `aircraft_pulsetest` ([hardware.md](hardware.md), "Pulsos falsos no INT") |

Fora da lista, também em 21/10: histerese e tempo mínimo na saída do distúrbio magnético ([hardware.md](hardware.md), "Histerese do distúrbio magnético").

1. **Teclas ignoradas na calibração do magnetômetro.** Na coleta do `m` (até 180 s), o firmware ignora as teclas sem dizer nada, e o terminal parece travado. A revisão que vem depois, que só aceita `s`, `x` e `d`, também. Proposta: ao ignorar uma tecla, imprimir uma linha curta, por exemplo `[mag] calibração em andamento: x cancela`, no máximo uma vez por segundo. Fica para depois do passo 4, num commit à parte, porque muda o comportamento. Vista em 20/10, log `261020-133830`.
   - **Feita em 21/10/2026.** Avisos `[mag] calibração em andamento: x cancela` (coleta) e `[mag] revisão da calibração: s = gravar | x = sair | d = pontos em CSV` (revisão), no máximo um por `MAG_KEY_HINT_MS` (1 s), sem aviso para Enter e espaço. Bancada em [hardware.md](hardware.md), "Aviso de tecla ignorada na calibração do magnetômetro".
2. **Log de erro do núcleo parando o laço.** Com um sensor falhando e uma impressão longa em curso (o resumo `r`, o `c`), cada `[E][Wire.cpp]` pode parar o laço por ~50 ms. Isso descarta 10 amostras e reinicia a fusão a cada tentativa, quando a fase coincide. Já estava previsto para a semana 3 ([plano.md](plano.md): serial numa task de baixa prioridade e revisão do nível de log do núcleo). Agora há a medida do efeito na fusão. Decidir na migração: baixar o `CORE_DEBUG_LEVEL` ou redirecionar o log do núcleo para a fila do `lib/logger`. Vista em 20/10, log `261020-134704`.
   - **Formatação no laço de aquisição** (passo 7, A/B de 20/10): com o `r` e o `a` ligados juntos, o pulso seguinte a um resumo `r` espera a formatação dele (~40 números em ponto flutuante, perto de um período de 5 ms). O atraso pulso → leitura chegou a 1 949 µs, com 0,1–3,3 % das linhas acima de 200 µs, conforme o build e a fase. Não houve ambíguos, descartes nem reinícios nas condições testadas (caixa parada). A espera da leitura não entra no dt, que vem do carimbo da ISR, mas o dt tem a variação de ±50 µs da pendência 5 em alguns builds.
   - **Na migração, com a serial na task de baixa prioridade:** repetir ~80 s com o `r` e o `a` juntos e a caixa parada (como no log `261020-200224`). Critério: atraso máximo abaixo de ~200 µs em todas as linhas.
   - **Log do núcleo: feito em 21/10/2026, antes das tasks.** O `lib/logger` passou a ter um buffer próprio (`LOG_BUFFER_BYTES`), esvaziado sem esperar a UART, e o log do núcleo e do IDF foi redirecionado para ele (`CORE_DEBUG_LEVEL` continua em 3). Com o SDA do BMP280 solto e o `r` ligado, 73 janelas seguidas tiveram 199–200 Hz e `descartadas 0`. Detalhes em [hardware.md](hardware.md), "Logger com buffer e log do núcleo redirecionado". **Continua para as tasks** a formatação no laço: atraso de até 2 437 µs depois de um `c` e de algumas centenas de µs depois de um `r`.
3. **Cobertura da calibração completada pelo ruído.** Com a caixa parada, o centro provisório (mín./máx.) fica no meio da nuvem de pontos, e o ruído espalha as direções pelos setores. Em 20/10, a coleta chegou a 24/24 em 46 s sem girar a caixa. O resultado é recusado no fim, mas o guia `faltam:` e o fim antecipado confundem. Proposta: só contar a cobertura quando a amplitude (máx. − mín.) passar de uma fração do raio esperado em pelo menos dois eixos, por exemplo `MAG_CAL_MIN_SPAN_UT` no `config.h`. Muda o comportamento, então fica fora da refatoração. Vista no log `261020-144205`.
   - **Feita em 21/10/2026**, com uma regra diferente da proposta: em vez da amplitude do mín./máx., uma distância mínima ao centro provisório, ponto a ponto (`MAG_CAL_COVER_MIN_UT`, 10 µT). Ela filtra os pontos próximos ao centro provisório, evitando cobertura causada pelo ruído da caixa parada. O centro continua sendo o meio do mín./máx., então um pico isolado ainda pode deslocá-lo e alterar a cobertura do progresso. A recontagem do fim, que decide a aceitação, não mudou. Detalhes em [magnetometro.md](magnetometro.md) e [hardware.md](hardware.md), "Cobertura do `m` com distância mínima".
4. **`s` de uma calibração aceita do magnetômetro, sem conferência na bancada.** A gravação na NVS, o uso imediato da calibração nova, o reinício da proa e a **persistência depois de um reinício** (boot com `NVS: … magnetômetro calibrado` e o `c` com o offset e a W novos) ficaram sem conferir na refatoração. Os testes novos cobrem só o núcleo puro (`MagCalibrator`), não essa integração com a NVS e o filtro da proa. Conferir na próxima calibração real (modelo final), com os passos 8–12 do roteiro completo do passo 3+4. Registrada em 20/10/2026.
5. **Carimbo do pulso do MPU não determinístico.** O instante marcado pela ISR do INT pode atrasar ~50 µs, e isso depende do build (passo 5, teste A/B). As causas prováveis são a ISR do GPIO no mesmo nível da ISR da UART e o despachante do Arduino na flash. Na semana 3, quando a INT passar a acionar a `sensor_task`:
   - registrar a ISR pelo ESP-IDF (`gpio_install_isr_service` + `gpio_isr_handler_add`), com o handler em IRAM e `ESP_INTR_FLAG_IRAM`, num nível acima do da UART (ex.: `ESP_INTR_FLAG_LEVEL2`), sem passar pelo `attachInterrupt`;
   - conferir no `a` que o dt volta a 5,00–5,01 ms com as impressões ligadas.
   - se a variação continuar, quantificar o efeito na atitude com a caixa em movimento (giros com velocidade variável, comparando o filtro com o só-giroscópio);
   Registrada em 20/10/2026.
   - **Feita em 21/10/2026, antes das tasks** (no `loop()`): ISR pelo IDF, em IRAM e no nível 2 (`MPU_INT_INTR_LEVEL`), com o carimbo do `esp_timer_get_time()`. dt de 5,00–5,01 ms em todas as 946 linhas do `a`, inclusive com o `r` ligado. O A/B com o build do passo 5 não reproduziu a variação nem sem a correção, então a causa de 20/10 continua não confirmada. A mudança elimina a dependência do despachante GPIO na flash e permite preempção da UART de nível 1; outras fontes de latência da ISR continuam possíveis (seções críticas, que mascaram até o nível 3, e interrupções de nível mais alto). Detalhes em [hardware.md](hardware.md), "ISR do INT pelo ESP-IDF, em IRAM e no nível 2".
6. **`s` das seis posições sem conferência na bancada.** No passo 6b, o `6` foi conferido só até a primeira posição, porque gravar o acelerômetro descarta a montagem. Os testes do `SixPoseFlow` cobrem só o núcleo puro, não a integração com a NVS e a fusão. Na próxima calibração em seis posições (modelo final), conferir:
   - a gravação do acelerômetro: `[cal] calibração do acelerômetro gravada na NVS`;
   - o descarte da montagem: `[mont] montagem descartada: …` e o `c` com `montagem: NÃO calibrada`;
   - o reinício da fusão: ele não tem mensagem própria e não conta em `reinícios` do `a`, e o filtro volta em ~100 ms (20 amostras). Com o `a` ligado durante o `s`, a linha `[att] aguardando referência` pode aparecer ou não. O critério é a atitude parada no `a` e no `c`, logo depois, já coerente com a calibração nova, sem convergir devagar a partir do valor antigo;
   - a persistência depois do reboot: o `c` com o offset e a escala novos e a linha `NVS: … montagem sem calibração`;
   - em seguida, refazer o `z`.
   Registrada em 20/10/2026.
7. **Console chamando os serviços direto.** Hoje o `Console` roda no `loop()`, junto com a aquisição, e chama os serviços e o `FusionCore` direto: calibrações, zero, fluxos e o reinício da fusão. Ele também lê o estado deles para o `c`, o `r` e o `a`. Na semana 3, com o console na task da serial e a aquisição na `sensor_task`, isso vira acesso entre tasks sem proteção. Na migração:
   - os comandos passam por uma fila para a `sensor_task`, que é a camada única de comandos (item 8 da revisão de 15/10, que também recebe os `CMD_*` do B);
   - o `c`, o `r` e o `a` leem uma cópia do estado publicada pela `sensor_task`, com fila ou mutex, em vez das referências do `ConsoleDeps`.
   Registrada em 20/10/2026.
8. **Pulsos falsos no INT.** Com o fio do INT meio encaixado, a entrada captou 600 a 3 750 bordas por segundo, por ~40 s (log `261021-122153`). Leituras feitas fora do "data ready" deram picos de ~450 °/s com a caixa parada. A maioria caiu como ambígua, e a atitude não mudou. Ainda assim, uma leitura assim passa pela validade temporal (`checkFrame`) se nenhum pulso falso chegar durante ela. Proposta: tratar como falso um pulso que chega muito antes do período (por exemplo, a menos da metade de 5 ms do anterior), sem leitura e com contador próprio no `r` e no `c`. Muda o comportamento: num commit à parte, depois das pendências 1 e 3. Registrada em 21/10/2026.
