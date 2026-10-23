// radio_task.cpp — ver radio_task.h
#include "radio_task.h"
#include <Arduino.h>
#include <WiFi.h>
#include <atomic>
#include <esp_now.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <string.h>
#include "config.h"
#include "logger.h"
#include "send_gate.h"
#include "sensor_task.h"
#include "telemetry_pack.h"

static const ValidityLimits VALIDITY_LIMITS = {FUSION_MAX_DT_MS, BARO_STALE_MS, MAG_STALE_MS};

static std::atomic<bool> s_starting{false}, s_active{false};
static std::atomic<uint32_t> s_sent{0}, s_sendErrors{0}, s_macOk{0}, s_macFail{0}, s_cmdOk{0}, s_cmdBad{0};
// Um envio em voo por vez (lib/send_gate): com o anterior sem callback, o ciclo é pulado, e o pacote não entra na
// fila do ESP-NOW atrás dele (com o link ruim, os pacotes saíam em rajadas com até ~1 s de atraso; docs/radio.md,
// passo 5). Nunca liberado só pelo tempo: o callback não identifica o pacote. Preso por RADIO_INFLIGHT_STUCK_MS,
// o ESP-NOW é reiniciado. A SendGate é usada pela radio_task e pelo callback (task do Wi-Fi): seção crítica
static SendGate s_gate(RADIO_INFLIGHT_TIMEOUT_MS, RADIO_INFLIGHT_STUCK_MS);
static portMUX_TYPE s_gateMux = portMUX_INITIALIZER_UNLOCKED;
static std::atomic<uint32_t> s_skipped{0};

// ---------- Callbacks do ESP-NOW (task do Wi-Fi): só contam ----------

static void onSent(const uint8_t *, esp_now_send_status_t status) {
    (status == ESP_NOW_SEND_SUCCESS ? s_macOk : s_macFail).fetch_add(1, std::memory_order_relaxed);
    taskENTER_CRITICAL(&s_gateMux);
    s_gate.callback();
    taskEXIT_CRITICAL(&s_gateMux);
}

// Só o B (PEER_MAC) manda comandos; o buffer é copiado antes de ler (decodeCommand)
static void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
    CommandPacket cmd;
    const bool fromPeer = mac != nullptr && memcmp(mac, PEER_MAC, sizeof(PEER_MAC)) == 0;
    const bool ok = fromPeer && len > 0 && decodeCommand(data, (size_t)len, cmd) == CmdDecode::Ok;
    (ok ? s_cmdOk : s_cmdBad).fetch_add(1, std::memory_order_relaxed);
}

// ---------- Início do rádio ----------

// ESP-NOW, callbacks e peer: no início e na recuperação de um envio preso
static bool espnowStart() {
    if (esp_now_init() != ESP_OK) {
        logPrintln("[radio] ERRO: ESP-NOW não iniciado; rádio desligado");
        return false;
    }
    esp_now_register_send_cb(onSent);
    esp_now_register_recv_cb(onRecv);
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, PEER_MAC, sizeof(PEER_MAC));
    peer.channel = ESPNOW_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK) {
        logPrintln("[radio] ERRO: peer do ESP-NOW não registrado; rádio desligado");
        return false;
    }
    return true;
}

static bool startRadio() {
    WiFi.mode(WIFI_STA);   // STA sem se associar: o canal fica fixo, sem varredura
    esp_wifi_set_ps(WIFI_PS_NONE);
    if (esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        logPrintln("[radio] ERRO: canal do Wi-Fi não fixado; rádio desligado");
        return false;
    }
    if (!espnowStart()) return false;
    logPrintf("[radio] ESP-NOW ativo: MAC %s, canal %d, peer %02X:%02X:%02X:%02X:%02X:%02X, %d Hz | heap livre %u kB, "
              "maior bloco %u kB\n",
              WiFi.macAddress().c_str(), ESPNOW_CHANNEL, PEER_MAC[0], PEER_MAC[1], PEER_MAC[2], PEER_MAC[3],
              PEER_MAC[4], PEER_MAC[5], TX_RATE_HZ, (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024),
              (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024));
    return true;
}

// ---------- Envio a TX_RATE_HZ ----------

