// ESP B — estação de solo. main.cpp só inicializa hardware e cria as tasks.
// Tasks previstas (src/tasks/):
//   radio_rx     — callback ESP-NOW → fila (nunca processar dentro do callback)
//   link_task    (prio 3) — valida seq, conta perdas, detecta timeout de link
//   display_task (prio 2, core 1) — horizonte no TFT (sprite) + OLED secundário
//   input_task   (prio 2) — eventos do encoder (ISR → fila)
//   alarm_task   (prio 2) — buzzer + LEDs
//   web_task     (prio 1, core 0) — AP, HTTP (LittleFS) e WebSocket JSON
#include <Arduino.h>
#include "config.h"
#include "telemetry.h"

void setup() {
    Serial.begin(115200);
    pinMode(PIN_LED_LINK, OUTPUT);
    Serial.printf("[B] boot — TelemetryPacket = %u bytes\n", (unsigned)sizeof(TelemetryPacket));
}

void loop() {
    digitalWrite(PIN_LED_LINK, !digitalRead(PIN_LED_LINK));
    delay(500);
}
