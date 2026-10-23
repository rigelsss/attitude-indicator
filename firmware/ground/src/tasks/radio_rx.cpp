// radio_rx.cpp — ver radio_rx.h
#include "radio_rx.h"
#include <Arduino.h>
#include <WiFi.h>
#include <atomic>
#include <esp_now.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <string.h>
#include "config.h"
#include "telemetry_rx.h"

static QueueHandle_t s_queue = nullptr;
static RadioInfo s_info = {};
static std::atomic<uint32_t> s_badSize{0}, s_badMagic{0}, s_badVersion{0}, s_badType{0}, s_foreign{0},
    s_queueFull{0};

// ---------- Callback de recepção (task do Wi-Fi): confere, copia e enfileira ----------

static void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
    const uint64_t arrivalUs = (uint64_t)esp_timer_get_time();   // antes de qualquer conferência
    if (mac == nullptr || memcmp(mac, PEER_MAC, sizeof(PEER_MAC)) != 0) {
        s_foreign.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    RxItem item;
    item.arrivalUs = arrivalUs;
    item.delayMs = 0;
    switch (decodeTelemetry(data, len > 0 ? (size_t)len : 0, item.pkt)) {
    case RxDecode::Ok:
        // Não é ISR: xQueueSend, sem espera
        if (xQueueSend(s_queue, &item, 0) != pdTRUE) s_queueFull.fetch_add(1, std::memory_order_relaxed);
        break;
    case RxDecode::BadSize:    s_badSize.fetch_add(1, std::memory_order_relaxed); break;
    case RxDecode::BadMagic:   s_badMagic.fetch_add(1, std::memory_order_relaxed); break;
    case RxDecode::BadVersion: s_badVersion.fetch_add(1, std::memory_order_relaxed); break;
    case RxDecode::BadType:    s_badType.fetch_add(1, std::memory_order_relaxed); break;
    }
}

// ---------- Início do rádio ----------

static void macText(char *out, const uint8_t *m) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

bool radioRxStart() {
    s_queue = xQueueCreate(RX_QUEUE_LEN, sizeof(RxItem));
    if (s_queue == nullptr) {
        Serial.println("[radio] ERRO: sem memória para a fila de recepção; rádio desligado");
        return false;
    }
    Serial.println("[radio] ligando o Wi-Fi...");
    Serial.flush();   // a linha sai antes do pico de corrente da partida do RF (brownout no A, 21/10/2026)
    // AP + STA: o A envia para o MAC base, o da STA; em WIFI_AP puro, só o MAC do AP (base + 1) receberia.
    // A STA nunca se associa (sem WiFi.begin()): assim não varre canais e o rádio fica no canal do AP
    if (!WiFi.mode(WIFI_AP_STA)) {
        Serial.println("[radio] ERRO: Wi-Fi não iniciado em AP+STA; rádio desligado");
        return false;
    }
    if (!WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL)) {
        Serial.println("[radio] ERRO: AP não iniciado; rádio desligado");
        return false;
    }
    esp_wifi_set_ps(WIFI_PS_NONE);
    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&primary, &second);
    s_info.channel = primary;
    if (primary != ESPNOW_CHANNEL) {
        Serial.printf("[radio] ERRO: rádio no canal %u, e não no %d; rádio desligado\n", primary, ESPNOW_CHANNEL);
        return false;
    }
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    macText(s_info.staMac, mac);
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    macText(s_info.apMac, mac);

    if (esp_now_init() != ESP_OK) {
        Serial.println("[radio] ERRO: ESP-NOW não iniciado; rádio desligado");
        return false;
    }
    esp_now_register_recv_cb(onRecv);
    // O A como peer: o B ainda não envia nada (os comandos são da semana 5)
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, PEER_MAC, sizeof(PEER_MAC));
    peer.channel = ESPNOW_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK) {
        Serial.println("[radio] ERRO: peer do ESP-NOW não registrado; rádio desligado");
        return false;
    }
    Serial.printf("[radio] ESP-NOW ativo: MAC STA %s (recebe), MAC AP %s, AP \"%s\", canal %u, "
                  "peer %02X:%02X:%02X:%02X:%02X:%02X | heap livre %u kB, maior bloco %u kB\n",
                  s_info.staMac, s_info.apMac, AP_SSID, s_info.channel, PEER_MAC[0], PEER_MAC[1], PEER_MAC[2],
                  PEER_MAC[3], PEER_MAC[4], PEER_MAC[5], (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024),
                  (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024));
    s_info.active = true;
    return true;
}

QueueHandle_t radioRxQueue() { return s_queue; }

RxCounters radioRxCounters() {
    RxCounters c;
    c.badSize = s_badSize.load(std::memory_order_relaxed);
    c.badMagic = s_badMagic.load(std::memory_order_relaxed);
    c.badVersion = s_badVersion.load(std::memory_order_relaxed);
    c.badType = s_badType.load(std::memory_order_relaxed);
    c.foreign = s_foreign.load(std::memory_order_relaxed);
    c.queueFull = s_queueFull.load(std::memory_order_relaxed);
    return c;
}

const RadioInfo &radioInfo() { return s_info; }
