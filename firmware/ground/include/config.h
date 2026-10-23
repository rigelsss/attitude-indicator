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

// ---------- Alarmes (docs/solo.md) ----------
#define PIN_BUZZER       26       // buzzer ativo, ADIADO (sem transistor; usar um se a corrente passar de ~12 mA)
#define PIN_LED_LINK     2        // LED azul da placa: link ESP-NOW (o verde externo no 27 saiu, 23/10/2026)
#define PIN_LED_ALARM    13       // vermelho externo, 330 Ω: inclinação excessiva / perda de link

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
#define ESPNOW_CHANNEL   AP_CHANNEL       // o ESP-NOW usa o canal do AP (WIFI_AP_STA; docs/radio.md)

// ---------- Recepção do ESP-NOW e link (docs/radio.md, ESP B) ----------
#define RX_QUEUE_LEN     16       // pacotes entre o callback do ESP-NOW e a link_task (~320 ms a 50 Hz)
#define LINK_RESYNC_GAP  500      // salto do seq acima disto (10 s a 50 Hz): ressincroniza sem contar perdas
#define LINK_LATE_MAX_PKTS 100    // seq até isto atrás (2 s a 50 Hz) e t_ms até LINK_LATE_MAX_MS atrás:
#define LINK_LATE_MAX_MS   2000   // pacote atrasado, descartado (e não reinício do A). Rajadas de ~1 s na bancada
#define LINK_REPORT_MS   1000     // janela do resumo (r)
#define LINK_EVENT_QUEUE_LEN 8    // eventos do link (perdido, restabelecido, ressincronizado) para a serial

// ---------- Tasks ----------
#define LINK_TASK_CORE          1
#define LINK_TASK_PRIO          3
#define LINK_TASK_STACK         4096
#define LINK_TASK_WAIT_MS       10     // espera máxima por um pacote: o timeout é conferido a cada volta
#define SERIAL_TASK_CORE        0
#define SERIAL_TASK_PRIO        1
#define SERIAL_TASK_STACK       4096
#define SERIAL_TASK_PERIOD_MS   10
#define SERIAL_TX_BUFFER_BYTES  4096   // buffer de transmissão da serial: as impressões não esperam a UART
#define SERIAL_START_DELAY_MS   500    // espera depois do Serial.begin() para o monitor reabrir após o reset
#define STREAM_PKT_PERIOD_MS    100    // fluxo a: uma linha a cada 100 ms (10 Hz)

// ---------- Proa ----------
// Declinação magnética na bancada (WMM2025, João Pessoa, 13/10/2026; docs/magnetometro.md), negativa
// para oeste. O pacote traz a proa MAGNÉTICA; só a exibição (TFT e página, via "decl" no JSON) mostra a
// verdadeira: proa verdadeira = magnética + declinação, com o rótulo MAG ou VERD
#define MAG_DECLINATION_DEG  -20.7f

// ---------- Diagnóstico do hardware (ambiente ground_diag, src/diag/) ----------
#define DIAG_I2C_FREQ_HZ      100000   // scan do I2C
#define DIAG_OLED_FREQ_HZ     400000   // OLED no teste o (a frequência prevista para o firmware)
#define DIAG_ENC_PRINT_MS     100      // fluxo do encoder: no máximo uma linha a cada 100 ms, só quando muda
#define DIAG_BTN_DEBOUNCE_MS  20       // botão do encoder: estado estável por este tempo
#define DIAG_BTN_LONG_MS      600      // pressão longa a partir deste tempo
#define DIAG_TFT_COLOR_MS     700      // teste t: tempo de cada cor cheia
#define DIAG_SPRITE_FRAMES    20       // teste s: quadros enviados para a média do tempo

// MAC da Placa A (avião)
static const uint8_t PEER_MAC[6] = {0x1C, 0x69, 0x20, 0xA4, 0xE9, 0x44};
