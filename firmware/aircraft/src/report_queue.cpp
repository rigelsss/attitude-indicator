// report_queue.cpp — ver report_queue.h
#include "report_queue.h"
#include <Arduino.h>
#include <atomic>
#include "config.h"

static QueueHandle_t s_queue = nullptr;
// Escrito pela sensor_task e lido pela serial_task: atômico (contador de diagnóstico, sem ordem com outros dados)
static std::atomic<uint32_t> s_dropped{0};

bool reportQueueInit() {
    s_queue = xQueueCreate(REPORT_QUEUE_LEN, sizeof(ReportMsg));
    return s_queue != nullptr;
}

bool reportSendRaw(ReportKind kind, const void *r, size_t size) {
    static ReportMsg msg;   // só a sensor_task envia: fora da pilha dela (o item tem ~0,6 kB)
    msg.kind = kind;
    memcpy(msg.data, r, size);
    if (s_queue != nullptr && xQueueSend(s_queue, &msg, 0) == pdTRUE) return true;
    s_dropped.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool reportReceive(ReportMsg &out) { return s_queue != nullptr && xQueueReceive(s_queue, &out, 0) == pdTRUE; }

uint32_t reportDropped() { return s_dropped.load(std::memory_order_relaxed); }
