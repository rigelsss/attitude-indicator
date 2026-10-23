// link_task.cpp — ver link_task.h
#include "link_task.h"
#include <Arduino.h>
#include <atomic>
#include <esp_timer.h>
#include "config.h"
#include "delay_estimator.h"

static QueueHandle_t s_latestPkt = nullptr, s_latestReport = nullptr, s_events = nullptr;
static std::atomic<uint32_t> s_eventsDropped{0};

static void publishEvent(const LinkUpdate &u) {
    if (u.events == 0) return;
    const LinkEventMsg msg = {u, millis()};
    if (xQueueSend(s_events, &msg, 0) != pdTRUE) s_eventsDropped.fetch_add(1, std::memory_order_relaxed);
}

static RxCounters rxDelta(const RxCounters &now, const RxCounters &before) {
    RxCounters d;
    d.badSize = now.badSize - before.badSize;
    d.badMagic = now.badMagic - before.badMagic;
    d.badVersion = now.badVersion - before.badVersion;
    d.badType = now.badType - before.badType;
    d.foreign = now.foreign - before.foreign;
    d.queueFull = now.queueFull - before.queueFull;
    return d;
}

static void linkTask(void *) {
    const LinkConfig cfg = {(uint64_t)LINK_TIMEOUT_MS * 1000, LINK_RESYNC_GAP, LINK_LATE_MAX_PKTS, LINK_LATE_MAX_MS};
    LinkStats stats(cfg);
    DelayEstimator delay({DELAY_BUCKET_MS, DELAY_BUCKETS});
    uint32_t delayMaxWin = 0, delayLast = 0;
    QueueHandle_t rxQueue = radioRxQueue();
    uint64_t windowStartUs = (uint64_t)esp_timer_get_time();
    RxCounters rxBefore = radioRxCounters();
    uint32_t index = 0;
    for (;;) {
        RxItem item;
        // Sem rádio (fila nula), a task só publica os resumos: o r e o c mostram "sem sinal"
        if (rxQueue != nullptr && xQueueReceive(rxQueue, &item, pdMS_TO_TICKS(LINK_TASK_WAIT_MS)) == pdTRUE) {
            const LinkUpdate u = stats.onPacket(item.pkt.hdr.seq, item.pkt.t_ms, item.arrivalUs);
            publishEvent(u);
            // Duplicados e atrasados não renovam o último pacote (nem o instante de chegada) dos consumidores
            if (u.accepted) {
                // A reiniciado: o relógio dele voltou a zero, e a base do atraso recomeça
                if (u.events & LINK_EV_RESYNC) delay.reset();
                item.delayMs = delay.feed(item.pkt.t_ms, item.arrivalUs / 1000);
                delayLast = item.delayMs;
                if (item.delayMs > delayMaxWin) delayMaxWin = item.delayMs;
                xQueueOverwrite(s_latestPkt, &item);
            }
        } else if (rxQueue == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(LINK_TASK_WAIT_MS));
        }
        const uint64_t nowUs = (uint64_t)esp_timer_get_time();
        publishEvent(stats.poll(nowUs));

        if (nowUs - windowStartUs >= (uint64_t)LINK_REPORT_MS * 1000) {
            const RxCounters rxNow = radioRxCounters();
            LinkReport r;
            r.index = ++index;
            r.windowMs = (uint32_t)((nowUs - windowStartUs) / 1000);
            r.state = stats.state();
            r.win = stats.takeWindow();
            r.totals = stats.totals();
            r.rxWin = rxDelta(rxNow, rxBefore);
            r.rxTotals = rxNow;
            r.havePacket = stats.hasPacket();
            r.lastSeq = stats.lastSeq();
            r.lastTms = stats.lastTms();
            r.ageMs = r.havePacket && nowUs > stats.lastArrivalUs()
                          ? (uint32_t)((nowUs - stats.lastArrivalUs()) / 1000)
                          : 0;
            r.delayMs = delayLast;
            r.delayMaxMs = delayMaxWin;
            delayMaxWin = 0;
            xQueueOverwrite(s_latestReport, &r);
            rxBefore = rxNow;
            windowStartUs = nowUs;
        }
    }
}

bool linkTaskStart() {
    s_latestPkt = xQueueCreate(1, sizeof(RxItem));
    s_latestReport = xQueueCreate(1, sizeof(LinkReport));
    s_events = xQueueCreate(LINK_EVENT_QUEUE_LEN, sizeof(LinkEventMsg));
    if (s_latestPkt == nullptr || s_latestReport == nullptr || s_events == nullptr) return false;
    return xTaskCreatePinnedToCore(linkTask, "link", LINK_TASK_STACK, nullptr, LINK_TASK_PRIO, nullptr,
                                   LINK_TASK_CORE) == pdPASS;
}

bool linkLatestPacket(RxItem &out) { return s_latestPkt != nullptr && xQueuePeek(s_latestPkt, &out, 0) == pdTRUE; }

bool linkLatestReport(LinkReport &out) {
    return s_latestReport != nullptr && xQueuePeek(s_latestReport, &out, 0) == pdTRUE;
}

bool linkEventReceive(LinkEventMsg &out) { return s_events != nullptr && xQueueReceive(s_events, &out, 0) == pdTRUE; }

uint32_t linkEventsDropped() { return s_eventsDropped.load(std::memory_order_relaxed); }
