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
| `l` | Liga ou pausa o fluxo de leituras brutas do MPU (5 por segundo; começa pausado) |
| `m` | Média e desvio padrão de 400 leituras (2 s) com a placa parada |

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

- **Giroscópio.** O bias é grande, mas estável: entre as duas medidas variou menos de 0,01 °/s. Sem correção, o eixo X derivaria cerca de 295° por minuto. Uma média de 2 s parada no boot estima o bias com incerteza de cerca de 0,006 °/s (15 LSB / √400). O bias muda com a temperatura, então a calibração deve acontecer no boot e se repetir sob comando.
- **Acelerômetro.** O módulo da gravidade medido é 1,103 g, 10 % acima do esperado. Isso está acima da tolerância de escala do datasheet (±3 %) e perto do limite de offset do eixo Z (±80 mg). Com uma única posição não dá para separar offset de escala; isso exige a calibração em seis posições (cada eixo em +1 g e −1 g).
- **Inclinação aparente.** Os valores de ax e ay correspondem a cerca de 2,1° de pitch e 1,0° de roll. Na primeira sessão (leituras a olho, com a placa em outra posição) eles eram −0,02 g e +0,06 g. Como mudaram de uma sessão para outra, refletem sobretudo a posição em que a placa estava apoiada, e não um offset fixo. Para separar uma coisa da outra é preciso girar a placa 180° sobre a mesma superfície.
- **Ruído.** É compatível com o datasheet e não mostra nenhum sinal de mau contato. Com o ruído do acelerômetro, um ângulo calculado a partir de uma única amostra oscila cerca de 0,2°; o filtro complementar reduz isso. Ligar o DLPF do MPU (por exemplo, em 42 Hz) também reduz o ruído sem prejudicar a amostragem a 200 Hz.
- **Barramento.** Dois scans seguidos deram o mesmo resultado: apenas o MPU responde.

## Orientação de montagem do MPU6050

Pendente. Com a placa parada em cada posição, anote qual eixo do acelerômetro marca cerca de +1 g:

| Posição | Eixo em ≈ +1 g |
|---|---|
| Nivelado, face para cima | |
| Nariz para cima | |
| Asa direita para baixo | |

A partir dessa tabela se definem os sinais de roll e pitch, que depois entram no contrato (`telemetry.h`).
