# Barômetro: BME280 e altitude relativa

Estratégia da leitura do BME280 e da altitude relativa no ESP A. Os valores ficam em `firmware/aircraft/include/config.h` (seção BME280). Os resultados de bancada ficam em [hardware.md](hardware.md).

## Cadeia de processamento

1. **Driver (`lib/bme280`):** acesso direto aos registradores, sem biblioteca externa.
   - **Aceita o BME280 (chip_id 0x60) e o BMP280 (0x58)** desde 14/10/2026. Os dois têm os mesmos coeficientes de temperatura e pressão (0x88), reset, status e rajada a partir de 0xF4. O BMP280 não tem umidade nem o registrador `ctrl_hum` (0xF2), cuja escrita é pulada. Os códigos 110 e 111 da espera entre medições (`t_sb`) também mudam: 10 e 20 ms no BME280, 2 e 4 s no BMP280 (tabela 11 do datasheet do BMP280). O driver confere o `BARO_STANDBY_US` contra o modelo detectado e devolve `BadParam` se ele não existir naquele chip. O valor em uso, 0,5 ms, é igual nos dois. A serial mostra o modelo detectado. O BME280 da bancada foi danificado num curto em 14/10, e o substituto é um BMP280 ([hardware.md](hardware.md), "Montagem final na caixa"). O diagnóstico (`b` do `i2cscan`) também aceita o BMP280 e mostra a umidade como `--`.
   - **Ligação:** CSB e SDO soltos (os resistores do módulo fixam o modo I2C e o endereço 0x76).
   - **No `begin()`:** soft reset, cópia dos coeficientes de fábrica e configuração, conferida na releitura.
   - **A cada leitura:** uma rajada de 9 bytes a partir de `ctrl_meas` (0xF4). Ela traz a configuração e os dados brutos de pressão e temperatura na mesma transação.
   - **Saída:** °C e hPa.
2. **Lógica pura (`lib/baro`, sem `Arduino.h`):**
   - compensação pelas fórmulas inteiras do datasheet (temperatura em 32 bits, pressão em 64 bits);
   - altitude relativa;
   - avaliação da janela de zero.

   Testada com `pio test -e native`.
3. **`main.cpp` provisório:** agenda as leituras, faz o zero, avisa a deriva térmica e mostra tudo na serial.

A umidade fica desligada. Ela não entra na telemetria nem no horizonte, e desligada a medição fica mais curta.

## Configuração: perfil "indoor navigation"

Ponto de partida: tabela 9 do datasheet (seção 3.5.3), sem a umidade.

| Parâmetro | Valor | `config.h` |
|---|---|---|
| Modo | Normal: o chip mede sozinho e continuamente | — |
| Oversampling da pressão | x16 | `BARO_OSRS_P` |
| Oversampling da temperatura | x2 | `BARO_OSRS_T` |
| Filtro IIR | Coeficiente 16 | `BARO_IIR_COEF` |
| Espera entre medições | 0,5 ms | `BARO_STANDBY_US` |

Desempenho segundo o datasheet:
- **Ruído RMS:** 0,2 Pa, ou 1,7 cm. O diagnóstico, com x1 e sem filtro, mediu ~3 Pa, ou ~27 cm.
- **Taxa de saída:** a medição leva ~37,5 ms típicos (apêndice B: 1 + 2·2 + 2·16 + 0,5 ms), mais 0,5 ms de espera, o que dá ~26 Hz.
- **Resposta a um degrau:** o IIR 16 precisa de 22 amostras para chegar a 75 % do degrau (tabela 6). A ~25 Hz, isso dá **~0,9 s**. Se esse atraso incomodar, o IIR 4 precisa de 5 amostras (~0,2 s), com mais ruído.

**Por que o modo normal:** o laço precisa atender o MPU a cada 5 ms. No modo forçado, cada medição exigiria esperar ~40 ms ou gerenciar o tempo da conversão. No modo normal, o firmware só lê o último resultado, sem bloquear. Além disso, o filtro IIR é feito para esse modo.

**Por que o soft reset no `begin()`:** reiniciar o ESP não reinicia o BME280, e o registrador `config` só tem a escrita garantida em sleep. O reset devolve o chip ao padrão e garante que a configuração seja aplicada.

## Leitura e convivência com o MPU

