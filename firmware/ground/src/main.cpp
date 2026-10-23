// ESP B — estação de solo. main.cpp só inicializa e cria as tasks (src/tasks/); a lógica está em lib/:
//   radio_rx     — Wi-Fi em AP+STA no canal 1 e ESP-NOW; o callback de recepção só confere, copia e enfileira
//   link_task    (prio 3, núcleo 1) — consome a fila, conta perdas pelo seq, detecta o timeout do link e
//                publica o último pacote, o resumo de 1 s e os eventos do link (docs/radio.md, ESP B)
//   serial_task  (prio 1, núcleo 0) — única dona do Serial depois do boot: teclas e impressão
// Serial: c = estado | r = resumo por segundo (começa ligado) | a = fluxo de pacotes (10 Hz).
// Previstas (semanas 4 e 5): display_task (TFT + OLED), input_task (encoder), alarm_task (buzzer + LEDs) e
// web_task (HTTP + WebSocket), lendo as caixas de correio da link_task.
#include <Arduino.h>
#include "config.h"
#include "telemetry.h"
#include "tasks/link_task.h"
#include "tasks/radio_rx.h"
#include "tasks/serial_task.h"

extern const char *const BUILD_STAMP;   // build_info.cpp: data e hora da compilação

// Motivo do reinício anterior, na linha de boot: um laço de brownouts (queda de tensão) aparece aqui
static const char *resetReasonText(esp_reset_reason_t r) {
    switch (r) {
    case ESP_RST_POWERON:   return "energia ligada";
    case ESP_RST_EXT:       return "pino de reset";
    case ESP_RST_SW:        return "software (ESP.restart)";
    case ESP_RST_PANIC:     return "pânico";
    case ESP_RST_INT_WDT:   return "watchdog de interrupção";
    case ESP_RST_TASK_WDT:  return "watchdog de task";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (queda de tensão)";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "desconhecido";
    }
}

static void fatal(const char *msg) {
    Serial.println(msg);
    Serial.flush();
    delay(1000);
    ESP.restart();
}

void setup() {
    // Buffer de transmissão em software (antes do begin): sem ele, cada escrita espera a FIFO de 128 bytes
    Serial.setTxBufferSize(SERIAL_TX_BUFFER_BYTES);
    Serial.begin(115200);
    delay(SERIAL_START_DELAY_MS);   // tempo para o monitor reabrir depois do reset
    // Até a serial_task começar, só o setup() escreve no Serial
    Serial.printf("[B] boot — compilado em %s — TelemetryPacket = %u bytes, contrato v%d (tasks: link e serial)\n",
                  BUILD_STAMP, (unsigned)sizeof(TelemetryPacket), TELEMETRY_VERSION);
    Serial.printf("[B] reinício anterior: %s\n", resetReasonText(esp_reset_reason()));
    // Sem o rádio, a serial segue: o c mostra "rádio: DESLIGADO" e o r, "sem sinal"
    if (!radioRxStart()) Serial.println("[B] seguindo sem rádio");
    if (!linkTaskStart()) fatal("[B] ERRO: sem memória para a link_task; reiniciando em 1 s");
    if (!serialTaskStart()) fatal("[B] ERRO: sem memória para a serial_task; reiniciando em 1 s");
}

// A loopTask do Arduino não tem trabalho: tudo roda nas tasks criadas no setup()
void loop() { vTaskDelete(nullptr); }
