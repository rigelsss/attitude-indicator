# Magnetômetro: proa com compensação de inclinação

> **Status: estratégia aprovada em 13/10/2026 (decisões D1–D8), implementação não iniciada.** Este documento descreve o que será implementado. Os resultados da bancada entram aqui e no [hardware.md](hardware.md) conforme os testes forem feitos.

Último item da etapa 2 do [plano.md](plano.md): "magnetômetro com compensação de inclinação (proa)". O GY-273 (HMC5883L legítimo, ID "H43") já foi validado no diagnóstico: autoteste OK nos três eixos e ruído de ~1 LSB (ver [hardware.md](hardware.md), "HMC5883L no diagnóstico").

## Para retomar

1. **Bancada, antes do código:** levantar o mapeamento de eixos com o comando `h` do `i2cscan` (ver "Mapeamento de eixos"). Assim o `chipToBody` já entra certo no código e no teste.
2. **Implementação:** driver, `lib/mag`, `lib/mag_store`, testes `native`, integração no `main.cpp` provisório e o script `tools/mag_fit.py` (ver "Arquivos").
3. **Bancada:** seguir o "Roteiro de validação".
4. **Depois:** refatoração do `main.cpp`, como abertura da semana 3 (ver [plano.md](plano.md)).

**Pendência opcional da etapa anterior (BME280):** conferir na placa o ajuste do driver (`failStreak_` só zera com amostra válida, e a configuração incompleta é refeita).
- Boot com `zero aceito`.
- `r` com **só o SDA** do BME280 solto por ~10 s: `FALHANDO`, com as falhas subindo ~1 por segundo e o MPU sem descartes.
- Ao recolocar o SDA: `ok`, sem reconfiguração.

## Montagem física

Registrada em 13/10/2026. Ver também [hardware.md](hardware.md), "Religação dos três módulos".

- O GY-273 e o BME280 estão numa protoboard **dentro da caixa do MPU**. O magnetômetro inclina junto com o MPU, como a compensação de inclinação exige.
- **Orientação:** a seta X do GY-273 aponta na mesma direção da seta X do MPU, e os dois chips estão para cima. O mapeamento esperado é, portanto, o mesmo do MPU (ver "Mapeamento de eixos").
- **Distância** até o ESP32 e o cabo USB: **mais de 10 cm**. É o ideal: a 5 cm, o consumo do ESP (100 a 300 mA, com picos do rádio) daria ~1 µT variável.
- **Fios:** as ligações são dois jumpers emendados, sem solda. Elas estão presas o máximo possível, mas sem espaguete térmico, porque a bancada é desmontada todo dia. Os sensores estão bem presos. Por isso, **em toda sessão, antes da calibração**, deve-se rodar o `r` girando a caixa por ~30 s, com os mesmos movimentos da calibração. As falhas do mag e os descartes do MPU têm que continuar em zero.
- Diagnóstico com o módulo na caixa: autoteste OK, ruído de ±0,1 µT e \|B\| de ~26,9 µT sem calibração. Com o módulo solto (12/10), foram 23,7 µT, provavelmente com metal por perto.

**A calibração feita no protótipo é descartável:** ela tem que ser refeita no modelo final, com tudo instalado. O objetivo agora é validar o código e o procedimento.

## Campo de referência (bancada em João Pessoa, PB)

Modelo WMM2025, em 7,1° S e 34,9° O, para 13/10/2026:

| Grandeza | Valor |
|---|---|
| Intensidade total \|B\| | 26,2 µT |
| Componente horizontal H | 22,2 µT |
| Inclinação magnética | −31,8° (o campo aponta para cima) |
| Componente vertical V | −13,8 µT (eixo para baixo) |
| Declinação | −20,7° (o norte magnético fica ~21° a oeste do verdadeiro) |

Esses valores servem de alvo no mapeamento de eixos, na conferência do raio da calibração e na inclinação medida pelo fluxo `p`. Eles não entram em nenhum cálculo do firmware.

## Escopo

- Ler o HMC5883L no firmware de voo, em unidades físicas (µT).
- Mapear os eixos do chip para os eixos do modelo.
- Calibrar hard iron e soft iron, com o resultado gravado na NVS.
- Calcular a **proa magnética** compensada pela inclinação, usando o roll e o pitch da fusão.
- Filtrar a proa com o giroscópio.
- Mostrar tudo na serial do `main.cpp` provisório.

