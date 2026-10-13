# Magnetômetro: proa com compensação de inclinação — PLANO

> **Status: plano em revisão (12/10/2026), nenhum código escrito.** Responda às decisões da seção "Decisões para aprovar", aqui ou num `.txt` em `docs/`. Depois da aprovação, a implementação segue este documento, e ele passa a descrever a estratégia adotada, como o [barometro.md](barometro.md).

Último item da etapa 2 do [plano.md](plano.md): "magnetômetro com compensação de inclinação (proa)". O GY-273 (HMC5883L legítimo, ID "H43") já foi validado no diagnóstico: autoteste OK nos três eixos, campo local de ~23,7 µT e ruído de ~1 LSB (ver [hardware.md](hardware.md), "HMC5883L no diagnóstico").

## Para retomar

Pendências da etapa anterior (BME280):

1. **Opcional:** conferir na placa o ajuste do driver do BME280 (`failStreak_` só zera com amostra válida e a configuração incompleta é refeita).
   - Boot com `zero aceito`.
   - `r` com **só o SDA** do BME280 solto por ~10 s: `FALHANDO` com as falhas subindo ~1 por segundo e o MPU sem descartes.
   - Ao recolocar o SDA: `ok` sem reconfiguração.
2. Revisar este plano e responder às decisões.

**Montagem física (feita em 13/10/2026, ver [hardware.md](hardware.md), "Religação dos três módulos"):**
- O GY-273 e o BME280 estão numa protoboard **dentro da caixa do MPU**, então o magnetômetro inclina junto com o MPU, como a compensação de inclinação exige. O MPU não saiu do lugar, e a calibração de montagem (`z`) foi refeita e conferida depois da religação.
- As ligações são feitas com dois jumpers emendados, sem solda. Uma emenda soltou ao manusear a caixa. Antes da calibração do magnetômetro, que exige girar a caixa em todas as orientações, as emendas e os fios precisam estar presos.
- Diagnóstico com o módulo na caixa: autoteste OK, ruído de ±0,1 µT e \|B\| de ~26,9 µT sem calibração (23,7 µT com o módulo solto, em 12/10).
- A registrar antes da bancada: a orientação do GY-273 na caixa (seta X em relação ao nariz, chip para cima) e a distância até o ESP32 e o cabo USB. O ideal é 10 cm ou mais: o ESP consome de 100 a 300 mA, com picos do rádio, o que dá ~1 µT variável a 5 cm. O mapeamento de eixos (D3) é levantado na bancada de qualquer forma, mas a orientação deve ser a pretendida no modelo final.

**A calibração feita no protótipo é descartável:** ela tem que ser refeita no modelo final, com tudo instalado. O objetivo agora é validar o código e o procedimento.

## Escopo

- Ler o HMC5883L no firmware de voo, em unidades físicas (µT).
- Mapear os eixos do chip para os eixos do modelo.
- Calibrar hard iron e soft iron, com o resultado gravado na NVS.
- Calcular a **proa magnética** compensada pela inclinação, usando o roll e o pitch da fusão.
- Mostrar tudo na serial do `main.cpp` provisório.

O contrato já tem `heading` (0..360, "proa magnética compensada"), `ST_MAG_OK`, `ST_MAG_CAL` e `CMD_CALIBRATE_MAG`, então **o `telemetry.h` não muda**.

**Fora do escopo:** rádio, tasks do RTOS, OLED, o comando vindo do solo e a declinação magnética (proa verdadeira).

## Cadeia de processamento proposta

```
HMC5883L (eixos do chip, LSB) → µT (ganho configurado)
  → eixos do modelo (tabela levantada na bancada)
  → calibração: m = W · (bruto − b)      [hard iron b, soft iron W]
  → compensação de inclinação com roll e pitch da fusão
  → proa magnética 0..360°               [opcional: filtro com o giroscópio, D6]
```

Convenções de [fusao.md](fusao.md):
- eixos do modelo: x para o nariz, y para a asa direita, z para baixo;
- roll positivo com a asa direita descendo, pitch positivo com o nariz subindo;
- proa (yaw) positiva com o nariz virando para a direita, vista de cima: 0° no norte magnético, 90° no leste.

