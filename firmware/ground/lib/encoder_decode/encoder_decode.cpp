// encoder_decode.cpp — ver encoder_decode.h
#include "encoder_decode.h"

// Índice (anterior << 2) | atual: ±1 nas transições válidas, 0 nas que pulam um estado. Mesma tabela do
// ground_diag, em que o sentido horário deu passos positivos
static const int8_t QDEC[16] = {0, -1, +1, 0, +1, 0, 0, -1, -1, 0, 0, +1, 0, +1, -1, 0};

EncoderDecoder::EncoderDecoder(const EncoderConfig &cfg, uint8_t initialState)
    : cfg_(cfg), prev_((uint8_t)(initialState & 3)) {}

bool EncoderDecoder::isDetent(uint8_t state) const {
    return state == 3 || (cfg_.stepsPerClick == 2 && state == 0);
}

void EncoderDecoder::resync() {
    acc_ = 0;
    synced_ = false;
    havePrev_ = false;
}

int8_t EncoderDecoder::feed(uint8_t state) {
    state &= 3;
    if (!havePrev_) {   // primeiro estado depois de uma lacuna: só registra
        prev_ = state;
        havePrev_ = true;
        if (isDetent(state)) synced_ = true;
        return 0;
    }
    if (state == prev_) return 0;   // duas bordas lidas no mesmo estado (repique rápido)
    const int8_t d = QDEC[(prev_ << 2) | state];
    prev_ = state;
    if (d == 0) {
        invalid_++;
        acc_ = 0;
        synced_ = false;   // a soma só recomeça no próximo repouso
        if (isDetent(state)) synced_ = true;
        return 0;
    }
    if (!synced_) {
        if (isDetent(state)) {
            synced_ = true;
            acc_ = 0;
        }
        return 0;
    }
    acc_ += d;
    if (!isDetent(state)) return 0;
    int8_t click = 0;
    if (acc_ >= cfg_.stepsPerClick) click = cfg_.dir;
    else if (acc_ <= -cfg_.stepsPerClick) click = (int8_t)-cfg_.dir;
    acc_ = 0;   // num repouso, a soma recomeça, com ou sem clique
    return click;
}

ButtonEvent ButtonDebounce::update(bool pressedRaw, uint32_t nowMs) {
    if (pressedRaw != lastRaw_) {
        lastRaw_ = pressedRaw;
        changeMs_ = nowMs;
    }
    ButtonEvent ev = ButtonEvent::None;
    if (pressedRaw != stable_ && nowMs - changeMs_ >= debounceMs_) {
        stable_ = pressedRaw;
        if (stable_) {
            downMs_ = nowMs;
            longSent_ = false;
        } else if (!longSent_) {
            ev = ButtonEvent::Short;
        }
    }
    if (stable_ && !longSent_ && nowMs - downMs_ >= longMs_) {
        longSent_ = true;
        ev = ButtonEvent::Long;
    }
    return ev;
}
