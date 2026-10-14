// mag_store.cpp — leitura e gravação da calibração do magnetômetro na NVS. Um registro completo
// (versão, ganho, mapeamento de eixos e coeficientes) numa única chave: a NVS grava cada chave de forma atômica.
#include "mag_store.h"
#include <Preferences.h>
#include <math.h>

static const char *NAMESPACE = "mag_cal";
static const char *KEY = "calRec";
static const uint8_t FORMAT_VERSION = 2;   // 1: sem o mapeamento de eixos (descontinuado)
static const float OFFSET_MAX_UT = 1000.0f;   // acima disto, registro corrompido (faixa máxima do chip: 810 µT)
static const float RADIUS_MAX_UT = 1000.0f;

struct CalRecord {
    uint8_t version;
    uint8_t crb;        // ganho do chip na calibração
    uint8_t sectors;
    uint8_t axisMap;    // MAG_AXIS_MAP_ID da calibração
    Vec3    offset;
    Mat3    w;
    float   radiusUt;
    float   residPct;
};

static bool plausible(const CalRecord &r) {
    const float o[3] = {r.offset.x, r.offset.y, r.offset.z};
    for (int i = 0; i < 3; i++) {
        if (!isfinite(o[i]) || fabsf(o[i]) > OFFSET_MAX_UT) return false;
        for (int j = 0; j < 3; j++)
            if (!isfinite(r.w.m[i][j])) return false;
        if (!(r.w.m[i][i] > 0)) return false;
    }
    return isfinite(r.radiusUt) && r.radiusUt > 0 && r.radiusUt < RADIUS_MAX_UT && isfinite(r.residPct);
}

void magStoreLoad(MagCalData &out, uint8_t currentCrb) {
    out = MagCalData();
    Preferences p;
    if (!p.begin(NAMESPACE, true)) return;   // namespace ainda não existe
    CalRecord rec;
    // isKey() antes de ler: ler uma chave ausente gera um log de erro da biblioteca
    const bool read = p.isKey(KEY) && p.getBytesLength(KEY) == sizeof(rec) &&
                      p.getBytes(KEY, &rec, sizeof(rec)) == sizeof(rec) && rec.version == FORMAT_VERSION;
    p.end();
    if (!read) return;
    if (!plausible(rec)) {
        out.rejected = true;
        return;
    }
    if (rec.axisMap != MAG_AXIS_MAP_ID) {   // eixos do modelo diferentes: offset e W não valem
        out.otherAxisMap = true;
        return;
    }
    if (rec.crb != currentCrb) {   // outro ganho: offset e raio não valem
        out.otherGain = true;
        return;
    }
    out.valid = true;
    out.cal.offset = rec.offset;
    out.cal.w = rec.w;
    out.cal.radiusUt = rec.radiusUt;
    out.residPct = rec.residPct;
    out.sectors = rec.sectors;
}

bool magStoreSave(const MagCal &cal, float residPct, uint8_t sectors, uint8_t crb) {
    const CalRecord rec = {FORMAT_VERSION, crb, sectors, MAG_AXIS_MAP_ID, cal.offset, cal.w, cal.radiusUt, residPct};
    if (!plausible(rec)) return false;
    Preferences p;
    if (!p.begin(NAMESPACE, false)) return false;
    const bool ok = p.putBytes(KEY, &rec, sizeof(rec)) == sizeof(rec);
    p.end();
    return ok;
}
