// level_store.cpp — leitura e gravação da calibração de montagem na NVS. Um registro completo
// (versão e dados) numa única chave: a NVS grava cada chave de forma atômica.
#include "level_store.h"
#include <Preferences.h>
#include <math.h>

static const char *NAMESPACE = "att_level";
static const char *KEY = "mntRec";
// 1: sem a calibração do acelerômetro; 2: rotação medida sobre os eixos nominais da montagem 5
// (chipToBody anterior). Os dois são descontinuados: a rotação só vale para os eixos nominais atuais.
static const uint8_t FORMAT_VERSION = 3;
static const float ROTATION_TOL = 1e-3f;

struct MountRecord {
    uint8_t  version;
    uint8_t  reserved[3];
    Mat3     r;
    float    mountRollDeg;
    float    mountPitchDeg;
    float    tempC;
    uint8_t  accelCalValid;
    uint8_t  reserved2[3];
    AccelCal accelCal;
};

// Margem sobre o limite da calibração: a rotação gravada é a da inclinação aceita, sem folga numérica
static const float TILT_MARGIN_DEG = 0.5f;

static bool plausible(const MountRecord &r, float maxTiltDeg) {
    if (!mat3IsRotation(r.r, ROTATION_TOL)) return false;
    if (!(mat3RotationAngleDeg(r.r) <= maxTiltDeg + TILT_MARGIN_DEG)) return false;   // NaN também cai aqui
    return isfinite(r.mountRollDeg) && isfinite(r.mountPitchDeg) && isfinite(r.tempC);
}

void levelStoreLoad(LevelData &out, float maxTiltDeg) {
    out = LevelData();
    Preferences p;
    if (!p.begin(NAMESPACE, true)) return;   // namespace ainda não existe
    MountRecord rec;
    // isKey() antes de ler: ler uma chave ausente gera um log de erro da biblioteca
    const bool read = p.isKey(KEY) && p.getBytesLength(KEY) == sizeof(rec) &&
                      p.getBytes(KEY, &rec, sizeof(rec)) == sizeof(rec) && rec.version == FORMAT_VERSION;
    p.end();
    if (!read) return;
    if (!plausible(rec, maxTiltDeg)) {
        out.rejected = true;
        return;
    }
    out.valid = true;
    out.r = rec.r;
    out.mountRollDeg = rec.mountRollDeg;
    out.mountPitchDeg = rec.mountPitchDeg;
    out.tempC = rec.tempC;
    out.accelCalValid = rec.accelCalValid != 0;
    out.accelCal = rec.accelCal;
}

bool levelStoreSave(const LevelData &data) {
    if (!mat3IsRotation(data.r, ROTATION_TOL)) return false;
    Preferences p;
    if (!p.begin(NAMESPACE, false)) return false;
    const MountRecord rec = {FORMAT_VERSION, {0, 0, 0}, data.r, data.mountRollDeg, data.mountPitchDeg,
                             data.tempC, (uint8_t)data.accelCalValid, {0, 0, 0}, data.accelCal};
    const bool ok = p.putBytes(KEY, &rec, sizeof(rec)) == sizeof(rec);
    p.end();
    return ok;
}
