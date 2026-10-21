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
| `b` | Liga ou pausa o fluxo do BME280 (1 leitura por segundo): temperatura (°C), pressão (hPa), umidade (%) e a variação de pressão (Pa) e de altura (m) desde a primeira leitura do fluxo. Cada leitura é uma medição forçada com oversampling x1 e sem filtro, compensada com os coeficientes de fábrica do chip. A cada scan, o BME280 passa por um soft reset antes da leitura dos coeficientes, para que nenhuma configuração anterior (filtro IIR, por exemplo) fique no chip quando só o ESP reinicia |
| `h` | Medida do HMC5883L: contagem regressiva de 10 s (para soltar a caixa), 10 leituras (1 por segundo) e a média delas, e termina sozinha; outro `h` cancela. Cada linha mostra o campo em X, Y e Z (µT), o módulo \|B\| e o ângulo no plano XY (atan2(Y, X), sem calibração nem compensação de inclinação). Modo contínuo a 15 Hz, média de 8 amostras, ganho de ±1,3 Ga; um eixo saturado mostra `SATURADO` |
| `x` | Autoteste do HMC5883L: média de 4 saídas com o campo interno positivo e de 4 com o negativo, no ganho de 390 LSB/Ga. Com a polarização ligada, o próprio chip subtrai o campo externo. Cada polarização de cada eixo deve ficar entre 243 e 575 LSB em módulo, faixa do datasheet no ganho de 390 LSB/Ga, comparada diretamente em LSB (equivale a ±62,3 a ±147,4 µT; nominal ~116 µT em X e Y e ~108 µT em Z); (positivo − negativo)/2 aparece só como informação. Ao final, volta à configuração do `h`; se a restauração falhar, o HMC fica indisponível até um novo scan |

Para sair do monitor, use `Ctrl+C`; não é preciso desconectar a placa. Para voltar ao firmware normal, grave o ambiente `aircraft`.

### Componentes e endereços

| Componente | Módulo | Endereço no `config.h` | Observado | ID lido |
|---|---|---|---|---|
| IMU | MPU6050 (GY-521) | 0x68 | 0x68 (09/10) | WHO_AM_I = 0x68 |
| Barômetro | BMP280 (desde 19/10; antes BME280) | 0x76 | 0x76 (19/10). O BME280 (0x76 em 11/10) foi danificado num curto em 14/10 (ver "Montagem final na caixa") | chip_id = 0x58 (BMP280, sem umidade). O BME280 anterior tinha chip_id 0x60 |
| Magnetômetro | GY-273 com HMC5883L | 0x1E | 0x1E (11/10) | "H43" (HMC5883L legítimo, não o clone QMC5883L) |
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
| 10/10/2026 | A | Montagem 5, firmware `aircraft` com o driver próprio do MPU6050; fio do INT separado do SDA/SCL | 400 kHz | Conversão, taxa (≈ 199,9 Hz, após corrigir a medição), ruído com DLPF, faixa de ±500 °/s e recuperação após soltar SDA e VCC validados; 0 falhas em ~4,5 min | Ver "Driver do MPU6050 no firmware de voo" |
| 10/10/2026 | A | Montagem 5, firmware `aircraft` com calibração (`lib/imu_cal`, NVS) | 400 kHz | Seis posições pelo firmware em ±4 g: \|a\| corrigido de 1,0001 a 1,0015 g; bias do giroscópio no boot aceito só em repouso; persistência na NVS | Ver "Calibração no firmware de voo" |
| 11/10/2026 | A | Protótipo: BME280 e GY-273 recém-soldados, cada um sozinho na protoboard junto com o MPU da montagem 5, em 3V3 | 100 kHz | BME280 em 0x76 (chip_id 0x60); HMC5883L em 0x1E ("H43") | Teste dos dois juntos no barramento ainda pendente |
| 11/10/2026 | A | Protótipo: BME280 na protoboard, longe do ESP (fios mais longos), em paralelo com o MPU | 100 e 400 kHz | BME280 e MPU respondem nas duas frequências; `t` mexendo nos fios do BME280: 10 748 leituras **do MPU** com o BME280 no barramento, 0 falhas | Mostra que o BME280 não atrapalha o barramento (contagem igual à de antes, 10 749–10 751). Não valida o contato dos fios do BME280, que o `t` não lê |
| 11/10/2026 | A | Idem, comando `b` do diagnóstico | 100 kHz | Temperatura, pressão e umidade coerentes; subida de ~1 m detectada | Ver "BME280 no diagnóstico" |
| 11/10/2026 | A | Idem, diagnóstico com soft reset do BME280 no scan | 100 kHz | Reset aceito no boot e num segundo scan com o chip em uso; pressão média idêntica antes e depois do reset (1009,816 hPa); desvio de 2,9 e 3,6 Pa | Comportamento igual ao das sessões anteriores: o filtro já estava desligado nelas |
| 12/10/2026 | A | Protótipo: GY-273 na protoboard junto com o BME280 e o MPU (três módulos no barramento), em 3V3 | 100 e 400 kHz | 3 dispositivos no scan; autoteste do HMC5883L OK nos três eixos; `b` sem falhas; `t`: 3 043 leituras do MPU a 100 kHz e 10 769 a 400 kHz, 0 falhas | Fecha o teste dos módulos juntos no barramento. Ver "HMC5883L no diagnóstico" |
| 12/10/2026 | A | Protótipo, firmware `aircraft` com o BME280 (driver próprio, "indoor navigation") | 400 kHz | Ruído de 1,6 cm; subida de 1 m medida como 0,95–1,0 m; zero rejeita a caixa mexida; MPU sem perdas nem descartes com o BME280 presente; recuperação após queda de alimentação; BME280 ausente no boot sem afetar a atitude | Na queda de alimentação, provável latch-up do BME280 (chip a 40,5 °C). Ver "BME280 no firmware de voo" |
| 13/10/2026 | A | Protótipo religado: GY-273 e BME280 numa protoboard dentro da caixa do MPU (MPU no mesmo lugar); todas as ligações com dois jumpers emendados, sem solda; 3V3 | 100 e 400 kHz | 3 dispositivos no scan; `t`: 3 043 leituras a 100 kHz e 10 756 a 400 kHz, 0 falhas; autoteste do HMC5883L OK; `h` com ruído de ±0,1 µT; `b` com desvio de ~3 Pa; INT a 202,0 Hz | Diagnóstico igual ao de antes da religação. Firmware de voo: 0 falhas parado; 1 `Error -1` isolado mexendo nos fios; teste dos 180°: montagem desviada ~+1° em roll; `z` refeito (módulo em +4,58° / +1,08°), com resíduo de −0,18° na conferência. Um jumper emendado soltou ao manusear a caixa. Ver "Religação dos três módulos" |
| 13/10/2026 | A | Protótipo religado (idem), diagnóstico com o `h` de contagem regressiva e média | 100 kHz | Mapeamento de eixos do GY-273 confirmado: x = X, y = −Y, z = −Z. Giros de 90° na mesa deram passos de 120/75/62° e o Z variou com a proa | Giros irregulares atribuídos ao campo variando com os fios e o ambiente, não ao sensor. Ver "Magnetômetro: mapeamento de eixos e fontes de distorção" |
| 13/10/2026 | A | Idem, testes de controle: fios, ESP32, cabo USB, jumper e emenda avulsos | 100 kHz | Sem nada se mover, o campo repete em 0,2 µT. Fontes isoladas: jumper e emenda a 2 cm < 1 µT; ESP32 girado 1,5 µT; cabo USB 3,2 µT. Autoteste com ganho constante (0,3 %) e 3,32 V estáveis | Variações grandes (até ~23 µT e 79°) mexendo nos fios, sem causa isolada. Decisão: montagem final com tudo fixo |
| 14/10/2026 | A | Idem, powerbank a 30, 20 e 10 cm do GY-273 | 100 kHz | Desligado: < 0,1 µT mesmo a 10 cm. Ligado, alimentando a placa B: 0,8 µT a 10 cm e 1,6 µT encostado | Powerbank irrelevante a 10 cm ou mais, desde que fixo |
| 14/10/2026 | A | **Montagem final** na caixa de papelão (28 × 13 × 10 cm): MPU, GY-273 e BME280 numa mini protoboard no canto da caixa, ESP32 e powerbank na outra metade; jumpers macho-macho com no máximo dois conectores, sem emendas; 3V3 | 100 e 400 kHz | Dois curtos na montagem; o BME280 deixou de responder. MPU e HMC5883L intactos: `t` com 3 040 e 10 460 leituras, 0 falhas; autoteste OK; eixos do MPU confirmados (X asa direita, Y nariz, Z para cima) | MPU ~4° inclinado em roll em relação à caixa, aceito. Ver "Montagem final na caixa" |
| 14/10/2026 | A | Montagem final, firmware `aircraft` com o magnetômetro; calibração `m` (90, 120 e 180 s) | 400 kHz | 25 leituras/s, 0 falhas. Nenhuma calibração aceita: no ar, saltos de 19 µT; na mesa, 24/24 setores, mas resíduo de 7,1 %; o ajuste do firmware bateu com o `mag_fit.py` (diferença < 0,001 µT) | Bancada perto do notebook com gradiente de ~5 µT em 10 cm; cadeira giratória com apoio de 23 cm uniforme. Falta uma extensão USB. Ver "Calibração do magnetômetro: tentativas de 14/10/2026" |
| 15/10/2026 | A | Idem, calibração `m` na cadeira giratória a ~1,5 m do notebook, com extensão USB | 400 kHz | 24/24 setores em 100–120 s, raio 25,9 µT, mas resíduo de 4,6 % com o ajuste de 6 parâmetros; com o elipsoide completo (9 parâmetros), 0,63 % | Lugar adequado; o resíduo vinha de um acoplamento y–z não modelado. Firmware passou para o elipsoide completo. Ver "Calibração do magnetômetro: tentativas de 15/10/2026" |
| 19/10/2026 | A | Montagem final, com o **BMP280** no lugar do BME280; CSB e SDO soltos; 3V3 | 100 e 400 kHz | Scan com 0x1E, 0x68 e 0x76 (chip_id 0x58); autoteste do HMC OK. Firmware `aircraft`: ruído de 1,7 cm entre amostras, subida de ~1 m medida como 0,9–1,1 m, zero recusa a caixa mexida, MPU sem perdas nem descartes, SDA solto e boot sem o barômetro sem afetar a atitude, sem deriva térmica em 8,5 min | Brownouts e um laço de reconfiguração do BMP280 por mau contato no fio de VCC, que sumiram depois de o fio ser ajustado. O laço revelou uma falha do driver, corrigida no mesmo dia. Ver "BMP280 no firmware de voo" |

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

### Driver do MPU6050 no firmware de voo (10/10/2026)

Driver próprio em `firmware/aircraft/lib/mpu6050/`, com acesso direto aos registradores, validado pelo `main.cpp` provisório. Esse `main.cpp` lê uma amostra a cada pulso de "data ready" e imprime um resumo por segundo. Configuração: ±4 g, ±500 °/s, DLPF de 42 Hz (giroscópio) e 44 Hz (acelerômetro), relógio do PLL do giroscópio (`CLKSEL = 1`), 200 Hz, I2C a 400 kHz com timeout de 5 ms. Montagem 5, caixa apoiada, 26,1–26,4 °C. Valores sem calibração, nos eixos do chip.

| Teste | Resultado | Situação |
|---|---|---|
| Conversão de unidades (face para cima) | az = 1,101 g; giroscópio −4,89 / +2,67 / +1,00 °/s, iguais ao diagnóstico em ±2 g e ±250 °/s | ✔ |
| Taxa | ≈ 199,9 Hz com o relógio do PLL: 199 ou 200 pulsos por janela exata de 1 s, com 199 em cerca de 1 a cada 7 janelas (−0,07 %). O relógio interno, no teste `i` do diagnóstico, deu 201,5 Hz (+0,75 %). Os 202 Hz das primeiras versões do `main.cpp` eram erro de medição (ver abaixo). A fusão deve usar o intervalo medido (`micros()`), e não 5 ms fixos | ✔ |
| Amostras descartadas | 2 por janela (1 %): pulsos que chegam enquanto o `main.cpp` provisório imprime o resumo. A amostra é sobrescrita antes de ser lida. Não acontece mais nada além disso: perdidas 0, falhas 0 em ~4 min | ✔ |
| Ruído com o DLPF | 1,9–2,5 mg e 0,03–0,04 °/s (desvio padrão do pior eixo, janelas de 1 s). Sem o DLPF, no diagnóstico: 4–6 mg e 0,10–0,15 °/s | ✔ |
| Faixa de ±500 °/s | Giros rápidos com a mão: picos de 300,9 e 295,8 °/s, sem saturar o giroscópio. Na faixa padrão (±250 °/s), esses giros teriam saturado | ✔ |
| Detecção de saturação | Um impacto ao apoiar a caixa passou de ±4 g no acelerômetro e foi contado em "saturadas" | ✔ |
| Retorno ao repouso após movimentos | Mesmas médias e mesmo bias do giroscópio de antes dos movimentos | ✔ |
| SDA solto por ~3 s | Falhas contadas, barramento reiniciado a cada 5 falhas seguidas; ao recolocar, leituras voltaram sozinhas com a configuração intacta | ✔ |
| VCC solto por ~2,5 s | Ver abaixo | ✔ após correção |

**Falhas de I2C causadas pelo pino INT.** Na primeira versão, com o fio do INT no mesmo chicote do SDA e do SCL e a leitura começando logo no pulso, houve falhas isoladas (`Error -1`), que o diagnóstico nunca tinha mostrado. No `t` do diagnóstico o INT fica desligado.

| Situação | Falhas | Frequência |
|---|---|---|
| INT no chicote, leitura logo após o pulso | 13 em ~72 s | 1 a cada ~5 s (~0,09 % das leituras) |
| INT separado do SDA/SCL (1–2 cm) | 5 em ~190 s | 1 a cada ~38 s |
| INT separado e leitura 100 µs após o pulso (`MPU_INT_READ_DELAY_US`) | 0 em ~4,5 min | nenhuma |

Conclusão: as bordas do pulso do INT (50 µs, a 200 Hz) induziam ruído no SDA e no SCL. A borda de descida caía no meio da transação I2C. Separar o fio reduziu as falhas, e esperar o fim do pulso antes de ler as eliminou. O driver tratou cada falha isolada como previsto: descartou a amostra, sem afetar as médias.

**Queda de VCC e chip reiniciado.** Na primeira versão, ao soltar o VCC da GY-521 o firmware ficou parado em "0 Hz, sem pulsos do INT", sem registrar falhas, e não voltou depois de recolocar o VCC. Causa: a leitura só acontecia a cada pulso do INT. Sem pulsos, nenhuma leitura era tentada, e o chip voltava do reinício em sleep, com o INT desligado. Correção: se passarem `MPU_INT_TIMEOUT_MS` (50 ms) sem pulsos, o firmware chama `checkAlive()`, que relê a configuração. Sem resposta, conta falha e segue o fluxo de recuperação; com a configuração perdida, reconfigura. Resultado do teste repetido: com o VCC solto, as falhas subiram de 50 em 50 ms; ao recolocar o VCC, houve 1 recuperação, com a taxa normal, az = 1,101 g e o mesmo bias de antes. As 541 amostras "perdidas" correspondem aos ~2,7 s sem sensor.

Com a medição de janelas corrigida, o teste foi repetido 3 vezes seguidas, com ~30 s entre as desconexões. Resultados:
- As recuperações subiram 1 → 2 → 3, uma por desconexão.
- O resumo continuou saindo uma vez por segundo, sem rajadas.
- az voltou a 1,102 g em todas as vezes, e as perdidas (504, 602 e 492) correspondem aos ~2,5–3 s sem sensor.
- Na primeira janela depois de cada volta, o ruído ficou alto (74–162 mg, picos de ~100 °/s). Parte disso pode ser movimento da caixa ao recolocar o fio, mas a causa principal é o transitório de partida do giroscópio depois da reconfiguração, confirmado na calibração (ver "Calibração no firmware de voo"). A calibração descarta os primeiros 200 ms depois de cada configuração; a fusão deve fazer o mesmo.
- Logo após cada volta, o bias de gx estava em −4,80 a −4,85 °/s, e voltou a −4,87 °/s em ~5 s, enquanto a temperatura subia de 25,9 para 26,2 °C. O chip esfria quando fica sem alimentação, e o bias acompanha a temperatura. Isso reforça que a calibração do giroscópio no boot deve aceitar a janela só com o chip estável.

**Correção da medição de taxa e de perdas no `main.cpp` provisório.** As primeiras versões tinham dois erros de medição. Nenhum dos dois afeta o driver.

