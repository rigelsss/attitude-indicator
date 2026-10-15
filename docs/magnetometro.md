# Magnetômetro: proa com compensação de inclinação

> **Status (15/10/2026): implementado; calibração com elipsoide completo (D4, revisto em 15/10), calibração, proa nivelada e compensação de inclinação validadas em 15/10; filtro (distúrbio e giros rápidos) também em 15/10; falta o item 8 do roteiro, que depende do BMP280.** Estratégia aprovada em 13/10 (D1–D8). O código compila (`aircraft` e `i2cscan`) e passa nos testes `native` (28 casos do magnetômetro, 99 no total). O mapeamento de eixos do GY-273 foi confirmado na bancada. Os resultados da bancada entram aqui e no [hardware.md](hardware.md).

Último item da etapa 2 do [plano.md](plano.md): "magnetômetro com compensação de inclinação (proa)". O GY-273 (HMC5883L legítimo, ID "H43") já foi validado no diagnóstico: autoteste OK nos três eixos e ruído de ~1 LSB (ver [hardware.md](hardware.md), "HMC5883L no diagnóstico").

## Para retomar

1. **Mapeamento de eixos:** feito em 13/10 (x = X, y = −Y, z = −Z), e mantido na montagem final.
2. **Implementação:** feita em 14/10 (ver "Arquivos").
3. **`chipToBody` do MPU atualizado em 14/10** para a montagem final (x = Y, y = X, z = −Z). Sinais conferidos, seis posições e `z` refeitos e calço com erro ≤ 0,5°, também em 14/10 ([hardware.md](hardware.md)). **A bancada do magnetômetro está liberada.**
4. **Bancada:** seguir o "Roteiro de validação". A bancada perto do notebook tem gradiente de campo (~5 µT em 10 cm), então a calibração é feita na cadeira giratória com o apoio de 23 cm, a ~1,5 m do notebook, com uma extensão USB. Lá, em 15/10, a coleta cobriu 24/24 setores, mas o ajuste alinhado aos eixos deixou 4,6 % de resíduo. O D4 passou para o elipsoide completo, que dá 0,63 % com os mesmos pontos. Com o firmware novo, a calibração foi **aceita e gravada** (resíduo de 0,77 %, repetível em 0,2 µT) e o \|m\| corrigido ficou dentro de ±2 % em todas as orientações. Itens 4 e 5 do roteiro feitos em 15/10, na cadeira: giros de 90° com erro máximo de 2,8° e compensação de inclinação com 1,4 a 4,6° de erro a ±30° (contra 12 a 23° sem compensação). Itens 6 e 7 também em 15/10: com a chave de fenda, a proa filtrada não saltou e voltou em ~5 s, mas acompanhou ~5,6° de 8° enquanto o desvio ficou abaixo dos 15 % do limite (ajuste de `MAG_DISTURB_PCT` ou τ a decidir); nos giros rápidos, acompanhou a `mag` a até 2,1°, sem ultrapassar e sem deriva. **Falta:** o item 8 (convivência), que depende do BMP280. Ver [hardware.md](hardware.md), "Calibração do magnetômetro: tentativas de 15/10/2026". O BME280 foi danificado em 14/10, e o BMP280 chega em 17/10. O firmware funciona sem barômetro, então o roteiro do magnetômetro não depende dele.
5. **Depois:** refatoração do `main.cpp`, adiada em 15/10/2026 para outro momento, mas ainda pré-requisito das tasks (ver [plano.md](plano.md), "Refatorar antes das tasks").

## Montagem física

**Montagem final (14/10/2026):** caixa de papelão de 28 × 13 × 10 cm, com o nariz ao longo do comprimento. Detalhes, curtos e testes em [hardware.md](hardware.md), "Montagem final na caixa".

