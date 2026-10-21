// telemetry_pack.cpp — ver telemetry_pack.h
#include "telemetry_pack.h"
#include <math.h>
#include <string.h>

// Idade em ms a partir de instantes de 64 bits; um instante no futuro (não deveria acontecer) dá 0, e uma idade
// que não cabe em 32 bits satura logo abaixo de AGE_NEVER (que quer dizer "sem amostra")
static uint32_t ageMs64(uint64_t fromUs, uint64_t nowUs) {
    if (nowUs <= fromUs) return 0;
    const uint64_t ms = (nowUs - fromUs) / 1000ULL;
    return ms >= AGE_NEVER ? AGE_NEVER - 1 : (uint32_t)ms;
}

ValidityInputs validityAt(const TelemetryState &s, uint32_t nowMs, uint64_t nowUs64) {
    ValidityInputs in;
    in.attInitialized = s.attInitialized;
    in.attDegraded = s.attDegraded;
    in.attAgeMs = s.haveAttSample ? ageMs64(s.attSampleUs64, nowUs64) : AGE_NEVER;
    in.baroOk = s.baroOk;
    in.baroZeroValid = s.baroZeroValid;
    in.altFinite = isfinite(s.altitudeM);
    in.baroAgeMs = s.haveBaro ? nowMs - s.baroMs : AGE_NEVER;
    in.hdgInitialized = s.hdgInitialized;
    in.magCalibrated = s.magCalibrated;
    in.hdgMagSuspended = s.hdgMagSuspended;
    in.magAgeMs = s.haveMag ? nowMs - s.magMs : AGE_NEVER;
    return in;
}

uint16_t telemetryStatusFlags(const TelemetryState &s) {
    uint16_t f = 0;
    if (s.mpuOk) f |= ST_MPU_OK;
    if (s.magOk) f |= ST_MAG_OK;
    if (s.baroOk) f |= ST_BARO_OK;
    if (s.calibrating) f |= ST_CALIBRATING;
    if (s.gyroCalibrated) f |= ST_GYRO_CAL;
    if (s.magCalibrated) f |= ST_MAG_CAL;
    return f;
}

TelemetryPacket buildTelemetry(const TelemetryState &s, uint32_t nowMs, uint64_t nowUs64, uint16_t seq,
                               const ValidityLimits &lim) {
    TelemetryPacket p;
    memset(&p, 0, sizeof(p));   // reservas em 0
    p.hdr.magic = TELEMETRY_MAGIC;
    p.hdr.version = TELEMETRY_VERSION;
    p.hdr.type = MSG_TELEMETRY;
    p.hdr.seq = seq;
    p.t_ms = nowMs;
    p.roll = s.rollDeg;
    p.pitch = s.pitchDeg;
    p.heading = s.headingDeg;
    p.gx = s.gx;
    p.gy = s.gy;
    p.gz = s.gz;
    p.altitude_m = s.altitudeM;
    p.pressure_hpa = s.pressureHpa;
    p.temp_c = s.tempC;
    p.status = telemetryStatusFlags(s);
    p.valid = telemetryValidFlags(validityAt(s, nowMs, nowUs64), lim);
    p.loop_us = s.loopUs > UINT16_MAX ? UINT16_MAX : (uint16_t)s.loopUs;
    return p;
}

CmdDecode decodeCommand(const uint8_t *data, size_t len, CommandPacket &out) {
    if (data == nullptr || len != sizeof(CommandPacket)) return CmdDecode::BadSize;
    memcpy(&out, data, sizeof(CommandPacket));
    if (out.hdr.magic != TELEMETRY_MAGIC) return CmdDecode::BadMagic;
    if (out.hdr.version != TELEMETRY_VERSION) return CmdDecode::BadVersion;
    if (out.hdr.type != MSG_COMMAND) return CmdDecode::BadType;
    return CmdDecode::Ok;
}

const char *cmdDecodeText(CmdDecode r) {
    switch (r) {
    case CmdDecode::Ok:         return "ok";
    case CmdDecode::BadSize:    return "tamanho errado";
    case CmdDecode::BadMagic:   return "magic errado";
    case CmdDecode::BadVersion: return "outra versão do contrato";
    case CmdDecode::BadType:    return "não é comando";
    }
    return "?";
}