O contrato já tem `heading` (0..360, "proa magnética compensada"), `ST_MAG_OK`, `ST_MAG_CAL` e `CMD_CALIBRATE_MAG`, então **o `telemetry.h` não muda**.

**Fora do escopo:** rádio, tasks do RTOS, OLED, o comando vindo do solo, a declinação magnética (proa verdadeira) e a recalibração automática em voo.

## Cadeia de processamento

```
HMC5883L: rajada de 9 bytes a partir de 0x00 (CRA, CRB, Mode, X, Z, Y)
  → decodificação: contagens, saturação, conferência da configuração   [lib/mag]
  → µT (1090 LSB/Ga; 1 Ga = 100 µT)
  → eixos do modelo (chipToBody, levantado na bancada)
  → calibração: m = W · (bruto − b)         [hard iron b, soft iron W]
  → compensação de inclinação com roll e pitch da fusão
  → proa do magnetômetro 0..360°
  → filtro complementar com o giroscópio → proa
```

Convenções de [fusao.md](fusao.md):
- eixos do modelo: x para o nariz, y para a asa direita, z para baixo;
- roll positivo com a asa direita descendo, pitch positivo com o nariz subindo;
- proa (yaw) positiva com o nariz virando para a direita, vista de cima: 0° no norte magnético, 90° no leste.

Compensação de inclinação, com φ = roll, θ = pitch e m = (mx, my, mz) nos eixos do modelo:

```
Xh = mx·cosθ + my·sinφ·sinθ + mz·cosφ·sinθ
Yh = my·cosφ − mz·sinφ
proa_mag = atan2(−Yh, Xh), levada para 0..360°
```

Conferência: nariz para o norte e nivelado dá m = (H, 0, V) e proa 0°; nariz para o leste dá m = (0, −H, V) e proa 90°.

## Decisões adotadas

### D1. Driver próprio em `lib/hmc5883l`

- Mesmo padrão do MPU e do BME280:
  - acesso direto aos registradores;
  - contagem de falhas e `ok()` só com amostra válida;
  - uma tentativa por segundo enquanto falha, para não atrasar o MPU;
  - reconfiguração quando o chip volta.
- **Cada leitura é uma rajada de 9 bytes a partir de 0x00:** CRA, CRB e Mode, seguidos dos 6 bytes de dados. Depois de uma queda de alimentação, o HMC volta com a configuração padrão (modo de medição única). A leitura detecta isso e reconfigura o chip sem gastar uma transação a mais.
- A decodificação da rajada é uma função pura do `lib/mag` (D8). O driver só lê os bytes.
- A Adafruit HMC5883 Unified e a Adafruit Unified Sensor saem do `lib_deps` do aircraft. Nenhum código as usa, e com isso somem os avisos de compilação que vinham delas.

### D2. Configuração do chip

| Parâmetro | Valor | Motivo |
|---|---|---|
| Ganho | ±1,3 Ga (1090 LSB/Ga, ~0,092 µT/LSB) | Validado no diagnóstico com ~1 LSB de ruído. Faixa de ~130 µT por eixo, com margem para o hard iron do modelo final. ±0,88 Ga daria resolução que o ruído não deixa aproveitar, e saturaria perto de 88 µT |
| Modo | Contínuo | O modo de medição única arrisca ler dado antigo (visto no diagnóstico) |
| Taxa de saída e média | 30 Hz, média de 8 | A média no chip reduz o ruído sem custo de CPU nem I2C. A taxa real é conferida na bancada |
| Leitura pelo firmware | 25 Hz, logo depois de uma leitura do MPU | Como o BME280. A rajada leva ~0,25 ms a 400 kHz e nunca atrasa o MPU |

- O PFD não precisa de proa a mais de ~25 Hz. A telemetria (50 Hz) repete o último valor.
- **Saturação** (−4096 em algum eixo): a amostra é descartada e contada.

### D3. Mapeamento de eixos levantado na bancada

O mapeamento é medido, e não deduzido. Um eixo trocado ou com o sinal invertido não aparece com a caixa nivelada, só quando ela inclina.

**O que se espera:** o MPU tem X para o nariz, Y para a asa esquerda e Z para cima (`chipToBody`: x = X, y = −Y, z = −Z, ver [hardware.md](hardware.md)). Como o GY-273 está paralelo ao MPU e com o chip para cima, espera-se o mesmo mapeamento.

