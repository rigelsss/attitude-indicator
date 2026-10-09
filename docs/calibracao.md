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

- **Critério de repouso.** O desvio padrão de cada eixo do giroscópio fica abaixo de ~50 LSB (≈ 0,4 °/s) e |a| entre 0,95 g e 1,05 g. Os limiares vêm do ruído medido em `hardware.md` e devem ser revalidados com o MPU montado.
- **Sem bloqueio.** O sistema nunca fica parado esperando a calibração. Com o bias provisório o horizonte funciona, e a estação de solo mostra o estado pela flag.
- **Temperatura.** O sistema guarda a temperatura do chip no momento da calibração e avisa se ela variar mais de ~10 °C, o que pede uma nova calibração.
- **Recalibração.** O comando `CMD_CALIBRATE_GYRO` (B → A) roda o mesmo fluxo; durante a janela, `ST_CALIBRATING` fica ligado.
- **Implementação.** A correção do bias é subtraída no software, sem gravar nos registradores de offset do MPU. Assim é mais simples de testar (`pio test -e native`) e de explicar.

## Diagnóstico no firmware principal

O ambiente `i2cscan` é um firmware separado. O firmware de voo também precisa de um comando pela serial (por exemplo, `c`) que mostre, sem regravar a placa:

- leituras brutas e corrigidas;
- coeficientes em uso (offset e escala do acelerômetro, bias do giroscópio);
- estado da calibração: definitiva ou provisória, temperatura no momento da calibração e temperatura atual.

## Ligação com o contrato (`telemetry.h`)

| Item | Uso |
|---|---|
| `ST_CALIBRATING` | janela de calibração em andamento |
| `ST_GYRO_CAL` | bias do giroscópio aceito nesta sessão |
| `CMD_CALIBRATE_GYRO` | recalibração sob comando |

O contrato já cobre o fluxo e não precisa mudar. A calibração em seis posições é feita pela serial na bancada e não passa pelo rádio.