- *Taxa inflada.* A janela seguinte só começava a contar o tempo depois da impressão do resumo (~15–20 ms), mas os pulsos que chegavam durante a impressão entravam na sua contagem. Resultado: ~2 pulsos a mais por segundo, e daí os 202 Hz. Correção: janelas contínuas de exatamente 1 s, com limites dados pelos instantes que a interrupção grava. A comparação dos instantes é feita com sinal, para tratar corretamente a virada do `micros()` e os pulsos atrasados.
- *Leituras repetidas.* A fila guarda os instantes dos pulsos, mas o MPU guarda só a última amostra. Depois de um bloqueio, cada pulso pendente gerava uma leitura da mesma amostra, contada como distinta. As amostras sobrescritas não apareciam em "perdidas", que só detecta pulsos ausentes. Correção: a fila é esvaziada a cada ciclo, só o pulso mais recente gera leitura, e os demais são contados em "descartadas".
- O efeito nos resultados anteriores é pequeno: ~2 duplicatas em 200 por segundo não mudam as médias e reduzem o ruído calculado em menos de 2 %. Os valores de ruído da tabela continuam válidos. A versão corrigida mediu 2,0–2,4 mg e 0,030–0,037 °/s.
- Uma revisão de código encontrou ainda um defeito na primeira versão das janelas: um pulso enfileirado antes de a janela avançar pelo relógio, no caminho de timeout, era interpretado como um intervalo enorme (subtração sem sinal) e disparava ~4 300 fechamentos de janela. O defeito foi corrigido antes de aparecer na bancada.

**Observações.**
- Cada falha gera uma linha `[E][Wire.cpp…]` na serial, que leva ~7 ms a 115200 baud. Em falhas seguidas, esse log, e não o timeout do I2C, é o que limita a taxa do laço.
- Algumas linhas chegaram ao PC com os primeiros 64 caracteres corrompidos (`����`). 64 bytes é o tamanho de um pacote USB, o que indica o caminho USB até o PC (cabo, porta ou driver), e não o firmware. Os dados do MPU não são afetados.

### Calibração no firmware de voo (10/10/2026)

Firmware `aircraft` com a calibração de [calibracao.md](calibracao.md): lógica pura em `lib/imu_cal` (12 testes `native`), NVS em `lib/imu_store` e comandos `c`, `6`, `g` e `r` pelo `main.cpp` provisório. Montagem 5, caixa apoiada, 26,4–26,9 °C, MPU em ±4 g e ±500 °/s com DLPF.

**Seis posições (comando `6`).** Todas as medidas foram aceitas na primeira tentativa.

| Posição | Média medida (g) | \|a\| corrigido |
|---|---|---|
| Face para cima | +0,0173 +0,0314 +1,1013 | 1,0005 |
| Face para baixo | −0,0063 −0,0487 −0,9161 | 1,0015 |
| Nariz para cima | +1,0217 +0,0060 +0,1070 | 1,0001 |
| Nariz para baixo | −0,9886 −0,0354 +0,1191 | 1,0010 |
| Asa direita para baixo | −0,0073 +1,0017 +0,0499 | 1,0012 |
| Asa esquerda para baixo | +0,0340 −1,0008 +0,1076 | 1,0003 |

| Eixo | Offset (±4 g, firmware) | Montagem 5 (±2 g, diagnóstico) | Escala (firmware) | Montagem 5 |
|---|---|---|---|---|
| X | +16,5 mg | +16,5 mg | +0,51 % | +0,51 % |
| Y | +0,5 mg | +0,8 mg | +0,13 % | +0,13 % |
| Z | +92,6 mg | +90,7 mg | +0,87 % | +0,86 % |

- Os coeficientes medidos em ±4 g pelo firmware coincidem com os medidos em ±2 g pelo diagnóstico, com diferença de até 1,9 mg e 0,01 %. A conversão de unidades do driver e o cálculo de `lib/imu_cal` estão confirmados com dados reais.
- **Comando `c` depois de gravar:** \|a\| corrigido de 0,9996 g (face para cima), 1,0000 g (nariz para cima) e 1,0003 g (asa esquerda para baixo), dentro de 0,98–1,02 g. Com a face para cima, ay corrigido = +0,035 g (~2°): é o desalinhamento do módulo em roll, já registrado, a ser tratado pelo zero de nível. O giroscópio corrigido ficou entre −0,033 e +0,011 °/s.
- **Reinício:** os coeficientes voltaram da NVS ("acelerômetro calibrado"), sem nova calibração.

**Bias do giroscópio no boot.**

| Situação | Resultado |
|---|---|
| Primeira versão, caixa parada | A primeira janela **sempre** era recusada como "placa em movimento", com desvio de exatamente 3,382 °/s em dois boots seguidos. A segunda janela era aceita |
| Com descarte de 200 ms após configurar o MPU (`CAL_MPU_SETTLE_MS`), caixa parada | Primeira janela aceita em dois boots: desvio de 0,033 °/s, bias −4,913 / +2,654…2,659 / +1,011…1,013 °/s |
| Boot mexendo a caixa | 6 janelas recusadas (desvios de 9 a 91 °/s); `c` mostrou "PROVISÓRIO (bias salvo de antes)"; ao parar, o bias foi aceito |
| Comando `g` parado / mexendo | Parado: recalibrou. Mexendo (43 °/s): recusado, com o bias anterior mantido |

- **Transitório de partida do giroscópio.** O desvio idêntico nas primeiras janelas (3,382 °/s) mostra que não se tratava de movimento, e sim de um transitório logo depois da configuração do MPU. Ele explica também a primeira linha de todos os resumos do driver (pico de ~60 °/s e ruído de ~5,5 °/s, sempre iguais), que antes tinha sido atribuída à caixa sendo ajeitada. Essa explicação estava errada.
- **Limite de repouso revisto.** No teste "boot mexendo", a janela aceita no fim teve desvio de 0,122 °/s (caixa ainda se acomodando), contra 0,033 °/s parada, e o bias saiu com gz = +1,066 °/s, contra +1,011 °/s: ~0,055 °/s de erro, ou ~3°/min de deriva em yaw. O limite de 0,4 °/s, definido antes do DLPF, foi reduzido para 0,1 °/s (~3× o ruído em repouso), e o calibracao.md foi atualizado. O mesmo limite vale para as medidas das seis posições.
- **Revisão de código e nova calibração (11/10/2026).** Uma revisão encontrou três defeitos, corrigidos antes de aparecerem na bancada:
  - o prazo de descarte voltava a ser comparado depois de vencido, o que bloquearia a calibração após ~36 min ligada;
  - uma calibração reprovada na validação ainda podia ser gravada;
  - a gravação na NVS em várias chaves podia ficar parcial.

  Agora cada calibração é um registro único e atômico, e a calibração antiga, no formato anterior, foi descartada. A nova calibração, com o limite de 0,1 °/s, recusou duas medidas da posição 5 por movimento (0,129 e 0,173 °/s) e aceitou a terceira. Resultado: offset +16,8 / −2,4 / +92,5 mg e escala +0,52 / −0,19 / +0,79 %, com |a| corrigido de 1,0005 a 1,0065 g. O Y mudou ~3 mg e 0,3 % em relação à calibração anterior, porque a posição 5 ficou ~6° inclinada. A inclinação não aparece no valor bruto (az = −0,020 g), só depois de descontar o offset de Z (−0,11 g corrigido). Depois do reinício, `c` mostrou os mesmos coeficientes carregados da NVS e |a| corrigido = 1,0004 g.
- **Bias aceito com a caixa ainda se acomodando.** Depois desse reinício, a primeira janela foi recusada (0,113 °/s), e a segunda foi aceita com desvio de 0,065 °/s (cerca de 2× o ruído em repouso), dando gx = −4,860 °/s. Em boots com a caixa parada, gx deu −4,786 a −4,820 °/s, a 25,7–25,9 °C. Logo depois, `c` mostrou gx corrigido = +0,078 °/s com a caixa parada, um erro bem acima dos ±0,03 °/s das janelas limpas. Ou seja, uma janela que passa no limite de 0,1 °/s ainda pode carregar um erro de bias de ~0,08 °/s.
- **Desvio não indica bem a qualidade do bias.** No boot seguinte, com a caixa parada, mas digitando no teclado sobre a mesma mesa, as janelas tiveram desvio de 0,094 e 0,080 °/s (`g`), com bias gx −4,807 e −4,817 °/s, e o `c` logo depois mostrou gx corrigido = −0,002 °/s. Janelas com mais desvio (vibração) deram o bias certo, e a janela com menos desvio (0,065 °/s, acomodação) deu o bias errado. Decisão: o critério principal passou a ser a confirmação por duas janelas seguidas com médias diferindo menos de 0,03 °/s, e o limite de desvio subiu para 0,15 °/s, como filtro de movimento. Detalhes em [calibracao.md](calibracao.md).
- **Confirmação por duas janelas na bancada (11/10/2026).**

  | Teste | Resultado |
  |---|---|
  | Boot parado | Diferença entre janelas de 0,006 °/s; g corrigido entre −0,005 e +0,000 °/s |
  | Toques leves na mesa | Desvio de 0,22–0,40 °/s, recusado pelo filtro de movimento; ao parar, aceito com diferença de 0,005 °/s, e g corrigido ≤ 0,009 °/s |
  | Mexendo desde o boot e depois apoiando num canto | Recusado enquanto se mexia; aceito com diferença de 0,030 °/s, no limite. O bias (−4,869 °/s) coincidiu com o de boots parados à mesma temperatura (−4,871 e −4,879 °/s). O `c` logo depois mostrou gx corrigido = −0,035 °/s, provavelmente porque a caixa ainda assentava naquele segundo (o `c` mostra dados ao vivo) |
  | Boot e `g` parados | Diferenças de 0,013 e 0,006 °/s; g corrigido ≤ 0,005 °/s |

- **Deriva térmica do bias do giroscópio.** Bias de gx aceitos com a caixa parada, em 10 e 11/10/2026:

  | Temperatura do chip | Bias gx (°/s) |
  |---|---|
  | 25,7 °C | −4,786 |
  | 25,9 °C | −4,820 |
  | 26,1–26,2 °C | −4,807 a −4,837 |
  | 26,3 °C | −4,846 a −4,857 |
  | 26,5 °C | −4,855 a −4,863 |
  | 26,6 °C | −4,869 a −4,879 |

  A inclinação é de ~−0,1 °/s por °C. Numa única sessão, o chip esquentou 0,5 °C e o bias andou ~0,06 °/s, um efeito maior que o erro de acomodação corrigido acima. O aviso de temperatura foi reduzido de 10 °C para 1 °C, e a correção da deriva ficou como pendência para a etapa da fusão (ver [calibracao.md](calibracao.md)). Na mesma faixa, gy ficou entre +2,650 e +2,682 °/s e gz entre +0,996 e +1,025 °/s (com um caso de +1,066 °/s a 25,7 °C), sem uma tendência clara com a temperatura.
- **Linhas corrompidas na serial.** Com `--echo`, os 64 caracteres corrompidos (`����`) passaram a aparecer logo depois de cada tecla digitada. Isso reforça que o problema está no caminho USB/PC, e não no firmware.

### Fusão no firmware de voo: primeira sessão (11/10/2026)

Filtro complementar de [fusao.md](fusao.md), com τ = 1 s. Montagem 5, caixa apoiada na bancada, 26,7–26,8 °C. Bias do giroscópio aceito no boot (diferença entre janelas de 0,004 e 0,020 °/s).

**Calibração de montagem (`z`), primeira execução:**

| Medida | Roll | Pitch |
|---|---|---|
| Posição 1 (sem correção) | +2,41° | +0,44° |
| Posição 2, girada 180° (sem correção) | +4,73° | +0,88° |
| Desalinhamento do módulo (resultado) | +3,57° | +0,66° |
| Inclinação da mesa, vista na posição 1 (resultado) | −1,16° | −0,22° |

- **O resultado é coerente com a geometria.** Depois de gravar, com a caixa ainda na posição 2, o filtro leu +1,21° e +0,23°. É a inclinação da mesa com o sinal trocado, como esperado na posição girada, dentro de 0,05°.
- **O desalinhamento em roll (3,6°) ficou acima dos ~2,4° estimados na montagem 5.** Aquela estimativa vinha da comparação entre a face para cima e a face para baixo, sem separar a inclinação da mesa. A conferir: com a caixa de volta na posição 1, o filtro deve ler ≈ −1,16° e −0,22°, e o inclinômetro na borda de referência deve indicar ~1,2° de roll na mesa.
- Antes da primeira gravação, o boot mostra `[E][Preferences.cpp:50] begin(): nvs_open failed: NOT_FOUND`. É o namespace `att_level`, que ainda não existia, aberto para leitura. A mensagem não volta depois da gravação.

**Repouso (fluxo `a` por ~3 s, resumo `r` por ~15 s):**

| Grandeza | Resultado |
|---|---|
| Atitude do filtro | roll +1,20 a +1,24°, pitch +0,22 a +0,24°; desvio no `r` de 0,00–0,01° |
| Atitude só pelo acelerômetro | roll +1,05 a +1,39°, pitch +0,06 a +0,38° (±0,15°) |
| Atitude só pelo giroscópio | +1,23° → +1,26° em ~3 s: bias residual de ~0,01 °/s |
| \|a\| corrigido e peso | 0,996–1,005 g; peso 1,00 em todas as linhas |
| Custo do `update()` | máximo de 184–212 µs (~4 % do período de 5 ms) |
| Falhas, saturações, reinícios | nenhum |

**Associação pulso → quadro, primeira versão.** Com o critério só por atraso (limite de 1 ms), cada linha do `a` gerou 1 quadro "ambíguo" e 1 descartado: 18 intervalos integrados por 100 ms, com dt médio de 5,56 ms e máximo de 15 ms. A impressão bloqueava ~10 ms, e o último pulso chegava 2–4 ms antes da leitura. O quadro era desse pulso, mas passava do limite e era recusado. Correções feitas em seguida (ver [fusao.md](fusao.md)) e validadas na sessão seguinte (abaixo):
- ambíguo passou a ser só um pulso mais novo chegando antes do fim da leitura, com uma rede de segurança de 4 ms;
- a linha do `a` passou a mostrar o custo médio do filtro;
- a serial ganhou um buffer de transmissão de 1 kB. Sem ele, o `r` causava 3 amostras descartadas por segundo.

### Fusão no firmware de voo: roteiro de validação (11/10/2026)

Roteiro do [fusao.md](fusao.md), com a guarda nova de "ambíguos" e o buffer de transmissão da serial. Montagem 5, montagem calibrada (`z` da primeira sessão), um log contínuo de ~28 min (`device-monitor-261011-165319.log`). Chip a 24,6 °C no boot e 26,4 °C no fim; o bias de gx foi de −4,677 °/s (24,6 °C) para −4,803 °/s (26,2 °C, comando `g` antes do teste de giros), ou seja, −0,08 °/s por °C.

**Referências.**
- Inclinômetro do celular, com resolução de 1°.
- Na mesa, ao longo das asas: 1° e 359° (= −1°) com o celular girado 180°. A leitura trocar de sinal é o esperado: a mesa tem ~1° em roll, e o erro do celular é ~0°. Isso confirma, de forma independente, a inclinação de 1,16° obtida pelo `z`.
- Ao longo do nariz: 0° nas duas orientações, ou seja, mesa abaixo de ~0,5° em pitch. O filtro leu −0,1 a −0,4°.
- Calço de 2,5 cm. A caixa mede 15,5 cm (nariz–cauda) por 10 cm (asa–asa), o que dá asin(2,5/15,5) = 9,28° em pitch e asin(2,5/10) = 14,48° em roll.

| Teste | Resultado | Situação |
|---|---|---|
| Temporização (todo o fluxo `a`, ~4 000 linhas) | dt entre 4,97 e 5,04 ms; 0 ambíguos, 0 descartados e 0 reinícios; atraso pulso → leitura de até 147 µs; resumo `r` sem descartadas | ✔ |
| Custo do `update()` | médio de 67 µs (1,3 % do período), máximo de 237 µs | ✔ |
| Nivelado na posição 1 do `z` | roll −1,06 a −1,40° e pitch −0,11 a −0,33° em reposições diferentes da caixa (esperado: −1,16° e −0,22°). A variação de ±0,2° vem do apoio da caixa | ✔ |
| Sinais | asa direita para baixo: roll até +29° (positivo); nariz para cima: pitch até +26° (positivo) | ✔ |
| Ângulos conhecidos (calço de 2,5 cm) | ver a tabela abaixo: escala dentro de 0,7 % da geometria | ✔ |
| Deriva parada, 312 s | filtro: roll −1,20 a −1,12° (0,08° pico a pico) e pitch −0,40 a −0,28° (0,12° pico a pico); meta < 0,2° | ✔ |
| Deriva só do giroscópio, mesmos 312 s | roll −10,1° (−0,032 °/s em média, acelerando com o aquecimento de 24,8 para 25,2 °C) e pitch −4,0° (−0,013 °/s) | referência |
| Giros (~37 °/s) | roll até +31,7° e pitch até +39,6°, segurando ~4 s; filtro acompanhou sem atraso visível, sem DEGRADADO; o só-giroscópio voltou a 0,12° do filtro depois das duas excursões | ✔ |
| Aceleração linear (deslizar a caixa) | só acelerômetro: desvio de até 19,6° em roll e 25,5° em pitch; filtro: até 1,34° e 1,26°, com o peso caindo a 0; voltou à linha de base em ~3 s | ✔ (experimental) |
| Recuperação (VCC solto ~8 s) | 127 falhas, 1 recuperação, 1 576 amostras perdidas; o transitório de partida (pico de 59 °/s no `r`) ficou fora da fusão; o filtro reinicializou e convergiu ao acelerômetro em ~3 s | ✔ |

