# Fusão de atitude: filtro complementar (roll e pitch)

Estratégia, limites e decisões da fusão do ESP A. A evidência de bancada fica em [hardware.md](hardware.md); a calibração que alimenta a fusão, em [calibracao.md](calibracao.md).

## Cadeia de processamento

```
pulso INT (instante em micros) → leitura do MPU (eixos do chip, g e °/s)
  → calibração nos eixos do chip (offset/escala do acelerômetro, bias do giroscópio)
  → eixos do modelo: x = X, y = −Y, z = −Z
  → rotação de montagem (comando z)
  → filtro complementar → roll, pitch
```

- **Eixos do modelo (aeronáuticos):** x para o nariz, y para a asa direita, z para baixo. Roll é positivo com a asa direita descendo, e pitch é positivo com o nariz subindo. O mapeamento e os sinais foram confirmados na bancada (hardware.md, montagem 5).
- **Código:** a lógica pura fica em `firmware/aircraft/lib/attitude` (sem `Arduino.h`), com testes em `test/test_attitude` (`pio test -e native`). A API usa unidades físicas (g, °/s, graus, segundos); internamente, o filtro trabalha em radianos.

## Filtro

- **Referência pelo acelerômetro** (força específica f, nos eixos do modelo; em repouso nivelado, f = (0, 0, −1 g)):
  - roll = atan2(−f_y, −f_z)
  - pitch = atan2(f_x, √(f_y² + f_z²))
- **Predição pelo giroscópio**, pela cinemática de Euler:
  - φ̇ = p + (q·sinφ + r·cosφ)·tanθ
  - θ̇ = q·cosφ − r·sinφ

  Integrar p e q diretamente faria roll e pitch derivarem ao girar a caixa inclinada em torno da vertical. Esse caso é coberto pelo teste `test_yaw_with_constant_tilt`.
- **Correção:** ângulo += k·(referência − ângulo), com k = w·dt/(τ + dt) = w·(1 − α). Para o roll, a diferença é tomada pelo caminho curto, então perto de ±180° ele não passa por 0°.
- **Constante de tempo em vez de α fixo.** O parâmetro é `COMP_FILTER_TAU_S` = 1 s, e α = τ/(τ + dt) é calculado com o dt medido a cada amostra. O antigo α = 0,98 a 200 Hz equivalia a τ ≈ 0,25 s, curto demais para filtrar a aceleração da mão. Além disso, o significado de um α fixo muda com o dt. Com peso parcial, a constante de tempo efetiva passa a ser aproximadamente τ/w.
- **Inicialização:** pela **média** do acelerômetro em `FUSION_INIT_SAMPLES` (20) amostras seguidas, ou seja, 0,1 s. Todas as amostras precisam de uma referência completa: peso pleno, roll observável, giroscópio não saturado e |giroscópio| abaixo de `FUSION_INIT_MAX_RATE_DPS` (5 °/s, caixa sem girar). Uma amostra vertical pode ter |a| = 1 g sem fornecer o roll. Qualquer amostra inutilizável recomeça a contagem, e o mesmo vale para um dado inválido (NaN ou infinito) e para uma lacuna acima de `FUSION_MAX_DT_MS` entre amostras da média: a caixa pode ter se movido sem que o filtro visse, e nesse caso a amostra atual vira a primeira da nova média. Uma amostra com dt ≤ 0 no meio da média não entra nela. Só a primeira amostra é aceita com qualquer dt (depois de um reset, ele é 0 ou indefinido). Até lá, `initialized()` é falso e nada vai para a atitude.
  - *Por que a média (11/10/2026).* Na primeira versão, a inicialização usava uma única amostra. Na bancada, depois de uma recuperação, o filtro começou 0,3° fora e levou ~3 s (≈ 3τ) para convergir. Uma amostra carrega todo o ruído do acelerômetro (~0,1° com 2 mg); a média de 20 reduz isso para ~0,03°. A condição do giroscópio evita inicializar com a caixa ainda em movimento, como ao recolocar um fio.

## Quando confiar menos no acelerômetro

