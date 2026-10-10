# Hardware — registro de montagem e testes

Este documento guarda a **evidência** observada em bancada. Os valores que o firmware usa ficam em `include/config.h` de cada projeto. Se um teste contradisser o `config.h`, corrija o `config.h` e registre aqui o teste que motivou a mudança.

## Diagnóstico I2C (ESP A)

Ambiente `i2cscan`, em `firmware/aircraft` (fonte em `src/diag/i2c_diag.cpp`):

```powershell
cd firmware/aircraft
pio run -e i2cscan -t upload
pio device monitor -b 115200
```

O diagnóstico varre o barramento, identifica os chips pelos registradores de ID (WHO_AM_I do MPU, chip_id do BME280, ID "H43" do HMC5883L) e compara com o `config.h`. Depois espera comandos digitados no monitor:

| Tecla | Ação |
|---|---|
| `s` | Varre o barramento de novo (depois de mexer em cabos) |
| `l` | Liga ou pausa o fluxo de leituras brutas do MPU (`DIAG_PRINT_HZ` leituras por segundo, hoje 1; começa pausado) |
| `m` | Média e desvio padrão de 400 leituras (2 s) com a placa parada; informa quantas leituras foram válidas e a frequência I2C |
| `f` | Alterna o I2C entre 100 kHz (`I2C_FREQ_DIAG_HZ`) e 400 kHz (`I2C_FREQ_HZ`, a frequência do firmware de voo) |
| `t` | Teste de contato: lê sem pausa por 5 s e conta falhas e quadros suspeitos (todos os bytes iguais a 0x00 ou a 0xFF). Mexa nos fios durante o teste |
| `i` | Teste do pino INT: configura "data ready" a 200 Hz e conta os pulsos no GPIO 19 por 2 s; ao final, volta à configuração padrão |

Para sair do monitor, use `Ctrl+C`; não é preciso desconectar a placa. Para voltar ao firmware normal, grave o ambiente `aircraft`.

### Componentes e endereços

| Componente | Módulo | Endereço no `config.h` | Observado | ID lido |
|---|---|---|---|---|
| IMU | MPU6050 (GY-521) | 0x68 | 0x68 (09/10) | WHO_AM_I = 0x68 |
| Barômetro | BME280 | 0x76 | ausente (09/10) | pendente (esperado chip_id = 0x60) |
| Magnetômetro | GY-273 com HMC5883L | 0x1E | ausente (09/10) | pendente (esperado "H43") |
| OLED de status | SSD1306 0,91" 128x32 | 0x3C | ausente (09/10) | não tem ID |

Alguns GY-273 vendidos como HMC5883L trazem um QMC5883L, que responde em 0x0D e precisa de outra biblioteca. O diagnóstico detecta esse caso.

### Registro de testes

| Data | Placa | Ligação | Frequência I2C | Resultado | Observações |
|---|---|---|---|---|---|
| 09/10/2026 | A (1C:69:20:A4:E9:44, ESP32-D0WD-V3 rev 3.1, COM7) | não registrada | 100 kHz | 1 dispositivo: MPU6050 em 0x68 (WHO_AM_I correto). BME280, HMC5883L e OLED ausentes | Ver medidas abaixo. Falta saber se os outros três módulos estavam ligados |
| 10/10/2026 | A | Montagem 3: MPU preso numa caixa, com fiação presa | 100 kHz | Seis posições sem falhas de leitura | Ver "Calibração em seis posições" |
| 10/10/2026 | A | Montagem 4: MPU recolado depois de soltar da caixa | 100 kHz | Seis posições sem falhas na média; `Error -1` de I2C antes da posição 6 | Erro sumiu depois de ajustar os fios: há mau contato na fiação |
| 10/10/2026 | A | Montagem 5: fiação reorganizada e presa melhor | 100 kHz | Seis posições, 400 de 400 leituras válidas em todas, nenhum erro de I2C | Ver "Montagem 5" |
| 10/10/2026 | A | Montagem 5, com o INT da GY-521 ligado ao GPIO 19 | 100 e 400 kHz | Teste de contato sem falhas nas duas frequências; INT a 201,5 Hz | Ver "Contato a 400 kHz e pino INT" |
| 10/10/2026 | A | Montagem 5, depois de soltar o AD0 do chicote e prender o fio do INT | 100 e 400 kHz | Eixos alinhados à gravidade dentro de ±6 LSB da montagem 5 (Z: +17); contato sem falhas; INT a 201,5 Hz | Ver "Revalidação após mexer no AD0 e no INT" |