Conclusões:

- **A correção da guarda e o buffer da serial funcionaram.** Não houve nenhum quadro ambíguo, nenhuma amostra descartada e nenhum reinício em ~7 min de fluxo a 10 Hz com o resumo ligado. O dt ficou sempre entre 4,97 e 5,04 ms.
- **O erro estacionário b·τ se confirmou na placa.** Na deriva parada, a diferença entre o filtro e a média do acelerômetro foi de −0,02° no primeiro minuto e de −0,06° no último. O bias residual de roll medido pelo só-giroscópio foi de −0,02 °/s no início e de −0,05 °/s no fim, e com τ = 1 s isso dá exatamente esses erros. O bias de gx, ao derivar com o aquecimento, levaria o horizonte a andar ~10° em 5 min sem o acelerômetro; com o filtro, o efeito ficou em 0,06°. Isso sustenta a decisão de adiar a compensação térmica (calibracao.md).
- **A tendência de pitch na deriva (−0,05° em 5 min) é real, e não do filtro.** A média do acelerômetro andou o mesmo valor (−0,305 → −0,354°), provavelmente pela acomodação da caixa ou pelo aquecimento.
- **Aceleração linear:** deslizando a caixa, o acelerômetro sozinho erraria até ~25°; o filtro errou ~1,3°. O resultado depende da intensidade e da duração do movimento, como previsto em fusao.md.
- **Inicialização a partir de uma única amostra.** Depois da recuperação, o filtro começou em −1,25° de roll, contra −0,95° da média do acelerômetro, e levou ~3 s (≈ 3τ) para convergir. O erro de 0,3° é do ruído de uma amostra (~0,1°) somado a um provável movimento da caixa ao recolocar o fio. Corrigido em seguida: a inicialização passou a usar a média de 20 amostras seguidas (0,1 s), com a caixa sem girar (|giroscópio| < 5 °/s), ver [fusao.md](fusao.md).
- **Inicialização pela média, validada (11/10/2026, log `device-monitor-261011-173745.log`).** Caixa parada sobre o calço, em roll ~+15°; VCC solto por ~4 s (62 falhas, 783 amostras perdidas, 1 recuperação). Antes da queda, o filtro estava em +15,11° de roll. Depois da volta, apareceu uma linha "aguardando referência" (média em andamento), e a primeira linha inicializada já marcou +15,14°. O valor final foi +15,16° ± 0,01°, sem a convergência de ~3 s da versão de uma amostra. O pitch voltou para −0,36°, contra −0,37° no fim. O transitório de partida (pico de 59,6 °/s no `r`) ficou fora da fusão.
- **Durante a falta do sensor, o fluxo `a` fica em silêncio.** As linhas só saem quando há uma amostra válida, então o estado "aguardando referência" não aparece nesse caso. O resumo `r` mostra as falhas e o "sem pulsos do INT".
- **Ângulos conhecidos.** O calço de 2,5 cm foi posto sob cada um dos quatro lados. O nariz está no log acima; os outros três estão no log `device-monitor-261011-172939.log`. Cada valor é a média de 1 s no resumo `r`, ao longo de 10–19 s parado, com desvio de 0,00–0,02°.

  | Calço sob | Filtro | Geometria (asin(h/L)) |
  |---|---|---|
  | nariz | pitch +9,29° | +9,28° |
  | cauda | pitch −9,35° | −9,28° |
  | lado da asa esquerda (asa direita para baixo) | roll +13,90° | +14,48° |
  | lado da asa direita (asa esquerda para baixo) | roll −15,27° | −14,48° |

  Cada leitura soma o ângulo do calço com a inclinação da mesa e o apoio da caixa naquele ponto. Para separar os dois, usa-se o par de lados opostos:
  - **meia diferença** = ângulo do calço: pitch 9,32° (geometria 9,28°, +0,4 %) e roll 14,59° (geometria 14,48°, +0,7 %). As diferenças equivalem a ~0,6–0,7 mm em L, dentro da precisão com que se mede a caixa e o ponto de apoio do calço;
  - **meia soma** = inclinação da base: −0,03° em pitch e −0,69° em roll. O roll fica entre os −0,1 e −1,4° lidos com a caixa nivelada em reposições diferentes, e perto do ~1° de roll da mesa visto pelo celular.

  A caixa também girou um pouco no outro eixo ao ser apoiada no calço: pitch de −0,09° e −0,65° nos dois lados das asas, roll de −2,37° e −1,37° no nariz e na cauda. Isso é apoio, e não erro do filtro: as leituras ficaram estáveis em 0,01°. Conclusão: a escala da atitude está correta dentro de 1 %, e o erro absoluto de cada leitura fica dentro de ~0,5°, limitado pelo apoio da caixa e pela mesa.
- **Log corrompido no PC.** O log `device-monitor-261011-172907.log` tem 2 MB e 75 mil linhas repetidas (o início do boot), gravadas em menos de um minuto. A 115200 baud, a placa transmite no máximo ~690 kB por minuto, então a repetição vem do monitor ou do driver USB no PC, e não do firmware. É o mesmo caminho das linhas corrompidas já registradas. O log seguinte, aberto 32 s depois, saiu normal.

### BME280 no diagnóstico (11/10/2026)

Protótipo, BME280 na protoboard, comando `b` (1 leitura por segundo, oversampling x1, sem filtro IIR). Sala a ~23 °C, 1010,1 hPa, UR ~41 %. Objetivo: confirmar que a solda não afetou a medição. Logs `device-monitor-261011-230655.log` e `device-monitor-261011-231431.log`.

| Teste | Resultado |
|---|---|
| Repouso, ~30 s (duas sessões) | T estável em ±0,1 °C; UR em ±0,2 %; desvio padrão da pressão de ~3,1–3,3 Pa (≈ 0,27 m) com oversampling x1 e sem filtro. Como referência: é próximo do ruído RMS típico de 3,3 Pa que a Bosch informa para x1 e filtro desligado (datasheet, seção 3.5.1, tabela 7, "weather monitoring"); aquela tabela usa uma amostra por minuto, e o diagnóstico, uma por segundo |
| Placa levantada ~1 m por ~35 s, mão longe do chip | Δp médio de −12,3 Pa no patamar contra −1,4 Pa antes e −4,0 Pa depois: subida de −9,6 a −10,9 Pa, ou seja, **0,8 a 0,9 m**. A resposta foi imediata e a temperatura não mudou (23,1 °C), então o sinal é de pressão, não térmico |
| Respiração perto do sensor | UR de 43 % para 61 % em 2 s e volta em ~15 s |
| Dedo no chip por ~14 s | T de 24 para 30 °C; ao retirar, volta devagar e a UR cai para 35 %, abaixo do ambiente, porque é relativa à temperatura do chip ainda quente |
| Falhas | Nenhuma falha de leitura do BME280 |
| Soft reset no scan (log `device-monitor-261011-233144.log`) | Dois fluxos de ~30 s, um depois do boot e outro depois de um novo scan (`s`): médias iguais (1009,816 hPa) e desvio de 2,9 e 3,6 Pa, na mesma faixa das sessões sem reset. Confirma que os resultados anteriores foram obtidos com o filtro desligado |

Observações para a etapa do BME280 no firmware de voo:

- **Pressão sensível à temperatura do chip.** Com o dedo (+6 °C), o Δp subiu +10 a +18 Pa, um falso "desceu ~1 m", e voltou com o resfriamento: ~1,5 Pa/°C, ou ~13 cm/°C. Por isso o BME280 deve ficar longe do calor do ESP32 e a altitude só deve ser zerada depois que a placa estabilizar termicamente.
- **Zerar com uma única leitura erra.** Na primeira sessão, a leitura de referência ficou 4 Pa abaixo da média do repouso, o que deu Δh ≈ −0,33 m com a placa parada. O zero deve ser a média de várias leituras.
- **Deriva lenta.** O repouso depois da subida ficou 2,5 Pa (~0,2 m) abaixo do repouso anterior em ~1 min. Pode ser a placa recolocada em outra altura ou variação da pressão ambiente (portas, ventilação), e limita a precisão da altitude relativa em sessões longas.
- **O ruído de ~0,27 m é alto para mostrar alturas de bancada.** Oversampling maior e o filtro IIR do chip devem reduzi-lo; a escolha fica para o plano da etapa.
- **Lixo na serial (`����`).** Apareceu três vezes nas duas sessões, sempre ao mexer na placa, às vezes cortando o começo de uma linha. Não houve reboot nem falha de leitura de I2C. Suspeita, não confirmada: problema no caminho serial/USB (cabo ou conector), correlacionado ao movimento da placa. Prender o cabo USB e ver se some.

### HMC5883L no diagnóstico (12/10/2026)

Protótipo, GY-273 solto na protoboard, junto com o BME280 e o MPU. Comandos `x` e `h` (modo contínuo a 15 Hz, média de 8, ganho de 1090 LSB/Ga). Objetivo: confirmar que a solda não afetou a medição. Log `device-monitor-261012-001245.log`.

**Autoteste (`x`, duas vezes seguidas):**

| Eixo | +pol (LSB) | −pol (LSB) | Campo interno | Faixa aceita |
|---|---|---|---|---|
| X | 487,5 / 488,0 | −478,0 / −478,2 | 123,8 / 123,9 µT | 62,3 a 147,4 µT: OK |
| Y | 449,2 / 449,8 | −457,5 / −458,5 | 116,2 / 116,4 µT | OK |
| Z | 465,8 / 466,0 | −457,2 / −457,5 | 118,3 / 118,4 µT | OK |

- Os três eixos passaram, com repetição de 0,2 µT entre as duas execuções. A parte de medição do chip está íntegra.
- Pelo critério adotado depois (cada polarização validada separadamente), os mesmos valores também passam: positivo de +115,2 a +125,0 µT e negativo de −117,2 a −122,6 µT, todos dentro de 243 a 575 LSB em módulo (≈ 62,3 a 147,4 µT).
- Z ficou ~10 % acima do nominal do datasheet (~108 µT), dentro da faixa.
- Conferência do critério por polarização na placa (log `device-monitor-261012-002828.log`): positivo de +115,4 a +125,4 µT e negativo de −117,3 a −122,4 µT, todos OK, a menos de 0,4 µT das execuções anteriores. Logo depois, o `h` mostrou \|B\| = 21,5 µT com ruído de ±0,1 µT. O fluxo é consistente com a restauração da configuração normal. A diferença para os 23,7 µT anteriores vem do módulo em outra orientação (ângulo XY de 106° contra 167°) sem calibração.
- Com a polarização ligada, o chip faz duas aquisições: a primeira só com o campo externo e a segunda com o campo externo mais o da bobina (~1,1 Ga). A primeira é subtraída da segunda, e só a diferença vai para os registradores de dados (datasheet do HMC5883L, "Self Test Operation"). Os dados são compatíveis com isso: (positivo + negativo)/2 deu só ±5 LSB (~1 µT) por eixo, enquanto o campo ambiente medido logo depois era X ≈ −22 µT (~−86 LSB nesse ganho).

**Fluxo (`h`):**

| Teste | Resultado |
|---|---|
| Repouso, ~9 s | X −22,0, Y +5,0, Z +7,1 µT; \|B\| = 23,7 µT, compatível com o campo da Terra no Brasil (~23–25 µT). Ruído de ±0,1 µT, cerca de 1 LSB |
| Módulo girado com a mão | Y de −15 a +13 µT e Z de +9 a −19 µT, com X entre −18 e −26 µT: o giro foi principalmente em torno de X, não deitado na mesa. Os três eixos respondem à orientação. O \|B\| variou de 22 a 33 µT, o que é esperado sem calibração e com a mão e os objetos em volta |
| Chave de fenda perto do sensor | X de −22 para −7 µT e \|B\| de 24 para 9 µT, sem saturar; voltou a \|B\| = 23,9 µT ao afastar (Y e Z um pouco diferentes: o módulo mexeu de posição) |
| Falhas | Nenhuma falha de leitura nem saturação |

Observações para a etapa do magnetômetro:

- A volta completa com o módulo deitado na mesa não foi feita. Ela fica para a calibração de hard/soft iron, que também precisa de giros em todas as orientações e deve ser feita no modelo final.
- Com 1 LSB de ruído, a resolução não limita a proa: o erro virá da calibração e das distorções do ambiente.

### BME280 no firmware de voo (12/10/2026)

Protótipo com o MPU, o BME280 e o GY-273 no barramento a 400 kHz. Firmware `aircraft` com o driver próprio do BME280, no perfil "indoor navigation" sem umidade (ver [barometro.md](barometro.md)). Logs `device-monitor-261012-010217.log`, `-011524.log`, `-012159.log` e `-012809.log`.

| Teste | Resultado |
|---|---|
| Boot e zero | Zero aceito em ~4 s, com desvio de 0,36 a 0,63 Pa e 50–51 leituras, junto com a calibração do giroscópio |
| Ruído, sala quieta (48 s, `b`) | Diferença entre leituras seguidas: 1,6 cm de desvio (estimado como desvio das diferenças ÷ √2), praticamente o ruído RMS de 1,7 cm do datasheet. Somada, a variação lenta da sala dá desvio de 9 cm e médias de 1 s entre −0,18 e +0,16 m |
| Subida de ~1 m | Patamar de 0,95–1,0 m. Subida de 10 % a 90 % em ~4 s e descida em ~3 s: esses tempos são dominados pelo movimento da mão e não medem o filtro IIR (esperado: ~1,3 s de 10 % a 90 %) |
| Zero com a caixa mexida (`0`) | Janelas recusadas com desvio de 1,94 a 4,03 Pa; aceita quando a caixa parou |
| Convivência com o MPU (`r` por vários minutos, com e sem `b`) | Perdidas 0, descartadas 0, falhas 0 nos dois sensores |
| Aviso de temperatura | Apareceu com ΔT de 1,0 °C |
| BME280 ausente no boot | Mensagem de indisponível, nova tentativa a cada 5 s, atitude e giroscópio normais. Ao religar: configurado e zero aceito a 23,9 °C |
| Queda de alimentação | Falhas subindo e `FALHANDO`; ao voltar, reconfiguração com o mesmo p₀ (`recuperações 2`) |

**Alimentação parasita (primeira tentativa).** Soltar só o VCC do BME280 não o desligou. O chip continuou medindo, sem falhas, alimentado pelos pull-ups dos outros módulos através dos diodos de proteção dos pinos de I2C. O roteiro do [barometro.md](barometro.md) foi corrigido.

**Provável latch-up (segunda tentativa).** O módulo foi religado com o SDA e o SCL antes do VCC. Em seguida:
- o barramento travou por ~3 s (`Error 263`, timeout), e o MPU caiu para 4–10 Hz, com 286 amostras perdidas;
- quando o BME280 voltou a responder, estava a **40,5 °C**, contra 24 °C antes. Esse calor só pode ter vindo do próprio chip;
- o chip esfriou para 32 °C em 15 s e para ~25 °C em alguns minutos;
- a pressão lida ficou ~1 hPa abaixo da anterior. Nas sessões anteriores, a pressão vinha caindo só ~0,7 hPa por hora;
- durante o resfriamento, a altitude variou ~0,8 m por °C, contra os ~13 cm/°C do coeficiente térmico do datasheet.

A sequência (pinos de I/O energizados antes da alimentação, chip aquecendo sozinho e segurando o barramento) é compatível com latch-up. A saúde do sensor está sendo verificada numa sessão nova, com o chip frio. Decisão: **não repetir a queda de alimentação no hardware.** O caminho de recuperação do firmware já foi validado.

**Saúde do BME280 depois do evento (log `device-monitor-261012-013609.log`, sem mexer nos fios).**
- **Boot:** chip a 23,9 °C, a temperatura da sala, e zero aceito com desvio de 0,38 Pa.
- **`r` por ~8,5 min:** temperatura do chip estável (ΔT de 0 a −0,3 °C) e MPU sem perdas, descartes nem falhas.
- **Deriva da altitude:** de +0,11 para cerca de −0,7 m, ou seja, ~10 Pa de aumento da pressão. A deriva térmica explicaria só ~4 cm. O resto é compatível com variação da pressão ambiente (~0,7 hPa por hora), e sem uma referência independente não dá para separar as duas coisas.
- **Duas subidas rápidas:** degraus de **0,71 m e 0,89 m**, medidos como a média do patamar menos a média antes da subida. De 10 % a 90 % em ~1,2–1,5 s, compatível com o filtro IIR 16 (esperado ~1,3 s). A diferença em relação a 1 m pode ser a altura real da subida, que não foi medida.

