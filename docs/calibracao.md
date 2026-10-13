# Calibração do MPU6050 — estratégia

Decisões de calibração do ESP A. A evidência de bancada que as motiva está em [hardware.md](hardware.md).

## Acelerômetro: seis posições, persistente

- **Por quê.** Em repouso, |a| = 1,103 g, com uma única posição medida. Não dá para saber se o erro é de offset, de escala ou se difere por eixo.
- **Como.** Cada eixo é apontado para +1 g e −1 g, com a média de ~2 s por posição. Para cada eixo:
  - offset = (leitura₊ + leitura₋) / 2
  - escala = (leitura₊ − leitura₋) / 2 / 1 g
- **Correção aplicada:** a_corrigido = (a_bruto − offset) / escala.
- **Persistência.** Os coeficientes ficam na NVS (biblioteca `Preferences`) e são carregados no boot. A calibração se refaz só sob comando, porque o acelerômetro não deriva de forma relevante com o tempo.
- **Validação.** Depois de calibrar, |a| fica entre 0,98 g e 1,02 g em várias posições paradas.
- **Procedimento no firmware (comando `6`).** As posições seguem uma ordem fixa: face para cima, face para baixo, nariz para cima, nariz para baixo, asa direita para baixo e asa esquerda para baixo. Para cada uma, o firmware mede 2 s e recusa a medida se a caixa se mexeu (mesmo critério de repouso do giroscópio) ou se o eixo esperado não for o dominante, ou estiver fora de 0,8–1,2 g. No fim, mostra os coeficientes e o |a| corrigido de cada posição. Se a validação (|a| entre 0,98 e 1,02 g em todas as posições) falhar, o resultado é descartado e não pode ser gravado. Posições inclinadas ~20° passam pelos outros critérios, mas dão |a| ≈ 1,064 g. Se a validação passar, a gravação na NVS só acontece depois da confirmação (`s`). Cada calibração é gravada como um registro único (versão, faixa do acelerômetro, coeficientes e temperatura) numa só chave, o que torna a gravação atômica. Uma calibração feita em outra faixa é ignorada. Ao carregar, os coeficientes também são conferidos: valores não finitos, escala fora de 0,5–1,5, offset acima de 0,5 g (`CAL_STORE_ACC_OFFSET_MAX_G`) ou bias do giroscópio acima de 20 °/s (`CAL_STORE_GYRO_BIAS_MAX_DPS`, o limite do datasheet) indicam registro corrompido, que é ignorado com aviso na serial. Gravar uma nova calibração do acelerômetro descarta a calibração de montagem, que foi medida com a anterior e precisa ser refeita com `z` ([fusao.md](fusao.md)).

## Giroscópio: no boot, aceito só em repouso

O bias medido é grande (até 4,9 °/s), mas estável, e muda com a temperatura. Por isso é estimado a cada boot. Uma calibração feita com a placa em movimento, porém, grava um bias errado e faz o horizonte derivar.

```
boot → carrega calibração do acelerômetro (NVS)
     → coleta janela de 2 s (400 amostras a 200 Hz)
     → placa parada?
         sim → bias = média; liga ST_GYRO_CAL; salva bias e temperatura na NVS
         não → repete a janela automaticamente
               enquanto isso usa o último bias salvo (ST_GYRO_CAL desligado = "provisória")
```