| Parâmetro (`config.h`) | Valor | Efeito |
|---|---|---|
| `FUSION_ACC_FULL_DEV_G` | 0,05 g | \|\|a\| − 1 g\| até este valor: peso pleno |
| `FUSION_ACC_ZERO_DEV_G` | 0,15 g | a partir deste valor: peso zero (linear entre os dois) |
| `FUSION_ROLL_MIN_G` | 0,3 g | projeção √(f_y² + f_z²) mínima para corrigir o roll (≈ \|pitch\| 72,5°) |
| `FUSION_MAX_DT_MS` | 50 ms | intervalo acima disto: o filtro é reiniciado e espera nova referência |
| `FUSION_INIT_SAMPLES` | 20 | amostras seguidas cuja média define a atitude inicial (0,1 s) |
| `FUSION_INIT_MAX_RATE_DPS` | 5 °/s | inicialização só com a caixa sem girar |

- **Saturação (o driver separa os dois sensores):**
  - Acelerômetro saturado: peso zero nessa amostra.
  - Giroscópio saturado: a predição continua com a taxa limitada, que é a melhor disponível, mas o filtro fica *degradado* até a correção acumulada desde a última saturação (∑ w·dt) chegar a 3τ. Só contam as amostras com roll observável: perto da vertical o peso pode ser pleno, mas só o pitch é corrigido (revisão de 13/10/2026: o filtro saía do estado degradado em ~3 s com o roll ainda sem referência). Zerar o peso do acelerômetro não resolveria isso: a correção é justamente o que recupera o erro.
- **Limitação: aceleração horizontal.** O peso baseado em |a| não rejeita aceleração perpendicular à gravidade. Por exemplo, 0,1 g para a frente dá uma inclinação aparente de 5,7°, mas |a| = 1,005 g, e o peso continua pleno. Em 0,5 s, com τ = 1 s, entram ~2,2° desse erro (teste `test_horizontal_acceleration`). Nesta etapa, a solução simples fica assim. O erro ao deslizar a caixa na bancada é um resultado experimental, que depende da intensidade e da duração do movimento.
- **Erro por bias residual b:** em regime, perto do nível e com peso pleno, o erro é b·τ. Com 0,03 °/s (bias limpo), dá 0,03°. Com a deriva térmica de ~0,1 °/s por °C, dá ~0,1° por °C. Com peso baixo, o erro cresce; com peso zero, volta a acumular como no giroscópio puro.
- **Faixa validada: |pitch| ≤ 70°.** tanθ é limitado ao valor de 85° para evitar a explosão numérica, mas a singularidade de Euler em 90° continua existindo. Acima de ~72,5°, a correção de roll é suspensa. Fora da faixa, o comportamento é limitado (valores finitos e pitch em ±90°), mas não validado.

## Tempo: dt medido

- **dt** é a diferença entre os instantes do INT, gravados na ISR, das amostras aceitas pela fusão. A subtração é em `uint32_t` (`elapsedUs`) e atravessa a virada do `micros()`.
- **Ordem no laço:** a fila de pulsos é esvaziada num buffer, o MPU é lido, e só então as janelas do resumo são contadas. A contagem pode imprimir, e uma leitura feita depois da impressão seria de um pulso posterior ao último visto.
- **Associação pulso → quadro:** sem FIFO, o MPU guarda só a última amostra. Logo depois da leitura, o laço olha a fila. Se um pulso mais novo que o último visto chegou antes do fim da leitura, o quadro pode ser dele: esse ciclo fica fora da fusão ("ambíguos"), e o dt seguinte cobre a lacuna. Há também uma rede de segurança: um atraso do pulso até a leitura acima de `FUSION_MAX_INT_LAG_US` (4 ms, abaixo do período de 5 ms) também tira o ciclo da fusão. O atraso normal é de ~100 µs.
  - *Primeira versão (11/10/2026).* O critério era só o atraso, com limite de 1 ms. Na bancada, cada linha do fluxo `a` gerava 1 ambíguo e 1 descartado (dt máximo de 15 ms). A impressão bloqueava ~10 ms, e o último pulso chegava 2–4 ms antes da leitura. O quadro era desse pulso, mas era recusado. O critério passou a ser o pulso mais novo durante a leitura.
