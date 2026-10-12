// config.h — ESP A (avião): pinagem e parâmetros
#pragma once

// ---------- I2C (barramento único) ----------
#define PIN_I2C_SDA      21
#define PIN_I2C_SCL      22
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
#define MAIN_PRINT_MS           1000   // main provisório: intervalo do resumo na serial
#define SERIAL_TX_BUFFER_BYTES  1024   // buffer de transmissão da serial: as impressões não bloqueiam o laço
                                       // (resumo + fluxo a 10 Hz ≈ 4 kB/s, abaixo dos ~11,5 kB/s de 115200)

// ---------- Calibração do MPU6050 (docs/calibracao.md) ----------
#define CAL_WINDOW_MS                 2000    // janela de média: 400 amostras a 200 Hz
#define CAL_MIN_SAMPLES_PCT           90      // janela aceita com pelo menos este % das amostras
#define CAL_REST_GYRO_MAX_STD_DPS     0.15f   // repouso: desvio de cada eixo do giroscópio abaixo disto
                                              // (filtro de movimento; tolera vibração da mesa, ~0,09 °/s)
#define CAL_BIAS_MAX_DELTA_DPS        0.03f   // bias aceito só com duas janelas seguidas com médias
                                              // diferindo menos que isto em cada eixo (~3σ da diferença)
#define CAL_REST_ACC_MIN_G            0.95f   // repouso: |a| corrigido dentro desta faixa
#define CAL_REST_ACC_MAX_G            1.05f
#define CAL_POSE_MIN_G                0.8f    // seis posições: eixo esperado dentro desta faixa
#define CAL_POSE_MAX_G                1.2f
#define CAL_VALID_MIN_G               0.98f   // validação: |a| corrigido em cada posição
#define CAL_VALID_MAX_G               1.02f
#define CAL_TEMP_WARN_C               1.0f    // aviso: chip a mais disto da temperatura da calibração
                                              // (bias de gx varia ~−0,1 °/s por °C, hardware.md)
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

// ---------- Rádio ----------
#define ESPNOW_CHANNEL   1        // deve ser IGUAL ao canal do AP no ESP B
// MAC da Placa B (estação de solo)
static const uint8_t PEER_MAC[6] = {0x14, 0x33, 0x5C, 0x03, 0x8A, 0xB4};
