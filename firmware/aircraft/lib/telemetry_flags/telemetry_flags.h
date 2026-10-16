// telemetry_flags.h — regras das flags de validade (campo valid do TelemetryPacket, ValidFlags em
// telemetry.h). Lógica pura (sem Arduino.h), testável com pio test -e native.
#pragma once
#include <stdint.h>
#include "telemetry.h"

// Idade "sem leitura": nenhuma amostra desde o boot (ou desde o último reinício do sensor)
static const uint32_t AGE_NEVER = UINT32_MAX;

// Fatos do instante em que o pacote é montado
struct ValidityInputs {
    bool     attInitialized;     // AttitudeFilter::initialized()
    bool     attDegraded;        // AttitudeFilter::degraded()
    uint32_t attAgeMs;           // desde a última amostra do MPU usada na fusão
    bool     baroOk;             // barômetro configurado e respondendo
    bool     baroZeroValid;      // há um zero (p₀)
    bool     altFinite;          // baroAltitudeM() finita
    uint32_t baroAgeMs;          // desde a última leitura válida do barômetro
    bool     hdgInitialized;     // HeadingFilter::initialized()
    bool     magCalibrated;      // há calibração do magnetômetro carregada (sem ela, VAL_HDG desliga)
    bool     hdgMagSuspended;    // última correção da proa suspensa (distúrbio, inclinação alta, atitude degradada)
    uint32_t magAgeMs;           // desde a última leitura válida do magnetômetro
};

// Idade máxima de cada sensor para o dado valer (config.h)
struct ValidityLimits {
    uint32_t attMaxAgeMs;
    uint32_t baroMaxAgeMs;
    uint32_t magMaxAgeMs;
};

// Monta o campo valid. VAL_ATT_DEGRADED só com VAL_ATT, VAL_HDG só com VAL_ATT e calibração, e VAL_HDG_GYRO_ONLY
// só com VAL_HDG: sem o dado, o qualificador não tem sentido.
uint16_t telemetryValidFlags(const ValidityInputs &in, const ValidityLimits &lim);