- **Serial sem bloqueio:** a serial tem um buffer de transmissão de 1 kB (`SERIAL_TX_BUFFER_BYTES`). Sem ele, cada impressão longa bloqueava o laço até sobrarem só os 128 bytes da FIFO da UART, e isso descartava amostras (3 por segundo com o resumo `r`).
- **Amostras descartadas:** um dt de 10 ms representa corretamente o intervalo entre as leituras aceitas, mas a velocidade angular intermediária se perdeu. Integrar a última taxa é uma aproximação.
- **Reconfiguração do MPU** (boot, ou recuperação em `read()` ou `checkAlive()`): a fila é limpa, a fusão é reiniciada sem referência de tempo e as amostras são descartadas por 200 ms (`CAL_MPU_SETTLE_MS`), como na calibração.
- **Mudanças de referência:** a fusão é reiniciada quando a calibração do acelerômetro ou a rotação de montagem mudam. O bias do giroscópio, provisório ou definitivo, entra sem reinício: o sistema nunca espera pela calibração.

## Calibração de montagem (comando `z`)

São duas operações distintas:

- **Calibração de montagem (`z`, agora):** identifica o desalinhamento do MPU em relação à caixa e o grava. Na montagem 5, são ~2,4° em roll.
- **Zero operacional (`CMD_ZERO_ATTITUDE`, etapa dos comandos):** faz a posição atual virar roll = pitch = 0. Ainda não foi implementado. O procedimento de duas posições do `z` não serve para um comando de rádio de um passo só.

**Procedimento:**
1. Pré-requisitos: acelerômetro calibrado (`6`) e bias do giroscópio definitivo.
2. Medida 1: caixa em pé, na posição normal (fundo apoiado na mesa, sem virar), nariz encostado numa referência (borda da mesa ou esquadro).
3. Medida 2: caixa ainda em pé, girada 180° no plano da mesa (sem virar), com a cauda encostada na mesma referência. Com a caixa de cabeça para baixo, as medidas dão roll de ~180° e o limite de 10° descarta a calibração. Em 13/10/2026, o texto anterior ("apoiada na face de baixo") foi lido como "caixa virada".
4. Cada medida dura 2 s, com o critério de repouso da calibração do giroscópio **mais** a média do giroscópio corrigido abaixo de 0,2 °/s em cada eixo (`LEVEL_REST_GYRO_MAX_MEAN_DPS`). Só o desvio baixo e médias estáveis aceitariam uma rotação lenta e constante.
5. O resultado mostra o desalinhamento do módulo e a inclinação da mesa: `s` grava, `x` descarta.

**Cálculo:**
- Nos eixos da caixa, as duas medidas são (gx, gy, gz) e (−gx, −gy, gz). A soma dos vetores unitários é paralela ao eixo z da caixa, **qualquer que seja a inclinação da mesa** (teste com mesa a 10°).
- O nariz é o nominal (1, 0, 0) projetado no plano perpendicular a esse eixo (Gram-Schmidt), o que preserva a direção nominal do nariz.
- O desalinhamento em yaw não é observável pela gravidade.
- A rotação é aplicada ao acelerômetro e ao giroscópio. Um giro diferente de 180° por δψ erra cerca de inclinação da mesa × δψ/2 (0,7° e 5° dão ~0,03°).
- Medidas a mais de 120° entre si são recusadas. Montagem ou mesa acima de 10° (`LEVEL_MAX_TILT_DEG`) também.

**Persistência:** `lib/level_store`, namespace `att_level`, chave `mntRec`. É um registro versionado e atômico (matriz, ângulos de montagem, temperatura e a calibração do acelerômetro com que foi medida). Uma matriz que não seja rotação é ignorada ao carregar. Sem calibração, vale a identidade (eixos nominais).