static void radioTask(void *) {
    // O Wi-Fi liga depois do boot impresso: o pico de corrente da partida do RF (calibração e primeiras
    // transmissões) pode derrubar a alimentação (laço de reinícios na bancada de 21/10/2026, brownout suspeito),
    // e assim o log mostra onde o reinício acontece
    vTaskDelay(pdMS_TO_TICKS(RADIO_START_DELAY_MS));
    logPrintln("[radio] ligando o Wi-Fi...");
    logFlush();   // a linha sai antes do pico de corrente
    const bool ok = startRadio();
    s_active.store(ok);
    s_starting.store(false);
    if (!ok) {
        vTaskDelete(nullptr);
        return;
    }
    uint16_t seq = 0;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(1000 / TX_RATE_HZ));
        TelemetryState st;
        if (!sensorLatestState(st)) continue;   // a sensor_task ainda não publicou
        // O seq avança mesmo no pulo: o B conta a amostra que não foi entregue como perdida
        const uint16_t pktSeq = seq++;
        const uint32_t nowMs = millis();
        taskENTER_CRITICAL(&s_gateMux);
        const GateAction action = s_gate.poll(nowMs);
        taskEXIT_CRITICAL(&s_gateMux);
        if (action == GateAction::Skip) {
            s_skipped.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (action == GateAction::Recover) {
            // Sem callback por RADIO_INFLIGHT_STUCK_MS (não deveria acontecer): o deinit descarta o envio pendente
            // e o callback dele, e o próximo ciclo começa limpo. Este ciclo não envia
            logPrintf("[radio] envio sem callback há %d ms: reiniciando o ESP-NOW\n", RADIO_INFLIGHT_STUCK_MS);
            esp_now_deinit();
            const bool restarted = espnowStart();
            taskENTER_CRITICAL(&s_gateMux);
            s_gate.recovered();
            taskEXIT_CRITICAL(&s_gateMux);
            if (!restarted) {
                s_active.store(false);
                vTaskDelete(nullptr);
                return;
            }
            continue;
        }
        const TelemetryPacket pkt = buildTelemetry(st, nowMs, (uint64_t)esp_timer_get_time(), pktSeq, VALIDITY_LIMITS);
        taskENTER_CRITICAL(&s_gateMux);
        s_gate.sent(nowMs);   // antes do envio: o callback pode vir antes de o esp_now_send retornar
        taskEXIT_CRITICAL(&s_gateMux);
        const esp_err_t err = esp_now_send(PEER_MAC, (const uint8_t *)&pkt, sizeof(pkt));
        if (err != ESP_OK) {   // recusado: não haverá callback
            taskENTER_CRITICAL(&s_gateMux);
            s_gate.sendFailed();
            taskEXIT_CRITICAL(&s_gateMux);
        }
        (err == ESP_OK ? s_sent : s_sendErrors).fetch_add(1, std::memory_order_relaxed);
    }
}

bool radioTaskStart() {
    s_starting.store(true);   // antes da task: ela pode terminar o início antes do retorno
    const bool ok = xTaskCreatePinnedToCore(radioTask, "radio", RADIO_TASK_STACK, nullptr, RADIO_TASK_PRIO, nullptr,
                                            RADIO_TASK_CORE) == pdPASS;
    if (!ok) s_starting.store(false);
    return ok;
}

RadioCounters radioCounters() {
    RadioCounters c;
    c.starting = s_starting.load();
    c.active = s_active.load();
    c.sent = s_sent.load(std::memory_order_relaxed);
    c.sendErrors = s_sendErrors.load(std::memory_order_relaxed);
    c.macOk = s_macOk.load(std::memory_order_relaxed);
    c.macFail = s_macFail.load(std::memory_order_relaxed);
    c.cmdOk = s_cmdOk.load(std::memory_order_relaxed);
    c.cmdBad = s_cmdBad.load(std::memory_order_relaxed);
    c.skipped = s_skipped.load(std::memory_order_relaxed);
    taskENTER_CRITICAL(&s_gateMux);
    c.inFlightTimeouts = s_gate.timeouts();
    c.inFlightRecoveries = s_gate.recoveries();
    c.unexpectedCallbacks = s_gate.unexpectedCallbacks();
    taskEXIT_CRITICAL(&s_gateMux);
    return c;
}
