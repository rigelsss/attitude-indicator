// config.h — ESP A (avião): pinagem e parâmetros
#pragma once

// ---------- I2C ----------
// Barramento 0 (Wire): sensores (MPU6050, BME280/BMP280, HMC5883L)
#define PIN_I2C_SDA      21
#define PIN_I2C_SCL      22
// Barramento 1 (Wire1): OLED de status, separado para um quadro (~12 ms) não atrasar o MPU (plano.md,
// semana 3). GPIO 32 e 33: uso geral, fora dos pinos de strapping e da flash. Ainda sem uso
#define PIN_I2C1_SDA     32
#define PIN_I2C1_SCL     33
#define I2C_FREQ_HZ      400000
#define I2C_FREQ_DIAG_HZ 100000   // diagnóstico: mais tolerante a fios longos e soldas ruins
#define I2C_TIMEOUT_MS   5        // limite por transação (padrão do Wire: 50 ms); 128 bytes a 400 kHz ≈ 3 ms

// Endereços esperados (confirmar com I2C scan)
#define ADDR_MPU6050     0x68
#define ADDR_BME280      0x76     // 0x77 se SDO estiver em VCC
#define ADDR_HMC5883L    0x1E     // GY-273 com HMC5883L (um clone QMC5883L responderia em 0x0D)
#define ADDR_OLED_STATUS 0x3C

// ---------- GPIO ----------
#define PIN_MPU_INT      19       // interrupção "data ready" do MPU6050
#define MPU_INT_INTR_LEVEL 2      // nível da interrupção do INT (1–3; C não atende níveis acima de 3):
                                  // acima do nível 1 da UART, para o carimbo do pulso não esperar a ISR
                                  // de transmissão (refatoracao.md, pendência 5)
#define PIN_LED_STATUS   2        // LED onboard: pisca = transmitindo

// ---------- Temporização ----------
#define SAMPLE_RATE_HZ   200      // leitura do IMU / fusão
#define TX_RATE_HZ       50       // envio ESP-NOW
#define OLED_RATE_HZ     5
#define DIAG_PRINT_HZ    1        // diagnóstico: taxa de impressão das leituras brutas
#define DIAG_AVG_SAMPLES 400      // diagnóstico: leituras na média com a placa parada (2 s)
#define DIAG_STRESS_MS   5000     // diagnóstico: duração do teste de contato
#define DIAG_INT_TEST_MS 2000     // diagnóstico: duração da contagem de pulsos do INT
#define DIAG_BME_WAIT_MS 10       // diagnóstico: espera da medição forçada do BME280 (máx. 9,3 ms com oversampling x1)
#define DIAG_BME_TIMEOUT_MS 50    // diagnóstico: limite para o BME280 terminar a medição ou o reset
#define DIAG_BME_RESET_WAIT_MS 2  // diagnóstico: espera após o soft reset do BME280 (partida do chip: 2 ms)
#define DIAG_HMC_PERIOD_MS  70    // diagnóstico: intervalo entre saídas do HMC5883L (15 Hz = 66,7 ms)
#define DIAG_HMC_SETTLE_MS  140   // diagnóstico: duas saídas após mudar a configuração (a 1ª usa a antiga)
#define DIAG_HMC_ST_SAMPLES 4     // diagnóstico: saídas na média de cada polarização do autoteste
#define DIAG_HMC_DELAY_S    10    // diagnóstico: contagem regressiva do h antes da medida (tempo de afastar a mão)
#define DIAG_HMC_LINES      10    // diagnóstico: linhas por medida do h (a DIAG_PRINT_HZ), seguidas da média
#define DIAG_HMC_ST_MIN_LSB 243     // autoteste: módulo aceito em cada polarização e eixo (datasheet).
#define DIAG_HMC_ST_MAX_LSB 575     // Exceção à regra de limiares em unidades físicas: o datasheet define a faixa
                                    // em LSB no ganho de 390 LSB/Ga, que o próprio autoteste fixa
                                    // (±62,3 a ±147,4 µT; nominal ~116 µT em X e Y, ~108 µT em Z)