**Procedimento:** com o `i2cscan`, comando `h` (X, Y e Z do chip em µT), a caixa nivelada e longe de metal, e o norte magnético dado pela bússola do celular. Só importam os sinais e o eixo dominante, então um erro de 10 a 20° na bússola não atrapalha.

| Passo | Posição | Esperado nos eixos do modelo | Esperado no chip (previsão) |
|---|---|---|---|
| 1 | Nariz para o norte | x ≈ +22, y ≈ 0, z ≈ −14 µT | X ≈ +22, Z ≈ +14 |
| 2 | Asa direita para o norte | y ≈ +22, x ≈ 0, z ≈ −14 µT | Y ≈ −22, Z ≈ +14 |
| 3 | z = x × y (para baixo) | z negativo nas duas posições, porque o campo aponta para cima | Z do chip positivo |

Antes da calibração, os valores podem se desviar alguns µT por causa do hard iron, mas os sinais e o eixo dominante não mudam.

**Resultado:** a tabela medida vai para o [hardware.md](hardware.md), e o mapeamento é fixado numa função `chipToBody` do `lib/mag`, como no MPU, e não numa tabela no `config.h`, porque é uma propriedade da montagem física. O teste do `chipToBody` é escrito depois da bancada, com a tabela medida.

### D4. Calibração por ajuste de elipsoide alinhado aos eixos

- **Modelo:** `A·x² + B·y² + C·z² + D·x + E·y + F·z = 1`, ajustado por mínimos quadrados com **todos** os pontos coletados.
- **Resultado:** o centro (offset b, o hard iron) e as três escalas (a diagonal de W, o soft iron).
- **Conta:** equações normais 6×6, resolvidas por eliminação de Gauss, uma vez ao fim da coleta. Antes do ajuste, os pontos são centralizados e normalizados para o sistema não ficar mal condicionado.
- **Por que não mín./máx.:** essa opção usa só 6 pontos e depende de alinhar cada eixo exatamente com o campo, o que é difícil girando à mão. Um pico de ruído também viraria o extremo.
- **Evolução prevista:** elipsoide completo, com 9 parâmetros e acoplamento entre os eixos. Entra se o resíduo ficar alto no modelo final, por exemplo com o powerbank. O script `tools/mag_fit.py` (D8) compara os dois ajustes com dados reais e serve de critério. A NVS já grava **W como matriz 3×3** (diagonal por enquanto), então a evolução não muda o formato.

### D5. Procedimento de calibração (comando `m`)

**Coleta:**
- `m` inicia a coleta, que termina sozinha ao completar os 24 setores, ou depois de `MAG_CAL_MS` (90 s).
- Os pontos (até ~2 250, a 25 Hz) ficam guardados como contagem bruta `int16` num buffer de ~13 KB, alocado só durante a coleta. O buffer é necessário porque o resíduo precisa de uma segunda passada sobre os pontos.
- Durante a coleta, a proa e o fluxo `p` ficam suspensos. O MPU continua normal. `x` cancela.

**Como girar a caixa:** no ar, passando por todas as orientações, e não apoiada na mesa. Para cada face da caixa voltada para cima, faça um giro em torno da vertical, devagar (~10 s por giro):

1. topo para cima (caixa normal);
2. fundo para cima (de cabeça para baixo);
3. asa direita para cima;
4. asa esquerda para cima;
5. nariz para cima;
6. nariz para baixo.

Não é preciso dar volta inteira contínua. Girar 180° para um lado e voltar cobre os mesmos setores sem torcer os jumpers. Deixe folga no chicote e segure a caixa junto dos fios. O ESP32 e o USB ficam parados na mesa.

**Cobertura, com 24 setores:**
- A direção de cada ponto é tomada em relação a um centro provisório, o meio do mín./máx. corrente de cada eixo.
- O setor é a face do cubo do eixo dominante (±x, ±y, ±z), dividida em 4 quadrantes pelos sinais dos outros dois eixos.
- Um setor só conta como coberto com pelo menos `MAG_CAL_MIN_PTS_SECTOR` (5) pontos.
- A coleta é aceita com pelo menos `MAG_CAL_MIN_SECTORS` (20) setores. Ao fim da coleta, a cobertura é recontada com o centro ajustado.
- **Progresso a cada 2 s:** por exemplo, `cobertura 17/24  faltam: -z(4) +y(2) -x(1)`. As faces são do campo nos eixos do modelo. Faltar −z, por exemplo, significa virar a caixa de cabeça para baixo.

