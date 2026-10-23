// config.h — ESP B (estação de solo): pinagem e parâmetros
#pragma once
#include <stdint.h>   // uint8_t do PEER_MAC: o config.h não depende de quem o inclui antes

// ---------- SPI (TFT ST7789) ----------
// Pinos definidos em platformio.ini (build_flags do TFT_eSPI):
// MOSI 23, SCLK 18, CS 5, DC 16, RST 17 (o BLK do módulo vai no 3V3)

// ---------- Horizonte no TFT (docs/solo.md, passo 5) ----------
#define DISPLAY_FPS        25     // quadros por segundo: maior quadro de 36–39 ms no período de 40 ms (bancada de 23/10/2026)
#define PFD_PX_PER_DEG     4.0f   // escala do pitch: ±30° visíveis
#define PFD_LADDER_SPAN_DEG 25    // escala de pitch desenhada até ±isto em torno do pitch atual
#define PFD_ROLL_RADIUS_PX 100    // raio da escala de roll (centro no avião)

// ---------- I2C (OLED 0,96") ----------
#define PIN_I2C_SDA      21
#define PIN_I2C_SCL      22
#define ADDR_OLED_DATA   0x3C
#define OLED_I2C_FREQ_HZ 400000   // quadro de 128x64 em 32,7 ms (hardware.md, 23/10/2026)
#define OLED_RATE_HZ     5        // redesenho; a troca de página redesenha na hora
#define OLED_RETRY_MS    5000     // OLED ausente: nova tentativa de iniciar
#define OLED_BOOT_SCREEN_MS 1000  // tela de boot
#define OLED_HEADING_TRUE 0       // 0: proa magnética (MAG), como nos instrumentos de cabine; 1: verdadeira (VERD)

// ---------- Encoder KY-040 (interrupção) ----------
#define PIN_ENC_CLK      32
#define PIN_ENC_DT       33
#define PIN_ENC_SW       25       // botão: curto = reconhecer o alarme; longo = reservado (comandos, semana 5)
#define ENC_STEPS_PER_CLICK 2     // o KY-040 da bancada: 2 passos por clique, repouso em 11 e 00 (hardware.md)
#define ENC_DIR          1        // +1: horário = próxima página; −1 inverte
#define ENC_QUEUE_LEN    64       // estados lidos pela ISR até a input_task decodificar. Com 32, o repique de um
                                  // giro rápido encheu a fila uma vez (hardware.md, 23/10/2026)
#define BTN_DEBOUNCE_MS  20       // botão: estado estável por este tempo
#define BTN_LONG_MS      1000     // pressão longa (comandos da semana 5: não disparar sem querer)
#define OLED_PAGES       2        // páginas do OLED trocadas pelo giro: dados; link e saúde do A

// ---------- Alarmes (docs/solo.md) ----------
#define PIN_BUZZER       26       // buzzer ativo, ADIADO (sem transistor; usar um se a corrente passar de ~12 mA)
#define PIN_LED_LINK     2        // LED azul da placa: link ESP-NOW (o verde externo no 27 saiu, 23/10/2026)
#define PIN_LED_ALARM    13       // vermelho externo, 330 Ω: inclinação excessiva / perda de link

// ---------- Limites de alarme (docs/solo.md, passo 1) ----------
// Alarme (warning) = atitude anormal ("upset") pela definição do Airplane Upset Recovery Training Aid, também
// usada pela FAA (AC 120-111): bank > 45°, pitch > +25° ou < −10°. Aviso (caution) = "BANK ANGLE" do EGPWS,
// que em aeronaves como o 737 começa em 35°. Cada limite sai com ALARM_HYST_DEG a menos
#define ALARM_BANK_CAUTION_DEG 35.0f
#define ALARM_BANK_DEG         45.0f
#define ALARM_PITCH_UP_DEG     25.0f
#define ALARM_PITCH_DOWN_DEG   10.0f    // nariz para baixo (pitch < −10°)
#define ALARM_HYST_DEG         3.0f
#define ALARM_INVALID_DELAY_MS 1000     // atitude inválida (fora das calibrações) por mais que isto: alarme
#define LINK_TIMEOUT_MS  500

// Padrões do LED de alarme (o buzzer, quando ligado, segue o mesmo ritmo)
#define ALARM_LINK_BLINK_MS     500     // link perdido: 1 Hz
#define ALARM_CAUTION_BLINK_MS  250     // aviso de bank: 2 Hz
#define ALARM_WARNING_BLINK_MS  100     // atitude anormal: 5 Hz
#define ALARM_DOUBLE_PERIOD_MS  2000    // atitude inválida: duas piscadas de ALARM_DOUBLE_ON_MS a cada 2 s
#define ALARM_DOUBLE_ON_MS      100
#define ALARM_BUZZER_ENABLED    0       // buzzer adiado (sem transistor); 1 liga o PIN_BUZZER junto com o LED

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
#define LINK_STALE_DELAY_MS 250   // idade estimada do dado (atraso na chegada + tempo desde a chegada) acima disto:
                                  // DADO ATRASADO (metade do LINK_TIMEOUT_MS)
#define DELAY_BUCKET_MS  1000     // atraso estimado: menor offset em DELAY_BUCKETS faixas de DELAY_BUCKET_MS
#define DELAY_BUCKETS    10       // (10 s: acompanha a deriva entre os cristais; lib/delay_estimator)
#define LINK_EVENT_QUEUE_LEN 8    // eventos do link (perdido, restabelecido, ressincronizado) para a serial

// ---------- Tasks ----------
#define LINK_TASK_CORE          1
#define LINK_TASK_PRIO          3
#define LINK_TASK_STACK         4096
#define LINK_TASK_WAIT_MS       10     // espera máxima por um pacote: o timeout é conferido a cada volta
#define ALARM_TASK_CORE         0
#define ALARM_TASK_PRIO         2
#define ALARM_TASK_STACK        3072
#define ALARM_TASK_PERIOD_MS    20     // volta da alarm_task (LEDs e condições)
#define ALARM_EVENT_QUEUE_LEN   8      // mudanças de alarme para a serial
#define INPUT_TASK_CORE         0
#define INPUT_TASK_PRIO         2
#define INPUT_TASK_STACK        3072
#define INPUT_TASK_WAIT_MS      5      // espera máxima pela fila do encoder: o botão é lido nesse ritmo
#define INPUT_EVENT_QUEUE_LEN   8      // eventos do encoder para a serial
#define DISPLAY_TASK_CORE       1      // fora do núcleo do Wi-Fi; abaixo da link_task (prioridade 3)
#define DISPLAY_TASK_PRIO       2
#define DISPLAY_TASK_STACK      6144   // desenho com fontes e vsnprintf
#define OLED_TASK_CORE          0
#define OLED_TASK_PRIO          1
#define OLED_TASK_STACK         4096
#define OLED_TASK_PERIOD_MS     50     // volta da oled_task: confere a página e o tempo do redesenho
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