// ---------- MPU6050 (firmware de voo) ----------
#define MPU_ACCEL_RANGE_G       4      // faixa do acelerômetro: ±4 g
#define MPU_GYRO_RANGE_DPS      500    // faixa do giroscópio: ±500 °/s
#define MPU_DLPF_HZ             42     // banda do filtro interno (giroscópio 42 Hz, acelerômetro 44 Hz)
#define MPU_MAX_FALHAS_SEGUIDAS 5      // leituras falhas seguidas antes de reiniciar o barramento
#define MPU_INT_QUEUE_LEN       16     // pulsos de "data ready" na fila (80 ms a 200 Hz)
#define MPU_INT_READ_DELAY_US   100    // espera após o pulso do INT (50 µs) para a borda não cair na leitura
#define MPU_INT_TIMEOUT_MS      50     // sem pulsos do INT por esse tempo (10 amostras): conferir o MPU
#define MPU_INT_WAIT_SLICE_MS   10     // espera do pulso em fatias: com o INT parado, o barômetro é
                                       // atendido entre elas (abaixo do período do BME280, 40 ms)
#define MPU_RETRY_MS            2000   // MPU6050 ausente: nova tentativa de configuração
#define MAIN_PRINT_MS           1000   // main provisório: intervalo do resumo na serial
#define SERIAL_TX_BUFFER_BYTES  4096   // buffer de transmissão da serial: as impressões não bloqueiam o laço
                                       // (resumo + fluxo a 10 Hz ≈ 4 kB/s, abaixo dos ~11,5 kB/s de 115200).
                                       // O c imprime ~1,4 kB de uma vez: com 1 kB, o laço bloqueava ~30 ms,
                                       // perto do reinício da fusão (FUSION_MAX_DT_MS)
#define LOG_BUFFER_BYTES        8192   // buffer do lib/logger, antes do Serial: quem imprime não espera a UART;
                                       // cheio, a mensagem é descartada e contada. Com o do Serial, ~12 kB
                                       // (~1 s a 115200): cabe o c (~1,4 kB) junto com o r e o a

// ---------- Calibração do MPU6050 (docs/calibracao.md) ----------
#define CAL_WINDOW_MS                 2000    // janela de média: 400 amostras a 200 Hz
#define CAL_MIN_SAMPLES_PCT           90      // janela aceita com pelo menos este % das amostras
#define CAL_REST_GYRO_MAX_STD_DPS     0.15f   // repouso: desvio de cada eixo do giroscópio abaixo disto
                                              // (filtro de movimento; tolera vibração da mesa, ~0,09 °/s)
#define CAL_BIAS_MAX_DELTA_DPS        0.03f   // bias aceito só com duas janelas seguidas com médias
                                              // diferindo menos que isto em cada eixo (~3σ da diferença)
#define CAL_BIAS_MAX_TILT_DEG         0.1f    // e com a direção da gravidade mudando menos que isto entre elas:
                                              // rotação constante em eixo não vertical acima de ~0,05 °/s
                                              // (0,1° em 2 s) é recusada. Em repouso: 0,003–0,027°
                                              // em 8 confirmações (hardware.md, 13/10/2026)
#define CAL_REST_ACC_MIN_G            0.95f   // repouso: |a| corrigido dentro desta faixa
#define CAL_REST_ACC_MAX_G            1.05f
#define CAL_REST_ACC_RAW_MIN_G        0.7f    // idem, antes das seis posições: |a| bruto ≈ 1,10 g na bancada;
#define CAL_REST_ACC_RAW_MAX_G        1.5f    // faixa larga, mas recusa vetor nulo e leitura absurda
#define CAL_POSE_MIN_G                0.8f    // seis posições: eixo esperado dentro desta faixa
#define CAL_POSE_MAX_G                1.2f
#define CAL_VALID_MIN_G               0.98f   // validação: |a| corrigido em cada posição
#define CAL_VALID_MAX_G               1.02f
#define CAL_TEMP_WARN_C               1.0f    // aviso: chip a mais disto da temperatura da calibração
                                              // (bias de gx varia ~−0,1 °/s por °C, hardware.md)