- MPU, GY-273 e BME280 numa mini protoboard no canto da caixa. O magnetômetro inclina junto com o MPU, como a compensação de inclinação exige.
- **Orientação do GY-273:** seta X para o nariz, chip para cima (mapeamento x = X, y = −Y, z = −Z). O MPU foi girado 90° (X asa direita, Y nariz), o que não afeta o magnetômetro.
- **Distâncias:** ESP32 a ~15 cm e powerbank a ~13 cm do GY-273, os dois presos. Medido: powerbank ligado ≤ 1,6 µT mesmo encostado; ESP32 girado 1,5 µT; cabo USB 3,2 µT.
- **Fiação:** jumpers macho-macho sem emendas, presos na caixa. **Nada pode se mover em relação ao sensor**: nos testes do protótipo, mexer na fiação improvisada mudou o campo em até ~23 µT (ver [hardware.md](hardware.md), "Magnetômetro: mapeamento de eixos e fontes de distorção").
- **Durante as medidas:** controle remoto, celular, multímetro e ferramentas a mais de 30 cm.

A calibração do magnetômetro vale para esta montagem. Se algo mudar de lugar dentro da caixa (powerbank, ESP32, fios), ela tem que ser refeita.

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

> **Protótipo anterior (13/10/2026).** Este teste foi feito na montagem do protótipo, com o GY-273 paralelo ao MPU. Na montagem final (14/10), o GY-273 manteve a mesma orientação (X nariz, chip para cima), e o mapeamento do magnetômetro continua valendo. O **MPU foi girado 90°** (X asa direita, Y nariz), então o paralelismo descrito abaixo não vale mais. Ver [hardware.md](hardware.md), "Orientação de montagem do MPU6050".

**O que se esperava (protótipo):** o MPU tinha X para o nariz, Y para a asa esquerda e Z para cima (`chipToBody`: x = X, y = −Y, z = −Z, montagem 5). Como o GY-273 estava paralelo ao MPU e com o chip para cima, esperava-se o mesmo mapeamento.

**Procedimento:** com o `i2cscan`, comando `h` (X, Y e Z do chip em µT e ângulo atan2(Y, X)), a caixa nivelada e longe de metal. Em vez de apontar para o norte, a caixa parte da borda da mesa, com o nariz em **85° verdadeiros** (bússola do celular ajustada para o norte verdadeiro), o que dá **~106° magnéticos** (85° + 20,7° de declinação). A caixa gira 90° por vez no sentido horário (nariz para a direita), alinhada com as bordas. Só importam os sinais e o eixo dominante, então um erro de 10 a 20° na proa não atrapalha.

Com a proa magnética ψ, o campo nos eixos do modelo é x = H·cosψ, y = −H·sinψ, z = V (H = 22,2 µT e V = −13,8 µT):

| Posição | Proa do nariz (verdadeira / magnética) | Esperado no modelo (x, y, z) | Esperado no chip (X, Y, Z), previsão | Ângulo do `h` |
|---|---|---|---|---|
| P1 | 85° / 106° | −6, −21, −14 | −6, +21, +14 | ~106° |
| P2 | 175° / 196° | −21, +6, −14 | −21, −6, +14 | ~196° |
| P3 | 265° / 286° | +6, +21, −14 | +6, −21, +14 | ~286° |
| P4 | 355° / 16° | +21, −6, −14 | +21, +6, +14 | ~16° |
| P5 | 85° / 106°, de cabeça para baixo (roll de 180°, virando o conjunto todo) | −6, +21, +14 | −6, −21, −14 | — |

- **Com o mapeamento previsto, o ângulo atan2(Y, X) do diagnóstico coincide com a proa magnética** e cresce no giro horário. Ângulos decrescentes indicariam um eixo invertido.
- As quatro proas dão os dois sinais de X e de Y. O meio entre posições opostas (P1 e P3, P2 e P4) é uma primeira estimativa do hard iron no plano.
- **O z:** sai de z = x × y. Nas posições P1 a P4, o Z do chip tem que ficar positivo (~+14 µT) e quase constante, coerente com o campo apontando para cima. **P5 confirma o z diretamente:** só o Y e o Z do chip trocam de sinal, e o X fica igual ao de P1. O meio entre P1 e P5 estima o hard iron em Z, separado do V.
- Antes da calibração, os valores podem se desviar alguns µT por causa do hard iron, mas os sinais e o eixo dominante não mudam.

