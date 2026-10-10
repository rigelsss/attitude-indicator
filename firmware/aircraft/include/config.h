// config.h — ESP A (avião): pinagem e parâmetros
#pragma once

// ---------- I2C (barramento único) ----------
#define PIN_I2C_SDA      21
#define PIN_I2C_SCL      22
#define I2C_FREQ_HZ      400000
#define I2C_FREQ_DIAG_HZ 100000   // diagnóstico: mais tolerante a fios longos e soldas ruins

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

// ---------- Fusão ----------
#define COMP_FILTER_ALPHA 0.98f   // peso do giroscópio no filtro complementar

// ---------- Rádio ----------
#define ESPNOW_CHANNEL   1        // deve ser IGUAL ao canal do AP no ESP B
// MAC da Placa B (estação de solo)
static const uint8_t PEER_MAC[6] = {0x14, 0x33, 0x5C, 0x03, 0x8A, 0xB4};