Conclusão: temperatura, ruído, tempo de resposta e comportamento térmico normais. Não houve como medir uma diferença de altura conhecida. A sensibilidade fica validada só de forma qualitativa, com subidas feitas à mão (0,71 a 1,0 m em três rodadas, sem trena). O sensor foi mantido.

**Defeito do firmware encontrado.** Com o BME280 fora do barramento, cada uma das 25 tentativas por segundo imprime um erro do I2C (`Error -1`) e atrasa o laço. Resultado: o MPU descartou de 2 a 25 amostras por segundo, contra 0 com o BME280 presente. Correção: enquanto o BME280 está falhando, o firmware tenta só a cada `BARO_FAIL_RETRY_MS` (1 s). Validação (log `device-monitor-261012-015754.log`):
- **Teste:** só o SDA do BME280 solto por ~17 s, com o chip alimentado.
- **Falhas:** 5 seguidas a 25 Hz e depois exatamente 1 por segundo, com um `Error -1` por segundo. Total de 22.
- **MPU:** perdidas, descartadas e falhas em 0 durante todos os 34 s do resumo, inclusive com o BME280 fora.
- **Ao recolocar o SDA:** `baro: ok` sem reconfiguração (recuperações 0), com a pressão contínua (1007,74 hPa antes e depois).

### Religação dos três módulos (13/10/2026)

Toda a fiação do ESP A foi refeita com o MPU6050, o BME280 e o GY-273. O GY-273 e o BME280 ficam numa protoboard dentro da caixa do MPU. O MPU não saiu do lugar. A hipótese inicial era que a calibração de montagem (`z`) continuaria valendo, mas os testes dos 180° mostraram um desvio de ~1° em roll, e o `z` foi refeito (ver abaixo). Cada ligação é feita com dois jumpers emendados, sem placa nem solda. Isso dá mais um ponto de contato em série em cada fio, por isso o teste de contato é o mais importante. Pinagem: SDA no GPIO 21, SCL no GPIO 22, INT do MPU no GPIO 19, todos em 3V3.

Diagnóstico `i2cscan`, logs `device-monitor-261013-132044.log`, `-132643.log` e `-132737.log`:

| Teste | 13/10 | Antes da religação | Situação |
|---|---|---|---|
| Scan (três boots) | 0x1E "H43", 0x68 WHO_AM_I 0x68, 0x76 chip_id 0x60 | igual (12/10) | ✔ |
| `t` a 100 kHz, mexendo nos fios | 3 043 leituras, 0 falhas, 0 suspeitas | 3 043 | ✔ |
| `t` a 400 kHz, mexendo nos fios | 10 756 leituras, 0 falhas, 0 suspeitas | 10 749–10 769 | ✔ |
| `x` (duas vezes) | Campo interno X 122,9–123,2, Y 115,5–115,7, Z 117,5–117,8 µT; todas as polarizações OK | X 123,9, Y 116,4, Z 118,4 µT | ✔ |
| `h` parado, 27 leituras | X +23,7, Y +12,5, Z +0,7 µT; \|B\| = 26,8–27,0 µT; ângulo XY 27,5–28,0° | ruído de ±0,1 µT | ✔ |
| `b` parado (duas sessões) | 1007,71–1007,82 hPa e depois 1007,61–1007,74 hPa; desvio de 2,7 e 3,0 Pa | 2,9–3,6 Pa | ✔ |
| `i` (quatro vezes) | 404 pulsos em 2 s = 202,0 Hz | 201,5 Hz (403 pulsos) | ✔ |

- **Sem falhas no diagnóstico.** O `t` repetiu a contagem de antes nas duas frequências, mexendo nos fios. Uma falha teria consumido o tempo de timeout e baixado a contagem. A conclusão vale só para esse teste: depois, o firmware de voo teve um `Error -1` com a caixa sacudindo, e uma emenda soltou ao manusear a caixa (ver abaixo).
- **Os jumpers emendados não causaram falhas** no teste de contato. Mesmo assim, eles são o ponto mais provável de mau contato futuro. Se aparecer `Error -1`, comece por eles.
- **INT: 202,0 Hz contra 201,5 Hz.** É 1 pulso de diferença na janela de 2 s, o que está dentro da margem da contagem. O diagnóstico usa o oscilador interno.
- **Autoteste do HMC5883L até 1 µT abaixo de 12/10**, a 24,5 °C. Diferença normal entre sessões: a medição do chip continua íntegra.
- **Magnetômetro na caixa:** \|B\| de ~26,9 µT, contra 23,7 µT solto na protoboard em 12/10. Sem calibração, o módulo agora fica perto do MPU, dos jumpers e em outra orientação, então a diferença é esperada. A calibração de hard/soft iron deve ser feita com o módulo já na posição final (ver [magnetometro.md](magnetometro.md)).
- **Pressão:** na primeira sessão o Δp desceu ~5 Pa em 18 s; na segunda não houve deriva. É a mesma variação lenta da pressão ambiente já registrada, e o ruído ficou normal.

**Firmware de voo com a fiação nova** (log `device-monitor-261013-133411.log`):

| Teste | Resultado | Situação |
|---|---|---|
| Boot | NVS com as três calibrações; bias do giroscópio aceito (−5,003, +2,680, +1,033 °/s; diferença entre janelas de 0,008 °/s); zero da altitude com desvio de 0,28 Pa | ✔ |
| `r` parado, ~67 s | 199–200 Hz; perdidas, descartadas e falhas em 0; ruído de 2,1–2,9 mg e 0,04–0,05 °/s; `baro: ok` | ✔ |
| `r` mexendo nos fios, ~30 s | 1 `Error -1` (1 falha isolada do MPU, sem falhas seguidas nem recuperação); perdidas e descartadas em 0; BME280 sem falhas | ⚠ |
| `a` parado, 8 s | dt médio de 5,01 ms; atraso máximo de 104–113 µs após o pulso; ambíguos, descartados e reinícios em 0; roll +2,34°, pitch −0,77° | ✔ / ver abaixo |

- **A falha isolada ao mexer nos fios** aconteceu com a caixa sacudindo (pico de 40 °/s, ruído de 132 mg naquele segundo). Foi uma leitura perdida em ~6 000, e o driver a descartou, como previsto. No diagnóstico, o `t` a 400 kHz não mostrou falhas mexendo nos fios, mas lá o INT fica desligado. Não dá para dizer se a causa foi uma emenda de jumper abrindo por um instante ou o INT. Com a caixa parada, não houve falhas.
- **O fluxo `a` confirma a temporização:** o atraso de ~108 µs vai do pulso do INT até o início da leitura e corresponde à espera de `MPU_INT_READ_DELAY_US` (100 µs).
- **Roll de +2,3–2,5° com a caixa na mesa**, contra −0,1 a −1,4° nas sessões de 11/10 (mesa com ~1,2° de roll). Como o MPU não saiu do lugar, a diferença de ~3,5° pode vir de outro apoio da caixa (mesa, fios ou a protoboard por baixo) ou de o módulo ter se mexido ao montar a protoboard dentro da caixa. A conferir com a caixa girada 180° na mesa: (r₁ − r₂)/2 é a inclinação do apoio, e (r₁ + r₂)/2 é o quanto o módulo saiu da calibração de montagem.

**Teste dos 180° (log `device-monitor-261013-134229.log`).** Mesmo lugar da mesa, nariz na mesma referência, fluxo `a` de ~15 s em cada posição (valores do filtro, estáveis em ±0,02°):

| Posição | Roll | Pitch |
|---|---|---|
| 1, normal | −0,10° | +0,04° |
| 2, girada 180° | +2,19° | +0,30° |
| 1 de novo | −0,16° | +0,02° |

- **Inclinação da mesa** = (r₁ − r₂)/2: roll **−1,15°**, pitch −0,13°. O roll bate com os −1,16° medidos pelo `z` em 11/10 e com o ~1° do celular: a mesa é a mesma.
- **Desvio em relação à calibração de montagem** = (r₁ + r₂)/2: roll **+1,05°**, pitch +0,17°. A posição 1 repetiu dentro de 0,06°, então esse resultado não é ruído de apoio desta sessão.
- **Os +2,4° da sessão anterior eram do apoio.** Na mesma "posição 1", a leitura agora é −0,1°. Naquela sessão a caixa estava apoiada de outro jeito ou em outro ponto da mesa.
- O ~1° de desvio em roll mostra que o módulo, ou o plano de apoio da caixa, mudou em relação à calibração de montagem de 11/10. A causa provável é a montagem da protoboard dentro da caixa, embora o MPU não tenha sido mexido de propósito. O valor está perto da variação já vista entre reposições da caixa (−0,1 a −1,4° em 11/10), então o `z` refeito deve ser conferido com um novo par de 180°.

**Primeira tentativa de refazer o `z`.**
- **Fio solto (log `device-monitor-261013-134634.log`).** Logo depois do prompt da posição 1/2 (63,9 s), um dos jumpers emendados se soltou ao manusear a caixa. Daí até o fim do log (79,5 s), todas as leituras falharam: 574 `Error -1`, alguns `Error 263` (timeout) e 94 reinícios do barramento, sem nenhuma recuperação, porque o contato ficou aberto. No boot seguinte, o BME280 fez o zero a 26,1 °C, igual ao de antes, sem sinal de aquecimento por latch-up. Conclusão: as emendas soltam quando a caixa é pegada e girada. Elas precisam ser presas (fita ou termo-retrátil, e os fios fixados na caixa) antes de qualquer teste que mexa na caixa. Recolocar fios só com o USB desligado.
- **`z` recusado (log `device-monitor-261013-134926.log`).** As medidas deram roll de −175,5° e −174,5° (sem correção de montagem): a caixa estava de cabeça para baixo. O prompt dizia "caixa apoiada na face de baixo", o que foi lido como "virar a caixa". O limite de 10° descartou a calibração, como previsto. O texto do prompt, a mensagem de descarte e o roteiro do [fusao.md](fusao.md) foram corrigidos para "caixa em pé, sem virar". Sete medidas foram recusadas por movimento (toque no Enter, mão saindo da caixa), também o comportamento previsto.
- **Segundo teste dos 180°, com a montagem antiga (log `device-monitor-261013-135401.log`).** Posição 1: −0,05° de roll, +0,58° de pitch. Girada: +1,90°, +0,60°. Posição 1 de novo: −0,23°, +0,34°. Mesa a −1,0° de roll e módulo desviado **+0,9° em roll**: o desvio em roll se confirma. Em pitch, o desvio do módulo passou de +0,17° para +0,5°, e a posição 1 repetiu só dentro de ~0,2°. Depois da religação do fio, a caixa está assentando de forma menos repetível.

**`z` refeito e conferido.**
- **Calibração (log `device-monitor-261013-140303.log`, caixa em pé).** Medidas sem correção: +3,39° / +0,86° e, girada, +5,77° / +1,30°. Resultado: módulo em roll **+4,58°**, pitch **+1,08°**; mesa em roll −1,19°, pitch −0,22°. A mesa ficou igual à de 11/10 (−1,16°, −0,22°). A diferença para a montagem antiga (+1,01° em roll e +0,42° em pitch) bate com os testes dos 180° (+1,05° e +0,9° em roll; +0,17° e +0,5° em pitch). Gravada na NVS. Logo depois, ainda na posição girada, o filtro leu +1,37° / +0,23°, contra +1,19° / +0,22° esperados.
- **Conferência dos 180° (log `device-monitor-261013-140704.log`, médias de ~8–10 s do filtro, estáveis em ±0,03°):**

  | Posição | Roll | Pitch |
  |---|---|---|
  | 1 | −1,63° | −0,10° |
  | 2 (180°) | +1,27° | +0,13° |
  | 1 de novo | −1,37° | −0,17° |

  Desvio residual da montagem = (r₁ + r₂)/2: roll **−0,18°** (−0,05° com a repetição da posição 1), pitch +0,02°. Fica abaixo do critério de 0,3° e da repetibilidade do assento da caixa, que foi de 0,26° em roll entre as duas posições 1. A mesa saiu com −1,3° a −1,45° de roll, contra −1,19° no `z`: diferença do ponto de apoio na mesa, dentro dessa mesma repetibilidade. **Calibração de montagem validada.**
- **Reinício do filtro ao apertar `c` com o `r` e o `a` ligados** (log `140303`): os comandos seguidos encheram a serial, e o laço ficou bloqueado por mais de `FUSION_MAX_DT_MS`. O filtro reiniciou (1 reinício, 12 descartados, 1 ambíguo) e voltou à mesma atitude após as amostras de inicialização. É comportamento previsto do main provisório. Não houve falhas de I2C nesta sessão.
- O fluxo `b` não foi feito nesta sessão. A altitude no `r` andou entre −0,57 e +0,06 m em ~100 s, de forma lenta e contínua. Isso é compatível com a variação da pressão ambiente já registrada.

### Correções da revisão: montagem versão 2, barômetro sem o INT e gravidade no bias (13/10/2026)

Firmware com o lote 1 da revisão de 13/10: barômetro independente dos pulsos do MPU, montagem atrelada à calibração do acelerômetro (registro versão 2), validação dos coeficientes lidos da NVS, recuperação do estado degradado só com roll observável, zero barométrico recusando valores não finitos e conferência da criação da fila do INT.

- **`z` refeito no formato novo.** O registro da versão 1 foi ignorado no boot, como previsto. Medidas sem correção: +3,28° / +0,87° e, girada, +5,82° / +1,11°. Resultado: módulo em roll **+4,55°**, pitch **+0,99°**; mesa em roll −1,27°, pitch −0,12°. A diferença para o `z` das 14:03 (+4,58° / +1,08°) é de −0,03° em roll e −0,09° em pitch, abaixo da repetibilidade do assento (0,26°). Gravado na NVS.
- **Boot depois do `z`** (logs `144145` e `144337`): `acelerômetro calibrado | giroscópio com bias salvo | montagem calibrada`. O registro novo é lido e a calibração do acelerômetro guardada nele bate com a atual. Nenhum aviso de valores impossíveis.
- **Fio do INT (GPIO19) solto por ~8 s, com `r` e `b` ligados** (log `device-monitor-261013-144337.log`):

  | Trecho | Resumos | Taxa do MPU | Linhas `[baro]` por resumo |
  |---|---|---|---|
  | Antes | 14 | 199–200 Hz | 8–9 (os 2 primeiros antes do `b`) |
  | Fio solto | 7 (mais 1 de transição a 100 Hz) | 0 Hz, `sem pulsos do INT` | 8–9 |
  | Reconexão | 1 | 89 Hz, 1611 perdidas | 8 |
  | Depois | 4 | 199–200 Hz | 8–9 |

  - **O barômetro manteve a mesma taxa com o INT solto.** Antes da correção, ele parava junto com os pulsos. O resumo mostrou `baro: ok` e falhas 0 em todos os segundos.
  - As 1611 perdidas correspondem a ~8 s sem pulsos a 200 Hz. Com o fio de volta, o MPU seguiu sem reconfiguração (recuperações 0), porque só o INT tinha saído. A fusão reiniciou pela lacuna e voltou à mesma atitude. O segundo depois da reconexão teve pico de 16,5 °/s: foi a mão na caixa ao encaixar o fio.
  - Nesta repetição não houve erro de I2C. Na primeira tentativa (log `144145`, só com o `b`), encaixar o fio deu um `Error 263` (timeout do I2C) e uma linha corrompida na serial, sem efeito nos chips.
- **Direção da gravidade na confirmação do bias** (log `device-monitor-261013-150353.log`, item 2A da revisão, `CAL_BIAS_MAX_TILT_DEG` = 0,1°):

  | Situação | Resultado | Ângulo entre janelas |
  |---|---|---|
  | 5 boots seguidos, caixa parada | aceito em todos, na primeira confirmação | 0,027°, 0,017°, 0,007°, 0,011°, 0,003° |
  | `g` batucando na mesa | 4 janelas recusadas por movimento (desvio 0,20–0,36 °/s); aceito ao parar | 0,003° e 0,015° |
  | `g` inclinando a caixa à mão | 4 janelas recusadas por movimento (desvio 3,2–11,9 °/s); aceito ao soltar | 0,016° |

  - **O ruído da direção ficou em 0,003–0,027°** (média ~0,012°). Isso é coerente com a estimativa de ~0,01° por janela, com algum aumento pela correlação do DLPF. O limite de 0,1° tem uma margem de ~4 vezes, e nenhuma confirmação em repouso foi recusada por ele. **Limite mantido.**
  - **A recusa pelo ângulo não foi exercitada na bancada.** A mão não consegue girar a caixa de forma suave o bastante para ficar abaixo de 0,15 °/s de desvio, então o filtro de movimento recusou antes. Uma rotação constante e suave, que é o caso que o critério cobre, exige um prato giratório ou uma acomodação lenta do apoio. Esse caso está coberto pelos testes `native` (`test_bias_rejects_constant_horizontal_rotation` e `test_bias_tilt_threshold`).
  - O batuque desta sessão foi mais forte que a digitação de 11/10 (0,08–0,09 °/s), por isso foi recusado como movimento.
  - O bias de gx ficou entre −4,826 e −4,843 °/s, a 25,8–26,1 °C, contra −4,92 °/s a 27,1 °C às 14:43. Isso dá ~−0,08 °/s por °C, coerente com os ~−0,1 °/s por °C já registrados.