- **Critério de repouso, em duas etapas.** Os limites estão em unidades físicas no `config.h`, porque o tamanho do LSB depende da faixa configurada.
  1. *Filtro de movimento, em cada janela de 2 s:* o desvio padrão de cada eixo do giroscópio fica abaixo de **0,15 °/s** (`CAL_REST_GYRO_MAX_STD_DPS`), e |a| corrigido fica entre 0,95 g e 1,05 g.
  2. *Confirmação, entre duas janelas seguidas:* as médias do giroscópio diferem menos de **0,03 °/s** em cada eixo (`CAL_BIAS_MAX_DELTA_DPS`), e a direção da média do acelerômetro muda menos de **0,1°** (`CAL_BIAS_MAX_TILT_DEG`). O bias aceito é a média das duas janelas. Se diferirem, a janela mais recente vira a nova candidata, e o processo segue. O bias definitivo leva, no mínimo, ~4 s.
  - *Por que duas etapas (10–11/10/2026).* O limite original, ~50 LSB (≈ 0,4 °/s), foi definido antes do DLPF, que reduziu o ruído em repouso para ~0,035 °/s. Reduzir só o limite de desvio não resolveu: com 0,1 °/s, uma janela com desvio de 0,065 °/s, de uma caixa se acomodando, foi aceita com erro de bias de ~0,08 °/s. Já janelas com 0,08–0,09 °/s, por vibração da mesa durante a digitação, deram o bias correto. A vibração aumenta o desvio sem deslocar a média, e a acomodação lenta desloca a média com desvio baixo. Por isso o critério principal passou a ser a estabilidade da média entre janelas, e o desvio ficou só como filtro de movimento. Com desvio de 0,15 °/s e 400 amostras, a diferença entre duas médias varia ~0,011 °/s só por ruído, e o limite de 0,03 °/s fica perto de 3 vezes isso.
  - *Por que a direção da gravidade (13/10/2026, revisão).* Uma rotação **constante** passa pelos critérios do giroscópio: desvio baixo e média estável. A taxa de rotação seria aceita como bias. Num eixo horizontal, isso vai direto para o horizonte: o erro em regime do filtro é bias × τ, e 10 °/s dariam ~10°. Uma rotação ω em torno de um eixo não vertical inclina a gravidade de ω × 2 s entre os centros das janelas. Com o limite de 0,1°, rotações a partir de ~0,05 °/s são recusadas. A média de 400 amostras com ~2,5 mg de ruído varia ~0,01° de direção, ou um pouco mais, porque o DLPF correlaciona amostras vizinhas. A vibração da mesa espalha as amostras sem deslocar a média. A acomodação da caixa também inclina a gravidade, e esse critério reforça a comparação das médias. O ângulo aparece na linha `giroscópio calibrado`. Na bancada, em 8 confirmações em repouso, ele ficou entre 0,003° e 0,027°, e nenhuma foi recusada por esse critério. O limite de 0,1° tem uma margem de ~4 vezes sobre o maior valor visto ([hardware.md](hardware.md), 13/10/2026).
  - *Limitação.* Uma rotação constante em torno do eixo vertical não muda a gravidade, e a IMU sozinha não a distingue de bias (teste `test_bias_vertical_rotation_not_detectable`). Com a caixa nivelada, um bias em gz quase não afeta roll e pitch: ele só pesa com a caixa inclinada, na proporção do seno do ângulo. Só uma referência externa, como o magnetômetro, cobre esse caso.
  - Antes da calibração do acelerômetro, |a| em repouso é ~1,10 g, e a faixa de 0,95–1,05 g recusaria o giroscópio no primeiro boot. Nesse caso vale uma faixa larga, de **0,7 a 1,5 g** (`CAL_REST_ACC_RAW_MIN_G` e `CAL_REST_ACC_RAW_MAX_G`). Até 13/10/2026 a faixa era aberta (0 a 10⁹), e um acelerômetro zerado passava, sem direção da gravidade para comparar entre as janelas. Além disso, o ângulo entre as janelas é indefinido (NaN) quando um dos vetores é nulo, e o bias não é confirmado.
  - **Dados não finitos.** Uma janela com média ou desvio não finitos em qualquer eixo, do giroscópio ou do acelerômetro, é recusada como "leitura inválida". O desvio máximo usa `fmaxf`, que descarta o NaN, e por isso a verificação é feita eixo a eixo (revisão de 13/10/2026).
  - **Quadros ambíguos.** Quadros que podem ter sido lidos depois de um pulso mais novo ficam fora da calibração, e não só da fusão ([fusao.md](fusao.md)): o ciclo seguinte leria o mesmo quadro, e ele entraria duas vezes na janela.