**Resultado (13/10/2026, log `device-monitor-261013-185902.log`):** sinais e eixo dominante conforme a previsão nas cinco posições, com o Z invertendo em P5. **Mapeamento: x = X, y = −Y, z = −Z**, fixado em `magChipToBody` (`lib/mag`), e não numa tabela no `config.h`, porque é uma propriedade da montagem física. O teste do mapeamento usa os valores de P1 e P2. Tabela e anomalias (giros irregulares) no [hardware.md](hardware.md), "Magnetômetro: mapeamento de eixos e fontes de distorção".

### D4. Calibração por ajuste de elipsoide completo (revisto em 15/10/2026)

- **Modelo:** `x·Q·x + L·x = 1`, com Q simétrica: 9 parâmetros (x², y², z², xy, xz, yz, x, y, z), ajustados por mínimos quadrados com **todos** os pontos coletados.
- **Resultado:**
  - o centro b = −½·Q⁻¹·L (offset, o hard iron);
  - **W**, a raiz simétrica da forma do elipsoide (o soft iron, com os acoplamentos entre eixos): com os semieixos rₖ e as direções vₖ (autovetores de Q), W = Σ (R/rₖ)·vₖ·vₖᵀ, e R = média geométrica dos semieixos.
- **Conta:** equações normais 9×9, resolvidas por eliminação de Gauss, e autovalores 3×3 por Jacobi, uma vez ao fim da coleta. Antes do ajuste, os pontos são centralizados e normalizados para o sistema não ficar mal condicionado.
- **Por que não mín./máx.:** essa opção usa só 6 pontos e depende de alinhar cada eixo exatamente com o campo, o que é difícil girando à mão. Um pico de ruído também viraria o extremo.
- **Por que o elipsoide completo:** até 15/10, o firmware usava o elipsoide alinhado aos eixos (6 parâmetros, W diagonal), com o completo previsto como evolução se o resíduo ficasse alto. Na cadeira giratória, longe do notebook, o alinhado deixou **4,6 %** de resíduo em duas coletas, e o completo, **0,63 %** (~0,16 µT, quase o ruído). O termo que faltava era um acoplamento y–z de 0,073 na W. Com o lugar ruim (mesa perto do notebook, 14/10), o completo não ajudava (7,1 → 6,1 %): o modelo só resolve o que gira junto com o sensor. Ver [hardware.md](hardware.md), "Calibração do magnetômetro: tentativas de 15/10/2026".
- **NVS:** já gravava W como matriz 3×3, então o formato não mudou. Uma calibração diagonal antiga continua válida.

### D5. Procedimento de calibração (comando `m`)

**Coleta:**
- `m` inicia a coleta, que termina sozinha ao completar os 24 setores, ou depois de `MAG_CAL_MS` (180 s; 90 e 120 s ficaram curtos para girar com calma).
- Os pontos (até 4 500, a 25 Hz) ficam guardados em µT, nos eixos do modelo e sem calibração, num buffer de ~54 kB alocado só durante a calibração. O buffer é necessário porque o resíduo e a recontagem da cobertura precisam de uma segunda passada sobre os pontos, e a tecla `d` os imprime.
- Durante a coleta e a revisão, a proa e o fluxo `p` ficam suspensos. O MPU continua normal. `x` cancela.

**Como girar a caixa (revisto na bancada em 14/10/2026):** **no mesmo lugar**, sem transportá-la, e num ponto onde o campo seja uniforme. Para cada face apoiada, um giro de 360° em torno da vertical, devagar (~20–25 s por giro):

1. fundo apoiado (caixa normal);
2. tampa apoiada (de cabeça para baixo);
3. asa direita apoiada;
4. asa esquerda apoiada;
5. cauda apoiada (nariz para cima);
6. nariz apoiado (nariz para baixo).

