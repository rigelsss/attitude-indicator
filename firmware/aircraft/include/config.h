// config.h — ESP A (avião): pinagem e parâmetros
#pragma once

// ---------- I2C (barramento único) ----------
#define PIN_I2C_SDA      21
#define PIN_I2C_SCL      22
#define I2C_FREQ_HZ      400000

// Endereços esperados (confirmar com I2C scan)
#define ADDR_MPU6050     0x68
#define ADDR_BME280      0x76     // 0x77 se SDO estiver em VCC
#define ADDR_QMC5883L    0x0D     // se aparecer 0x1E, o GY-273 é HMC5883L
#define ADDR_OLED_STATUS 0x3C

// ---------- GPIO ----------
#define PIN_MPU_INT      19       // interrupção "data ready" do MPU6050
#define PIN_LED_STATUS   2        // LED onboard: pisca = transmitindo

// ---------- Temporização ----------
#define SAMPLE_RATE_HZ   200      // leitura do IMU / fusão
#define TX_RATE_HZ       50       // envio ESP-NOW
#define OLED_RATE_HZ     5

// ---------- Fusão ----------
#define COMP_FILTER_ALPHA 0.98f   // peso do giroscópio no filtro complementar

// ---------- Rádio ----------
#define ESPNOW_CHANNEL   1        // deve ser IGUAL ao canal do AP no ESP B
// MAC da Placa B (estação de solo)
static const uint8_t PEER_MAC[6] = {0x14, 0x33, 0x5C, 0x03, 0x8A, 0xB4};