- **O fluxo `b` imprime 8–9 linhas por segundo, não 10.** A próxima linha é agendada a partir da leitura que imprimiu, e as leituras ficam numa grade de 40 ms (25 Hz), então o intervalo efetivo é de 120 ms. Isso já acontecia antes da correção e não afeta a medida.

### Magnetômetro: mapeamento de eixos e fontes de distorção (13–14/10/2026)

Protótipo religado (GY-273 na protoboard dentro da caixa do MPU, com a seta X paralela à do MPU e o chip para cima, a mais de 10 cm do ESP32 e do USB). Diagnóstico `i2cscan`, comando `h` com contagem regressiva de 10 s e média de 10 leituras. Campo de referência (WMM2025, João Pessoa): \|B\| = 26,2 µT, H = 22,2 µT, inclinação −31,8°, declinação −20,7°.

**Mapeamento de eixos** (log `device-monitor-261013-185902.log`). Caixa nivelada, girada 90° por vez no sentido horário pelas bordas da mesa, partindo do nariz em 85° verdadeiros (~106° magnéticos); P5 com o conjunto de cabeça para baixo, nariz na proa de P1:

| Posição | X, Y, Z do chip (µT) | \|B\| | Ângulo atan2(Y, X) | Esperado (X, Y, Z) |
|---|---|---|---|---|
| P1 | +11,4, **+28,6**, +10,0 | 32,3 | 68° | −6, +21, +14 |
| P2 | **−19,2**, −7,9, +5,5 | 21,5 | 202° | −21, −6, +14 |
| P3 | +7,2, **−29,7**, +2,4 | 30,6 | 284° | +6, −21, +14 |
| P4 | **+28,6**, −10,6, −2,4 | 30,6 | 340° | +21, +6, +14 |
| P5 | +5,3, **−38,3**, **−21,6** | 44,3 | — | −6, −21, −14 |

- Sinais e eixo dominante batem nas cinco posições, e o ângulo cresce no giro horário. O Z inverte de sinal em P5: pelo P1 e o P5, a componente vertical é de +15,8 µT no chip (esperado 13,8) e o hard iron em Z, de ~−5,8 µT.
- **Mapeamento: x = X, y = −Y, z = −Z**, igual ao do MPU na montagem 5 (`magChipToBody` em `lib/mag`).
- Anomalias: descontado o centro estimado (5,6, 0,3 µT), os giros de 90° deram **120°, 75° e 62°**; o Z variou com a proa (+10 a −2,4 µT, o equivalente a ~12° de inclinação); e o \|B\| de P5 ficou em ~42 µT depois de descontar os offsets.

**Investigação das anomalias.** Cada teste mudou uma coisa só, com o sensor parado:

| Log | Teste | Resultado |
|---|---|---|
| `261013-191105` | Mexer nos jumpers e voltar | Ângulo 72° → 353° → 42°: mudou 79° e não voltou |
| `261013-192116` | Fios distantes (> 5 cm), depois conectores perto do chip, depois tudo de volta | 18° → 31° → 36° → 44°: deriva num sentido só; os fios distantes mudaram mais (~8 µT) que os conectores perto do chip (~3 µT) |
| `261013-192632` | Controle: nada (3×), mão na caixa, fios distantes, fios de volta | **Nada mexido: 0,2 µT de repetição**; mão na caixa sem efeito; fios: 2,2 e 3,5 µT, sem voltar |
| `261013-193708` | Autoteste (`x`) e `h` mexendo nos fios; tensão no GY-273 com multímetro | **Ganho constante** (123,9/116,3/118,5 a 124,0/116,5/118,8 µT, ≤ 0,3 %); 3,32 V estável; o campo foi de \|B\| 39 a 19 µT (~23 µT de variação) |
| `261013-194255` | Jumper avulso a 2 e 5 cm do GY-273 | 0,7 µT a 2 cm; um salto de 3,6 µT entre duas medidas, sem relação com o jumper |
| `261013-194758` | ESP32 girado 90°, afastado; cabo USB mexido | ESP32 girado: 1,5 µT; afastado: ~0; cabo USB: 3,2 µT |
| `261014-114920` | Powerbank desligado a 30, 20 e 10 cm | < 0,1 µT nas três distâncias |
| `261014-115353` | Powerbank ligado, alimentando a placa B, a 10 cm e encostado | 0,8 µT a 10 cm; 1,6 µT encostado (≈ 2° de proa sem calibração) |

Conclusões:

- **Sem nada se mover, o campo é estável** (0,1–0,2 µT). O sensor está saudável: autoteste e ganho constantes, alimentação estável.
- **As fontes medidas isoladamente são pequenas:** jumper e emenda a 2 cm < 1 µT; ESP32 1,5 µT; cabo USB 3,2 µT; powerbank ≤ 1,6 µT. Fixas em relação ao sensor, todas viram hard iron constante, que a calibração corrige.
- **As variações grandes (até ~23 µT e 79°) apareceram só ao mexer na fiação improvisada** e não foram reproduzidas nos testes controlados. A causa não foi isolada. O que elas exigem é o mesmo: nada pode se mover em relação ao sensor. Isso levou à montagem final, com tudo fixo numa caixa (seção seguinte). O teste de rotação das 4 proas deve ser refeito nela.
- **Regra para a bancada:** controle remoto (pilhas de aço), celular, multímetro e ferramentas a mais de 30 cm do GY-273 durante as medidas.

### Montagem final na caixa (14/10/2026)

O modelo final passa a ser uma caixa de papelão (caixa de sapato, 28 × 13 × 10 cm), com o nariz ao longo dos 28 cm.

- **Sensores:** MPU, GY-273 e BME280 numa mini protoboard colada no canto da caixa, encostada nas duas paredes (que servem de esquadro). Os módulos ficam presos com cola quente; as partes que ficam para fora da protoboard se apoiam em calços.
- **ESP32** numa protoboard na outra metade, a ~15 cm do GY-273; **powerbank** preso com abraçadeiras de plástico na parede, a ~13 cm do GY-273, com cabo curto até o ESP32.
- **Fiação:** cinco jumpers macho-macho (3V3, GND, SDA no GPIO 21, SCL no GPIO 22, INT no GPIO 19), com no máximo dois conectores, sem emendas. Na protoboard dos sensores, uma fileira para cada sinal comum. XDA, XCL, DRDY, AD0, CSB e SDO sem ligação.
- **Orientação:** GY-273 com X para o nariz, Y para a asa esquerda e chip para cima (mesmo mapeamento do protótipo). MPU **girado 90°**: X para a asa direita, Y para o nariz, Z para cima (ver "Orientação de montagem do MPU6050").

**Dois curtos durante a montagem:**

1. **Primeira ligação:** o regulador do ESP32 esquentou e os módulos ficaram com 1,12 V. O caminho do 3V3 tinha < 3 Ω, mas perdia 0,9 V, ou seja, a corrente passava de 300 mA. Refazer as ligações de VCC e GND resolveu (3,3 V nos três módulos, regulador frio, LED do MPU aceso). A causa provável é um fio numa fileira errada.
2. **CSB e SDO do BME280 ligados (invertidos: CSB em GND e SDO em 3V3):** a alimentação despencou, o regulador esquentou muito e o MPU apagou. Depois de retirar os dois fios, o BME280 **deixou de responder** no I2C.

**Diagnóstico do BME280 depois do curto:** CSB em 3,3 V (modo I2C), SDO em 0 V (endereço 0x76), SDA e SCL em 3,3 V, continuidade até os GPIO 21 e 22 de 1,5 Ω, chip frio, ausente também sozinho no barramento e depois de desligar a alimentação. Conclusão: **chip danificado**. O mecanismo provável é o SDO, que no modo SPI vira saída: ligado a 3V3, ele "briga" com a saída do chip, ou faz um curto direto se o módulo liga o SDO ao GND na placa. Na queda de alimentação, os pinos de I2C ficaram acima do VDD do chip, puxados pelos outros módulos. O substituto é um **BMP280** (mesmo elemento de pressão, sem umidade, chip_id 0x58).

**Regras para a montagem:**

- **CSB e SDO do BME280/BMP280 soltos.** Os resistores do módulo já fixam o modo I2C e o endereço 0x76; ligá-los numa protoboard apertada só trouxe risco.
- **Cada fileira de 5 furos com um único pino de módulo.**
- **Antes de ligar a alimentação, medir a resistência entre 3V3 e GND:** centenas de Ω ou mais. A ~2 kΩ foi o valor normal com os três módulos.
- Os módulos expostos a um curto devem ser reconferidos: scan, autoteste do HMC (`x`) e eixos do MPU (`l`).
- **Ao mexer num fio com a placa ligada (19/10/2026), não esbarrar no VCC nem no GND dos módulos.** Um VCC com mau contato causou brownouts no ESP e deixou o BMP280 reiniciando sem parar ("BMP280 no firmware de voo").

**Diagnóstico na montagem final** (log `device-monitor-261014-184539.log`, sem o BME280):

| Teste | Resultado | |
|---|---|---|
| Scan | 0x1E (HMC5883L) e 0x68 (MPU6050) | ✔ |
| `t` a 100 e 400 kHz, mexendo nos fios | 3 040 e 10 460 leituras, 0 falhas | ✔ |
| Autoteste do HMC5883L | OK: 122,5 / 115,1 / 117,0 µT (~1 % abaixo de 13/10) | ✔ |
| Bias do giroscópio | −4,9 / +2,7 / +0,9 °/s, igual ao de antes | ✔ |
| Eixos do MPU (`l`) | Nivelada az = 1,10 g; nariz para cima ay = 1,00 g; asa direita para baixo ax = −0,98 g | ✔ X asa direita, Y nariz, Z para cima |

**Alinhamento do MPU com a caixa** (comando `m`, logs `261014-185617` e `261014-190319`, offsets da calibração anterior descontados: X +17, Y −2, Z +92 mg):

| Posição | ax | ay | az |
|---|---|---|---|
| Nivelada | +0,103 g | +0,040 g | +1,092 g |
| Nariz para cima | +0,008 g | +1,001 g | +0,083 g |
| Asa direita para baixo | −0,984 g | −0,030 g | +0,146 g |
| Nivelada de novo | +0,099 g | +0,042 g | +1,093 g |

- **Roll:** o X sobe 4,9° com a caixa sobre o fundo e o Z sobe 3,1° com ela apoiada na parede da direita. As duas faces indicam um **desalinhamento do módulo de ~4° em roll**; a diferença entre elas é a mesa e o esquadro da caixa.
- **Pitch:** com o nariz para cima, o Z fica a −0,5°, o que sugeria o módulo alinhado em pitch e os ~2,5° do ay nivelado vindos da mesa. **O `z` desmentiu essa interpretação** (seção seguinte): a medida com a caixa girada 180° cancela a mesa e mostra **+2,5° do módulo em pitch**, com a mesa a −0,2°. A estimativa por uma só face depende do esquadro da caixa (aqui, da face da cauda) e do offset de Z, que mudou com a fixação nova.
- Repetição: as duas medidas niveladas diferem 0,2°.
- **Aquecimento passageiro:** a temperatura do MPU, de 28,2 °C às 18:56, estava em 34,3–34,6 °C às 19:03, e o bias de gx foi de −4,80 para −5,22 °/s (coerente com ~−0,07 °/s por °C). Ela voltou a ~28 °C sem nenhuma mudança na montagem, com o BME280 ainda ligado: não há fonte de calor permanente na protoboard. A causa provável é externa (mãos segurando a caixa nas posições, objeto morno por perto). Por isso, antes da calibração do giroscópio e das seis posições, convém conferir se a temperatura está estável.
- **Aceito sem reajuste:** o `z` corrige o desalinhamento (limite de 10°, sobram ~5° para a mesa) e, nas seis posições, o efeito na escala é de 0,25 % (cos 4°). A calibração em seis posições e o `z` precisam ser refeitos nesta montagem.

### Montagem final: mapeamento novo, seis posições, `z` e calço (14/10/2026)

Firmware `aircraft` com o `chipToBody` da montagem final (x = Y, y = X, z = −Z). Log `device-monitor-261014-195738.log`, 27,7–27,9 °C, BME280 ausente (as tentativas a cada 5 s geram um `Error -1` da biblioteca, sem efeito no MPU).

- **Boot:** o `z` antigo foi ignorado ("montagem sem calibração", formato 3 do registro). Giroscópio calibrado: bias −4,668 / +2,548 / +1,264 °/s, igual aos dois boots anteriores dentro de 0,01–0,09 °/s.
- **Sinais (`a`, calço de 2,5 cm, antes do `6`):** calço sob o lado esquerdo, roll de −4,4° para +6,8° (+11°); calço sob o nariz, pitch de +2,6° para +7,6° (+5°). A coluna "só giroscópio" acompanhou o filtro nos dois movimentos (diferença ≤ 0,2°). **Acelerômetro e giroscópio com os sinais certos.**
- **Seis posições:** offset +18,8 / +3,4 / +102,9 mg, escala +0,36 / +0,11 / +0,65 %, \|a\| corrigido de 1,0000 a 1,0041 g. O offset de Z subiu ~10 mg em relação à montagem 5 (+92,5 mg), outra vez acompanhando a mudança de fixação.
- **`z`:** módulo em **roll −3,28°, pitch +2,52°**; mesa em roll −0,95°, pitch −0,23°.
- **Calço** (2,5 cm, rente à borda; fundo da caixa de 28 × 13 cm; `r` ligado). Ângulos em relação à caixa nivelada no mesmo lugar (N ≈ −1,0° de roll e −0,3° de pitch, a inclinação da mesa):

  | Posição | Esperado asin(h/d) | Medido (variação) | Diferença |
  |---|---|---|---|
  | R+ (calço sob o lado esquerdo) | +11,09° | +11,21° | +0,12° |
  | R− (calço sob o lado direito) | −11,09° | −11,2° a −11,6° | −0,1° a −0,5° |
  | P+ (calço sob o nariz) | +5,12° | +4,95° | −0,17° |
  | P− (calço sob a cauda) | −5,12° | −5,15° | −0,03° |

  - O R− teve problemas de posicionamento (a caixa escorregou e foi reposicionada) e oscilou entre −12,2° e −12,5° absolutos. Isso explica a faixa.
  - Repetibilidade: nivelada no início −1,07° / −0,24°, no fim −1,08° / −0,28°. Depois do R− problemático, a caixa voltou a −0,72… −0,96° de roll (o lugar mudou um pouco).
  - Em repouso, ±0,00–0,01° em cada janela de 1 s.
  - **Todos dentro da meta de ≤ 1°** do [fusao.md](fusao.md). O celular mostrou os mesmos 11° e 5°.
- **Magnetômetro no `r`:** 25 leituras/s, 0 falhas, \|m\| 25,6–27,4 µT sem calibração.

### Calibração do magnetômetro: tentativas de 14/10/2026

Firmware `aircraft`, montagem final, BME280 ausente. Procedimento e conclusões em [magnetometro.md](magnetometro.md), D5.

| Log | Método | Resultado |
|---|---|---|
| `261014-204104` | No ar, uma face para cima e giro em torno da vertical (90 s) | 12/24: o nariz nunca passou pelo norte magnético |
| `261014-210631` | No ar, cada face apontada para o norte magnético e giro de "volante" (90 s) | 19/24; resíduo de 17,6 %; \|B\| bruto de 8 a 49 µT e um salto de 19 µT em 40 ms, impossíveis só com o campo da Terra e o hard iron da caixa |
| `261014-211746` | Idem, 120 s | 17/24 (±y e ±z pela metade: giros de 180° num sentido só); resíduo de 12 % |
| `261014-212227` | Fluxo `p`: mesa, mãos na caixa, junto ao peito | O corpo muda o \|m\| em ~1,7 µT |
| `261014-212650` | Fluxo `p`: vira-e-volta (de lado, de cabeça para baixo, nariz para cima) e volta ao contorno | Volta a 0,1 µT e 0,3° do início: **boa estabilidade da montagem** nessas três posições. Não prova que nada se mova em todas as orientações nem com movimentos mais bruscos |
| `261014-213305` | Na mesa, no lugar, cada face apoiada e giro de 360° (180 s) | **24/24** em 132 s, raio de 27,3 µT, escalas 1,00/1,14/0,86, resíduo de **7,1 %**, sem saltos. Elipsoide de 9 parâmetros: 6,1 %. O firmware bateu com o `mag_fit.py` |
| `261014-213828` | Fluxo `p`: mapa da mesa (±10 cm, um ponto de frente para o notebook) | \|m\| corrigido de 24,9 a 29,8 µT com a proa quase igual: **gradiente de ~5 µT em 10 cm** |
| `261014-214602` | Fluxo `p`: assento da cadeira giratória, apoio de 23 cm, fora da cadeira | \|m\| corrigido de 25,5 / 25,8 / 25,0 µT: **lugar uniforme**; o aço da cadeira não pesou nessa orientação |
| `261014-215323` e seguintes | Cadeira com o apoio, ainda perto do notebook (cabo curto) | 16/24 aos 80 s, coleta interrompida |

