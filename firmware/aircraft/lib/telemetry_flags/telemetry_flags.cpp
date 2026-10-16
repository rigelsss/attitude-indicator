// telemetry_flags.cpp — ver telemetry_flags.h
#include "telemetry_flags.h"

uint16_t telemetryValidFlags(const ValidityInputs &in, const ValidityLimits &lim) {
    uint16_t v = 0;
    const bool att = in.attInitialized && in.attAgeMs <= lim.attMaxAgeMs;
    if (att) {
        v |= VAL_ATT;
        if (in.attDegraded) v |= VAL_ATT_DEGRADED;
    }
    if (in.baroOk && in.baroZeroValid && in.altFinite && in.baroAgeMs <= lim.baroMaxAgeMs) v |= VAL_ALT;
    if (att && in.hdgInitialized && in.magCalibrated) {
        v |= VAL_HDG;
        // Atitude degradada suspende a correção (magnetometro.md, D6) já agora, e não só na próxima
        // leitura do magnetômetro (até 40 ms depois), quando hdgMagSuspended passaria a dizer isso
        if (in.attDegraded || in.hdgMagSuspended || in.magAgeMs > lim.magMaxAgeMs) v |= VAL_HDG_GYRO_ONLY;
    }
    return v;
}