Compensação de inclinação, com φ = roll, θ = pitch e m = (mx, my, mz) nos eixos do modelo:

```
Xh = mx·cosθ + my·sinφ·sinθ + mz·cosφ·sinθ
Yh = my·cosφ − mz·sinφ
proa = atan2(−Yh, Xh), levada para 0..360°
```

Conferência: nariz para o norte e nivelado dá m = (H, 0, V) e proa 0°; nariz para o leste dá m = (0, −H, V) e proa 90°.

## Decisões para aprovar

**D1. Driver próprio em `lib/hmc5883l`, sem a biblioteca Adafruit (recomendo).**
- Mesmo padrão do MPU e do BME280:
  - acesso direto aos registradores;
  - contagem de falhas e `ok()` só com amostra válida;
  - intervalo de 1 s entre tentativas enquanto falha, para não atrasar o MPU;
  - reconfiguração quando o chip volta.
- **Conferência da configuração em toda leitura, numa rajada de 9 bytes a partir de 0x00:** CRA, CRB e Mode, seguidos dos 6 bytes de dados. Depois de uma queda de alimentação, o HMC volta com a configuração padrão (modo de medição única), e a leitura detecta isso.
- A Adafruit HMC5883 sai do `lib_deps`. Isso também elimina os avisos de compilação que vêm dela.

**D2. Configuração do chip.**

| Parâmetro | Recomendação | Alternativa |
|---|---|---|
| Ganho | **±1,3 Ga (1090 LSB/Ga)**, já validado no diagnóstico: ~1 LSB de ruído, margem para o campo da Terra (~24 µT) mais distorções de até ~100 µT | ±0,88 Ga (1370 LSB/Ga): resolução um pouco melhor, mas satura com distorções acima de ~60 µT, e o powerbank no modelo final pode chegar perto disso |
| Modo | **Contínuo** (o diagnóstico mostrou o risco de ler dado antigo no modo de medição única) | — |
| Taxa de saída e média | **30 Hz com média de 8**, confirmando na bancada que a taxa real é 30 Hz | 75 Hz com média de 1, mais ruído |
| Leitura pelo firmware | **25 Hz, logo depois de uma leitura do MPU**, como o BME280 | — |

- O PFD não precisa de proa a mais de ~25 Hz, e a telemetria (50 Hz) repete o último valor.
- Saturação (−4096 em algum eixo): amostra descartada e contada.

**D3. Mapeamento de eixos levantado na bancada.** Com a caixa nivelada, sem tirar o GY-273 dela:
1. **Nariz para o norte magnético** (bússola do celular, longe de metal). O eixo do chip com o maior valor positivo em módulo, no plano, é o x do modelo.
2. **Asa direita para o norte.** O eixo do chip com o maior valor positivo é o y do modelo.
3. **O z do modelo sai pela regra da mão direita** (x × y = z, para baixo). Para confirmar, a componente vertical do campo tem que ter o sinal esperado para a inclinação magnética local, que no Sudeste do Brasil é negativa (o campo aponta para cima). Confiro o valor esperado para a sua região antes do teste.
4. O resultado vira uma tabela em [hardware.md](hardware.md), como a do MPU, e o mapeamento entra no `config.h` (ou numa função fixa, como o `chipToBody` do MPU).

**D4. Modelo de calibração.**

| Opção | O que corrige | Custo |
|---|---|---|
| a) Só hard iron (centro mín./máx. de cada eixo) | Offset | Muito simples, mas ignora as diferenças de escala |
| b) Mín./máx. por eixo | Offset + escala por eixo | Simples. Depende de atingir os extremos exatos de cada eixo, o que é difícil girando à mão |
| **c) Ajuste de elipsoide alinhado aos eixos, por mínimos quadrados (recomendo)** | Offset + escala por eixo, usando **todos** os pontos | Sistema linear 6×6, lógica pura, testes `native` com dados sintéticos. Tolera cobertura incompleta melhor que a opção b |
| d) Elipsoide completo (9 parâmetros) | Também os acoplamentos entre eixos | Exige autovalores 3×3. Ganho pequeno num modelo de bancada; fica para depois, se o resíduo de (c) não ficar bom |