Conclusões:

- **O ajuste numérico do firmware está confirmado:** duas coletas reais deram o mesmo resultado no firmware e no `mag_fit.py`. Parada, a caixa repete o campo em 0,1 µT. **A proa ainda não foi validada:** isso depende de uma calibração aceita e da parte 2 do roteiro (proa nivelada, compensação de inclinação, distúrbio e filtro).
- **O problema é o lugar e o jeito de girar.** Transportar a caixa no ar, ou girá-la em torno do centro (o sensor fica num canto e percorre ~15 cm), atravessa as variações do campo perto do notebook. A calibração só corrige o que gira junto com o sensor.
- **Próxima tentativa:** extensão USB de 2–3 m, cadeira com o apoio a 1 m ou mais do notebook, canto dos sensores perto do eixo, 360° em cada face, `d` sempre. No uso real, sem cabo, a calibração poderá vir pelo rádio (`CMD_CALIBRATE_MAG`).

### Calibração do magnetômetro: tentativas de 15/10/2026

Firmware `aircraft` (ainda com o ajuste de 6 parâmetros), montagem final, BME280 ausente. Cadeira giratória com o apoio de 23 cm, a ~1,5 m do notebook, com extensão USB. Cada face apoiada, com giro de 360° pelo encosto da cadeira e o canto dos sensores perto do eixo.

| Log | Método | Resultado |
|---|---|---|
| `261015-151422` | `m` (sem `d`) | 24/24 em 100 s, antes de passar pelas seis faces (a coleta termina sozinha). Offset +8,0 / +5,5 / +11,3 µT, escalas 1,005 / 1,125 / 0,870, raio 25,89 µT, **resíduo 4,62 %**, recusada |
| `261015-152051` | Fluxo `p`, mapa: centro → 10 cm à frente → centro → 10 cm atrás → centro, movendo a cadeira (esquerda, direita e altura não dava para medir) | \|m\| bruto (sem calibração) de 18,3 / 17,0 / 18,2 / 19,0 / 18,8 µT: **~1 µT em 10 cm**, em parte por a cadeira ter girado 3–5° ao ser empurrada (sem calibração, o offset da caixa faz o \|m\| depender da orientação) |
| `261015-152613`, `261015-152844` | `m` interrompidas | — |
| `261015-153132` | `m` com `d` | 24/24 em 120 s. Offset +8,3 / +6,5 / +9,9 µT, escalas 1,012 / 1,121 / 0,868, raio 25,85 µT, **resíduo 4,62 %**, recusada. O `mag_fit.py` bateu com o firmware (diferença 0,0000). **Elipsoide de 9 parâmetros: resíduo 0,63 %**, offset +8,12 / +6,05 / +10,38 µT, raio 25,66 µT, acoplamento y–z de −0,073 na W |

Conclusões:

- **O lugar resolveu o que dependia dele:** o resíduo caiu de 7,1 % (mesa) para 4,6 %, e o raio ficou a 1 % do WMM (26,2 µT). Com o elipsoide completo, o resíduo de 0,63 % (~0,16 µT) mostra que não sobra distorção do lugar nem movimento da fiação.
- **Os 4,6 % eram do modelo:** a deformação da caixa tem um acoplamento y–z que o ajuste alinhado aos eixos não representa. As escalas repetiram em dois lugares (dentro de 1,2 %), então a deformação é da caixa e do sensor. O firmware passou para o elipsoide completo ([magnetometro.md](magnetometro.md), D4). Com os pontos de `261015-153132`, o ajuste novo em C++ deu o mesmo resultado do `mag_fit.py` e **teria aceitado a calibração**.
- **Repetibilidade com o ajuste antigo:** offsets diferentes em até 1,4 µT entre as duas coletas. A conferência fica para as coletas com o ajuste novo.

**Com o firmware novo (elipsoide completo):**

| Log | Método | Resultado |
|---|---|---|
| `261015-155440` | `m` na cadeira, mesmo lugar; o `d` não chegou à placa | **Aceita e gravada:** 24/24 em 116 s (fundo, tampa e asas bastaram), offset +8,14 / +6,21 / +10,56 µT, semieixos 1,019 / 1,108 / 0,872, y–z da W −0,074, raio 25,70 µT, **resíduo 0,77 %**. Repetibilidade contra o ajuste de 9 parâmetros da coleta `261015-153132`: offsets dentro de 0,18 µT, semieixos dentro de 0,2 %, raio dentro de 0,04 µT. Depois do `s`, a caixa foi levada para outro lugar: `DISTÚRBIO` com \|m\| de 36,3 µT |
| `261015-160131` | Boot, `c` e fluxo `p` com a caixa no apoio da cadeira: normal, asa direita para baixo, asa esquerda para baixo, nariz para cima, nariz para baixo; no fim, deslocada em direção ao notebook | NVS: "magnetômetro calibrado", W completa no `c`. **\|m\| de 25,2 a 26,2 µT** em todas as posições (−2 a +2 % do raio). **Inclinação de −29,6 a −32,3°** (WMM: −31,8°). Proa compensada de 94 a 102° com a caixa de lado (roll ±85–90°), contra 96,7° nivelada, enquanto a "deitada" ia a 112° e 260°. Nariz para cima ou para baixo além de 70°: `INCL_ALTA`, como previsto. A caminho do notebook, \|m\| caiu para 23,9 µT (−7 %, sem chegar ao limite de distúrbio) |

- **A calibração está validada:** aceita, repetível, gravada na NVS e com o \|m\| corrigido dentro de ±2 % em todas as orientações testadas na cadeira. A inclinação medida bate com o WMM. Falta a parte 2 do roteiro ([magnetometro.md](magnetometro.md), itens 4 a 7).

### Magnetômetro: lugar para os testes de proa (15/10/2026)

Mapas com o fluxo `p` e a caixa já calibrada: caixa nivelada, sem girar, deslocada ~10 cm em cada direção.

| Log | Lugar | Resultado |
|---|---|---|
| `261015-161229` | Mesa (notebook e caixa de som afastados), nos alinhamentos de 45/90° | Centro com \|m\| de 27,5 µT e inclinação de −24,5° (cadeira: 25,3 µT e −31°); um ponto a 10 cm com a proa 15° diferente e inclinação de −5,5°. **Não serve:** metal na mesa ou sob ela |
| `261015-161724` | Cadeira, frente e trás (as laterais não dão) | Centro repetível: 25,2–25,3 µT e −31 a −32°. Frente −1,5 µT, trás +0,8 µT. A proa no centro mudou 12° porque a cadeira girou ao ser empurrada. **Serve, com o sensor no centro:** nos testes, gira-se a cadeira com o canto dos sensores sobre o eixo |
| `261015-163019` | Piso, num cruzamento de rejunte | Centro com 19,8 µT e −15°; a 10 cm, de 15 a 28 µT e proa de 66 a 111°. **Não serve:** ferragem da laje. O `DISTURBIO` entrou e saiu nos limites certos |

### Magnetômetro: proa nivelada (15/10/2026)

Log `261015-164018`. Cadeira girada pelo encosto, com o canto dos sensores sobre o eixo e a caixa nivelada (roll e pitch dentro de ±1,5°). Posições de 90 em 90°, com a borda da caixa alinhada a olho com o rejunte do piso (erro estimado de 2–3°). Proa compensada (`mag`) em cada parada:

| Posição | Horário | Anti-horário |
|---|---|---|
| 0° (início) | 98,6° | — |
| 90° | 190,1° | 190,0° |
| 180° | 282,9° | 282,5° |
| 270° | 10,5° | 12,0° |
| 0° | 101,3° | 100,6° |

- **Giros de 90°:** +91,5 / +92,8 / +87,6 / +90,8° no horário e −89,3 / −89,5 / −92,5 / −89,4° no anti-horário. Erro máximo de 2,8°, dentro dos ±3–5° esperados com o alinhamento a olho. A proa **cresce no giro horário**, como manda a convenção.
- **Repetibilidade:** a mesma posição física repete em 0,1 a 1,5° entre o giro horário e o anti-horário. A volta ao 0° ficou a 2,0–2,7° do início, o que é compatível com o alinhamento a olho.
- **\|m\|** de 25,5 a 26,1 µT em todas as posições. A proa filtrada acompanhou a do magnetômetro sem atraso visível nos giros lentos.
- **A inclinação varia com a proa:** −25,8° com o nariz perto do norte magnético (~10°), −32,1° perto do sul (~190°), −29,9° e −27,7° no leste e no oeste. Isso dá ~±3° em torno de −29°, com o eixo quase alinhado ao nariz. Indica um **desalinhamento de ~3° em pitch entre o GY-273 e a caixa** (ou entre o GY-273 e o MPU, cuja montagem tem roll −3,3° e pitch +2,5°). É a limitação já prevista ("Desalinhamento entre o GY-273 e a caixa"): pela inclinação de −32°, um desalinhamento de 3° dá até ~2° de erro de proa, o que é compatível com os desvios dos giros. Aceitável na bancada.

### Magnetômetro: compensação de inclinação com o calço (15/10/2026)

Log `261015-164533`. Cadeira parada, nariz perto do sul magnético (proa de ~186–192°), calço de 2,5 cm. Sob o nariz, o calço ficou a 5,5 cm da ponta, e sob a cauda, a 7,5 cm da ponta (os ângulos usados são os medidos pela fusão). Entre as posições, a caixa voltou a ficar nivelada, e a proa nivelada escorregou de 191,7° para 185,9–188,2° ao mexer no calço. Por isso, cada inclinação é comparada com a média das posições niveladas vizinhas:

| Posição | Roll / pitch | `mag` (compensada) | `deitada` | Nivelada vizinha |
|---|---|---|---|---|
| Asa esquerda para baixo | −11,3° / +2,5° | 187,0° (−3,0°) | 194,5° (+4,5°) | ~190° |
| Asa direita para baixo | +12,3° / +2,7° | 185,0° (−3,2°) | 176,6° (−11,6°) | 188,2° |
| Nariz para cima | +2,3° / +11,3° | 187,7° (+0,7°) | 186,9° (−0,1°) | ~187° |
| Cauda para cima | +4,2° / −7,1° | 187,8° (+1,3°) | 185,3° (−1,2°) | ~186,5° |

- **Com roll de ~12°, a compensação reduziu o erro de até 11,6° para ~3°.** O resíduo de ~3° é compatível com o desalinhamento de ~3° do GY-273 visto na proa nivelada, somado ao escorregamento da caixa ao mexer no calço.
- **Com pitch de 7 a 11°, nem a `deitada` errou**: com o nariz para o sul, o erro da proa sem compensação aparece pelo roll, e o do pitch fica perto de zero (ele é máximo com o nariz para leste ou oeste). O teste de pitch precisa ser feito com o nariz para leste ou oeste.
- \|m\| de 25,5 a 26,1 µT e inclinação de −31,2 a −32,1° em todas as posições.

### Magnetômetro: compensação de inclinação a ±30° (15/10/2026)

Log `261015-165644`. Caixa segurada à mão na cadeira, com o ângulo lido no `p`. Roll com o nariz perto do sul (~187–191°), onde o erro sem compensação vem do roll. Pitch com o nariz perto do leste (~100°), onde ele vem do pitch. Cada inclinação é comparada com a média das posições niveladas vizinhas:

| Inclinação | Nivelada vizinha | `mag` (compensada) | `deitada` (sem compensação) |
|---|---|---|---|
| Roll +30° | 189,4° | 185,5° (**−3,9°**) | 166,8° (−22,6°) |
| Roll −30° | 190,0° | 188,6° (**−1,4°**) | 201,7° (+11,7°) |
| Pitch +30° | 100,3° | 96,4° (**−3,9°**) | 78,8° (−21,5°) |
| Pitch −30° | 98,9° | 103,5° (**+4,6°**) | 116,8° (+17,9°) |

- **A compensação reduz o erro de 12 a 23° para 1,4 a 4,6°**, dentro dos ~3–5° esperados. O resíduo vem principalmente do desalinhamento de ~3° do GY-273 e da caixa segurada à mão (a proa nivelada antes e depois de cada inclinação difere em até 4°).
- \|m\| de 25,7 a 26,9 µT e inclinação de −29,5 a −32,9° em todas as posições: a calibração vale também com a caixa inclinada.

### Magnetômetro: distúrbio com chave de fenda (15/10/2026)

Log `261015-170239`. Caixa nivelada na cadeira, proa de ~99,6°, τ = 1 s, limite de distúrbio de 15 %. Chave de fenda de aço aproximada devagar (~3 s) do canto dos sensores até ~3–5 cm, mantida ~5 s e afastada.

| Momento | `mag` | `proa` (filtrada) | \|m\| | Estado |
|---|---|---|---|---|
| Parada, antes | 99,6° | 99,6° | 25,7 µT | ok |
| Aproximando (\|m\| ainda dentro de 15 %) | até 107,8° | sobe até 105,2° | 25,7 → 22,1 µT | ok |
| Pico (~0,6 s) | 105–106° | 105,2° (parada) | 21,7–21,9 µT | `DISTURBIO` |
| Chave afastada | 99,6° | 105 → 99,7° em ~5 s | 25,6 µT | ok |

- **A detecção funcionou**, mas a chave a 3–5 cm só levou o \|m\| a −15,6 %, então o `DISTURBIO` durou ~0,6 s. Durante a aproximação, com o desvio abaixo de 15 %, a correção continuou ativa e a proa filtrada acompanhou ~5,6° dos 8° de erro da `mag`. **A proa não saltou** (sempre andou suave) e **voltou ao valor certo em ~5 s** depois de afastar a chave.
- É a limitação prevista: o filtro só atenua distúrbios abaixo do limite. Um ajuste possível é baixar `MAG_DISTURB_PCT` para ~10 % (no lugar de teste, o \|m\| fica de −2 a +5 % em todas as orientações, até ±30° de inclinação) e/ou aumentar τ. Isso fica para decidir depois.

### Magnetômetro: giros rápidos (15/10/2026)

Log `261015-170459`. Caixa nivelada na cadeira, giros de ~90° em ~1,5–2,5 s (até ~60 °/s) pelo encosto: quatro no sentido horário e quatro no anti-horário, com 5 s ou mais parada entre eles.

- **A proa filtrada acompanha os giros sem atraso:** a diferença para a `mag` ficou em até 2,1° durante o movimento. A filtrada vai um pouco à frente, porque o giroscópio responde na hora e a `mag` (média de 8 no chip) atrasa um pouco.
- **Sem ultrapassar o valor final:** ao parar, a filtrada encontra a `mag` em ~1–2 s, ficando a até 0,2° dela. Os pequenos repiques (até ~3°) acompanham a própria `mag`, porque a cadeira balança ao parar.
- **Sem deriva parada:** nos trechos de 10–20 s parada, a filtrada variou até 0,1°.
- Paradas: 99,0 → 191,6 → 283,2 → 10,7 → 99,5° no horário, e 10,7 → 282,8 → 190,7 → 98,2° no anti-horário. \|m\| de 25,0 a 25,7 µT, sem `DISTURBIO`.

### Flags de validade da telemetria (contrato v2, 15/10/2026)

Log `261015-203538`. Firmware com o contrato v2, BMP280 ainda ausente, caixa na mesa. Linha "validade" do `c`:

| Momento | Linha "validade" | `valid` |
|---|---|---|
| Parada, depois do boot | ATT ok, ATT_DEGRADED não, ALT não (sem barômetro), HDG ok, HDG_GYRO_ONLY não | `0x0009` |
| Chave de fenda a 1–2 cm (\|m\| 19,2–21,6 µT, `DISTÚRBIO`) | HDG_GYRO_ONLY sim (DISTURBIO) | `0x0019` |
| Chave afastada | volta ao estado inicial | `0x0009` |

- Boot com `TelemetryPacket = 56 bytes` (era 49): o layout novo está no firmware.
- As flags seguiram as regras de `lib/telemetry_flags`. Durante o distúrbio, a proa filtrada ficou em 101,0°, contra 98,7–98,8° antes e depois. `ALT` e `ATT_DEGRADED` ligados ficam para conferir com o BMP280 e numa saturação do giroscópio.

### Buffer da serial e `m` durante a calibração do boot (15/10/2026)

Logs `261015-214828` (firmware antigo: a gravação não tinha acontecido) e `261015-215433` (firmware novo). Caixa na mesa, BMP280 ainda ausente.