O que a bancada mostrou:
- **Girar no ar** desloca o sensor em dezenas de centímetros. Num lugar com ferro por perto, isso deu saltos de 19 µT e leituras de 8 µT, impossíveis só com o campo da Terra e a caixa.
- **Girar sobre a mesa ao lado do notebook** eliminou os saltos, mas o lugar tem gradiente: ~5 µT em 10 cm. Como o sensor fica num canto, ele percorre um círculo de ~15 cm a cada giro, e o resíduo ficou em 7 %, sem correção possível pela calibração.
- **Uma cadeira giratória com um apoio de 23 cm** (para afastar a caixa do aço do mecanismo), longe do notebook, mostrou campo uniforme: 25,0–25,8 µT entre o assento, o apoio e fora da cadeira. Gira-se a cadeira pelo encosto, com o canto dos sensores perto do eixo. É o método adotado; ele precisa de uma extensão USB para ficar a 1 m ou mais do notebook.
- Antes de calibrar num lugar novo, vale o "mapa": com o `p` e o nariz fixo, deslocar a caixa ~10 cm em várias direções; o \|m\| tem que variar menos de ~1 µT.

**Cada giro precisa cobrir os 360°:** só assim o campo passa por todas as direções horizontais da caixa (nariz, asas e cauda). Ida e volta de 180° para o mesmo lado cobre só metade. Para não torcer o cabo USB, dá para fazer a volta inteira num sentido e desfazê-la no outro, ou meia volta para cada lado, voltando ao centro entre elas. Na montagem final, o ESP32 e o powerbank giram junto com a caixa. O cabo USB (só na bancada, para a serial) fica com folga e preso à caixa nos primeiros ~20 cm.

**Cobertura, com 24 setores:**
- A direção de cada ponto é tomada em relação a um centro provisório, o meio do mín./máx. corrente de cada eixo.
- O setor é a face do cubo do eixo dominante (±x, ±y, ±z), dividida em 4 quadrantes pelos sinais dos outros dois eixos.
- Um setor só conta como coberto com pelo menos `MAG_CAL_MIN_PTS_SECTOR` (5) pontos.
- A coleta é aceita com pelo menos `MAG_CAL_MIN_SECTORS` (20) setores. Ao fim da coleta, a cobertura é recontada com o centro ajustado.
- **Progresso a cada 2 s:** por exemplo, `[mag] 34 s | 850 pontos | cobertura 17/24 | faltam: +z(4) +y(2) -x(1)`. As faces são a direção do campo nos eixos do modelo:
  - **±x e ±y:** o campo ao longo do nariz ou das asas. Saem dos giros com a caixa em pé, de lado ou com o nariz para cima ou para baixo;
  - **−z:** o campo saindo pelo topo da caixa. Aqui o campo aponta para cima (inclinação de −32°), então essa face aparece com a caixa quase nivelada;
  - **+z:** o campo saindo pelo fundo. Exige a caixa **de cabeça para baixo** ou bem deitada de lado.

**Ajuste e conferência.** O resultado é recusado, com o motivo na serial, se:
- o raio ficar fora de 10 a 60 µT (faixa larga, para o modelo final). A serial mostra também a comparação com os 26,2 µT do WMM, só como informação;
- o resíduo RMS de \|m\| corrigido passar de 3 % do raio;
- algum semieixo, normalizado para média 1, sair de 0,7 a 1,3;
- o sistema 9×9 for singular, ou a forma quadrática não for positiva definida (a forma ajustada não é um elipsoide);
- a cobertura for insuficiente.

**Revisão:**
- O firmware mostra o offset (µT), os semieixos normalizados (na ordem do eixo do modelo em que cada um mais pesa, como x, y, z), a W em três linhas, o raio, o resíduo e a cobertura.
- `s` grava na NVS: namespace `mag_cal`, com versão, ganho, **mapeamento de eixos** (`MAG_AXIS_MAP_ID`, ao lado de `magChipToBody` em `lib/mag`), offset b e matriz W. Uma calibração feita com outro mapeamento ou com outro ganho é ignorada no boot, com aviso. Um registro com W não simétrica, não positiva definida ou com autovalores fora de 0,25..4 (`magWPlausible`) também é ignorado, como corrompido. Se a orientação do GY-273 mudar, `magChipToBody` e `MAG_AXIS_MAP_ID` mudam juntos.
- `x` descarta e mantém a calibração anterior.
- `d` imprime os pontos em CSV para o `tools/mag_fit.py`, entre as linhas `# mag_cal inicio` e `# mag_cal fim`, com o ajuste do firmware numa linha `# firmware`. A revisão continua aberta depois disso. A impressão bloqueia o laço por ~11 s (4 500 linhas a 115200), e a atitude recomeça depois.
- **Calibração recusada:** a revisão também abre, com o motivo e os valores, mas o `s` não grava. Só `d` (para analisar a coleta no `mag_fit.py`) e `x`.