**Ajuste e conferência.** O resultado é recusado, com o motivo na serial, se:
- o raio ficar fora de 10 a 60 µT (faixa larga, para o modelo final). A serial mostra também a comparação com os 26,2 µT do WMM, só como informação;
- o resíduo RMS de \|m\| corrigido passar de 3 % do raio;
- alguma escala, normalizada para média 1, sair de 0,7 a 1,3;
- o sistema 6×6 for singular, ou algum coeficiente quadrático não for positivo (a forma ajustada não é um elipsoide);
- a cobertura for insuficiente.

**Revisão:**
- O firmware mostra o offset (µT), as escalas, o raio, o resíduo e a cobertura.
- `s` grava na NVS: namespace `mag_cal`, com versão, ganho, offset b e matriz W.
- `x` descarta e mantém a calibração anterior.
- `d` imprime os pontos brutos em CSV para o `tools/mag_fit.py`. A revisão continua aberta depois disso.

**Sem calibração na NVS:** offset zero e W identidade. `ST_MAG_CAL` fica desligado, e há aviso no boot e no `c`. A proa é calculada assim mesmo.

### D6. Filtro complementar da proa com o giroscópio

- **Predição a 200 Hz,** junto com a fusão, pela taxa de guinada no referencial da Terra: ψ̇ = (q·sinφ + r·cosφ)/cosθ. Usar só o r estaria errado com a caixa inclinada.
- **Correção a 25 Hz** em direção à proa do magnetômetro, com τ = `MAG_HEADING_TAU_S` (1 s, a ajustar no teste de distúrbio). A diferença é sempre pelo caminho curto: de 359° para 1° são +2°.
- **Inicialização:** com a primeira proa válida do magnetômetro, sem esperar convergir.
- **Suspensão da correção,** com o giroscópio mantendo a proa sozinho:
  - |pitch| > `MAG_MAX_PITCH_DEG` (70°, faixa validada da fusão), porque a proa fica mal definida perto de ±90°;
  - \|m\| corrigido afastado do raio da calibração por mais de `MAG_DISTURB_PCT` (15 %), o distúrbio. A serial sinaliza.
- **Deriva sem correção:** com o bias do giroscópio calibrado no boot (incerteza de ~0,006 °/s), um minuto inteiro deriva menos de 1°.
- Se o filtro não se comportar bem na bancada, a abordagem é revista.

### D7. Comandos no `main.cpp` provisório

| Tecla | Quando | Ação |
|---|---|---|
| `m` | Modo normal | Calibração do magnetômetro (D5). Recusada se outra calibração estiver em andamento ou se o HMC estiver falhando |
| `x` | Coleta | Cancela |
| `s` / `x` / `d` | Revisão | Grava / descarta / imprime os pontos em CSV |
| `p` | Modo normal | Liga ou desliga o fluxo de proa a 10 Hz |

**Colunas do `p`:**
- proa filtrada;
- proa do magnetômetro (compensada, sem filtro);
- proa "deitada" (sem compensação, para comparar);
- \|m\| corrigido;
- inclinação magnética medida (esperado: ~−32°);
- roll e pitch;
- estado: `ok`, `DISTURBIO`, `INCL_ALTA` ou `SEM_CAL`.

**Linhas novas no `c` e no `r`:**
- `c` ganha uma linha `mag`: estado do chip, calibração (offset, escalas, raio e resíduo, ou "sem calibração") e configuração (ganho e taxa);
- `r` ganha uma linha `mag`: leituras por segundo (~25), falhas, saturações e \|m\|.

O `printHelp()` passa a listar `m` e `p`.

### D8. Lógica pura e testes

| Biblioteca | Conteúdo | Testável no PC |
|---|---|---|
| `lib/hmc5883l` | Driver: I2C, falhas, nova tentativa, reconfiguração | Não (`Wire`) |
| `lib/mag` | Decodificação da rajada (ordem X, Z, Y, big-endian, saturação, configuração), `chipToBody`, ajuste do elipsoide, setores e cobertura, aplicação da calibração, proa compensada, filtro da proa | Sim, sem `Arduino.h` |
| `lib/mag_store` | NVS, namespace `mag_cal` | Não (`Preferences`) |

