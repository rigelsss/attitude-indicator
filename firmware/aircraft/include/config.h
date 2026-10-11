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

// ---------- MPU6050 (firmware de voo) ----------
#define MPU_ACCEL_RANGE_G       4      // faixa do acelerômetro: ±4 g
#define MPU_GYRO_RANGE_DPS      500    // faixa do giroscópio: ±500 °/s
#define MPU_DLPF_HZ             42     // banda do filtro interno (giroscópio 42 Hz, acelerômetro 44 Hz)
#define MPU_MAX_FALHAS_SEGUIDAS 5      // leituras falhas seguidas antes de reiniciar o barramento
#define MPU_INT_QUEUE_LEN       16     // pulsos de "data ready" na fila (80 ms a 200 Hz)
#define MPU_INT_READ_DELAY_US   100    // espera após o pulso do INT (50 µs) para a borda não cair na leitura
#define MPU_INT_TIMEOUT_MS      50     // sem pulsos do INT por esse tempo (10 amostras): conferir o MPU
#define MAIN_PRINT_MS           1000   // main provisório: intervalo do resumo na serial

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

// ---------- Fusão ----------
#define COMP_FILTER_ALPHA 0.98f   // peso do giroscópio no filtro complementar

// ---------- Rádio ----------
#define ESPNOW_CHANNEL   1        // deve ser IGUAL ao canal do AP no ESP B
// MAC da Placa B (estação de solo)
static const uint8_t PEER_MAC[6] = {0x14, 0x33, 0x5C, 0x03, 0x8A, 0xB4};