#define CAL_STORE_ACC_OFFSET_MAX_G    0.5f    // NVS: offset do acelerômetro acima disto é registro corrompido
                                              // (datasheet: zero-g de ±50 mg em X/Y e ±80 mg em Z)
#define CAL_STORE_GYRO_BIAS_MAX_DPS   20.0f   // NVS: bias acima disto é registro corrompido (datasheet: ±20 °/s)
#define CAL_MPU_SETTLE_MS             200     // descarte após configurar o MPU (boot e recuperações):
                                              // transitório de partida do giroscópio (pico ~60 °/s)

// ---------- Fusão: filtro complementar de roll e pitch (docs/fusao.md) ----------
#define COMP_FILTER_TAU_S       1.0f    // constante de tempo; α = τ/(τ + dt) com o dt medido a cada amostra
                                        // (α fixo de 0,98 a 200 Hz equivaleria a τ ≈ 0,25 s)
#define FUSION_ACC_FULL_DEV_G   0.05f   // ||a| − 1 g| até isto: peso pleno do acelerômetro
#define FUSION_ACC_ZERO_DEV_G   0.15f   // a partir disto: peso zero (linear entre os dois)
#define FUSION_ROLL_MIN_G       0.3f    // projeção √(ay² + az²) mínima para corrigir o roll (≈ |pitch| 72,5°)
#define FUSION_MAX_DT_MS        50      // intervalo maior que isto: filtro reiniciado (= MPU_INT_TIMEOUT_MS)
#define FUSION_INIT_SAMPLES     20      // atitude inicial = média de 20 amostras seguidas (0,1 s): ruído ~0,02°
#define FUSION_INIT_MAX_RATE_DPS 5.0f   // inicialização só com |giroscópio| abaixo disto (caixa sem girar)
#define FUSION_MAX_INT_LAG_US   4000    // rede de segurança: leitura mais que isto depois do pulso fica fora da
                                        // fusão (abaixo do período de 5 ms); o critério principal é um pulso
                                        // mais novo chegar antes do fim da leitura
#define ATT_PRINT_HZ            10      // main provisório: taxa do fluxo de atitude (comando a)

// ---------- Calibração de montagem (comando z, docs/fusao.md) ----------
#define LEVEL_REST_GYRO_MAX_MEAN_DPS 0.2f    // repouso: média do giroscópio corrigido abaixo disto em cada eixo
#define LEVEL_MAX_TILT_DEG           10.0f   // desalinhamento do módulo e inclinação da mesa aceitos

// ---------- BME280: temperatura, pressão e altitude relativa (docs/barometro.md) ----------
// Perfil "indoor navigation" do datasheet (tabela 9), sem a umidade: ruído RMS de 0,2 Pa (1,7 cm),
// resposta a 75 % de um degrau em ~0,9 s (22 amostras do filtro IIR 16 a ~25 Hz)
#define BARO_OSRS_P              16       // oversampling da pressão
#define BARO_OSRS_T              2        // oversampling da temperatura
#define BARO_IIR_COEF            16       // filtro IIR do chip
#define BARO_STANDBY_US          500      // espera entre medições: medição de ~37,5 ms + 0,5 ms ≈ 26 Hz
#define BARO_READ_HZ             25       // leitura pelo firmware, logo depois de uma leitura do MPU
#define BARO_MAX_FAIL_STREAK     5        // falhas seguidas até ST_BARO_OK cair
#define BARO_RETRY_MS            5000     // BME280 ausente no boot: nova tentativa
#define BARO_FAIL_RETRY_MS       1000     // BME280 falhando (ST_BARO_OK caído): uma tentativa por intervalo,
                                          // para as falhas não atrasarem as leituras do MPU
#define BARO_SETTLE_MS           2000     // após configurar: espera antes da janela de zero
#define BARO_ZERO_WINDOW_MS      2000     // janela de zero: média de ~50 leituras
#define BARO_ZERO_MIN_SAMPLES_PCT 90      // janela aceita com pelo menos este % das leituras
#define BARO_ZERO_MAX_STD_PA     1.5f     // desvio máximo da pressão na janela (≈ 12 cm): placa parada
#define BARO_REF_MIN_HPA         300.0f   // p0 aceito: faixa de operação do BME280/BMP280 (300..1100 hPa)
#define BARO_REF_MAX_HPA         1100.0f
#define BARO_TEMP_WARN_C         1.0f     // aviso: chip a mais disto da temperatura do zero
                                          // (TCOP do datasheet: ±1,5 Pa/K ≈ ±13 cm/K)