**D5. Procedimento de calibração (comando `m`), seguindo o padrão das seis posições.**
1. `m` inicia a coleta por `MAG_CAL_MS` (~60 s). O operador gira a caixa devagar **em todas as orientações**: voltas completas com o nariz para cima, de lado, de cabeça para baixo e assim por diante.
2. **Cobertura:** a esfera de direções é dividida em setores. A coleta só é aceita com pontos em todos os setores, ou em uma fração mínima deles (valor a definir). O firmware mostra o progresso, para o operador saber para onde ainda falta girar.
3. **Ajuste e conferência:**
   - raio do campo dentro de uma faixa larga (10 a 60 µT);
   - resíduo RMS de |m| corrigido abaixo de um limite (proposta inicial: 3 % do raio);
   - escalas dentro de 0,7 a 1,3.
4. **Revisão:** o firmware mostra offset, escalas, raio e resíduo. `s` grava na NVS (namespace `mag_cal`, com versão e ganho), e `x` descarta, mantendo a calibração anterior.
5. **Sem calibração na NVS:** a proa é calculada com offset zero e escala 1, mas `ST_MAG_CAL` fica desligado e a serial avisa.

**D6. Filtro da proa.**
- **a) Sem filtro:** proa calculada a cada leitura do magnetômetro. É ruidosa, e qualquer metal perto altera a proa na hora.
- **b) Filtro complementar com o giroscópio (recomendo):**
  - predição pela taxa de guinada no referencial da Terra: ψ̇ = (q·sinφ + r·cosφ)/cosθ;
  - correção lenta pela proa do magnetômetro, com τ = `MAG_HEADING_TAU_S` (proposta: 1 s);
  - diferença sempre pelo caminho curto, então 359° → 1° não passa por 180°.

  É a mesma estrutura do filtro de roll e pitch, que já está validada, e dá uma proa suave no PFD.
- Nos dois casos:
  - **inclinação alta:** com |pitch| acima de 70° (faixa validada da fusão), a correção pelo magnetômetro é suspensa, porque a proa fica mal definida perto de ±90°;
  - **distúrbio magnético:** se |m| corrigido se afastar do raio da calibração mais que `MAG_DISTURB_PCT` (proposta: 15 %), por exemplo com uma ferramenta perto, a correção é suspensa e a serial sinaliza;
  - **sem giroscópio** (opção a): a proa congela.

**D7. Comandos novos no `main.cpp` provisório.**

| Tecla | Ação |
|---|---|
| `m` | Calibração do magnetômetro (D5) |
| `p` | Fluxo de proa a 10 Hz: proa compensada, proa "deitada" sem compensação (para comparar), \|m\|, inclinação magnética medida, roll, pitch e o estado (distúrbio, inclinação alta) |

O `c` e o `r` ganham uma linha `mag`, como a `baro`.

**D8. Lógica pura (`lib/mag`, sem `Arduino.h`) e testes `native`.**
- Ajuste do elipsoide com dados sintéticos: offset, escalas e ruído conhecidos, cobertura parcial, recuperação dos parâmetros e rejeição de dados ruins.
- Teste de cobertura.
- Proa compensada: campo sintético em várias proas, com roll e pitch de até ±45°, e conferência da proa e do caminho curto em 0/360°.
- Filtro da proa, se escolhido: degrau de proa, giro com o giroscópio, distúrbio e inclinação alta.

## Limitações conhecidas (a registrar)

- **Desalinhamento entre o GY-273 e a caixa.** A rotação de montagem (`z`) corrige só o MPU. Um desalinhamento de poucos graus do GY-273 vira um erro de proa de poucos graus que aumenta com a inclinação. A soft iron (D4) não corrige rotação. Aceitável num modelo de bancada; fica documentado.
- **Distorções que se movem em relação à caixa:** cabo USB, mão, mesa de aço, objetos próximos. A calibração não as corrige.
- **Referência de proa na bancada:** a bússola do celular tem erro de alguns graus e também sofre com metal. Os testes medem principalmente **giros relativos**, por exemplo de 90° alinhando com a borda da mesa, e repetibilidade.
- **Proa magnética**, sem declinação. No Sudeste do Brasil, a declinação é de cerca de −20°. Isso vai documentado, não corrigido.