**Testes em `test/test_mag` (`pio test -e native`):**
1. **Decodificação:**
   - bytes conhecidos → contagens nos eixos certos;
   - −4096 em cada eixo → saturação;
   - configuração alterada → detectada.
2. **`chipToBody`:** com a tabela do D3, escrito depois da bancada.
3. **Ajuste:**
   - offset (+15, −8, +30 µT) e escalas (1,1 / 0,9 / 1,05) conhecidos, com ruído de 0,1 µT, são recuperados;
   - funciona com 20 de 24 setores;
   - recusa plano único, raio fora da faixa, salto de distúrbio, escala fora da faixa e forma não elipsoidal.
4. **Setores:** direções conhecidas caem no setor certo; o mínimo de pontos por setor é respeitado.
5. **Proa compensada:** campo de João Pessoa (H = 22,2 µT, inclinação de −31,8°), proas de 0 a 345° de 15 em 15, com roll e pitch de até ±45°. A proa sai igual em todas as combinações e passa corretamente por 0/360°.
6. **Filtro:**
   - degrau de proa converge com τ;
   - giro só pelo giroscópio acompanha;
   - caminho curto de 359° para 1°;
   - distúrbio e pitch > 70° suspendem a correção;
   - inicialização com a primeira proa válida.

**Conferência fora do firmware:** `firmware/aircraft/tools/mag_fit.py` (usa só `numpy`) lê o CSV da tecla `d`, salvo em `firmware/aircraft/logs`, e ajusta elipsoides de 6 e de 9 parâmetros. O script mostra duas coisas:
- se o ajuste do firmware está certo com dados reais (se as duas contas de 6 parâmetros baterem);
- se o elipsoide completo reduz muito o resíduo, o que é o critério para a evolução do D4.

## Valores para o `config.h` (seção MAG, a ajustar na bancada)

| Constante | Valor inicial | Uso |
|---|---|---|
| `MAG_RANGE_GA_X10` (ou equivalente) | 13 (±1,3 Ga) | Ganho (D2) |
| `MAG_OUTPUT_HZ` / `MAG_AVG_SAMPLES` | 30 / 8 | Configuração do chip (D2) |
| `MAG_READ_HZ` | 25 | Leitura pelo firmware |
| `MAG_MAX_FAIL_STREAK` / `MAG_FAIL_RETRY_MS` | 5 / 1000 | Falhas, como no BME280 |
| `MAG_CAL_MS` | 90000 | Duração máxima da coleta |
| `MAG_CAL_MIN_SECTORS` | 20 (de 24) | Cobertura mínima |
| `MAG_CAL_MIN_PTS_SECTOR` | 5 | Pontos para um setor contar |
| `MAG_CAL_MIN_RADIUS_UT` / `MAG_CAL_MAX_RADIUS_UT` | 10 / 60 | Faixa do raio ajustado |
| `MAG_CAL_MAX_RESID_PCT` | 3 | Resíduo máximo do ajuste |
| `MAG_CAL_MIN_SCALE` / `MAG_CAL_MAX_SCALE` | 0,7 / 1,3 | Faixa das escalas |
| `MAG_DISTURB_PCT` | 15 | Desvio de \|m\| que suspende a correção |
| `MAG_HEADING_TAU_S` | 1,0 | Filtro da proa (D6) |
| `MAG_MAX_PITCH_DEG` | 70 | Acima disso, sem correção pelo magnetômetro |
| `MAG_PRINT_HZ` | 10 | Fluxo `p` |

## Limitações conhecidas

- **Desalinhamento entre o GY-273 e a caixa.** A rotação de montagem (`z`) corrige só o MPU. Um desalinhamento de poucos graus do GY-273 vira um erro de proa de poucos graus, que aumenta com a inclinação. A soft iron não corrige rotação. É aceitável num modelo de bancada.
- **Distorções que se movem em relação à caixa:** cabo USB, mão, mesa de aço, objetos próximos. A calibração não as corrige. O filtro (D6) só atenua as passageiras.
- **Referência de proa na bancada:** a bússola do celular tem erro de alguns graus e também sofre com metal. Os testes medem principalmente **giros relativos**, por exemplo de 90° alinhando com a borda da mesa, e a repetibilidade.
- **Proa magnética, sem declinação.** Em João Pessoa, a declinação é de −20,7°: o norte verdadeiro fica 20,7° no sentido horário a partir do norte magnético. Isso fica documentado, não corrigido.

