// command_task.cpp — ver command_task.h
#include "command_task.h"
#include <Arduino.h>
#include <atomic>
#include <esp_random.h>
#include "config.h"
#include "radio_rx.h"

struct CmdRequest {
    uint8_t cmd;
    int32_t arg;
    uint8_t origin;
    uint16_t rid;
};

static QueueHandle_t s_req = nullptr, s_events = nullptr, s_lastEncoder = nullptr, s_webEvents = nullptr;
static std::atomic<uint32_t> s_webEventsDropped{0};
static std::atomic<uint32_t> s_eventsDropped{0};
static std::atomic<uint32_t> s_submitted{0}, s_sends{0}, s_sendErrors{0}, s_acksMatched{0}, s_acksIgnored{0},
    s_localRejects{0}, s_polls{0};

const char *cmdOriginText(uint8_t origin) {
    switch (origin) {
    case CMD_FROM_CONSOLE: return "console";
    case CMD_FROM_ENCODER: return "encoder";
    case CMD_FROM_WEB:     return "página";
    case CMD_FROM_PC:      return "pc";
    }
    return "?";
}

static void publish(const CmdEvent &ev, uint8_t attempt, bool localFull) {
    const CmdEventMsg m = {ev, attempt, localFull, millis()};
    if (xQueueSend(s_events, &m, 0) != pdTRUE) s_eventsDropped.fetch_add(1, std::memory_order_relaxed);
    if (ev.origin == CMD_FROM_ENCODER) xQueueOverwrite(s_lastEncoder, &m);   // o OLED mostra o andamento
    // A página recebe todos os comandos (de qualquer origem): cada cliente vê o andamento
    if (xQueueSend(s_webEvents, &m, 0) != pdTRUE) s_webEventsDropped.fetch_add(1, std::memory_order_relaxed);
}

static void commandTask(void *) {
    const CmdLinkConfig cfg = {CMD_RETRY_MS, CMD_MAX_TRIES, CMD_COMPLETION_MS, CMD_POLL_MS};
    // Seq inicial aleatório: um B reiniciado não repete os seq que o A lembra (ele devolveria o ACK antigo sem
    // executar o comando novo)
    CommandLink link(cfg, (uint16_t)(esp_random() & 0xFFFF));
    QueueHandle_t acks = radioAckQueue();
    for (;;) {
        CmdRequest r;
        if (xQueueReceive(s_req, &r, pdMS_TO_TICKS(CMD_TASK_WAIT_MS)) == pdTRUE) {
            uint16_t seq;
            if (link.submit(r.cmd, r.arg, r.origin, millis(), seq, r.rid)) {
                s_submitted.fetch_add(1, std::memory_order_relaxed);
            } else {
                s_localRejects.fetch_add(1, std::memory_order_relaxed);
                publish({r.cmd, r.arg, 0, r.origin, CmdState::Rejected, r.rid}, 0, true);   // "busy", seq 0
            }
        }
        AckPacket a;
        while (acks != nullptr && xQueueReceive(acks, &a, 0) == pdTRUE) {
            CmdEvent ev;
            if (link.onAck(a.cmd_seq, a.cmd, a.result, millis(), ev)) {
                s_acksMatched.fetch_add(1, std::memory_order_relaxed);
                publish(ev, 0, false);
            } else {
                s_acksIgnored.fetch_add(1, std::memory_order_relaxed);   // repetido, atrasado ou desconhecido
            }
        }
        CmdSend s;
        while (link.nextSend(millis(), s)) {
            const bool ok = radioSendCommand(buildCommandPacket(s));
            (ok ? s_sends : s_sendErrors).fetch_add(1, std::memory_order_relaxed);
            if (s.attempt == 0) s_polls.fetch_add(1, std::memory_order_relaxed);   // reconsulta: sem evento
            else publish({s.cmd, s.arg, s.seq, s.origin, CmdState::Sending, s.rid}, s.attempt, false);
        }
        CmdEvent ev;
        while (link.nextTimeout(millis(), ev)) publish(ev, 0, false);
    }
}

bool commandTaskStart() {
    s_req = xQueueCreate(CMD_REQ_QUEUE_LEN, sizeof(CmdRequest));
    s_events = xQueueCreate(CMD_EVENT_QUEUE_LEN, sizeof(CmdEventMsg));
    s_lastEncoder = xQueueCreate(1, sizeof(CmdEventMsg));
    s_webEvents = xQueueCreate(CMD_EVENT_QUEUE_LEN, sizeof(CmdEventMsg));
    if (s_req == nullptr || s_events == nullptr || s_lastEncoder == nullptr || s_webEvents == nullptr) return false;
    return xTaskCreatePinnedToCore(commandTask, "command", CMD_TASK_STACK, nullptr, CMD_TASK_PRIO, nullptr,
                                   CMD_TASK_CORE) == pdPASS;
}

bool commandSubmit(uint8_t cmd, int32_t arg, uint8_t origin, uint16_t rid) {
    const CmdRequest r = {cmd, arg, origin, rid};
    return s_req != nullptr && xQueueSend(s_req, &r, 0) == pdTRUE;
}

bool commandEventReceive(CmdEventMsg &out) { return s_events != nullptr && xQueueReceive(s_events, &out, 0) == pdTRUE; }

bool commandLastFromEncoder(CmdEventMsg &out) {
    return s_lastEncoder != nullptr && xQueuePeek(s_lastEncoder, &out, 0) == pdTRUE;
}

bool commandWebEventReceive(CmdEventMsg &out) {
    return s_webEvents != nullptr && xQueueReceive(s_webEvents, &out, 0) == pdTRUE;
}

uint32_t commandWebEventsDropped() { return s_webEventsDropped.load(std::memory_order_relaxed); }

uint32_t commandEventsDropped() { return s_eventsDropped.load(std::memory_order_relaxed); }

CmdStats commandStats() {
    return {s_submitted.load(), s_sends.load(), s_sendErrors.load(), s_acksMatched.load(), s_acksIgnored.load(),
            s_localRejects.load(), s_polls.load()};
}