Na coluna "Ligação", anote como os módulos estavam ligados (protoboard ou soldados, comprimento dos fios, alimentação em 3V3). Ela ajuda a explicar falhas intermitentes.

### MPU6050 em repouso (09/10/2026)

Comando `m` (400 leituras em 2 s), executado duas vezes seguidas, com a placa parada na bancada e a face do chip para cima. Configuração padrão do chip: ±2 g, ±250 °/s, sem DLPF. Temperatura do chip: 27,7 e 27,9 °C.

| Eixo | Média, 1ª medida | Média, 2ª medida | Desvio padrão |
|---|---|---|---|
| ax | −666,8 LSB (−0,041 g) | −663,8 LSB (−0,041 g) | 60–65 LSB (≈ 4 mg) |
| ay | +321,2 LSB (+0,020 g) | +321,3 LSB (+0,020 g) | 54–58 LSB (≈ 3,5 mg) |
| az | +18 049,8 LSB (+1,102 g) | +18 031,7 LSB (+1,101 g) | 95–103 LSB (≈ 6 mg) |
| gx | −644,1 LSB (−4,917 °/s) | −645,2 LSB (−4,925 °/s) | 15–20 LSB (≈ 0,13 °/s) |
| gy | +343,4 LSB (+2,621 °/s) | +343,8 LSB (+2,624 °/s) | 15 LSB (≈ 0,11 °/s) |
| gz | +130,6 LSB (+0,997 °/s) | +130,4 LSB (+0,995 °/s) | 11–13 LSB (≈ 0,09 °/s) |

Conclusões:

- **Giroscópio.** O bias é grande, mas estável: entre as duas medidas variou menos de 0,01 °/s. Sem correção, o eixo X derivaria cerca de 295° por minuto. Uma média de 2 s parada no boot estima o bias com incerteza de cerca de 0,006 °/s (15 LSB / √400). O bias muda com a temperatura, então a calibração deve acontecer no boot e se repetir sob comando, mas só deve ser aceita com a placa comprovadamente parada (ver [calibracao.md](calibracao.md)).
- **Acelerômetro.** O módulo da gravidade medido é 1,103 g, 10 % acima do esperado. Isso está acima da tolerância de escala do datasheet (±3 %) e perto do limite de offset do eixo Z (±80 mg). Com uma única posição não dá para separar offset de escala; isso exige a calibração em seis posições (cada eixo em +1 g e −1 g). Sobre o efeito na atitude: roll = atan2(ay, az) e pitch = atan2(−ax, √(ay² + az²)) dependem só de razões entre eixos, então um erro de escala **igual nos três eixos** se cancela no ângulo. Offsets e escalas **diferentes por eixo** distorcem o ângulo, e com uma posição só não dá para saber qual é o caso. Mesmo um erro uniforme precisa ser corrigido, porque o módulo |a| será usado como critério: na detecção de repouso (|a| ≈ 1 g) e no filtro complementar (confiar menos no acelerômetro quando |a| se afasta de 1 g). Com 1,10 g em repouso, os dois critérios falhariam.
- **Inclinação aparente.** Os valores de ax e ay correspondem a cerca de 2,1° de pitch e 1,0° de roll. Na primeira sessão (leituras a olho, com a placa em outra posição) eles eram −0,02 g e +0,06 g. Como mudaram de uma sessão para outra, refletem sobretudo a posição em que a placa estava apoiada, e não um offset fixo. Para separar uma coisa da outra é preciso girar a placa 180° sobre a mesma superfície.
- **Ruído.** É compatível com o datasheet e não mostra nenhum sinal de mau contato. Com o ruído do acelerômetro, um ângulo calculado a partir de uma única amostra oscila cerca de 0,2°; o filtro complementar reduz isso. Ligar o DLPF do MPU (por exemplo, em 42 Hz) também reduz o ruído sem prejudicar a amostragem a 200 Hz.
- **Limiares de repouso.** O ruído medido define o critério de "placa parada" usado na calibração do giroscópio: desvio padrão de cada eixo do giroscópio abaixo de ~50 LSB (≈ 0,4 °/s, cerca de 3× o maior desvio observado) e |a| entre 0,95 g e 1,05 g, este último só depois da calibração do acelerômetro. Revalidar os limiares com o MPU montado no modelo e o DLPF ligado.
- **Barramento.** Dois scans seguidos deram o mesmo resultado: apenas o MPU responde.

