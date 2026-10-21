// serial_task.cpp — ver serial_task.h
#include "serial_task.h"
#include <Arduino.h>
#include "config.h"
#include "logger.h"
#include "sensor_task.h"

static void serialTask(void *) {
    // Buffer de transmissão em software (antes do begin): sem ele, cada escrita espera a FIFO de 128 bytes
    Serial.setTxBufferSize(SERIAL_TX_BUFFER_BYTES);
    Serial.begin(115200);
    logAttachSerial();   // esta task passa a ser a única que escreve no Serial
    // Tempo para o monitor reabrir depois do reset; as mensagens do boot esperam no buffer do logger
    vTaskDelay(pdMS_TO_TICKS(SERIAL_START_DELAY_MS));
    for (;;) {
        logPump();
        while (Serial.available()) {
            const SensorCmd c = {CMD_SRC_CONSOLE, (char)Serial.read()};
            if (!sensorTaskSend(c)) logPrintf("[A] tecla '%c' descartada: fila de comandos cheia\n", c.key);
        }
        vTaskDelay(pdMS_TO_TICKS(SERIAL_TASK_PERIOD_MS));
    }
}

bool serialTaskStart() {
    return xTaskCreatePinnedToCore(serialTask, "serial", SERIAL_TASK_STACK, nullptr, SERIAL_TASK_PRIO, nullptr,
                                   SERIAL_TASK_CORE) == pdPASS;
}