## Valores propostos para o `config.h` (seção MAG, a ajustar na bancada)

| Constante | Valor inicial | Uso |
|---|---|---|
| `MAG_RANGE_GA_X10` (ou equivalente) | 13 (±1,3 Ga) | Ganho (D2) |
| `MAG_OUTPUT_HZ` / `MAG_AVG_SAMPLES` | 30 / 8 | Configuração do chip (D2) |
| `MAG_READ_HZ` | 25 | Leitura pelo firmware |
| `MAG_MAX_FAIL_STREAK` / `MAG_FAIL_RETRY_MS` | 5 / 1000 | Falhas, como no BME280 |
| `MAG_CAL_MS` | 60000 | Coleta da calibração |
| `MAG_CAL_MIN_COVERAGE_PCT` | a definir | Setores da esfera cobertos |
| `MAG_CAL_MAX_RESID_PCT` | 3 | Resíduo máximo do ajuste |
| `MAG_DISTURB_PCT` | 15 | Desvio de \|m\| que suspende a correção |
| `MAG_HEADING_TAU_S` | 1,0 | Filtro da proa (D6b) |
| `MAG_MAX_PITCH_DEG` | 70 | Acima disso, sem correção pelo magnetômetro |

## Roteiro de validação na bancada (rascunho)

Pré-requisito: GY-273 preso na caixa do MPU, boot parado, giroscópio DEFINITIVO e montagem (`z`) calibrada.

1. **Driver:**
   - taxa real de saída (30 Hz) e ruído parado (~0,1 µT);
   - `r` por alguns minutos com o MPU sem perdas nem descartes;
   - SDA do GY-273 solto (com o chip alimentado): `FALHANDO` a 1 tentativa por segundo e volta sem reconfiguração;
   - **não fazer queda de alimentação no hardware** (ver o latch-up do BME280 em hardware.md).
2. **Mapeamento de eixos (D3):** levantar a tabela e conferir os sinais.
3. **Calibração (D5):**
   - duas calibrações seguidas: offsets e escalas devem se repetir dentro de poucos µT e poucos %;
   - depois da calibração, |m| quase constante em qualquer orientação (resíduo).
4. **Proa nivelada:**
   - 8 proas a cada 45° (marcas na mesa ou borda da mesa);
   - erro de cada giro relativo de 90°, voltas nos dois sentidos e repetibilidade;
   - comparação absoluta com a bússola do celular, só como referência grosseira.
5. **Compensação de inclinação:** proa fixa, roll e pitch de ±15 a ±30°. A proa compensada deve variar poucos graus, contra dezenas na proa "deitada".
6. **Distúrbio:** aproximar uma chave de fenda. O firmware sinaliza o distúrbio, a proa não salta (com o filtro D6b) e se recupera ao afastar.
7. **Filtro (se D6b):** giros rápidos de 90°, resposta imediata pelo giroscópio, sem ultrapassar o valor final e sem deriva parado.
8. **Convivência:** atitude e altitude sem degradação com os três sensores no barramento.

## Arquivos previstos

| Arquivo | Conteúdo |
|---|---|
| `lib/hmc5883l/` (novo) | Driver |
| `lib/mag/` (novo) | Lógica pura: ajuste, cobertura, proa e filtro |
| `lib/mag_store/` (novo) | Calibração na NVS (namespace `mag_cal`) |
| `test/test_mag/` (novo) | Testes `native` |
| `src/main.cpp` | Integração, comandos `m` e `p`, linhas `mag` no `c` e no `r` |
| `include/config.h` | Seção MAG |
| `platformio.ini` | Remove a Adafruit HMC5883 |
| `docs/magnetometro.md`, `docs/hardware.md`, `README.md`, `CLAUDE.md` | Estratégia, resultados, comandos e referência |