### Calibração em seis posições: montagens 3 e 4 (10/10/2026)

Procedimento do [calibracao.md](calibracao.md): duas medidas `m` (400 leituras, 2 s) por posição, com a caixa apoiada e sem as mãos. Diagnóstico em ±2 g e ±250 °/s, sem DLPF, a 100 kHz. Todas as medidas tiveram 400 de 400 leituras válidas. Nas tabelas, cada valor é a média das duas medidas, em LSB.

**Montagem 3**, com o MPU e a fiação presos numa caixa, a 26,2–26,6 °C:

| Posição | ax | ay | az |
|---|---|---|---|
| 1. Face para cima | +732 | +215 | **+18 027** |
| 2. Face para baixo | −48 | −256 | **−15 064** |
| 3. Nariz para cima | **+16 742** | −419 | +1 285 |
| 4. Nariz para baixo | **−16 194** | −327 | +1 884 |
| 5. Asa direita para baixo | +157 | **+16 388** | +401 |
| 6. Asa esquerda para baixo | +178 | **−16 402** | +1 861 |

**Montagem 4**, com o MPU recolado na caixa depois de se soltar, a 26,0–26,9 °C:

| Posição | ax | ay | az |
|---|---|---|---|
| 1. Face para cima | −454 | +1 100 | **+17 942** |
| 2. Face para baixo | +392 | −1 233 | **−15 015** |
| 3. Nariz para cima | **+16 741** | +75 | +2 097 |
| 4. Nariz para baixo | **−16 193** | −589 | +1 107 |
| 5. Asa direita para baixo | −56 | **+16 375** | +149 |
| 6. Asa esquerda para baixo | +512 | **−16 361** | +2 493 |

Offset = (leitura em +1 g + leitura em −1 g) / 2. Escala = (leitura em +1 g − leitura em −1 g) / 2.

| Eixo | Offset, montagem 3 | Offset, montagem 4 | Escala, montagem 3 | Escala, montagem 4 |
|---|---|---|---|---|
| X | +274 LSB (+16,7 mg) | +274 LSB (+16,7 mg) | 16 468 LSB/g (+0,51 %) | 16 467 LSB/g (+0,51 %) |
| Y | −7 LSB (−0,4 mg) | +7 LSB (+0,4 mg) | 16 395 LSB/g (+0,06 %) | 16 368 LSB/g (−0,10 %) |
| Z | +1 482 LSB (+90,4 mg) | +1 463 LSB (+89,3 mg) | 16 546 LSB/g (+0,99 %) | 16 479 LSB/g (+0,58 %) |

Conclusões:

- **Os coeficientes se repetem.** Entre as duas montagens, o offset ficou igual em X e variou cerca de 1 mg em Y e em Z. Com qualquer um dos dois conjuntos, |a| corrigido fica entre 1,000 e 1,003 g nas seis posições, dentro do critério de 0,98–1,02 g.
- **O offset de Z depende da fixação do módulo.** Nas duas montagens anteriores, ele foi de cerca de +1 630 LSB (+99 mg); nestas duas, de cerca de +1 470 LSB (+90 mg). X e Y não mudaram mais que cerca de 2 mg. A temperatura explica só 1 a 2 mg da diferença, então a causa mais provável é a tensão mecânica do suporte sobre o módulo, à qual o MPU6050 é sensível. A calibração em seis posições vale para uma montagem específica e deve ser refeita sempre que o suporte mudar.
- **A queda de escala de Z na montagem 4 vem da inclinação, e não do sensor.** Nas posições 1 e 2, o módulo estava cerca de 4–5° inclinado em relação à caixa. Com isso, o eixo Z mede cerca de 0,3 % a menos (cos 4,6°), o que reduz a escala estimada em cerca de 50 LSB. Descontado esse efeito, a escala fica próxima das anteriores (≈ 16 530). O erro em |a| é de cerca de 0,003 g.
- **O alinhamento do módulo com a caixa piorou quando ele foi recolado.** Depois de descontar os offsets, ay indica cerca de +3,8° com a face para cima e −4,3° com a face para baixo. Como o sinal troca quando a caixa vira, a inclinação é do módulo em relação à caixa (cerca de 4° em roll), e não da mesa. Na montagem 3, o maior desalinhamento era de cerca de 1,6°. Isso é corrigido pelo zero de nível, mas convém alinhar o módulo a 1–2° na montagem definitiva.
- **A estabilidade melhorou muito.** Entre as medidas A e B de uma mesma posição, a diferença ficou entre 2 e 32 LSB (≤ 0,1°). O desvio do giroscópio ficou em 12–19 LSB na maioria das posições, que é o valor de repouso total. As exceções foram a face para baixo na montagem 3 (143–197 LSB, com a caixa balançando) e a posição 5 na montagem 4 (70 LSB na medida A). As duas ficaram acima do critério de repouso de 50 LSB e mostram que ele detecta esse tipo de movimento.
- **O bias do giroscópio varia cerca de 0,1–0,15 °/s entre posições e sessões.** Foi de −4,92 a −5,00 °/s em gx, de +2,61 a +2,74 °/s em gy e de +0,89 a +1,07 °/s em gz, a 26–27 °C. Isso reforça a calibração a cada boot.
- **Falha de I2C na montagem 4.** Antes da posição 6 apareceu `[E][Wire.cpp:499] requestFrom(): i2cWriteReadNonStop returned Error -1`, que sumiu depois de ajustar os fios. As médias não foram afetadas: o diagnóstico descarta as leituras que falham, todas as medidas registradas tiveram 400 de 400 leituras válidas, e os desvios de 6A e 6B estão normais, sem valores fora da curva. Mesmo assim, há um mau contato na fiação, que deve ser corrigido na montagem final do modelo de bancada. O firmware de voo precisa tratar falhas de leitura (contar, sinalizar no status e recuperar o barramento) sem usar a amostra.

### Calibração em seis posições: montagem 5 (10/10/2026)

A fiação foi reorganizada e presa melhor. O procedimento e a configuração do diagnóstico são os mesmos das montagens 3 e 4 (versão com os comandos `s`, `l` e `m`). Temperatura: 26,0–26,6 °C. Todas as medidas tiveram 400 de 400 leituras válidas, sem nenhum erro de I2C.

| Posição | ax | ay | az |
|---|---|---|---|
| 1. Face para cima | +70 | +615 | **+18 010** |
| 2. Face para baixo | +63 | −766 | **−15 038** |
| 3. Nariz para cima | **+16 738** | +217 | +1 745 |
| 4. Nariz para baixo | **−16 198** | −677 | +1 468 |
| 5. Asa direita para baixo | −111 | **+16 419** | +910 |
| 6. Asa esquerda para baixo | +580 | **−16 392** | +1 769 |

| Eixo | Offset | Escala | Faixa nas montagens 3 a 5 |
|---|---|---|---|
| X | +270 LSB (+16,5 mg) | 16 468 LSB/g (+0,51 %) | offset 270–274, escala 16 467–16 468 |
| Y | +14 LSB (+0,8 mg) | 16 406 LSB/g (+0,13 %) | offset −7 a +14, escala 16 368–16 406 |
| Z | +1 486 LSB (+90,7 mg) | 16 524 LSB/g (+0,86 %) | offset 1 463–1 486, escala 16 479–16 546 |

Conclusões:

- **A calibração convergiu.** Nas três últimas montagens, os offsets variaram no máximo 0,3 mg em X, 1,3 mg em Y e 1,4 mg em Z, e as escalas no máximo 0,4 %. Com os coeficientes desta montagem, |a| corrigido fica entre 1,000 e 1,001 g nas seis posições.
- **O alinhamento está melhor que na montagem 4.** Depois de descontar os offsets, ay indica +2,1° com a face para cima e −2,7° com a face para baixo, ou seja, cerca de 2,4° de desalinhamento em roll do módulo em relação à caixa (na montagem 4 eram cerca de 4°). Em X, ax corrigido dá cerca de −0,7° nas duas faces: como o sinal não troca, isso vem da superfície de apoio, e o desalinhamento em pitch é desprezível. O efeito da inclinação na escala de Z é de cerca de 15 LSB (0,1 %).
- **A estabilidade é a melhor até agora.** Entre as medidas A e B de uma mesma posição, a diferença ficou em até 15 LSB (≈ 0,05°). O desvio do giroscópio ficou em 12–21 LSB em quase todas as medidas.
- **A posição 5 tem movimento de novo na medida A**, como na montagem 4: desvio de 74 LSB em gx (acima do critério de 50) e de 223 LSB em az. A medida 5B, feita logo depois, estava parada (13 LSB), e as médias de 5A e 5B praticamente coincidem. A caixa provavelmente ainda se acomoda nos primeiros segundos depois de ser deitada de lado. Na prática, vale esperar alguns segundos antes do `m`. O critério de repouso detecta esse caso.
- **O bias do giroscópio ficou mais estável que na montagem 4.** Foi de −4,905 a −4,960 °/s em gx, de +2,681 a +2,713 °/s em gy e de +0,955 a +0,983 °/s em gz, ou seja, uma faixa de cerca de 0,05 °/s na sessão inteira.
- **Sinais do giroscópio (comando `l`, 1 leitura por segundo).** Partindo do modelo nivelado, foram feitos três movimentos lentos, voltando a nivelar depois de cada um: asa direita para baixo, nariz para cima e nariz para a direita. Cada linha do fluxo é uma amostra instantânea, então os valores abaixo já descontam o bias e mostram o comportamento predominante. O acelerômetro confirma cada movimento.

  | Movimento | Acelerômetro | Giroscópio na ida | Giroscópio na volta | Esperado |
  |---|---|---|---|---|
  | Asa direita para baixo (até cerca de 50°) | ay de 0,04 a 0,79 g | gx de +3 a +12 °/s | gx de −6 a −12 °/s | gx + ✔ |
  | Nariz para cima (até cerca de 30°) | ax de 0,0 a 0,52 g | gy de −6 a −32 °/s | gy de +5 a +17 °/s | gy − ✔ |
  | Nariz para a direita (sobre a mesa) | inalterado (az ≈ 1,10 g) | gz de −5 a −9 °/s | gz de +9 a +20 °/s | gz − ✔ |

  Os três sinais coincidem com o mapeamento obtido pelo acelerômetro (X para o nariz, Y para a asa esquerda, Z para cima), então o giroscópio usa os mesmos eixos. Houve uma amostra fora do padrão no movimento do nariz (gy ≈ +17 °/s ainda na subida), atribuída a oscilação da mão. A essa taxa, o teste é qualitativo.
- **Contato com os fios em movimento (comando `l`, 1 leitura por segundo).** Foram cerca de 40 s mexendo nos fios: 39 leituras, sem nenhuma falha de leitura, nenhum `Error -1` e nenhum quadro com todos os bytes iguais a 0x00 ou a 0xFF. Três amostras se afastaram do repouso: az = 1,33 g com gx ≈ +12,6 °/s e gz ≈ −13,3 °/s; uma torção em gz de −7,7 e depois +5,9 °/s; e ax = −0,18 g com gz ≈ +8,9 °/s. Nos três casos o acelerômetro e o giroscópio mudaram juntos e de forma coerente, e a amostra seguinte voltou ao repouso. Isso é movimento da caixa ao mexer nos fios, e não corrupção de dados. A temperatura, que vem no mesmo quadro I2C, ficou entre 25,8 e 26,0 °C em todas as amostras: um bit corrompido faria ela saltar. Com só 39 transações, uma falha intermitente rápida pode ter passado sem ser vista. O teste rigoroso é o `t` da versão nova do diagnóstico, que faz milhares de leituras por segundo.
- **Fiação.** Não houve nenhuma falha de I2C na sessão inteira. A fiação foi testada só a 100 kHz. O teste de contato com `t` e o teste a 400 kHz dependem da versão nova do diagnóstico.

