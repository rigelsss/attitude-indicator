// radio_rx.h — recepção do ESP-NOW no ESP B (docs/radio.md, ESP B). Wi-Fi em WIFI_AP_STA: o AP no canal
// ESPNOW_CHANNEL e a STA sem se associar (nunca WiFi.begin(), para o rádio não sair do canal). O A envia para
// o MAC base (o da STA), que só vale com a STA ligada.
// O callback de recepção (task do Wi-Fi, núcleo 0) só filtra o remetente (PEER_MAC, o A), confere e copia o
// pacote (decodeTelemetry), carimba a chegada e enfileira para a link_task. Nada de impressão no callback.
#pragma once
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "telemetry.h"

// Item da fila de recepção
struct RxItem {
    TelemetryPacket pkt;
    uint64_t        arrivalUs;   // esp_timer_get_time() na chegada
};

// Contadores do callback desde o boot (cada um lido de forma atômica, sem ordem entre eles)
struct RxCounters {
    uint32_t badSize, badMagic, badVersion, badType;   // recusados, por motivo (RxDecode)
    uint32_t foreign;     // outro remetente
    uint32_t queueFull;   // fila da link_task cheia: descartado
};

// Dados da configuração, para a linha de boot e o c
struct RadioInfo {
    bool    active;    // Wi-Fi, AP e ESP-NOW iniciados
    char    staMac[18], apMac[18];
    uint8_t channel;   // canal lido do rádio depois da configuração
};

// Inicia o Wi-Fi (AP + STA), o ESP-NOW e o peer (o A). Imprime direto no Serial: chamar no setup(), antes das
// tasks. false em falha (o motivo é impresso)
bool radioRxStart();
// Fila de recepção (RxItem), consumida pela link_task
QueueHandle_t radioRxQueue();
RxCounters radioRxCounters();
const RadioInfo &radioInfo();
