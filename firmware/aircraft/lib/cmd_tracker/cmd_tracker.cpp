// cmd_tracker.cpp — ver cmd_tracker.h
#include "cmd_tracker.h"
#include "telemetry.h"

void CmdTracker::expire(uint32_t nowMs) {
    for (Rec &r : recs_)
        if (r.used && nowMs - r.finishedMs >= r.keepMs) r.used = false;
}

int CmdTracker::freeSlots() const {
    int n = 0;
    for (const Rec &r : recs_)
        if (!r.used) n++;
    return n;
}

int CmdTracker::retained() const { return CAPACITY - freeSlots(); }

void CmdTracker::store(const CmdAck &a, uint32_t nowMs, uint32_t keepMs) {
    for (Rec &r : recs_) {
        if (r.used) continue;
        r = {true, a, nowMs, keepMs};
        return;
    }
    // Sem lugar: o resultado não é guardado (arrive já recusou com Full; o pendente tem lugar reservado)
}

CmdArrival CmdTracker::arrive(uint16_t seq, uint8_t cmd, uint32_t nowMs, CmdAck &ack) {
    (void)cmd;
    expire(nowMs);
    if (pending_ && pend_.cmdSeq == seq) {
        ack = pend_;
        return CmdArrival::Duplicate;
    }
    for (const Rec &r : recs_) {
        if (r.used && r.ack.cmdSeq == seq) {
            ack = r.ack;
            return CmdArrival::Duplicate;
        }
    }
    // Com um demorado em curso, um lugar fica reservado para o resultado dele
    const int needed = pending_ ? 2 : 1;
    if (freeSlots() < needed) return CmdArrival::Full;
    return pending_ ? CmdArrival::Busy : CmdArrival::New;
}

CmdAck CmdTracker::finish(uint16_t seq, uint8_t cmd, uint8_t result, uint32_t nowMs) {
    const CmdAck a = {cmd, result, seq};
    store(a, nowMs, shortMs_);   // imediato: só os reenvios do B o pedem de novo
    return a;
}

CmdAck CmdTracker::refuse(uint16_t seq, uint8_t cmd) const { return {cmd, ACK_REJECTED, seq}; }

CmdAck CmdTracker::start(uint16_t seq, uint8_t cmd, uint32_t nowMs, uint32_t timeoutMs) {
    pend_ = {cmd, ACK_ACCEPTED, seq};
    pending_ = true;
    startMs_ = nowMs;
    timeoutMs_ = timeoutMs;
    return pend_;
}

CmdAck CmdTracker::complete(uint8_t result, uint32_t nowMs) {
    pend_.result = result;
    pending_ = false;
    expire(nowMs);
    store(pend_, nowMs, longMs_);   // demorado: o B o reconsulta
    return pend_;
}