- **Transitório de partida.** Logo depois de configurar o MPU, o giroscópio tem um transitório (pico de ~60 °/s, repetido em todo boot). A calibração e a fusão descartam os primeiros 200 ms (`CAL_MPU_SETTLE_MS`), no boot e depois de cada recuperação do driver (em `read()` ou em `checkAlive()`), e a janela de 2 s só começa depois do descarte.
- **Sem bloqueio.** O sistema nunca fica parado esperando a calibração. Com o bias provisório o horizonte funciona, e a estação de solo mostra o estado pela flag.
- **Temperatura.** O sistema guarda a temperatura do chip no momento da calibração e avisa se ela variar mais de **1 °C** (`CAL_TEMP_WARN_C`), o que pede uma nova calibração (`g`). O limite era 10 °C e foi reduzido em 11/10/2026: na bancada, o bias de gx variou ~−0,1 °/s por °C (de −4,79 a −4,88 °/s entre 25,7 e 26,6 °C), e 10 °C equivaleria a ~1 °/s de erro.
- **Deriva térmica do bias: decidida na etapa da fusão (11/10/2026).** O aviso e o comando `g` cobrem a bancada, mas não corrigem a deriva. Havia duas soluções candidatas:
  - recalibração automática enquanto a placa estiver parada, usando o mesmo detector de repouso; isso corrige a temperatura e a acomodação;
  - compensação por um modelo linear do bias em função da temperatura, o que exige medir o bias numa faixa bem maior (por exemplo, de 20 a 35 °C, ligando a placa fria).

  **Decisão:** as duas ficam adiadas, e continuam o aviso e o `g`. No filtro complementar, o acelerômetro limita o erro de roll e pitch a ~b·τ (~0,1° por °C com τ = 1 s, perto do nível). A decisão está condicionada à medição de deriva na bancada (diagnóstico só de giroscópio do comando `a`). O magnetômetro poderá limitar a deriva da proa, mas isso depende da validação dele. Detalhes em [fusao.md](fusao.md).
- **Recalibração.** O comando `CMD_CALIBRATE_GYRO` (B → A) roda o mesmo fluxo; durante a janela, `ST_CALIBRATING` fica ligado.
- **Implementação.** A correção do bias é subtraída no software, sem gravar nos registradores de offset do MPU. Assim é mais simples de testar (`pio test -e native`) e de explicar.

## Diagnóstico no firmware principal

O ambiente `i2cscan` é um firmware separado. O firmware de voo tem o comando `c` pela serial, implementado em 10/10/2026, que mostra sem regravar a placa:

- leituras brutas e corrigidas;
- coeficientes em uso (offset e escala do acelerômetro, bias do giroscópio);
- estado da calibração: definitiva ou provisória, temperatura no momento da calibração e temperatura atual.

Outros comandos: `6` (seis posições), `g` (recalibrar o giroscópio, como o futuro `CMD_CALIBRATE_GYRO`) e `r` (liga ou desliga o resumo por segundo do driver). Os comandos da fusão, `z` (calibração de montagem) e `a` (fluxo de atitude), estão em [fusao.md](fusao.md). A lógica está em `lib/imu_cal` (pura, com testes `native`), e a gravação na NVS em `lib/imu_store`.

## Ligação com o contrato (`telemetry.h`)

| Item | Uso |
|---|---|
| `ST_CALIBRATING` | janela de calibração em andamento |
| `ST_GYRO_CAL` | bias do giroscópio aceito nesta sessão |
| `CMD_CALIBRATE_GYRO` | recalibração sob comando |

O contrato já cobre o fluxo e não precisa mudar. A calibração em seis posições é feita pela serial na bancada e não passa pelo rádio.
