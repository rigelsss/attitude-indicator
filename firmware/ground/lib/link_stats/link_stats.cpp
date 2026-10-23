// link_stats.cpp — ver link_stats.h
#include "link_stats.h"

const char *linkStateText(LinkState s) {
    switch (s) {
    case LinkState::NoSignal: return "sem sinal";
    case LinkState::Ok:       return "OK";
    case LinkState::Lost:     return "PERDIDO";
    }
    return "?";
}

LinkStats::LinkStats(const LinkConfig &cfg) : cfg_(cfg) { resetWindow(); }

void LinkStats::count(uint32_t LinkCounts::*field, uint32_t n) {
    totals_.*field += n;
    win_.counts.*field += n;
}

void LinkStats::resetWindow() {
    win_ = {};
    win_.intervalMinUs = UINT32_MAX;
    win_.srcIntervalMinMs = UINT32_MAX;
}

LinkUpdate LinkStats::onPacket(uint16_t seq, uint32_t tMs, uint64_t arrivalUs) {
    LinkUpdate u = {};
    if (state_ == LinkState::NoSignal) {
        u.events = LINK_EV_FIRST;
    } else {
        const uint16_t d = (uint16_t)(seq - lastSeq_);
        const uint16_t back = (uint16_t)(lastSeq_ - seq);   // quanto o seq está atrás do último
        const bool tBack = tMs < lastTms_;
        if (!tBack && d == 0) {
            count(&LinkCounts::duplicates, 1);
            return u;
        }
        // Atrasado: seq e t_ms pouco atrás do último aceito. Descartado; se tinha sido contado como perdido,
        // continua contado (chegou tarde demais para ser usado)
        if (tBack && back > 0 && back <= cfg_.lateMaxPkts && lastTms_ - tMs <= cfg_.lateMaxMs) {
            count(&LinkCounts::late, 1);
            return u;
        }
        // Chegadas separadas por mais que o timeout: a queda não foi vista pelo poll (task atrasada, com os
        // pacotes esperando na fila). Registrada aqui, junto com o restabelecimento
        if (state_ == LinkState::Ok && arrivalUs > lastArrivalUs_ && arrivalUs - lastArrivalUs_ > cfg_.timeoutUs) {
            state_ = LinkState::Lost;
            count(&LinkCounts::timeouts, 1);
            u.events |= LINK_EV_LOST;
        }
        if (state_ == LinkState::Lost) {
            u.events |= LINK_EV_RESTORED;
            u.outageUs = arrivalUs > lastArrivalUs_ ? arrivalUs - lastArrivalUs_ : 0;
        }
        if (tBack || d > cfg_.resyncGap) {   // t_ms do A voltou além do atraso aceito: reinício
            u.events |= LINK_EV_RESYNC;
            u.seqFrom = lastSeq_;
            u.seqTo = seq;
            u.tFrom = lastTms_;
            u.tTo = tMs;
            count(&LinkCounts::resyncs, 1);
        } else {
            count(&LinkCounts::lost, d - 1u);
            // Intervalo só entre dois pacotes com o link OK (sem timeout nem ressincronização no meio)
            if (state_ == LinkState::Ok && arrivalUs >= lastArrivalUs_) {
                const uint64_t dt = arrivalUs - lastArrivalUs_;
                const uint32_t dt32 = dt > UINT32_MAX ? UINT32_MAX : (uint32_t)dt;
                win_.intervals++;
                win_.intervalSumUs += dt32;
                if (dt32 < win_.intervalMinUs) win_.intervalMinUs = dt32;
                if (dt32 > win_.intervalMaxUs) win_.intervalMaxUs = dt32;
                const uint32_t srcMs = tMs - lastTms_;   // t_ms não voltou (tBack é falso)
                if (srcMs < win_.srcIntervalMinMs) win_.srcIntervalMinMs = srcMs;
                if (srcMs > win_.srcIntervalMaxMs) win_.srcIntervalMaxMs = srcMs;
            }
        }
    }
    u.accepted = true;
    count(&LinkCounts::received, 1);
    state_ = LinkState::Ok;
    lastSeq_ = seq;
    lastTms_ = tMs;
    lastArrivalUs_ = arrivalUs;
    return u;
}

LinkUpdate LinkStats::poll(uint64_t nowUs) {
    LinkUpdate u = {};
    // A chegada é carimbada na task do Wi-Fi e pode ser um pouco posterior ao nowUs desta chamada
    if (state_ == LinkState::Ok && nowUs > lastArrivalUs_ && nowUs - lastArrivalUs_ > cfg_.timeoutUs) {
        state_ = LinkState::Lost;
        count(&LinkCounts::timeouts, 1);
        u.events = LINK_EV_LOST;
        u.outageUs = nowUs - lastArrivalUs_;
    }
    return u;
}

LinkWindow LinkStats::takeWindow() {
    LinkWindow w = win_;
    if (w.intervals == 0) {
        w.intervalMinUs = 0;
        w.srcIntervalMinMs = 0;
    }
    resetWindow();
    return w;
}