**Sem calibração na NVS:** offset zero e W identidade. `ST_MAG_CAL` fica desligado, e há aviso no boot e no `c`. A proa é calculada assim mesmo.

### D6. Filtro complementar da proa com o giroscópio

- **Predição a 200 Hz,** junto com a fusão, pela taxa de guinada no referencial da Terra: ψ̇ = (q·sinφ + r·cosφ)/cosθ. Usar só o r estaria errado com a caixa inclinada.
- **Correção a 25 Hz** em direção à proa do magnetômetro, com τ = `MAG_HEADING_TAU_S` (1 s, a ajustar no teste de distúrbio). A diferença é sempre pelo caminho curto: de 359° para 1° são +2°.
- **Inicialização:** com a primeira proa válida do magnetômetro, sem esperar convergir.
- **Suspensão da correção,** com o giroscópio mantendo a proa sozinho:
  - |pitch| > `MAG_MAX_PITCH_DEG` (70°, faixa validada da fusão), porque a proa fica mal definida perto de ±90°;
  - \|m\| corrigido afastado do raio da calibração por mais de `MAG_DISTURB_PCT` (15 %), o distúrbio. A serial sinaliza;
  - **atitude degradada** (o giroscópio saturou, `AttitudeFilter::degraded()`): roll e pitch podem estar errados por alguns segundos, e a compensação de inclinação passaria o erro para a proa.
- **Deriva sem correção:** com o bias do giroscópio calibrado no boot (incerteza de ~0,006 °/s), um minuto inteiro deriva menos de 1°.
- **Reinício:** a proa recomeça da primeira proa válida do magnetômetro quando a atitude reinicia (ou ainda não foi inicializada), numa lacuna de mais de `MAG_MAX_DT_MS` (200 ms) entre amostras do MPU e depois de uma calibração gravada.
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
- estado: `ok`, `DISTURBIO`, `INCL_ALTA`, `ATITUDE_DEGRADADA`, `SEM_ATITUDE` (fusão ainda sem referência), mais `SEM_CAL` sem calibração na NVS.

A linha sai a ~10 Hz em média, alinhada às leituras de 25 Hz (intervalos de 80 e 120 ms). Ao entrar e sair de um distúrbio, a serial imprime uma linha `[mag] DISTÚRBIO: ...` e `[mag] distúrbio terminou`. O aviso é calculado pelo próprio \|m\| em relação ao raio da calibração, e não pelo estado da correção: com outra suspensão ativa (atitude degradada, inclinação alta), o estado mostra essa outra causa, mas o aviso de distúrbio continua correto.

**Linhas novas no `c` e no `r`:**
- `c`: calibração (offset, W, raio, resíduo e setores, ou "NÃO calibrado"), configuração (ganho, taxa, média e leitura) e a linha do `r`;
- `r`: estado do chip, leituras válidas no último 1 s (~25), \|m\| corrigido, proa filtrada, estado, falhas, recuperações e saturações.

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
2. **`magChipToBody`:** sinais, mais os valores medidos em P1 e P2 do D3 (proa de ~106° e ~196°, crescendo no giro horário).
3. **Ajuste:**
   - offset (+15, −8, +30 µT) e escalas (1,1 / 0,9 / 1,05) conhecidos, com ruído de 0,1 µT, são recuperados;
   - distorção com acoplamento entre eixos (y–z de 0,08, como na bancada) é recuperada: W simétrica e W·D proporcional à identidade;
   - o mesmo com autovalores iguais (esfera), com um autovalor duplo e com o elipsoide fortemente girado (45° em dois eixos);
   - `magWPlausible`: aceita a identidade e a W da bancada; recusa W assimétrica, indefinida, fora da faixa ou não finita;
   - funciona com 20 de 24 setores;
   - recusa plano único, raio fora da faixa, salto de distúrbio, escala fora da faixa e forma não elipsoidal.
