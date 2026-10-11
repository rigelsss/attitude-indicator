// imu_store.cpp — leitura e gravação na NVS. Cada calibração é um registro completo (versão,
// metadados e coeficientes) numa única chave: a NVS grava cada chave de forma atômica, então um
// reinício no meio da gravação deixa o registro antigo ou o novo, nunca uma mistura dos dois.
#include "imu_store.h"
#include <Preferences.h>

static const char *NAMESPACE = "imu_cal";
static const uint8_t FORMAT_VERSION = 2;   // 1: chaves separadas (descontinuado)

struct AccelRecord {
    uint8_t  version;
    uint8_t  rangeG;
    uint8_t  reserved[2];
    AccelCal cal;
    float    tempC;
};

struct GyroRecord {
    uint8_t version;
    uint8_t reserved[3];
    Vec3    bias;
    float   tempC;
};

// Lê um registro só se a chave existir com o tamanho exato. isKey() antes de ler: ler uma chave
// ausente gera um log de erro da biblioteca.
template <typename T>
static bool readRecord(Preferences &p, const char *key, T &rec) {
    return p.isKey(key) && p.getBytesLength(key) == sizeof(T) && p.getBytes(key, &rec, sizeof(T)) == sizeof(T) &&
           rec.version == FORMAT_VERSION;
}

void imuStoreLoad(ImuCalData &out, uint8_t currentAccelRangeG) {
    out = ImuCalData();
    Preferences p;
    if (!p.begin(NAMESPACE, true)) return;   // namespace ainda não existe
    AccelRecord a;
    if (readRecord(p, "accRec", a)) {
        out.accelRangeG = a.rangeG;
        out.accelTempC = a.tempC;
        if (a.rangeG == currentAccelRangeG) {
            out.accel = a.cal;
            out.accelValid = true;
        }
    }
    GyroRecord g;
    if (readRecord(p, "gyrRec", g)) {
        out.gyroBias = g.bias;
        out.gyroTempC = g.tempC;
        out.gyroValid = true;
    }
    p.end();
}

// Abre para escrita e remove as chaves do formato 1, que não são mais lidas
static bool openForWrite(Preferences &p) {
    if (!p.begin(NAMESPACE, false)) return false;
    static const char *const OLD_KEYS[] = {"ver", "acc", "accT", "accRng", "gyr", "gyrT"};
    for (const char *k : OLD_KEYS)
        if (p.isKey(k)) p.remove(k);
    return true;
}

bool imuStoreSaveAccel(const AccelCal &cal, float tempC, uint8_t accelRangeG) {
    Preferences p;
    if (!openForWrite(p)) return false;
    const AccelRecord rec = {FORMAT_VERSION, accelRangeG, {0, 0}, cal, tempC};
    const bool ok = p.putBytes("accRec", &rec, sizeof(rec)) == sizeof(rec);
    p.end();
    return ok;
}

bool imuStoreSaveGyro(const Vec3 &bias, float tempC) {
    Preferences p;
    if (!openForWrite(p)) return false;
    const GyroRecord rec = {FORMAT_VERSION, {0, 0, 0}, bias, tempC};
    const bool ok = p.putBytes("gyrRec", &rec, sizeof(rec)) == sizeof(rec);
    p.end();
    return ok;
}
