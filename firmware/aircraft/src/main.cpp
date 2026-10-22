// ESP A — avião. main.cpp só inicializa e cria as tasks (src/tasks/); a lógica está em lib/ e nas tasks:
//   serial_task  (prio 1, núcleo 0) — inicia o Serial, formata o c, o r e o a a partir das cópias da
//                sensor_task, passa as mensagens do lib/logger para a UART e entrega as teclas à sensor_task
//   sensor_task  (prio 4, núcleo 1) — acordada pela INT do MPU a SAMPLE_RATE_HZ; dona do barramento: lê o
//                MPU e, intercalados, o BME280 e o HMC5883L; calibração e fusão (roll/pitch/proa); executa
//                os comandos (console)
//   radio_task   (prio 2, núcleo 0) — a TX_RATE_HZ, monta o TelemetryPacket a partir do estado publicado pela
//                sensor_task e o envia pelo ESP-NOW ao ESP B (docs/radio.md)
//   status_task  (prio 1, núcleo 0) — a OLED_RATE_HZ, OLED de status no Wire1 (sensores, link, tempo ligado e
//                avisos) e LED, a partir das mesmas cópias (docs/tasks.md, passo 6)
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
#include "tasks/radio_task.h"
#include "tasks/status_task.h"
#include "report_queue.h"

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
    logPrintf("[A] boot — compilado em %s — TelemetryPacket = %u bytes (tasks: serial, sensor, rádio e status)\n", BUILD_STAMP,
              (unsigned)sizeof(TelemetryPacket));
    logPrintf("[A] reinício anterior: %s\n", resetReasonText(esp_reset_reason()));
    if (!logBuffered)
        logPrintln("[A] AVISO: sem memória para o buffer do logger; impressões direto no Serial (bloqueiam o laço)");
    if (!reportQueueInit()) fatal("[A] ERRO: sem memória para a fila de relatórios do console; reiniciando em 1 s");
    if (!sensorTaskStart()) fatal("[A] ERRO: sem memória para a sensor_task; reiniciando em 1 s");
    // Sem o rádio, a aquisição e a serial seguem: o r e o c mostram "radio: desligado".
    // RADIO_ENABLED 0 (ambiente aircraft_noradio, A/B de tempo): o código do rádio continua no binário (a flag é
    // lida em tempo de execução), para a posição do código na flash não mudar entre os dois firmwares
    static volatile bool radioEnabled = RADIO_ENABLED;
    if (!radioEnabled) logPrintln("[A] rádio desligado neste firmware (RADIO_ENABLED 0, diagnóstico)");
    else if (!radioTaskStart()) logPrintln("[A] ERRO: sem memória para a radio_task; seguindo sem rádio");
    // Sem o OLED e o LED, o resto segue: o c mostra "oled: task não iniciada"
    if (!statusTaskStart()) logPrintln("[A] ERRO: sem memória para a status_task; seguindo sem o OLED e o LED");
}

// A loopTask do Arduino não tem trabalho: tudo roda nas tasks criadas no setup()
void loop() { vTaskDelete(nullptr); }