### Contato a 400 kHz e pino INT (10/10/2026)

Montagem 5, com a caixa apoiada na posição 1 (nivelada, face para cima), a 25,8 °C. Foi a primeira execução da versão do diagnóstico com os comandos `f`, `t` e `i`. Esta é a saída que valida essa versão.

| Teste | 100 kHz | 400 kHz |
|---|---|---|
| `t` parado | 3 043 leituras, 0 falhas, 0 suspeitas (609 leituras/s) | 10 749 leituras, 0 falhas, 0 suspeitas (2 150 leituras/s) |
| `t` mexendo nos fios | 3 043 leituras, 0 falhas, 0 suspeitas | 10 749 leituras, 0 falhas, 0 suspeitas |
| `m` parado | 400 de 400 válidas | 400 de 400 válidas (antes e depois do teste do INT) |

| Teste do INT (`i`, duas vezes) | Resultado |
|---|---|
| Pulsos em 2 000 ms | 403 e 403 |
| Taxa | 201,5 Hz (esperado 200 Hz), OK |

Conclusões:

- **O barramento é confiável a 400 kHz.** Foram cerca de 27 600 leituras de 14 bytes sem nenhuma falha, inclusive mexendo nos fios. A taxa a 400 kHz foi 3,5 vezes a de 100 kHz, e uma leitura completa do MPU leva cerca de 0,47 ms. Isso confirma o `I2C_FREQ_HZ` de 400 kHz do `config.h` e deixa folga para a amostragem a 200 Hz (5 ms por ciclo). Nas repetições, a contagem de leituras foi idêntica: uma falha teria consumido o tempo de timeout e reduzido a contagem.
- **O pino INT funciona.** O "data ready" chegou a 201,5 Hz, 0,75 % acima do nominal. Até ±1 pulso vem do limite da janela de contagem; o resto é a tolerância do oscilador interno de 8 MHz, que o diagnóstico usa (`PWR_MGMT_1 = 0x00`). Para o firmware de voo, há duas recomendações: usar o PLL do giroscópio como relógio (`CLKSEL = 1`), que o datasheet indica como mais estável, e medir o intervalo real entre amostras (`micros()`) em vez de assumir 5 ms.
- **O MPU voltou à configuração padrão depois do teste do INT.** O último `m` coincide com o anterior, inclusive no desvio padrão. Se o DLPF tivesse ficado ligado, o desvio teria caído bastante.
- **A versão nova mede igual à antiga.** As médias e os desvios do `m` estão dentro da faixa da montagem 5, e o bias do giroscópio ficou em −4,89 a −4,91, +2,71 e +0,96 °/s.
- **Mexer nos fios moveu levemente a caixa ou o módulo.** O ay em repouso passou de +393 LSB, no primeiro `m`, para +586 e +589 LSB depois dos testes de contato, uma mudança de cerca de 0,7° em roll. O valor final está mais próximo do valor da montagem 5 na posição 1 (+615). A causa provável é a caixa se acomodar na mesa, e não o módulo se soltar. Mesmo assim, convém evitar que os fios fiquem esticados entre a caixa e o ESP32.

Com estes testes, o MPU6050 está validado na bancada: identificação, calibração em seis posições, orientação dos eixos, repouso e ruído, contato a 100 e a 400 kHz e pino INT.

### Revalidação após mexer no AD0 e no INT (10/10/2026)

O AD0 foi solto do chicote, ficando sem conexão, e o fio do INT foi preso ao GPIO 19, sem mexer de propósito no módulo. Para checar se algo mudou, foi feito um teste rápido em cinco posições, sem a face para baixo. Em cada posição, compara-se com a montagem 5 o eixo alinhado à gravidade: nesse eixo, uma inclinação de até cerca de 3° altera a leitura em menos de 25 LSB, então uma mudança maior que isso vem do sensor, e não do apoio da caixa. A tolerância adotada foi de ±30 LSB. Temperatura: 25,9–26,1 °C.