- **Taxa:** o firmware lê a `BARO_READ_HZ` (25 Hz). A leitura acontece **logo depois de uma leitura do MPU**:
  - a transação do BME280 (~0,25 ms a 400 kHz) cai longe do próximo pulso do INT, que vem ~4,5 ms depois;
  - as bordas desse pulso já causaram `Error -1` no I2C (ver [hardware.md](hardware.md)).
- **Sem pulsos do MPU (13/10/2026):** o barômetro não depende do MPU para ser atendido.
  - A espera pelo pulso do INT é feita em fatias de `MPU_INT_WAIT_SLICE_MS` (10 ms). Com o MPU funcionando, o pulso chega dentro da primeira fatia e nada muda. Se os pulsos pararem (fio do INT solto, MPU sem alimentação), o barômetro é atendido entre as fatias.
  - Com o MPU ausente, a configuração dele é tentada a cada `MPU_RETRY_MS` (2 s) sem bloquear o laço, e o barômetro segue a 25 Hz.
- **Leituras repetidas:** como a taxa de leitura (25 Hz) e a de saída do chip (~26 Hz) são próximas, às vezes a mesma medição é lida duas vezes ou uma é pulada. O efeito é desprezível para a média do zero e para a exibição.
- **Configuração conferida em toda leitura:**
  - se o BME280 perder a alimentação, ele volta em sleep com a configuração padrão, e sem a conferência os dados ficariam congelados sem ninguém perceber;
  - quando a releitura não bate, o driver reconfigura o chip e conta uma recuperação;
  - a reconfiguração leva alguns milissegundos e pode atrasar uma leitura do MPU. Isso só acontece numa recuperação.
- **Falhas:**
  - contam como falha: transação sem resposta, reconfiguração que falhou, compensação inválida (coeficientes inconsistentes) e "não medido" depois da tolerância inicial. A tolerância vale duas medições no tempo máximo do datasheet mais a espera entre elas (~87 ms no perfil atual);
  - a sequência de falhas só é zerada por uma **amostra válida**. Reconfigurar não basta: depois de uma recuperação, `ok()` só volta na primeira amostra boa;
  - se uma configuração falhar no meio, por exemplo só na releitura final, a leitura seguinte refaz a configuração em vez de seguir com o estado incerto. Esses caminhos (releitura falhando, "não medido" persistente e compensação inválida) não dá para provocar com segurança no hardware: estão verificados só pela leitura do código;
  - depois de `BARO_MAX_FAIL_STREAK` falhas seguidas, `ok()` fica falso, o que equivale a `ST_BARO_OK` desligado;
  - enquanto `ok()` estiver falso, o firmware tenta ler só a cada `BARO_FAIL_RETRY_MS` (1 s), e não a 25 Hz. Cada transação sem resposta imprime um erro do I2C e atrasa o laço: sem esse intervalo, o MPU chegou a descartar 25 amostras por segundo com o BME280 fora do barramento (hardware.md, 12/10/2026). Quando a leitura volta a funcionar, a taxa volta a 25 Hz;
  - o driver do BME280 não reinicia o barramento: essa tarefa é do driver do MPU, dono do barramento compartilhado.
- **BME280 ausente no boot:** a atitude funciona normalmente, e o firmware tenta de novo a cada `BARO_RETRY_MS` (5 s). Sem chip, a tentativa é só um endereço sem resposta, então é rápida.

## Altitude relativa

h = 44330 · (1 − (p/p₀)^0,1903), a fórmula barométrica padrão, com p₀ sendo a pressão do zero. Com p ou p₀ não finitas, nulas ou negativas, `baroAltitudeM()` devolve NaN (desde 15/10/2026, com teste `native`), e a serial mostra "alt inválida" em vez de um número.

- Perto de 1010 hPa, 12 Pa equivalem a ~1 m (teste `native`).
- Na escala da bancada, poucos metros, o erro do modelo é desprezível perto do ruído e da deriva.

## Zero

1. **No boot e depois de uma reconfiguração com o zero em andamento:** espera `BARO_SETTLE_MS` (2 s) pelo filtro IIR, que começa da primeira medição.
2. **Janela de `BARO_ZERO_WINDOW_MS` (2 s, ~50 leituras):** p₀ é a **média** da janela. No diagnóstico, zerar com uma leitura só errou até 0,33 m. Uma janela com média ou desvio não finitos é recusada (defesa; o driver não produziu esse valor na bancada).
3. **Aceite:** a janela só vale com pelo menos 90 % das leituras e com desvio da pressão até `BARO_ZERO_MAX_STD_PA` (1,5 Pa ≈ 12 cm). Se não passar, a janela é medida de novo.
4. **Comando `0`:** zera de novo, sem a espera do passo 1.