#define BARO_STALE_MS            200      // telemetria: última leitura mais velha que isto, altitude inválida (5 leituras)
#define BARO_PRINT_HZ            10       // main provisório: taxa do fluxo de altitude (comando b)

// ---------- HMC5883L: proa magnética (docs/magnetometro.md) ----------
#define MAG_RANGE_MGA            1300     // ±1,3 Ga (1090 LSB/Ga): ~130 µT por eixo, margem para o hard iron
#define MAG_OUTPUT_HZ            30.0f    // taxa de saída do chip
#define MAG_AVG_SAMPLES          8        // média no chip (reduz o ruído sem custo de CPU nem I2C)
#define MAG_READ_HZ              25       // leitura pelo firmware, logo depois de uma leitura do MPU
#define MAG_MAX_FAIL_STREAK      5        // falhas seguidas até ST_MAG_OK cair
#define MAG_RETRY_MS             5000     // HMC5883L ausente no boot: nova tentativa
#define MAG_FAIL_RETRY_MS        1000     // HMC5883L falhando: uma tentativa por intervalo (não atrasa o MPU)
#define MAG_CAL_MS               180000   // duração máxima da coleta da calibração (m): 6 giros de 360° feitos com
                                          // calma (90 e 120 s ficaram curtos na bancada, 14/10/2026); 4 500 pontos,
                                          // ~54 kB alocados só durante a calibração
#define MAG_CAL_PROGRESS_MS      2000     // intervalo das linhas de progresso da coleta
#define MAG_KEY_HINT_MS          1000     // tecla ignorada na coleta ou na revisão: no máximo um aviso por intervalo
#define MAG_CAL_MIN_SECTORS      20       // setores cobertos (de 24) para aceitar a coleta
#define MAG_CAL_MIN_PTS_SECTOR   5        // pontos para um setor contar
#define MAG_CAL_COVER_MIN_UT     10.0f    // progresso da coleta: ponto conta para a cobertura só a pelo menos isto
                                          // do centro provisório (~40 % do raio de 25,7 µT; ruído ~0,5 µT). Com a
                                          // caixa parada, a cobertura fica em 0 e a coleta não termina pelo ruído
#define MAG_CAL_MIN_RADIUS_UT    10.0f    // raio ajustado aceito (faixa larga: modelo final)
#define MAG_CAL_MAX_RADIUS_UT    60.0f
#define MAG_CAL_MAX_RESID_PCT    3.0f     // resíduo RMS de |m| corrigido, % do raio
#define MAG_CAL_MIN_SCALE        0.7f     // semieixos do elipsoide normalizados para média 1
#define MAG_CAL_MAX_SCALE        1.3f
#define MAG_REF_FIELD_UT         26.2f    // |B| do WMM2025 na bancada (João Pessoa): só informativo
#define MAG_DISTURB_PCT          15.0f    // |m| corrigido afastado do raio mais que isto: correção suspensa
#define MAG_HEADING_TAU_S        1.0f     // filtro da proa: constante de tempo da correção pelo magnetômetro
#define MAG_MAX_PITCH_DEG        70.0f    // acima disto, sem correção pelo magnetômetro (faixa validada da fusão)
#define MAG_MAX_DT_MS            200      // correção: maior passo considerado no ganho
#define MAG_STALE_MS             200      // telemetria: última leitura mais velha que isto, proa só pelo giroscópio
#define MAG_PRINT_HZ             10       // main provisório: taxa do fluxo de proa (comando p)

// ---------- Rádio ----------
#define ESPNOW_CHANNEL   1        // deve ser IGUAL ao canal do AP no ESP B
// MAC da Placa B (estação de solo)
static const uint8_t PEER_MAC[6] = {0x14, 0x33, 0x5C, 0x03, 0x8A, 0xB4};