- **Buffer de 1 kB (antigo):** cada `c` (~1,4 kB) deixou o laço parado ~55 ms, com `descartados 11 | reinícios 1` no fluxo `a`: **o `c` reiniciava a fusão.**
- **Buffer de 4 kB (novo):** 6 `c` com o `a` ligado. Em 5 deles, `descartados 0 | reinícios 0`. Em 1, `descartados 13 | reinícios 1`: nesse instante, a nova tentativa do barômetro ausente (a cada 5 s) imprimiu o erro do núcleo (`[E][Wire.cpp:499] requestFrom()`) no meio da saída do `c`. Esse log do núcleo não passa pelo buffer do `Serial` e esperou a transmissão em curso, parando o laço ~65 ms. Sem o `c`, o mesmo erro não causa descarte. Com o BMP280 ligado, a tentativa deixa de existir. Nas tasks (semana 3), a serial fica numa task de baixa prioridade.
- **`m` durante a calibração do giroscópio do boot:** com a caixa balançando, o giroscópio recusou janelas, e o `m` começou a coleta normalmente (antes: "espere a calibração em andamento terminar"). O `x` cancelou ("calibração anterior mantida"). O `z` continuou bloqueado. Com a caixa parada, o giroscópio calibrou e o magnetômetro seguiu com a mesma calibração.

### BMP280 no firmware de voo (19/10/2026)

O BMP280 substituiu o BME280 danificado em 14/10. Ele foi ligado na mesma posição, com CSB e SDO soltos. O firmware é o de 15/10 (carimbo `2026-10-19 20:44:58`), sem mudanças para o BMP280, que o driver já aceitava. A caixa ficou na mesa durante os testes. Logs `261019-204245` (diagnóstico) e `261019-204547` a `261019-211638` (firmware de voo).

**Diagnóstico (`i2cscan`):** 0x1E "H43", 0x68 WHO_AM_I 0x68 e **0x76 chip_id 0x58 (BMP280: sem umidade)**. No `b`, 26,2 °C, 1010,0 hPa e umidade `--`. A altura oscilou entre +0,09 e +0,87 m, o esperado para medições forçadas com oversampling x1 e sem filtro. O autoteste do HMC (`x`) passou nos três eixos (114–123 µT).

**Firmware de voo:**

| Teste | Resultado |
|---|---|
| Boot | `BMP280 configurado (chip_id=0x58)`, `zero aceito` em ~4 s com desvio de 0,27 a 1,36 Pa, `ALT ok` e `valid = 0x000D`. Com isso, o `ALT` ligado, pendente desde 15/10, está conferido |
| Ruído parado, ambiente quieto (35 s) | **1,7 cm** entre amostras seguidas, igual ao BME280 (1,6 cm). Oscilação lenta, de vários segundos: desvio de 8,9 cm, entre −0,19 e +0,28 m, com a temperatura constante |
| Subida de ~1 m (à mão, sem trena) | De ~+0,15 m para +1,0–1,3 m, e de volta a ~+0,2 m: degrau de 0,9–1,1 m |
| `0` com a caixa subindo e descendo ~30 cm | Recusado 5 vezes (1,77 a 2,53 Pa) e aceito parado. A margem é pequena: o filtro IIR atenua o movimento rápido, e uma janela foi aceita com 1,50 Pa, no limite |
| `r` com e sem o `b` | Perdidas, descartadas, falhas e fila cheia em 0 |
| SDA do BMP280 solto (~30 s) e recolocado | `FALHANDO`, com as falhas subindo 1 por segundo depois das 5 primeiras; `ALT não (sem barômetro)`, `valid = 0x0009`; atitude e proa normais; **0 descartes no MPU**. Ao recolocar, `baro: ok` sem reconfiguração |
| Boot com o SDA solto | `sem resposta no endereço I2C`, nova tentativa a cada 5 s; giroscópio calibrado e atitude normal (0 descartes, 0 reinícios). Ao recolocar o SDA, `configurado` e `zero aceito`. Na segunda tentativa: um atraso de 3,9 ms numa leitura do MPU, sem descarte. A primeira tentativa entrou no laço descrito abaixo |
| Deriva, 8,5 min parada | O chip esfriou de 27,0 para 26,6 °C, sem o aviso de temperatura. Média por minuto entre −0,24 e +0,11 m, **sem tendência**. Desvio total de 0,16 m, com picos de −0,53 e +0,40 m. 0 falhas e 0 recuperações |

- **A oscilação lenta é atribuída à pressão do ambiente.** O ruído rápido é igual ao do BME280, a temperatura ficou constante e a média não deriva com o tempo. Ela diminuiu com a janela e a porta fechadas. Na bancada, a altitude relativa vale com ~±0,3 m em 10 min.

**Brownout por mau contato no VCC:** com a caixa sendo movida, o ESP resetou várias vezes com `Brownout detector was triggered`. Numa das vezes, o log parou no meio do boot, a USB caiu e o monitor travou. O autor ajustou o fio de VCC, e depois disso houve ~10 min de fluxo sem nenhum brownout.

**Laço de reconfiguração do BMP280 (logs `210112` e `211203`):**
- Ao recolocar o SDA na primeira tentativa do boot sem o barômetro, o HMC5883L também perdeu a configuração. Isso indica uma perturbação na alimentação dos módulos naquele instante.
- Em seguida, o BMP280 foi configurado e a releitura conferiu os registradores, mas na leitura seguinte ele já estava sem a configuração. Isso se repetiu **54+ vezes**, sem nenhuma amostra e sem o zero terminar.
- No boot seguinte, com 43 inícios de boot cortados antes (alimentação da placa indo e voltando), o laço recomeçou logo depois do `BMP280 configurado`.
- Interpretação: o chip **reiniciava sozinho** a cada poucos ms, provavelmente com o VCC sem contato e alimentado só pelos pull-ups do I2C. É o mesmo mecanismo de 12/10. Com o fio ajustado, a sessão seguinte saiu limpa.

**Falha do driver revelada pelo laço, corrigida em 19/10/2026:**
- Cada reinício era tratado como uma recuperação bem-sucedida e não contava como falha. Então `ok()` continuava verdadeiro, e o `main.cpp` seguia com um reset completo (espera, status, coeficientes e escritas) a 25 Hz, em vez de passar a tentar a cada `BARO_FAIL_RETRY_MS`.
- Efeitos: `fila cheia 185` (pulsos do INT perdidos) e a calibração do giroscópio recusada por `janela incompleta (199 amostras)`.
- Correção em `lib/bme280`: a configuração perdida passa a contar como falha, mesmo quando a reconfiguração dá certo. Uma queda isolada continua sem derrubar `ok()`, porque a primeira amostra válida zera a sequência. Um chip que reinicia sem parar derruba `ok()` depois de `BARO_MAX_FAIL_STREAK` (5) recuperações, e o firmware passa a tentar 1 vez por segundo.
- **Conferido na bancada (log `261019-213533`, carimbo `2026-10-19 21:34:51`):**
  - boot com `zero aceito` (0,44 Pa) e `recuperações 0`;
  - SDA solto: `FALHANDO`, falhas subindo 1 por segundo, `valid = 0x0009` e 0 perdidos ou descartados;
  - SDA recolocado: `baro: ok`, `recuperações 0` e `ALT ok`.

  Igual ao comportamento de antes da correção. No encaixe do SDA, dois `Error 263` (timeout do I2C) seguidos travaram o barramento por ~100 ms e deram `fila cheia 22`. Isso vem do contato oscilando durante o encaixe com a placa ligada, e não da correção: não houve recuperação.
- O cenário do chip reiniciando não foi reproduzido de propósito, para não repetir o latch-up de 12/10. Ele fica verificado só pela leitura do código.

**Blocos ilegíveis:** três linhas dos logs começam com 64 caracteres ilegíveis. Isso é um bloco USB corrompido no PC (64 bytes é o tamanho do pacote), e não um erro do firmware.

### Linha de base antes da refatoração (20/10/2026)

Log `261020-130702`, firmware `af389ba` (carimbo `2026-10-20 13:06:22`). A caixa ficou parada na mesa durante todo o roteiro base do [refatoracao.md](refatoracao.md). As métricas completas estão lá.

- **Primeiro boot do log:** o giroscópio recusou duas janelas por movimento (desvio de 0,49 e 2,06 °/s), por causa do manuseio antes do reset pelo EN. No segundo boot, calibrou na primeira tentativa.
- **Com tudo ligado:**
  - 199–200 Hz, sem nenhuma perda, descarte, falha ou fila cheia;
  - dt de 5,01 ms;
  - atraso máximo de 110–125 µs;
  - filtro com custo médio de 60–73 µs e máximo de 85–193 µs.
- **Magnetômetro:** 25 leituras por segundo e proa estável em 102,2° (±0,1°).
- **Barômetro:** altitude entre −0,31 e −0,05 m em ~6 s.

### Barômetro falhando com o resumo ligado (20/10/2026)

Log `261020-134704`, firmware da refatoração, passo 2 (carimbo `2026-10-20 13:37:47`). O SDA do BMP280 ficou solto por ~50 s com o `r` ligado.

- **MPU:**
  - `descartadas 10` em quase toda janela de 1 s, e `1` nas outras;
  - atitude com 170 de 200 amostras: **a fusão reiniciou a cada segundo**, porque a lacuna de ~55 ms passa do `FUSION_MAX_DT_MS` (50);
  - perdidas e fila cheia em 0.
- **Causa:** é o mecanismo de 15/10 ("Buffer da serial"). O log de erro do núcleo (`[E][Wire.cpp:499]`) de cada tentativa do barômetro, 1 por segundo, espera a transmissão do resumo em curso (~600 bytes, ~52 ms).
- **Depende da fase entre as duas coisas, que têm o mesmo período de 1 s:**
  - neste log, a tentativa caía logo depois da primeira linha do resumo;
  - em 19/10 (log `261019-210112`, firmware antigo), caía logo antes, e houve 0 descartes.
- **Magnetômetro:** perdeu a configuração uma vez com o fio solto e voltou em ~1 s (`recuperações 1`).
- **Tratamento:** fica para a semana 3 (pendência 2 do [refatoracao.md](refatoracao.md)).
- **Depois do teste, o BMP280 ficou mudo de ~13:48 a 14:05 (logs `135408` a `140511`)**, mesmo com o SDA encaixado. Aos 25,1 s do log `140247` houve o `Error 263` do encaixe, e depois só `Error -1` por 80 s.
  - Às 14:08, o diagnóstico (`140827`) achou o 0x76 de novo, e o firmware de voo voltou a configurar o chip (`141053`).
  - O EN não desliga os módulos. Se um sensor parar de responder depois de mexer nos fios com a placa ligada, **tire o USB por 5 s** antes de procurar outro defeito.

### Carimbo do pulso do INT dependente do build (20/10/2026)

Teste A/B na mesma sessão, com a caixa parada e só o `a` por 10 s.

- **Passo 3+4 da refatoração** (log `261020-151621`): dt sempre 5,00/5,01/5,01 ms.
- **Passo 5** (log `261020-151801`): 75 de 85 linhas entre 4,95 e 5,06 ms, com a média em 5,01 ms.

O passo 5 não mexe na ISR. A hipótese é que o carimbo `micros()` da ISR do GPIO atrasa ~50 µs quando o pulso coincide com a ISR de transmissão da UART, que está no mesmo nível 1. Além disso, o despachante `__onPinInterrupt` do Arduino roda da flash: o `CONFIG_ARDUINO_ISR_IRAM` está desligado. Correção prevista para a semana 3 (pendência 5 do [refatoracao.md](refatoracao.md)).

Também neste dia, com um calço mais alto, a meia diferença entre lados opostos deu 21,97° de roll e 9,53° de pitch, contra ~20–21° e ~9–10° no inclinômetro do celular. O ponto médio entre os lados repetiu a caixa nivelada (log `261020-150426`).

### Atraso do pulso com o `r` e o `a` juntos (20/10/2026)

A/B do passo 7 da refatoração (logs `261020-195007`, `195242` e `200224`), com a caixa parada e o `r` e o `a` ligados juntos por ~80 s.

- **O atraso pulso → leitura sobe logo depois de cada resumo `r`.** O resumo é formatado no laço de aquisição e leva perto de um período de 5 ms, então o pulso seguinte espera.
  - Build do passo 6b: 0,1–0,2 % das linhas acima de 200 µs, com pico de 574 µs.
  - Build do passo 7: 3,3 % das linhas acima de 200 µs, com pico de 1 949 µs.
  - Nos dois, com a caixa parada: 0 ambíguos, descartes e reinícios. A espera da leitura não entra no dt, porque o instante do pulso vem da ISR. O dt tem a variação de ±50 µs de alguns builds (pendência 5), e os dois builds a mostraram nestas rodadas.
  - Com o `a` sozinho, o atraso fica em 108–121 µs.
- **O mesmo código do 6b, compilado em outra pasta (um worktree), teve dt com ±50 µs** em ~1,5 % das linhas. Compilado na pasta principal, não teve. Isso reforça a dependência da posição do código ("Carimbo do pulso do INT dependente do build", acima).
- **Passo 8 (log `261020-202749`), com o `a` sozinho:** atraso de 102–197 µs, com 48 % das linhas acima de 150 µs. Nos passos 6b e 7, 1 de 232 linhas passou de 125 µs. É o padrão por build já visto no passo 3+4 (cache da flash), dentro do critério de ~200 µs, mas com pouca margem.
- Volta a ser medido na semana 3, com a serial numa task própria ([refatoracao.md](refatoracao.md), pendência 2).

### ISR do INT pelo ESP-IDF, em IRAM e no nível 2 (21/10/2026)

Pendência 5 do [refatoracao.md](refatoracao.md). A ISR passou a ser registrada pelo `gpio_install_isr_service(ESP_INTR_FLAG_IRAM | nível 2)` + `gpio_isr_handler_add`, sem o `attachInterrupt`. O carimbo vem do `esp_timer_get_time()`. No binário, `onMpuInt`, `gpio_intr_service`, `esp_timer_get_time` e `xQueueGenericSendFromISR` ficam na IRAM (0x4008…), e o `micros()` do núcleo, usado antes na ISR, fica na flash (0x400d8ebc).

**Bancada (log `261021-112920`, carimbo `2026-10-21 11:28:21`), caixa parada:**

| Rodada | dt fora de 5,00–5,01 ms | Atraso pulso → leitura | Outros |
|---|---|---|---|
| `a` sozinho, 127 linhas | 0 | 104–112 µs | filtro méd. 32–93 µs, máx. 69–133 µs; ambíguos, descartados e reinícios em 0 |
| `r` e `a` juntos, 819 linhas (~82 s) | 0 | até 182 µs em 816 linhas; 301, 504 e 723 µs em 3, todas a 8 linhas de um resumo `r` | `r` com 199–200 Hz e tudo em 0; ambíguos e reinícios em 0 |
| INT solto ~14 s | — | — | 13 janelas `0 Hz … sem pulsos do INT`; na volta, `perdidas 2896` e depois 199–200 Hz com tudo em 0, sem registrar a ISR de novo; `c` final com `valid = 0x000D` |

- Os 3 picos acima de 200 µs são a formatação do `r` no laço (pendência 2), não a ISR: o carimbo vem da ISR, e o dt não mudou nessas linhas.

**A/B com o build do passo 5** (`ec2c701`, onde a variação apareceu em 75 de 85 linhas em 20/10). Dois worktrees com nomes de mesmo comprimento; só o `a`, por ~20 s:

| Build | Carimbo | Log | Linhas | dt fora de 5,00–5,01 ms | Atraso |
|---|---|---|---|---|---|
| A: passo 5 sem correção (flash 365 229 B, igual à de 20/10) | `11:48:15` | `261021-114911` | 185 | **0** | 108–173 µs |
| B: passo 5 com a correção | `11:50:23` | `261021-115112` | 208 | 0 | 104–185 µs |

- **A variação não se reproduziu no build que a mostrou em 20/10**, com o mesmo tamanho de flash. O A/B não confirma nem descarta a hipótese da ISR da UART. Ele enfraquece a explicação "depende só do build": a condição de 20/10 também dependia de algo da sessão (fase entre a impressão e os pulsos, temperatura ou relógio), que não foi identificado.
- **O atraso bimodal (~105 ou 150–185 µs) aparece nos dois builds do passo 5, com e sem a correção**, e não no build atual (104–112 µs). Então ele não vem do caminho da ISR, o que é coerente com a hipótese do cache da flash no caminho da leitura (passo 3+4).
- **Decisão:** a correção fica, porque é o desenho certo para as tasks (ISR curta em IRAM, acima da UART, que funciona com o cache desligado durante as gravações na NVS). A mudança elimina a dependência do despachante GPIO na flash e permite preempção da UART de nível 1; outras fontes de latência da ISR continuam possíveis: as seções críticas (`portENTER_CRITICAL` mascara até o nível 3, o que inclui o nível 2) e as interrupções de nível mais alto. O dt de 5,00–5,01 ms continua como critério em toda bancada.

