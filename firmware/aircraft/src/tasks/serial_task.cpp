// serial_task.cpp — ver serial_task.h
#include "serial_task.h"
#include <Arduino.h>
#include "config.h"
#include "logger.h"
#include "sensor_task.h"
#include "../console_view.h"

static void serialTask(void *) {
    // Buffer de transmissão em software (antes do begin): sem ele, cada escrita espera a FIFO de 128 bytes
    Serial.setTxBufferSize(SERIAL_TX_BUFFER_BYTES);
    Serial.begin(115200);
    logAttachSerial();   // esta task passa a ser a única que escreve no Serial
    // Tempo para o monitor reabrir depois do reset; as mensagens do boot esperam no buffer do logger
    vTaskDelay(pdMS_TO_TICKS(SERIAL_START_DELAY_MS));
    uint32_t droppedSeen = 0;
    for (;;) {
        // Relatórios do console (c, r, a): formatados aqui, a partir das cópias da sensor_task. O logger é
        // escoado depois de cada um (uma rajada de c não acumula vários blocos de ~1,4 kB antes de escoar), e
        // no máximo SERIAL_REPORTS_PER_CYCLE por volta: as teclas continuam sendo lidas a cada volta
        ReportMsg msg;
        for (int i = 0; i < SERIAL_REPORTS_PER_CYCLE && reportReceive(msg); i++) {
            consoleViewPrint(msg);
            logPump();
        }
        const uint32_t dropped = reportDropped();
        if (dropped != droppedSeen) {
            logPrintf("[A] %lu relatórios do console descartados: fila de relatórios cheia\n",
                      (unsigned long)(dropped - droppedSeen));
            droppedSeen = dropped;
        }
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
