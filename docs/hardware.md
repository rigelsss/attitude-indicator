# Hardware — registro de montagem e testes

Este documento guarda a **evidência** observada em bancada. Os valores que o firmware usa ficam em `include/config.h` de cada projeto. Se um teste contradisser o `config.h`, corrija o `config.h` e registre aqui o teste que motivou a mudança.

## Diagnóstico I2C (ESP A)

Ambiente `i2cscan`, em `firmware/aircraft` (fonte em `src/diag/i2c_diag.cpp`):

```powershell
cd firmware/aircraft
pio run -e i2cscan -t upload
pio device monitor -b 115200
```

O diagnóstico varre o barramento, identifica os chips pelos registradores de ID (WHO_AM_I do MPU, chip_id do BME280, ID "H43" do HMC5883L), compara com o `config.h` e, se o MPU responder, imprime as leituras brutas a 10 Hz. Envie `s` pelo monitor para varrer de novo depois de mexer em cabos. Para voltar ao firmware normal, grave o ambiente `aircraft`.

### Componentes e endereços

| Componente | Módulo | Endereço no `config.h` | Observado | ID lido |
|---|---|---|---|---|
| IMU | MPU6050 (GY-521) | 0x68 | pendente | pendente (esperado WHO_AM_I = 0x68) |
| Barômetro | BME280 | 0x76 | pendente | pendente (esperado chip_id = 0x60) |
| Magnetômetro | GY-273 com HMC5883L | 0x1E | pendente | pendente (esperado "H43") |
| OLED de status | SSD1306 0,91" 128x32 | 0x3C | pendente | não tem ID |

Alguns GY-273 vendidos como HMC5883L trazem um QMC5883L, que responde em 0x0D e precisa de outra biblioteca. O diagnóstico detecta esse caso.

### Registro de testes

| Data | Placa | Ligação | Frequência I2C | Resultado | Observações |
|---|---|---|---|---|---|
| | A (1C:69:20:A4:E9:44) | | 100 kHz | | |

Na coluna "Ligação", anote como os módulos estavam ligados (protoboard ou soldados, comprimento dos fios, alimentação em 3V3). Ela ajuda a explicar falhas intermitentes.

## Orientação de montagem do MPU6050

Pendente. Com a placa parada em cada posição, anote qual eixo do acelerômetro marca cerca de +1 g:

| Posição | Eixo em ≈ +1 g |
|---|---|
| Nivelado, face para cima | |
| Nariz para cima | |
| Asa direita para baixo | |

A partir dessa tabela se definem os sinais de roll e pitch, que depois entram no contrato (`telemetry.h`).