## Roteiro de validação na bancada

Pré-requisitos: GY-273 preso na caixa do MPU, boot parado, giroscópio DEFINITIVO e montagem (`z`) calibrada. Antes de começar, o teste das emendas com o `r` (ver "Montagem física").

1. **Mapeamento de eixos (D3)**, com o `i2cscan`, antes do código: tabela no [hardware.md](hardware.md).
2. **Driver:**
   - taxa real de saída (~30 Hz) e ruído parado (~0,1 µT);
   - `r` por alguns minutos: ~25 leituras do mag por segundo, MPU sem perdas nem descartes;
   - SDA do GY-273 solto (com o chip alimentado): `FALHANDO` com uma tentativa por segundo, e volta sem reconfiguração;
   - **não fazer queda de alimentação no hardware** (ver o latch-up do BME280 no [hardware.md](hardware.md)).
3. **Calibração (D5):**

   | Teste | Como | Passa se |
   |---|---|---|
   | Coleta boa | `m`, girando como descrito no D5 | 24/24 antes dos 90 s; raio perto de 26 µT; resíduo < 3 % |
   | Repetibilidade | Duas calibrações seguidas | Offsets dentro de ~1 µT; escalas dentro de ~2 % |
   | Recusa por cobertura | `m` girando só apoiada na mesa | Recusada; o "faltam" aponta ±z |
   | Guia de progresso | `m` sem nunca virar a caixa de cabeça para baixo | O "faltam" mostra a face; ao virar, ela some |
   | Recusa por distúrbio | Chave de fenda encostada por alguns segundos no meio da coleta | Resíduo > 3 %, recusada |
   | Resultado aplicado | Depois do `s`, fluxo `p` em várias orientações | \|m\| corrigido dentro de ±3 % |
   | Conferência independente | `d` → log → `tools/mag_fit.py` | Ajuste de 6 parâmetros igual ao do firmware |
   | NVS | `s` e reinício | Mesma calibração no boot; `ST_MAG_CAL` ligado |
   | Descartar | Nova `m`, depois `x` | A calibração anterior continua valendo, inclusive depois de reiniciar |
   | Sem calibração | NVS `mag_cal` apagada | Aviso no boot e no `c`; proa calculada |

4. **Proa nivelada:**
   - 8 proas a cada 45° (marcas na mesa ou borda da mesa);
   - erro de cada giro relativo de 90°, voltas nos dois sentidos e repetibilidade;
   - comparação absoluta com a bússola do celular, só como referência grosseira;
   - inclinação medida no `p` perto de −32°.
5. **Compensação de inclinação:** proa fixa, roll e pitch de ±15 a ±30°. A proa compensada deve variar poucos graus, contra dezenas na proa "deitada".
6. **Distúrbio:** aproximar uma chave de fenda. O `p` mostra `DISTURBIO`, a proa não salta e se recupera ao afastar. Ajustar τ, se preciso.
7. **Filtro:** giros rápidos de 90°, com resposta imediata pelo giroscópio, sem ultrapassar o valor final e sem deriva parado. Pitch acima de 70° mostra `INCL_ALTA`.
8. **Convivência:** atitude e altitude sem degradação com os três sensores no barramento.

## Arquivos

| Arquivo | Conteúdo |
|---|---|
| `lib/hmc5883l/` (novo) | Driver |
| `lib/mag/` (novo) | Lógica pura: decodificação, mapeamento, ajuste, cobertura, proa e filtro |
| `lib/mag_store/` (novo) | Calibração na NVS (namespace `mag_cal`) |
| `test/test_mag/` (novo) | Testes `native` |
| `tools/mag_fit.py` (novo) | Conferência do ajuste com o CSV da tecla `d` |
| `src/main.cpp` | Integração, comandos `m` e `p`, linhas `mag` no `c` e no `r` |
| `include/config.h` | Seção MAG |
| `platformio.ini` | Remove a Adafruit HMC5883 e a Adafruit Unified Sensor |
| `docs/magnetometro.md`, `docs/hardware.md`, `README.md`, `CLAUDE.md` | Estratégia, resultados, comandos e referência |
