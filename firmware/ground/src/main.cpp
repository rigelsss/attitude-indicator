// ESP B — estação de solo. main.cpp só inicializa e cria as tasks (src/tasks/); a lógica está em lib/:
//   radio_rx     — Wi-Fi em AP+STA no canal 1 e ESP-NOW; o callback de recepção só confere, copia e enfileira
//   link_task    (prio 3, núcleo 1) — consome a fila, conta perdas pelo seq, detecta o timeout do link e
//                publica o último pacote, o resumo de 1 s e os eventos do link (docs/radio.md, ESP B)
//   alarm_task   (prio 2, núcleo 0) — condições de alarme (link perdido, atitude inválida, atitude anormal,
//                aviso de bank) e LEDs de alarme e do link (docs/solo.md, passo 1)
//   input_task   (prio 2, núcleo 0) — encoder: ISR em IRAM enfileira os estados; cliques trocam a página do OLED,
//                botão curto reconhece o alarme, longo reservado (docs/solo.md, passo 2)
//   oled_task    (prio 1, núcleo 0) — única dona do Wire: OLED com as páginas de dados e de link a 5 Hz
//                (docs/solo.md, passo 3)
//   display_task (prio 2, núcleo 1) — horizonte no TFT com o sprite de 8 bits, a DISPLAY_FPS (docs/solo.md,
//                passo 5); o TFT e o sprite são iniciados no setup(), antes do Wi-Fi
//   command_task (prio 2, núcleo 0) — comandos para o A: envio com reenvio, casamento dos ACKs e eventos
//                (docs/web.md, passo 2)
//   web_task     (prio 1, núcleo 0) — página (HTTP, LittleFS) e WebSocket: telemetria em JSON e comandos com
//                PIN (docs/web.md, passo 3)
//   serial_task  (prio 1, núcleo 0) — única dona do Serial depois do boot: teclas e impressão
// Serial: c = estado | r = resumo por segundo (começa ligado) | a = fluxo de pacotes (10 Hz) | k = reconhecer o
// alarme | g, m, z, 0, f, X = comandos para o A.
#include <Arduino.h>
#include "config.h"
#include "telemetry.h"
#include "tasks/alarm_task.h"
#include "tasks/command_task.h"
#include "tasks/input_task.h"
#include "tasks/link_task.h"
#include "tasks/display_task.h"
#include "tasks/oled_task.h"
#include "tasks/radio_rx.h"
#include "tasks/serial_task.h"
#include "tasks/web_task.h"

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
    Serial.printf("[B] boot — compilado em %s — TelemetryPacket = %u bytes, contrato v%d (tasks: link, comandos, alarme, encoder, OLED, TFT, web e serial)\n",
                  BUILD_STAMP, (unsigned)sizeof(TelemetryPacket), TELEMETRY_VERSION);
    Serial.printf("[B] reinício anterior: %s\n", resetReasonText(esp_reset_reason()));
    // O sprite do TFT (57,6 kB contíguos) é alocado antes do Wi-Fi, com o heap ainda sem fragmentar. Sem ele, o
    // resto segue, e o TFT mostra a mensagem de erro
    const bool tftOk = displayInit();
    // Sem o rádio, a serial segue: o c mostra "rádio: DESLIGADO" e o r, "sem sinal"
    if (!radioRxStart()) Serial.println("[B] seguindo sem rádio");
    if (!linkTaskStart()) fatal("[B] ERRO: sem memória para a link_task; reiniciando em 1 s");
    // Sem os comandos, o resto segue (as teclas e a pressão longa avisam a fila cheia)
    if (!commandTaskStart()) Serial.println("[B] ERRO: sem memória para a command_task; seguindo sem comandos");
    // Sem os alarmes, o resto segue: o c mostra "alarme: task não iniciada"
    if (!alarmTaskStart()) Serial.println("[B] ERRO: sem memória para a alarm_task; seguindo sem alarmes");
    // Sem o encoder, o resto segue: a tecla k ainda reconhece o alarme
    if (!inputTaskStart()) Serial.println("[B] ERRO: encoder não iniciado (memória ou ISR); seguindo sem encoder");
    if (!oledTaskStart()) Serial.println("[B] ERRO: sem memória para a oled_task; seguindo sem o OLED");
    if (!displayTaskStart()) Serial.println("[B] ERRO: sem memória para a display_task; seguindo sem o TFT");
    else if (!tftOk) Serial.println("[B] TFT sem sprite: só a mensagem de erro na tela");
    // Sem a página, o resto segue
    if (!webTaskStart()) Serial.println("[B] ERRO: sem memória para a web_task; seguindo sem a página");
    if (!serialTaskStart()) fatal("[B] ERRO: sem memória para a serial_task; reiniciando em 1 s");
}

// A loopTask do Arduino não tem trabalho: tudo roda nas tasks criadas no setup()
void loop() { vTaskDelete(nullptr); }