**Dependência da calibração do acelerômetro (13/10/2026):** a montagem é medida com o acelerômetro já corrigido, então só vale com a mesma calibração. Gravar uma nova calibração de seis posições descarta a montagem em uso, e no boot uma montagem medida com outra calibração (ou sem calibração, ou com a calibração ausente agora) é ignorada. Nos dois casos o firmware pede um novo `z`. A ordem é sempre `6` e depois `z`. O formato 1 do registro não guardava essa calibração e é ignorado: depois desta mudança, a montagem precisa ser refeita uma vez.

## Deriva térmica do bias (pendência do calibracao.md)

**Decisão desta etapa (11/10/2026):** ficam adiados tanto o modelo linear do bias com a temperatura quanto a recalibração automática em repouso. Continuam o aviso de 1 °C e o comando `g`.

- Para roll e pitch, o acelerômetro limita o erro a ~b·τ (~0,1° por °C perto do nível, com peso pleno).
- O magnetômetro poderá limitar a deriva da proa, mas isso depende da validação dele.
- A decisão está condicionada à medição de deriva na bancada: o diagnóstico só de giroscópio do comando `a` mostra a deriva crua.

## Diagnóstico no `main.cpp` provisório

- **`a`: fluxo a 10 Hz (`ATT_PRINT_HZ`).** Duas linhas por leitura.
  - Primeira linha: roll e pitch do filtro, só do acelerômetro e só do giroscópio (integrado a partir da atitude do filtro no momento em que o `a` é ligado); |a|, peso, temperatura e o aviso DEGRADADO.
  - Segunda linha: dt mínimo, médio e máximo; atraso máximo do pulso até a leitura; custo médio e máximo do `update()` (só o filtro: o futuro `loop_us` medirá o ciclo completo); ambíguos, descartados e reinícios.
  - Com o buffer de transmissão, as linhas não deveriam mais causar descartes; o contador confirma isso na bancada.
- **`r`:** o resumo por segundo inclui a média e o desvio de roll e pitch e conta as saturações por sensor. A estatística dos ângulos (`AngleStats`) acumula cada valor como desvio em relação ao primeiro da janela, então +179,9° e −179,9° dão média de 180° e desvio de 0,1°, e não 0° e 179,9°. Isso vale enquanto a janela de 1 s variar menos de 180°.
- **`c`:** mostra o estado da montagem e da atitude.

## Roteiro de validação na bancada

Pré-requisito: boot parado e `c` mostrando o giroscópio DEFINITIVO.

1. **Sinais:** asa direita para baixo dá roll positivo; nariz para cima dá pitch positivo.
2. **Montagem (`z`):** fazer a calibração e conferir a inclinação da mesa no resultado.
3. **Ângulos conhecidos (inclinômetro e calços de altura medida, θ = asin(h/L)):** roll em 0, ±15, ±30 e ±45°; pitch em 0, ±15 e ±30°. Ler a média no `r`. São duas medidas separadas:
   - concordância entre o filtro e o acelerômetro em repouso;
   - exatidão em relação ao gabarito. A meta de ≤ 1° depende da referência.
4. **Deriva parada por 5 min (log em arquivo):** registrar o pico a pico, a tendência, a temperatura e a deriva só do giroscópio. A meta inicial é < 0,2° pico a pico, sem ser uma garantia.
5. **Resposta a giros:** inclinar ~45° e voltar. O giroscópio deve acompanhar o movimento prontamente. Depois de uma perturbação, a correção de 95 % do erro leva ~3τ ≈ 3 s: é outra medida.
6. **Aceleração linear:** deslizar a caixa para a frente e para trás. Registrar o erro do filtro, o erro só do acelerômetro e o peso. É um resultado experimental (ver a limitação acima).
7. **Recuperação (soltar o VCC):** conferir o descarte e o retorno correto. Sem amostras, o fluxo `a` fica em silêncio; as falhas aparecem no resumo `r`. A atitude pode mudar legitimamente depois de um período sem sensor.
8. **Temporização:** registrar a distribuição do dt, os descartes, o atraso e o custo do filtro, sem assumir um máximo de 10 ms.

Sem um suporte que mantenha a inclinação enquanto a caixa gira em torno da vertical, o teste de yaw inclinado fica só no native. Girar em torno do z da própria caixa inclinada muda fisicamente o roll e o pitch.
