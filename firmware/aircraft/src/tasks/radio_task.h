// radio_task.h — ESP-NOW do ESP A (docs/radio.md): a TX_RATE_HZ, monta o TelemetryPacket a partir do estado
// publicado pela sensor_task e o envia ao PEER_MAC (ESP B). Wi-Fi em modo STA sem se associar, canal fixo
// ESPNOW_CHANNEL. Um envio só sai com o anterior já confirmado ou recusado pelo callback; senão, o ciclo é
// pulado (dado velho descartado em vez de atrasado). Nunca liberado só pelo tempo; preso, o ESP-NOW é reiniciado
// (lib/send_gate). Os callbacks do ESP-NOW (task do Wi-Fi, núcleo 0) só contam: entregas e falhas MAC do envio,
// comandos aceitos e recusados na recepção (a execução dos comandos é da semana 5).
// Núcleo RADIO_TASK_CORE (0), prioridade RADIO_TASK_PRIO (config.h).
#pragma once
#include <stdint.h>

// Contadores desde o boot (cópia; cada um é lido de forma atômica, sem ordem entre eles)
struct RadioCounters {
    bool     starting;     // task criada, Wi-Fi ainda não iniciado (RADIO_START_DELAY_MS)
    bool     active;       // Wi-Fi e ESP-NOW iniciados
    uint32_t sent;         // esp_now_send aceito
    uint32_t sendErrors;   // esp_now_send recusado (fila do ESP-NOW cheia, por exemplo)
    uint32_t macOk;        // entregue: o B confirmou (ACK da camada MAC)
    uint32_t macFail;      // sem confirmação depois das retransmissões
    uint32_t cmdOk;        // CommandPacket do PEER_MAC, com tamanho, magic, versão e tipo certos
    uint32_t cmdBad;       // recebido e recusado (outro remetente ou pacote inválido)
    uint32_t skipped;      // ciclos pulados com o envio anterior ainda em voo (o seq avança: o B conta a perda)
    uint32_t inFlightTimeouts;     // episódios de envio em voo sem callback por RADIO_INFLIGHT_TIMEOUT_MS
    uint32_t inFlightRecoveries;   // ESP-NOW reiniciado com um envio preso por RADIO_INFLIGHT_STUCK_MS
    uint32_t unexpectedCallbacks;  // callbacks de envio sem envio em voo (ignorados)
};

// Cria a task (que inicia o Wi-Fi e o ESP-NOW no próprio núcleo). false sem memória
bool radioTaskStart();
RadioCounters radioCounters();