### Logger com buffer e log do núcleo redirecionado (21/10/2026)

Pendência 2 do [refatoracao.md](refatoracao.md), parte que não depende das tasks. O `lib/logger` ganhou um buffer de 8 kB (`LOG_BUFFER_BYTES`), esvaziado pelo `logPump()` no `loop()` só no espaço livre do `Serial`. Com ele cheio, a mensagem é descartada e contada. O `[E][Wire.cpp]` do núcleo passou a entrar nesse buffer: `setDebugOutput(false)` desliga a espera do `log_printfv` pela UART ociosa, e o `ets_install_putc1` desvia o `ets_printf`. Firmware com carimbo `2026-10-21 12:09:12`.

| Rodada | Log | Resultado |
|---|---|---|
| Roteiro base | `261021-120956` | `i2cInit()` do núcleo aparece no boot (passa pelo logger). `a`, 92 linhas: dt 5,00–5,01 ms, atraso de 100–113 µs, filtro méd. 25–73 / máx. 45–113 µs, ambíguos, descartados e reinícios em 0. `r` com 199–200 Hz e tudo em 0. Mensagens iguais às do passo 8 (`log_templates.py`) |
| **SDA do BMP280 solto, com o `r`** | `261021-122908` | **73 janelas com o barômetro `FALHANDO` (falhas de 5 a 77): todas com 199–200 Hz, `descartadas 0` e 199–200 amostras de atitude.** Um `[E][Wire.cpp:499] … Error -1` por segundo, completo, pelo logger. Em 20/10, com o firmware antigo, a mesma situação dava `descartadas 10` e ~170 amostras por janela |
| `r`, `a`, `b`, `p` e `c` juntos | `261021-121254` | Nenhuma mensagem descartada e nenhuma linha cortada. Atraso de 2 437 µs na linha do `a` logo depois do `c`, e de 447 e 381 µs depois de dois resumos: a formatação no laço, o resto da pendência 2. Uma linha (de 218) com dt máximo de 5,02 ms, sem ambíguos: uma observação, porque o logger acrescenta seções críticas, que mascaram até o nível 3 |
| Dump `d` | `261021-121254` | Coleta de 180 s com 4 500 pontos, recusada (raio de 0,16 µT, caixa parada). O `d` imprimiu as 4 500 linhas (~100 kB, oito vezes o buffer) sem descarte: cada linha espera espaço |

- **O BMP280 não voltou depois de recolocar o SDA** (77 falhas seguidas até o fim do log). No boot seguinte, depois de tirar o USB, ele configurou e aceitou o zero (log `261021-123137`). É o mesmo comportamento de 20/10: o módulo pode ficar mudo até a alimentação ser cortada. A volta do chip com o firmware rodando continua não vista ao vivo.
- **Início do log `261021-122908`:** 138 linhas cortadas (`urned Error -1`), seguidas de ~45 reinícios no ROM (`rst:ets Jul 29 …`, sem completar o boot) e de um `POWERON_RESET`. Tudo isso foi antes do boot da rodada, na troca do USB. Os reinícios em sequência indicam alimentação instável no encaixe. As linhas cortadas não podem vir do descarte do logger, que retira mensagens inteiras. A falta do aviso `[log] … descartadas` não é evidência: no firmware deste log, o aviso dependia de uma mensagem posterior, e os reinícios logo depois apagariam a contagem pendente. A causa não foi identificada. Se aparecer de novo com o monitor já aberto e a placa estável, investigar.
- **Correções da revisão (21/10, antes do commit):** a regra do buffer e do descarte foi para um núcleo puro (`LogCore`, em `lib/log_line`, testado no `native`). Os defeitos corrigidos:
  - dois produtores podiam anunciar o mesmo lote e descontá-lo duas vezes, e o contador `uint32_t` dava a volta;
  - a última perda de uma rajada ficava sem aviso;
  - uma mensagem posterior ao descarte podia sair antes do aviso;
  - os contadores eram lidos sem a trava.
  - **Desenho final:** só o consumidor (`logPump()`) publica o aviso, quando o buffer esvazia, e só ele escreve no `Serial`. Com um descarte pendente, os produtores descartam também as mensagens novas, com a conferência e o incremento na mesma seção crítica. Todas as leituras dos contadores usam a trava.
  - **Garantia:** o aviso sai depois de toda mensagem aceita antes do descarte e antes de toda mensagem que começou depois dele.
  - **Custo:** depois de um descarte, tudo é descartado até o buffer esvaziar (até ~0,7 s).
  - **Testes:** as intercalações são simuladas por preempção na trava falsa (`test_log_core`).
  - **Bancada do build corrigido** (log `261021-125953`, carimbo `12:59:11`):
    - boot com `i2cInit()` pelo logger, giroscópio calibrado e zero aceito; o `c` deu `valid = 0x000D`;
    - `r`, 15 janelas: 199–200 Hz com tudo em 0;
    - `a`, 155 linhas: dt 5,00–5,01 ms, atraso de 100–121 µs, filtro méd. 31–97 / máx. 60–161 µs, ambíguos, descartados e reinícios em 0;
    - nenhum `[log] … descartadas`.

**Fios mexidos por engano (logs `261021-121254` e `261021-122153`).** Nas duas primeiras tentativas da rodada do SDA, o fio solto foi o do INT (GPIO 19):

- **INT solto de vez:** 30 janelas com `sem pulsos do INT` e a volta com `perdidas` grande, como na P5.
- **INT meio encaixado:** **600 a 3 750 pulsos falsos por segundo** por ~40 s, com `descartadas` até 2 019 por janela, `fila cheia` de 0 para 54 e `pico |g|` de até 467 °/s com a caixa parada. A entrada com mau contato capta as bordas do I2C. Os picos são leituras feitas fora do "data ready", que pegam os registradores no meio da atualização. A atitude não mudou (±0,00°) e não houve reinícios, mas uma leitura assim pode passar pela validade temporal quando nenhum pulso falso chega durante ela (pendência 8 do [refatoracao.md](refatoracao.md)).

**Deslocamento das medidas do MPU depois do manuseio dos fios (21/10).**

| Logs | Bias z do giroscópio | `a` bruto em z | \|a\| corrigido |
|---|---|---|---|
| 10 boots de 11:28 a 12:18 | +1,15 a +1,18 °/s (29–33 °C) | +1,106 g | ~1,000 |
| `122153` (12:21) | +1,81 a +1,83 °/s (28,2 °C) | +1,118 g | 1,018 |
| `122908` (12:29) | +1,50 °/s (27,8 °C) | +1,115 g | 1,009 |
| `123137` (12:31) | +1,42 °/s (27,7 °C) | — | — |
| `125953` (12:59) | +1,42 °/s (27,2 °C) | — | — |

- A temperatura e a inclinação da caixa (~0,4°) não explicam a mudança. Ela começou depois do manuseio dos fios perto do módulo e está voltando aos poucos. A hipótese é tensão mecânica no módulo do MPU, que desloca os offsets do MEMS.
- O giroscópio é recalibrado a cada boot, então o bias novo é absorvido. O offset do acelerômetro (~10 mg em z) fica fora da calibração de seis posições até ela ser refeita, o que será feito no modelo final de qualquer forma.
- **Acompanhar:** se o bias z não voltar a ~+1,16 °/s nas próximas bancadas, conferir o assento do módulo.

### Aviso de tecla ignorada na calibração do magnetômetro (21/10/2026)

Pendência 1 do [refatoracao.md](refatoracao.md). Log `261021-131836`, carimbo `2026-10-21 13:10:11`, caixa parada na mesa. O monitor não grava as teclas digitadas, então cada tecla foi dada logo depois de uma linha de progresso conhecida (a cada 2 s), e o aviso foi conferido entre ela e a linha seguinte.

| Tecla | Depois da linha | Avisos até a linha seguinte |
|---|---|---|
| `c` uma vez | `4 s` | 1 (`[mag] calibração em andamento: x cancela`) |
| `c` quatro vezes rápido | `10 s` | **1**: limite de um por segundo (`MAG_KEY_HINT_MS`) |
| Enter três vezes | `22 s` | 0 |
| nenhuma, até o fim da coleta | — | 0 (180 s, 4 500 pontos, recusada: raio de 0,17 µT) |
| `c` na revisão | `calibração RECUSADA` | 1 (`[mag] revisão da calibração: s = gravar \| x = sair \| d = pontos em CSV`) |

- O `x` deu `resultado descartado; calibração anterior mantida`, e o `c` seguinte mostrou a mesma calibração (offset +8,1 +6,2 +10,6 µT) com `valid = 0x000D`.
- Um log anterior com o mesmo firmware (`261021-131127`) teve 4 avisos na coleta, todos a mais de 1 s um do outro, mas sem a sequência de teclas controlada, e o passo da revisão não foi feito.
- **Correções da revisão (21/10, antes do commit), depois desta bancada:**
  - **Intervalo:** passou a usar a diferença sem sinal do `millis()`, com um indicador de primeiro aviso. Com o `Periodic` (diferença com sinal em µs), depois de ~35 min sem avisos (a revisão pode ficar aberta) uma tecla ignorada deixaria de gerar resposta por mais ~35 min.
  - **Dica da revisão recusada:** passou a ser `[mag] revisão da calibração recusada: x = sair | d = pontos em CSV`, sem oferecer o `s`, que não grava nesse caso.
  - **Bancada do build corrigido** (carimbo `13:32:42`):
    - log `261021-133323`: um aviso entre as linhas `4 s` e `6 s` depois de um `c`, e dois entre `10 s` e `12 s` depois de quatro `c` rápidos. Isso é compatível com as teclas espalhadas por mais de 1 s; na rodada de 13:18 saiu um só;
    - log `261021-134014`: logo depois de `calibração RECUSADA`, um `c` deu `[mag] revisão da calibração recusada: x = sair | d = pontos em CSV`, e o `x` deu `resultado descartado`;
    - nas duas rodadas, a coleta terminou pelo ruído aos 72 e 86 s, com a caixa parada (pendência 3).
- **`fila cheia` depois da coleta:** o `c` seguinte mostrou `fila cheia 15` (1 801 pontos, log `133323`) e `59` (4 500 pontos, log `131836`). O ajuste do fim da coleta roda no laço e o para por mais de 80 ms (16 pulsos na fila). O comportamento já existia e é anterior a esta mudança. Na migração para as tasks, o ajuste pode ir para uma task de prioridade baixa.
- **Primeiro boot do log `133323`:** aos 9 s, uma rajada de `[E][Wire.cpp]` com reinícios do barramento, `HMC5883L perdeu a configuração` e um `POWERON_RESET`. É a alimentação ou o contato oscilando, como nos outros casos de hoje. O teste rodou no boot seguinte.

### Cobertura do `m` com distância mínima (21/10/2026)

Pendência 3 do [refatoracao.md](refatoracao.md). No progresso, um ponto só conta para a cobertura a pelo menos `MAG_CAL_COVER_MIN_UT` (10 µT) do centro provisório. Log `261021-135625`, carimbo `2026-10-21 13:55:41`.

| Rodada | Cobertura |
|---|---|
| Caixa parada, 30 s | `0/24` nas 15 linhas, com `faltam: +x(4) -x(4) +y(4) -y(4) +z(4) -z(4)`. Com o firmware anterior, a mesma situação chegava a 13/24 em 10 s e a 24/24 em 72 s (log `261021-133323`), 86 s (`134014`) e 46 s (20/10) |
| Caixa girando na mão, ~25 s, e de volta à mesa | 0 até 6 s (pegando a caixa), depois 3, 6, 8, 10 e 13/24 aos 28 s, com o `faltam:` diminuindo. Parada de novo: 12–13/24 |

- **Quedas de um setor (8→7 e 13→12):** o centro provisório se desloca com a coleta, e alguns pontos mudam de setor ou ficam a menos de 10 µT dele. Isso já acontecia antes (log `261021-131836`: 14→13 aos 18 s). A aceitação reconta os setores com o centro ajustado e não depende disso.
- O fim pela cobertura com uma calibração real não foi conferido aqui. O teste `native` do giro em esfera cobre esse caminho, e a calibração real fica para o modelo final (pendência 4).

### `z` refeito na montagem final (20/10/2026)

Feito na conferência do passo 6b da refatoração (log `261020-185947`, carimbo `2026-10-20 18:53:13`). A caixa foi encostada pelo nariz e, girada 180°, pela cauda, na mesma referência.

| Medida (sem correção de montagem) | Roll | Pitch |
|---|---|---|
| Posição 1 (nariz na referência) | −2,83° | +2,82° |
| Posição 2 (girada 180°, cauda na referência) | −2,48° | +1,06° |
| **Módulo (resultado, gravado)** | **−2,66°** | **+1,94°** |
| Mesa | −0,18° | +0,88° |

- **Conferência:** o módulo é a média das duas medidas e a mesa é a meia diferença, como previsto em [fusao.md](fusao.md). Logo depois do `s`, com a caixa ainda na posição 2, o `c` deu atitude de roll +0,16° e pitch −0,92°. Esse é o oposto da mesa vista na posição 1, como deve ser.
- **Persistência:** no boot seguinte, apareceu `NVS: … montagem calibrada`, e o `c` mostrou os mesmos −2,66° / +1,94°.
- **Diferença para o `z` de 14/10** (módulo em −3,28° / +2,52°, mesa em −0,95° / −0,23°): +0,62° em roll e −0,58° em pitch. Isso passa da repetibilidade do assento medida em 13/10 (0,26°). A mesa também mudou, então a caixa estava em outro ponto da mesa. A causa provável é o assento do fundo de papelão ou o módulo ter se acomodado depois de uma semana de manuseio. Não indica erro no cálculo, que fecha na conferência acima. A montagem será refeita no modelo final.
- **Medida recusada:** na segunda posição, duas medidas foram recusadas por `caixa em movimento` (20,9 e 30,6 °/s), e o firmware pediu a mesma posição de novo.

## Orientação de montagem do MPU6050

### Montagem final (14/10/2026, em uso)

O MPU foi girado 90° em relação à montagem 5. Eixos do acelerômetro confirmados com o `l` e o `m` do diagnóstico (seção "Montagem final na caixa"):

| Posição | Eixo em ≈ +1 g (ou −1 g) | Conclusão |
|---|---|---|
| Nivelada, face para cima | az = +1,10 g (com o offset de Z) | Z+ do chip aponta para cima |
| Nariz para cima | ay = +1,00 g | Y+ do chip aponta para o nariz |
| Asa direita para baixo | ax = −0,98 g | X+ do chip aponta para a asa direita |

Ainda um sistema destro (X × Y = Z). Mapeamento no firmware (`chipToBody` em `lib/attitude`, desde 14/10):

| Modelo | Chip |
|---|---|
| x (nariz) | +Y |
| y (asa direita) | +X |
| z (para baixo) | −Z |

- O giroscópio usa os mesmos eixos: asa direita descendo dá gy+, nariz subindo gx+, nariz à direita gz−; no modelo, roll, pitch e yaw positivos. Roll e pitch confirmados com o `a` em 14/10 (a coluna "só giroscópio" acompanhou o filtro); o yaw será conferido no teste de giros da proa.
- Os nomes das posições do `6` seguem o mapeamento novo: face para cima, face para baixo, asa esquerda para baixo (X para cima), asa direita para baixo, nariz para cima (Y para cima), nariz para baixo.
- **A calibração de montagem (`z`) gravada antes de 14/10 é ignorada** (formato 3 do registro `att_level`): a rotação era relativa aos eixos nominais da montagem 5. As seis posições também precisam ser refeitas, porque o suporte mudou (o offset de Z depende da fixação). Ordem: `6`, depois `z`.

### Montagem 5 (10/10/2026, descontinuada)

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
- Magnetômetro longe do powerbank e dos fios de alimentação: a corrente gera campo magnético e distorce a proa. Calibrar o magnetômetro já montado. Medido em 13–14/10: powerbank ligado ≤ 1,6 µT mesmo encostado, ESP32 1,5 µT, cabo USB 3,2 µT. Mais importante que a distância é que **nada se mova em relação ao sensor** (ver "Magnetômetro: mapeamento de eixos e fontes de distorção").

## Orientação de montagem do GY-273 (HMC5883L)

Levantada em 13/10/2026 (seção "Magnetômetro: mapeamento de eixos e fontes de distorção") e mantida na montagem final (14/10): chip para cima, seta X para o nariz.

| Modelo | Chip |
|---|---|
| x (nariz) | +X |
| y (asa direita) | −Y |
| z (para baixo) | −Z |

No firmware: `magChipToBody` em `lib/mag`. Se a montagem mudar, refaça o teste das 4 proas com o `h` e a calibração do magnetômetro. Um desalinhamento do GY-273 em relação à caixa não tem correção (a calibração de montagem `z` é só do MPU): alinhe-o a 1–2°.
