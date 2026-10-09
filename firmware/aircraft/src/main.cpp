// ESP A — avião. main.cpp só inicializa hardware e cria as tasks.
// Tasks previstas (src/tasks/):
//   sensor_task  (prio 4, core 1) — acordada pela INT do MPU, lê IMU a SAMPLE_RATE_HZ
//   fusion_task  (prio 3, core 1) — filtro complementar → roll/pitch/heading
//   radio_task   (prio 2, core 0) — monta TelemetryPacket e envia via ESP-NOW
//   status_task  (prio 1, core 0) — OLED de status + LED
#include <Arduino.h>
#include "config.h"
#include "telemetry.h"

void setup() {
    Serial.begin(115200);
    pinMode(PIN_LED_STATUS, OUTPUT);
    Serial.printf("[A] boot — TelemetryPacket = %u bytes\n", (unsigned)sizeof(TelemetryPacket));
}

void loop() {
    digitalWrite(PIN_LED_STATUS, !digitalRead(PIN_LED_STATUS));
    delay(500);
}
