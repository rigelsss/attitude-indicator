// imu_store.h — persistência da calibração do MPU6050 na NVS (Preferences, namespace "imu_cal").
// Guarda os coeficientes em unidades físicas, com a temperatura do chip no momento da calibração.
#pragma once
#include "imu_cal.h"

struct ImuCalData {
    bool     accelValid = false;   // coeficientes de seis posições presentes e na faixa atual
    AccelCal accel;
    float    accelTempC = 0;
    uint8_t  accelRangeG = 0;      // faixa do acelerômetro usada na calibração
    bool     gyroValid = false;    // existe um bias salvo (de um boot ou comando anterior)
    Vec3     gyroBias{0, 0, 0};
    float    gyroTempC = 0;
    bool     accelRejected = false;   // registro lido, mas com valores impossíveis (ignorado)
    bool     gyroRejected = false;
};

// Carrega o que houver. Itens ausentes, de outra versão do formato ou de outra faixa do
// acelerômetro voltam como inválidos (accelValid/gyroValid = false) com valores neutros.
// Registros com valores impossíveis (não finitos, escala fora de ACCEL_SCALE_MIN..MAX, offset acima
// de accOffsetMaxG, bias acima de gyroBiasMaxDps) também, com accelRejected/gyroRejected = true.
// Cada gravação substitui o registro inteiro de uma vez (atômico na NVS).
void imuStoreLoad(ImuCalData &out, uint8_t currentAccelRangeG, float accOffsetMaxG, float gyroBiasMaxDps);

bool imuStoreSaveAccel(const AccelCal &cal, float tempC, uint8_t accelRangeG);
bool imuStoreSaveGyro(const Vec3 &bias, float tempC);
