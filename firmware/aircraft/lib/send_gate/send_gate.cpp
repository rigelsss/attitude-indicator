// send_gate.cpp — ver send_gate.h
#include "send_gate.h"

GateAction SendGate::poll(uint32_t nowMs) {
    if (!inFlight_) return GateAction::Send;
    const uint32_t elapsed = nowMs - sinceMs_;
    if (elapsed >= stuckMs_) return GateAction::Recover;
    if (elapsed >= timeoutMs_ && !timeoutCounted_) {
        timeoutCounted_ = true;
        timeouts_++;
    }
    return GateAction::Skip;
}

void SendGate::sent(uint32_t nowMs) {
    inFlight_ = true;
    timeoutCounted_ = false;
    sinceMs_ = nowMs;
}

void SendGate::sendFailed() { inFlight_ = false; }

void SendGate::callback() {
    if (!inFlight_) {
        unexpected_++;
        return;
    }
    inFlight_ = false;
}

void SendGate::recovered() {
    inFlight_ = false;
    recoveries_++;
}