Decisões:
- **O zero não é gravado na NVS.** A pressão ambiente muda de uma sessão para outra, então um p₀ salvo não serviria para nada.
- **Se o chip for reconfigurado depois do zero, o p₀ continua o mesmo.** A altitude continua relativa ao mesmo ponto, e o filtro recomeça da medição atual.
- **O limite de 1,5 Pa ainda é provisório.** Ele vale ~7 vezes o ruído do datasheet, para tolerar pequenas variações da sala. Vai ser revisto com o ruído medido na bancada.

## Deriva térmica

O datasheet especifica um coeficiente térmico do offset da pressão (TCOP) de ±1,5 Pa/K, ou ~±13 cm/K. O diagnóstico observou o mesmo efeito ao aquecer o chip com o dedo.

**Não há compensação.** Um modelo calibrado numa placa só, sem câmara térmica, não seria confiável. Em vez disso:
- o firmware mostra o ΔT desde o zero;
- avisa uma vez quando ele passa de `BARO_TEMP_WARN_C` (1 °C).

**Recomendação de uso:** zerar com `0` depois de alguns minutos ligado, quando a placa já estiver na temperatura de trabalho.

## Diagnóstico no `main.cpp` provisório

- **`b`:** fluxo a `BARO_PRINT_HZ` (10 Hz) com temperatura, pressão, altitude e ΔT desde o zero.
- **`0`:** zera a altitude de novo.
- **`r`:** o resumo por segundo ganha uma linha `baro` com:
  - pressão, altitude e temperatura;
  - falhas, falhas seguidas e recuperações.
- **`c`:** mostra o p₀ e a temperatura do zero, mais a mesma linha do `r`.

## Roteiro de validação na bancada

Pré-requisito: protótipo com o MPU, o BME280 e o GY-273 no barramento, e boot com a caixa parada.

1. **Boot:**
   - `[baro] BME280 configurado`, e depois `zero aceito` em ~4 s, com desvio abaixo de 1,5 Pa;
   - o giroscópio também calibra nesse intervalo.
2. **Ruído parado (`b`, ~30 s):** desvio da altitude. A meta é algo perto dos ~2 cm do datasheet, bem abaixo dos ~27 cm do diagnóstico.
3. **Subida de ~1 m:**
   - a altitude deve chegar a ~1 m;
   - registrar o tempo até ~75 % (esperado ~0,9 s);
   - ao voltar para a mesa, a altitude retorna a ~0.
4. **Zero com a caixa mexida:**
   - `0` com a caixa sendo levantada e abaixada: a janela tem que ser recusada como instável;
   - `0` com a caixa parada: aceita.
5. **Deriva por ~10 min depois do boot (log em arquivo):** altitude e ΔT. Conferir o aviso de temperatura e o efeito de zerar de novo.
6. **BME280 fora do barramento, com o chip alimentado:** soltar **só o SDA** do módulo por ~10 s e depois recolocar. O VCC e o GND continuam ligados.
   - Esperado: `baro: FALHANDO`, com as falhas subindo **~1 por segundo** depois das 5 primeiras, e o MPU com no máximo ~1 descarte por segundo.
   - Ao recolocar, `baro: ok` sem reconfiguração, porque o chip não perdeu a configuração.
   - **Não fazer queda de alimentação no hardware.** Soltar só o VCC não desliga o chip: ele continua alimentado pelos pull-ups através dos diodos de proteção dos pinos de I2C. Religar o SDA e o SCL antes do VCC provocou um provável latch-up (chip a 40,5 °C, barramento travado por ~3 s; hardware.md, 12/10/2026). O caminho de recuperação do firmware (reconfiguração com o mesmo p₀) já foi validado nesse evento.
7. **Convivência com o MPU (`r` por alguns minutos, com e sem o `b`):**
   - **perdidas** e **descartadas** devem continuar em zero, como antes do BME280;
   - nenhuma falha de I2C nos dois sensores.
8. **BME280 desligado do barramento antes do boot:**
   - mensagem de indisponível;
   - a atitude funciona normalmente;
   - ao religar e esperar 5 s, o chip é configurado e o zero é feito.
