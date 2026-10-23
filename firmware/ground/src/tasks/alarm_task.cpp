// alarm_task.cpp — ver alarm_task.h
#include "alarm_task.h"
#include <Arduino.h>
#include <atomic>
#include <esp_timer.h>
#include "config.h"
#include "delay_estimator.h"
#include "link_task.h"

static QueueHandle_t s_ack = nullptr, s_events = nullptr, s_status = nullptr;
static std::atomic<uint32_t> s_eventsDropped{0};

static void publish(AlarmEventMsg::Type type, AlarmKind kind, const RxItem *last) {
    AlarmEventMsg e = {};
    e.type = type;
    e.kind = kind;
    if (last != nullptr) {
        e.rollDeg = last->pkt.roll;
        e.pitchDeg = last->pkt.pitch;
        e.valid = last->pkt.valid;
        e.status = last->pkt.status;
    }
    e.atMs = millis();
    // Com a fila cheia, a mudança é descartada e contada: a serial avisa, e o c mostra o estado atual
    if (xQueueSend(s_events, &e, 0) != pdTRUE) s_eventsDropped.fetch_add(1, std::memory_order_relaxed);
}

static void alarmTask(void *) {
    const AlarmConfig cfg = {ALARM_BANK_CAUTION_DEG, ALARM_BANK_DEG,       ALARM_PITCH_UP_DEG,     ALARM_PITCH_DOWN_DEG,
                             ALARM_HYST_DEG,         ALARM_INVALID_DELAY_MS, ALARM_LINK_BLINK_MS,  ALARM_CAUTION_BLINK_MS,
                             ALARM_WARNING_BLINK_MS, ALARM_DOUBLE_PERIOD_MS, ALARM_DOUBLE_ON_MS};
    AlarmLogic logic(cfg);
    pinMode(PIN_LED_ALARM, OUTPUT);
    pinMode(PIN_LED_LINK, OUTPUT);
    digitalWrite(PIN_LED_ALARM, LOW);
    digitalWrite(PIN_LED_LINK, LOW);
#if ALARM_BUZZER_ENABLED
    pinMode(PIN_BUZZER, OUTPUT);
    digitalWrite(PIN_BUZZER, LOW);
#endif
    AlarmKind lastKind = AlarmKind::None;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        RxItem last;
        const bool have = linkLatestPacket(last);
        AlarmInputs in = {};
        in.everLinked = have;
        if (have) {
            // Mesma regra do timeout da link_task: idade acima de LINK_TIMEOUT_MS = link perdido. A chegada é
            // carimbada na task do Wi-Fi e pode ser um pouco posterior a este instante
            const uint64_t nowUs = (uint64_t)esp_timer_get_time();
            const uint64_t ageUs = nowUs > last.arrivalUs ? nowUs - last.arrivalUs : 0;
            in.linkOk = ageUs <= (uint64_t)LINK_TIMEOUT_MS * 1000;
            // Idade total do dado (atraso na chegada + tempo desde a chegada): a mesma regra do OLED
            in.dataLate = dataAgeMs(last.delayMs, ageUs / 1000) > LINK_STALE_DELAY_MS;
            in.attValid = (last.pkt.valid & VAL_ATT) != 0;
            in.calibrating = (last.pkt.status & ST_CALIBRATING) != 0;
            in.rollDeg = last.pkt.roll;
            in.pitchDeg = last.pkt.pitch;
        }
        const uint32_t nowMs = millis();
        AlarmOutput out = logic.update(in, nowMs);

        uint8_t ack;
        while (xQueueReceive(s_ack, &ack, 0) == pdTRUE) {
            if (logic.acknowledge()) publish(AlarmEventMsg::Acked, logic.kind(), have ? &last : nullptr);
            else publish(AlarmEventMsg::NothingToAck, AlarmKind::None, nullptr);
            out = logic.update(in, nowMs);   // o LED já reflete o reconhecimento nesta volta
        }
        if (out.kind != lastKind) {
            publish(AlarmEventMsg::Changed, out.kind, have ? &last : nullptr);
            lastKind = out.kind;
        }

        digitalWrite(PIN_LED_ALARM, out.alarmLed ? HIGH : LOW);
        digitalWrite(PIN_LED_LINK, out.linkLed ? HIGH : LOW);
#if ALARM_BUZZER_ENABLED
        digitalWrite(PIN_BUZZER, out.alarmLed ? HIGH : LOW);
#endif
        const AlarmStatus st = {out.kind, out.acked, out.linkLed};
        xQueueOverwrite(s_status, &st);
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(ALARM_TASK_PERIOD_MS));
    }
}

bool alarmTaskStart() {
    s_ack = xQueueCreate(4, sizeof(uint8_t));
    s_events = xQueueCreate(ALARM_EVENT_QUEUE_LEN, sizeof(AlarmEventMsg));
    s_status = xQueueCreate(1, sizeof(AlarmStatus));
    if (s_ack == nullptr || s_events == nullptr || s_status == nullptr) return false;
    return xTaskCreatePinnedToCore(alarmTask, "alarm", ALARM_TASK_STACK, nullptr, ALARM_TASK_PRIO, nullptr,
                                   ALARM_TASK_CORE) == pdPASS;
}

bool alarmAcknowledge() {
    const uint8_t one = 1;
    return s_ack != nullptr && xQueueSend(s_ack, &one, 0) == pdTRUE;
}

bool alarmEventReceive(AlarmEventMsg &out) { return s_events != nullptr && xQueueReceive(s_events, &out, 0) == pdTRUE; }

uint32_t alarmEventsDropped() { return s_eventsDropped.load(std::memory_order_relaxed); }

bool alarmCurrent(AlarmStatus &out) { return s_status != nullptr && xQueuePeek(s_status, &out, 0) == pdTRUE; }
