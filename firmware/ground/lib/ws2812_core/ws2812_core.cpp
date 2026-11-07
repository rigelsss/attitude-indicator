// ws2812_core.cpp — ver ws2812_core.h
#include "ws2812_core.h"

const char *ws2812StateText(Ws2812State s) {
    switch (s) {
    case Ws2812State::Off:          return "não iniciado";
    case Ws2812State::Ready:        return "pronto";
    case Ws2812State::NotInstalled: return "ERRO: reinstalação falhou";
    case Ws2812State::BufferLocked: return "ERRO: desinstalação falhou (buffer preso)";
    }
    return "?";
}

bool Ws2812Driver::begin(const Ws2812Ops &ops, uint8_t *buf, uint16_t count, bool grb) {
    if (state_ != Ws2812State::Off || buf == nullptr || count == 0) return state_ == Ws2812State::Ready;
    ops_ = ops;
    buf_ = buf;
    count_ = count;
    grb_ = grb;
    for (uint32_t i = 0; i < (uint32_t)count * 3; i++) buf_[i] = 0;
    if (ops_.install(ops_.ctx)) state_ = Ws2812State::Ready;
    return ready();
}

void Ws2812Driver::recover() {
    state_ = Ws2812State::NotInstalled;   // antes de tudo: nenhum envio enquanto o canal está sendo refeito
    recoveries_++;
    ops_.stop(ops_.ctx);                  // falhar aqui não impede a desinstalação, que é o que libera o buffer
    if (!ops_.uninstall(ops_.ctx)) {
        state_ = Ws2812State::BufferLocked;
        recoveryFailures_++;
        return;
    }
    if (ops_.install(ops_.ctx)) {
        state_ = Ws2812State::Ready;
    } else {
        recoveryFailures_++;              // desinstalado: o buffer está livre, e retry() pode tentar de novo
    }
}

bool Ws2812Driver::show(const LedRgb *colors, uint16_t count, uint32_t timeoutMs) {
    if (state_ != Ws2812State::Ready) return false;   // na hora, sem tocar no buffer
    if (count > count_) count = count_;
    for (uint16_t i = 0; i < count; i++) {
        uint8_t *b = buf_ + (uint32_t)i * 3;
        b[0] = grb_ ? colors[i].g : colors[i].r;
        b[1] = grb_ ? colors[i].r : colors[i].g;
        b[2] = colors[i].b;
    }
    if (!ops_.write(ops_.ctx, buf_, (size_t)count * 3) || !ops_.waitDone(ops_.ctx, timeoutMs)) {
        failures_++;
        recover();
        return false;
    }
    return true;
}

bool Ws2812Driver::retry() {
    if (state_ != Ws2812State::NotInstalled) return false;
    if (!ops_.install(ops_.ctx)) return false;
    state_ = Ws2812State::Ready;
    return true;
}