4. **Setores:** direções conhecidas caem no setor certo; o mínimo de pontos por setor é respeitado.
5. **Proa compensada:** campo de João Pessoa (H = 22,2 µT, inclinação de −31,8°), proas de 0 a 345° de 15 em 15, com roll e pitch de até ±45°. A proa sai igual em todas as combinações e passa corretamente por 0/360°.
6. **Filtro:**
   - degrau de proa converge com τ;
   - giro só pelo giroscópio acompanha;
   - caminho curto de 359° para 1°;
   - distúrbio, pitch > 70° e atitude degradada suspendem a correção;
   - inicialização com a primeira proa válida.

**Conferência fora do firmware:** `firmware/aircraft/tools/mag_fit.py` (usa só `numpy`) lê o CSV da tecla `d`, salvo em `firmware/aircraft/logs`, e ajusta elipsoides de 6 e de 9 parâmetros. O script mostra duas coisas:
- se o ajuste do firmware está certo com dados reais: compara com a conta de 9 parâmetros (ou com a de 6, em logs anteriores a 15/10/2026, que têm só a diagonal de W);
- quanto o elipsoide completo reduz o resíduo em relação ao alinhado aos eixos.

## Valores no `config.h` (seção HMC5883L, a ajustar na bancada)

| Constante | Valor inicial | Uso |
|---|---|---|
| `MAG_RANGE_MGA` | 1300 (±1,3 Ga) | Ganho (D2) |
| `MAG_OUTPUT_HZ` / `MAG_AVG_SAMPLES` | 30 / 8 | Configuração do chip (D2) |
| `MAG_READ_HZ` | 25 | Leitura pelo firmware |
| `MAG_MAX_FAIL_STREAK` / `MAG_FAIL_RETRY_MS` | 5 / 1000 | Falhas, como no BME280 |
| `MAG_RETRY_MS` | 5000 | HMC ausente no boot: nova tentativa |
| `MAG_CAL_MS` | 180000 | Duração máxima da coleta |
| `MAG_CAL_PROGRESS_MS` | 2000 | Linhas de progresso da coleta |
| `MAG_REF_FIELD_UT` | 26,2 | \|B\| do WMM na bancada, só para comparação na revisão |
| `MAG_CAL_MIN_SECTORS` | 20 (de 24) | Cobertura mínima |
| `MAG_CAL_MIN_PTS_SECTOR` | 5 | Pontos para um setor contar |
| `MAG_CAL_MIN_RADIUS_UT` / `MAG_CAL_MAX_RADIUS_UT` | 10 / 60 | Faixa do raio ajustado |
| `MAG_CAL_MAX_RESID_PCT` | 3 | Resíduo máximo do ajuste |
| `MAG_CAL_MIN_SCALE` / `MAG_CAL_MAX_SCALE` | 0,7 / 1,3 | Faixa dos semieixos normalizados |
| `MAG_DISTURB_PCT` | 15 | Desvio de \|m\| que suspende a correção |
| `MAG_HEADING_TAU_S` | 1,0 | Filtro da proa (D6) |
| `MAG_MAX_PITCH_DEG` | 70 | Acima disso, sem correção pelo magnetômetro |
| `MAG_MAX_DT_MS` | 200 | Lacuna que reinicia a proa; limite do passo na correção |
| `MAG_PRINT_HZ` | 10 | Fluxo `p` |

## Limitações conhecidas

- **Desalinhamento entre o GY-273 e a caixa.** A rotação de montagem (`z`) corrige só o MPU. Um desalinhamento de poucos graus do GY-273 vira um erro de proa de poucos graus, que aumenta com a inclinação. A soft iron não corrige rotação. É aceitável num modelo de bancada.
- **Distorções que se movem em relação à caixa:** cabo USB, mão, mesa de aço, objetos próximos. A calibração não as corrige. O filtro (D6) só atenua as passageiras.
- **Referência de proa na bancada:** a bússola do celular tem erro de alguns graus e também sofre com metal. Os testes medem principalmente **giros relativos**, por exemplo de 90° alinhando com a borda da mesa, e a repetibilidade.
- **Proa magnética, sem declinação.** Em João Pessoa, a declinação é de −20,7°: o norte verdadeiro fica 20,7° no sentido horário a partir do norte magnético. Isso fica documentado, não corrigido.

