// ESP A — avião. main.cpp só inicializa e cria as tasks (src/tasks/); a lógica está em lib/ e nas tasks:
//   serial_task  (prio 1, núcleo 0) — inicia o Serial, formata o c, o r e o a a partir das cópias da
//                sensor_task, passa as mensagens do lib/logger para a UART e entrega as teclas à sensor_task
//   sensor_task  (prio 4, núcleo 1) — acordada pela INT do MPU a SAMPLE_RATE_HZ; dona do barramento: lê o
//                MPU e, intercalados, o BME280 e o HMC5883L; calibração e fusão (roll/pitch/proa); executa
//                os comandos (console)
//   próximos passos da semana 3: radio_task (ESP-NOW) e status_task (OLED de status no Wire1 + LED)
// Serial: c = estado | 6 = seis posições | g = calibrar giroscópio | z = calibrar montagem |
//         m = calibrar magnetômetro | a = fluxo de atitude | b = fluxo de altitude | p = fluxo de proa |
//         0 = zerar altitude | r = resumo por segundo.
// Docs: docs/plano.md (semana 3) e docs/refatoracao.md (pendências 2, 7 e 8).
#include <Arduino.h>
#include "config.h"
#include "telemetry.h"
#include "logger.h"
#include "tasks/serial_task.h"
#include "tasks/sensor_task.h"
#include "report_queue.h"

extern const char *const BUILD_STAMP;   // build_info.cpp: data e hora da compilação

static void fatal(const char *msg) {
    logPrintln(msg);
    logFlush();
    delay(1000);
    ESP.restart();
}

void setup() {
    // Antes de qualquer mensagem: até a serial_task iniciar o Serial, elas esperam no buffer
    const bool logBuffered = logBegin(LOG_BUFFER_BYTES, LOG_FLUSH_TIMEOUT_MS);   // e redireciona o log do núcleo
    // Sem a serial não há como avisar. A espera vale também para o modo degradado do logger (sem buffer), em
    // que uma mensagem anterior ao Serial pronto seria perdida
    if (!serialTaskStart() || !logWaitSerial(SERIAL_READY_TIMEOUT_MS)) ESP.restart();
    logPrintf("[A] boot — compilado em %s — TelemetryPacket = %u bytes (tasks: serial e sensor)\n", BUILD_STAMP,
              (unsigned)sizeof(TelemetryPacket));
    if (!logBuffered)
        logPrintln("[A] AVISO: sem memória para o buffer do logger; impressões direto no Serial (bloqueiam o laço)");
    if (!reportQueueInit()) fatal("[A] ERRO: sem memória para a fila de relatórios do console; reiniciando em 1 s");
    if (!sensorTaskStart()) fatal("[A] ERRO: sem memória para a sensor_task; reiniciando em 1 s");
}

// A loopTask do Arduino não tem trabalho: tudo roda nas tasks criadas no setup()
void loop() { vTaskDelete(nullptr); }
