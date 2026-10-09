// config.h — ESP B (estação de solo): pinagem e parâmetros
#pragma once

// ---------- SPI (TFT ST7789) ----------
// Pinos definidos em platformio.ini (build_flags do TFT_eSPI):
// MOSI 23, SCLK 18, CS 5, DC 16, RST 17, BL 4

// ---------- I2C (OLED 0,96") ----------
#define PIN_I2C_SDA      21
#define PIN_I2C_SCL      22
#define ADDR_OLED_DATA   0x3C

// ---------- Encoder KY-040 (interrupção) ----------
#define PIN_ENC_CLK      32
#define PIN_ENC_DT       33
#define PIN_ENC_SW       25       // botão: confirmar / calibrar (pressão longa)

// ---------- Alarmes ----------
#define PIN_BUZZER       26       // buzzer ativo — usar transistor se corrente > 20 mA
#define PIN_LED_LINK     27       // verde: link ESP-NOW ok
#define PIN_LED_ALARM    13       // vermelho: inclinação excessiva / perda de link

// ---------- Limites de alarme ----------
#define ALARM_BANK_DEG   45.0f
#define ALARM_PITCH_DEG  25.0f
#define LINK_TIMEOUT_MS  500

// ---------- Rede ----------
#define AP_SSID          "Horizonte-ESP"
#define AP_PASS          "horizonte123"   // mín. 8 caracteres
#define AP_CHANNEL       1                // IGUAL ao ESPNOW_CHANNEL do ESP A
#define HTTP_PORT        80
#define WS_PORT          81
#define WS_RATE_HZ       30

// MAC da Placa A (avião)
static const uint8_t PEER_MAC[6] = {0x1C, 0x69, 0x20, 0xA4, 0xE9, 0x44};
