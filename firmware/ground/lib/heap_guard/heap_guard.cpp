// heap_guard.cpp — ver heap_guard.h
#include "heap_guard.h"

int HeapGuard::culprit(uint32_t nowMs, const HeapClient *clients, int n, uint32_t &stalledMs) const {
    int alvo = -1;
    stalledMs = 0;
    for (int i = 0; i < n; i++) {
        const HeapClient &c = clients[i];
        if (!c.ready || !c.pending) continue;
        const uint32_t parado = nowMs - c.lastOkMs;
        if (parado >= cfg_.stallMs && (alvo < 0 || parado > stalledMs)) {
            alvo = i;
            stalledMs = parado;
        }
    }
    return alvo;
}

HeapDecision HeapGuard::step(uint32_t freeKb, uint32_t nowMs, const HeapClient *clients, int n) {
    if (paused_ && freeKb >= cfg_.resumeKb) {
        paused_ = false;
        return {HeapAction::Resume, -1, 0};
    }
    if (freeKb >= cfg_.pauseKb) return {HeapAction::None, -1, 0};
    const bool podeAgir = !dropped_ || nowMs - lastDropMs_ >= cfg_.dropGapMs;
    if (!podeAgir) return {HeapAction::None, -1, 0};   // a memória ainda voltando da derrubada anterior
    uint32_t parado = 0;
    const int alvo = culprit(nowMs, clients, n, parado);
    if (alvo >= 0) {
        dropped_ = true;
        lastDropMs_ = nowMs;
        return {HeapAction::Drop, alvo, parado};
    }
    if (!paused_) {
        paused_ = true;
        return {HeapAction::Pause, -1, 0};
    }
    return {HeapAction::None, -1, 0};
}
