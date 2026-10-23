// alarm_logic.cpp — ver alarm_logic.h
#include "alarm_logic.h"
#include <math.h>

const char *alarmKindText(AlarmKind k) {
    switch (k) {
    case AlarmKind::None:            return "nenhum";
    case AlarmKind::BankCaution:     return "AVISO DE BANK";
    case AlarmKind::AttitudeWarning: return "ATITUDE ANORMAL";
    case AlarmKind::AttInvalid:      return "ATITUDE INVÁLIDA";
    case AlarmKind::LinkLost:        return "LINK PERDIDO";
    }
    return "?";
}

const char *alarmKindShort(AlarmKind k) {
    switch (k) {
    case AlarmKind::None:            return "nenhum";
    case AlarmKind::BankCaution:     return "AVISO BANK";
    case AlarmKind::AttitudeWarning: return "ATIT ANORMAL";
    case AlarmKind::AttInvalid:      return "ATIT INVALIDA";
    case AlarmKind::LinkLost:        return "LINK PERDIDO";
    }
    return "?";
}

AlarmLogic::AlarmLogic(const AlarmConfig &cfg) : cfg_(cfg) {}

AlarmOutput AlarmLogic::update(const AlarmInputs &in, uint32_t nowMs) {
    // Ângulo não finito com VAL_ATT ligada é pacote incoerente: conta como atitude inválida
    const bool attValid = in.attValid && isfinite(in.rollDeg) && isfinite(in.pitchDeg);
    const bool attUsable = in.linkOk && attValid;
    if (attUsable) {
        const float bank = fabsf(in.rollDeg), pitch = in.pitchDeg;
        const float h = cfg_.hystDeg;
        if (!warning_)
            warning_ = bank > cfg_.bankWarningDeg || pitch > cfg_.pitchUpDeg || pitch < -cfg_.pitchDownDeg;
        else
            warning_ = !(bank < cfg_.bankWarningDeg - h && pitch < cfg_.pitchUpDeg - h &&
                         pitch > -(cfg_.pitchDownDeg - h));
        caution_ = caution_ ? bank >= cfg_.bankCautionDeg - h : bank > cfg_.bankCautionDeg;
    } else {
        // Sem atitude utilizável, a avaliação recomeça do zero quando ela voltar
        warning_ = caution_ = false;
    }

    // Atitude inválida: conta o tempo só com o link OK e fora das calibrações
    bool invalid = false;
    if (in.linkOk && !attValid && !in.calibrating) {
        if (!invalidTiming_) {
            invalidTiming_ = true;
            invalidSinceMs_ = nowMs;
        }
        invalid = nowMs - invalidSinceMs_ >= cfg_.invalidDelayMs;
    } else {
        invalidTiming_ = false;
    }

    AlarmKind k = AlarmKind::None;
    if (in.everLinked && !in.linkOk) k = AlarmKind::LinkLost;
    else if (invalid) k = AlarmKind::AttInvalid;
    else if (warning_) k = AlarmKind::AttitudeWarning;
    else if (caution_) k = AlarmKind::BankCaution;
    if (k != kind_) {
        kind_ = k;
        kindSinceMs_ = nowMs;   // a fase das piscadas conta daqui: um alarme novo acende na hora
        acked_ = false;         // condição nova (ou terminada): o próximo alarme volta a piscar
    }

    AlarmOutput out;
    out.kind = kind_;
    out.acked = acked_;
    out.alarmLed = ledOn(nowMs);
    out.linkLed = in.linkOk;
    return out;
}

bool AlarmLogic::acknowledge() {
    if (kind_ == AlarmKind::None) return false;
    acked_ = true;
    return true;
}

bool AlarmLogic::ledOn(uint32_t nowMs) const {
    if (kind_ == AlarmKind::None) return false;
    if (acked_) return true;
    const uint32_t phase = nowMs - kindSinceMs_;   // começa aceso
    uint32_t half = 0;
    switch (kind_) {
    case AlarmKind::LinkLost:        half = cfg_.linkBlinkMs; break;
    case AlarmKind::BankCaution:     half = cfg_.cautionBlinkMs; break;
    case AlarmKind::AttitudeWarning: half = cfg_.warningBlinkMs; break;
    case AlarmKind::AttInvalid: {
        // Duas piscadas curtas no começo de cada período
        const uint32_t t = phase % cfg_.doublePeriodMs, on = cfg_.doubleOnMs;
        return t < on || (t >= 2 * on && t < 3 * on);
    }
    case AlarmKind::None: return false;
    }
    return half > 0 && (phase / half) % 2 == 0;
}