## Roteiro de validação na bancada

Pré-requisitos:
- montagem final, com tudo preso na caixa;
- sinais do MPU conferidos na montagem final, e seis posições e `z` refeitos (`chipToBody` atualizado em 14/10);
- boot parado e giroscópio DEFINITIVO;
- celular, controle remoto, multímetro e ferramentas a mais de 30 cm.

Gravar: `pio run -e aircraft -t upload`. Monitor, salvando o log: `pio device monitor -b 115200 -p COM7 --echo -f log2file`.

1. **Mapeamento de eixos (D3):** feito (13/10). Na montagem final, refazer o **teste das 4 proas com o `h` do `i2cscan`, girando a caixa inteira no mesmo lugar**: giros de ~90° ± 5° depois de descontar o centro e Z quase constante (± 2 µT). É a confirmação de que a instabilidade do protótipo sumiu com tudo fixo.
2. **Driver:**
   - taxa real de saída (~30 Hz) e ruído parado (~0,1 µT);
   - `r` por alguns minutos: ~25 leituras do mag por segundo, MPU sem perdas nem descartes;
   - SDA do GY-273 solto (com o chip alimentado): `FALHANDO` com uma tentativa por segundo, e volta sem reconfiguração;
   - **não fazer queda de alimentação no hardware** (ver o latch-up do BME280 no [hardware.md](hardware.md)).
3. **Calibração (D5):**

   | Teste | Como | Passa se |
   |---|---|---|
   | Coleta boa | `m`, girando como descrito no D5 | 24/24 antes dos 180 s; raio perto de 26 µT; resíduo < 3 % |
   | Repetibilidade | Duas calibrações seguidas | Offsets dentro de ~1 µT; semieixos dentro de ~2 % |
   | Recusa por cobertura | `m` girando só apoiada na mesa | Recusada; o "faltam" aponta ±z |
   | Guia de progresso | `m` sem nunca virar a caixa de cabeça para baixo | O "faltam" mostra a face; ao virar, ela some |
   | Recusa por distúrbio | Chave de fenda encostada por alguns segundos no meio da coleta | Resíduo > 3 %, recusada |
   | Resultado aplicado | Depois do `s`, fluxo `p` em várias orientações | \|m\| corrigido dentro de ±3 % |
   | Conferência independente | `d` → log → `tools/mag_fit.py` | Ajuste de 9 parâmetros igual ao do firmware |
   | NVS | `s` e reinício | Mesma calibração no boot; `ST_MAG_CAL` ligado |
   | Descartar | Nova `m`, depois `x` | A calibração anterior continua valendo, inclusive depois de reiniciar |
   | Sem calibração | Primeiro boot, antes de qualquer `s` (não há comando para apagar a NVS) | Aviso no boot e no `c`; `SEM_CAL` no `p`; proa calculada |

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
| `lib/hmc5883l/` | Driver |
| `lib/mag/` | Lógica pura: decodificação, mapeamento, ajuste, cobertura, proa e filtro |
| `lib/mag_store/` | Calibração na NVS (namespace `mag_cal`) |
| `test/test_mag/` | Testes `native` (28 casos) |
| `tools/mag_fit.py` | Conferência do ajuste com o CSV da tecla `d`. Uso, a partir de `firmware/aircraft`: `python tools/mag_fit.py` (último log em `logs/`) ou `python tools/mag_fit.py logs/arquivo.log` |
| `src/main.cpp` | Integração, comandos `m` e `p`, linhas `mag` no `c` e no `r` |
| `include/config.h` | Seção MAG |
| `platformio.ini` | Remove a Adafruit HMC5883 e a Adafruit Unified Sensor |
| `docs/magnetometro.md`, `docs/hardware.md`, `README.md`, `CLAUDE.md` | Estratégia, resultados, comandos e referência |