| Passo | Posição | Eixo | Medido | Montagem 5 | Diferença |
|---|---|---|---|---|---|
| P2/P10 | Face para cima | az | +18 027 | +18 010 | +17 ✔ |
| P3 | Nariz para cima | ax | +16 744 | +16 738 | +6 ✔ |
| P4 | Nariz para baixo | ax | −16 193 | −16 198 | +5 ✔ |
| P5 | Asa direita para baixo | ay | +16 422 | +16 419 | +3 ✔ |
| P6 | Asa esquerda para baixo | ay | −16 388 | −16 392 | +4 ✔ |

| Eixo | Offset | Escala | Montagem 5 |
|---|---|---|---|
| X | +275 LSB | 16 468 LSB/g | +270, 16 468 |
| Y | +17 LSB | 16 405 LSB/g | +14, 16 406 |
| Z | não medido (sem a face para baixo) | — | +1 486, 16 524 |

- **A calibração da montagem 5 continua valendo.** Offset e escala de X e de Y repetiram dentro de 5 LSB (0,3 mg). O az com a face para cima ficou 17 LSB (≈ 1 mg) acima, bem abaixo da mudança de cerca de 100 LSB observada quando a fixação mudou entre as montagens 2 e 3. Com os coeficientes da montagem 5, |a| corrigido com a face para cima (P10) dá 1,002 g.
- **O P2 teve movimento:** o desvio de gz foi de 88 LSB, acima do critério de 50. A medida P10, na mesma posição e parada, deu o mesmo az (+18 027), então o valor de Z é confiável.
- **Os eixos laterais mudaram até cerca de 330 LSB (≈ 1,1°)** em relação à montagem 5. Isso mistura a recolocação da caixa no esquadro com um possível pequeno deslocamento do módulo, e não afeta a calibração. O desalinhamento fica para o zero de nível do firmware.
- **AD0 e INT:** o MPU continua em 0x68 com o AD0 solto. No teste de contato a 400 kHz, mexendo nos fios, foram 10 751 leituras sem falhas. O INT deu 201,5 Hz, igual ao teste anterior.

## Orientação de montagem do MPU6050

Preenchido em 10/10/2026 com a montagem 5, que é a montagem definitiva do modelo de bancada. O mesmo mapeamento se repetiu em todas as montagens testadas.

| Posição | Eixo em ≈ +1 g | Conclusão |
|---|---|---|
| Nivelado, face para cima | +Z | Z+ do chip aponta para cima |
| Nariz para cima | +X | X+ do chip aponta para o nariz |
| Asa direita para baixo | +Y | Y+ do chip aponta para a asa esquerda |

Os eixos do chip formam um sistema destro: X para a frente, Y para a esquerda, Z para cima. O teste de sinais do giroscópio (montagem 5) confirmou que o giroscópio usa os mesmos eixos.

Convenção proposta para o modelo, no padrão aeronáutico (x para o nariz, y para a asa direita, z para baixo): roll positivo com a asa direita descendo, pitch positivo com o nariz subindo, yaw positivo com o nariz virando para a direita, visto de cima. O mapeamento do chip para o modelo vale para o acelerômetro e para o giroscópio:

| Modelo | Chip |
|---|---|
| x (nariz) | +X |
| y (asa direita) | −Y |
| z (para baixo) | −Z |

Essas definições entram depois no contrato (`telemetry.h`). Se a montagem mudar, refaça esta tabela e a calibração em seis posições.

Requisitos de montagem:

- MPU rigidamente fixo e alinhado aos eixos do modelo, idealmente a 1–2°. Uma fixação que cede deixa o módulo inclinar, como na protoboard, ou se soltar, como antes da montagem 4. Fita dupla-face espessa tem o mesmo problema. O offset de Z depende da fixação, então a calibração em seis posições deve ser refeita sempre que o suporte mudar.
- Fiação presa e com contato firme: na montagem 4, um mau contato causou falhas de leitura I2C.
- Magnetômetro longe do powerbank e dos fios de alimentação: a corrente gera campo magnético e distorce a proa. Calibrar o magnetômetro já montado.
