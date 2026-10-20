// baro.cpp — ver baro.h
#include "baro.h"
#include <math.h>

static uint16_t u16le(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }

bool bme280ParseCalib(const uint8_t raw[BME280_CALIB_TP_LEN], Bme280Calib &out) {
    Bme280Calib c;
    c.T1 = u16le(raw + 0);
    c.T2 = (int16_t)u16le(raw + 2);
    c.T3 = (int16_t)u16le(raw + 4);
    c.P1 = u16le(raw + 6);
    c.P2 = (int16_t)u16le(raw + 8);
    c.P3 = (int16_t)u16le(raw + 10);
    c.P4 = (int16_t)u16le(raw + 12);
    c.P5 = (int16_t)u16le(raw + 14);
    c.P6 = (int16_t)u16le(raw + 16);
    c.P7 = (int16_t)u16le(raw + 18);
    c.P8 = (int16_t)u16le(raw + 20);
    c.P9 = (int16_t)u16le(raw + 22);
    if (c.T1 == 0 || c.P1 == 0) return false;
    out = c;
    return true;
}

void bme280ParseAdc(const uint8_t raw[6], int32_t &adcP, int32_t &adcT) {
    adcP = ((int32_t)raw[0] << 12) | ((int32_t)raw[1] << 4) | (raw[2] >> 4);
    adcT = ((int32_t)raw[3] << 12) | ((int32_t)raw[4] << 4) | (raw[5] >> 4);
}

// Fórmulas inteiras do datasheet (rev. 1.1 do código): temperatura em 0,01 °C, pressão em Pa no
// formato Q24.8. Os deslocamentos à esquerda de valores com sinal viraram multiplicações (mesmo
// resultado, sem comportamento indefinido); os deslocamentos à direita seguem o código da Bosch.
BaroComp bme280Compensate(const Bme280Calib &cal, int32_t adcT, int32_t adcP, float &tempC, float &pressHpa) {
    if (adcT == BME280_ADC_SKIPPED || adcP == BME280_ADC_SKIPPED) return BaroComp::Skipped;

    const int32_t dT = (adcT >> 4) - (int32_t)cal.T1;
    const int32_t v1t = (((adcT >> 3) - ((int32_t)cal.T1 * 2)) * (int32_t)cal.T2) >> 11;
    const int32_t v2t = (((dT * dT) >> 12) * (int32_t)cal.T3) >> 14;
    const int32_t tFine = v1t + v2t;
    const int32_t t100 = (tFine * 5 + 128) >> 8;

    int64_t var1 = (int64_t)tFine - 128000;
    int64_t var2 = var1 * var1 * (int64_t)cal.P6;
    var2 = var2 + var1 * (int64_t)cal.P5 * 131072;             // << 17
    var2 = var2 + (int64_t)cal.P4 * 34359738368LL;              // << 35
    var1 = ((var1 * var1 * (int64_t)cal.P3) >> 8) + var1 * (int64_t)cal.P2 * 4096;   // << 12
    var1 = ((140737488355328LL + var1) * (int64_t)cal.P1) >> 33;                     // 1 << 47
    if (var1 == 0) return BaroComp::Invalid;
    int64_t p = 1048576 - adcP;
    p = ((p * 2147483648LL - var2) * 3125) / var1;               // << 31
    var1 = ((int64_t)cal.P9 * (p >> 13) * (p >> 13)) >> 25;
    var2 = ((int64_t)cal.P8 * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (int64_t)cal.P7 * 16;          // << 4

    tempC = t100 / 100.0f;
    pressHpa = (float)(p / 256.0 / 100.0);
    return BaroComp::Ok;
}

float baroAltitudeM(float pressHpa, float refHpa) {
    if (!(isfinite(pressHpa) && pressHpa > 0 && isfinite(refHpa) && refHpa > 0)) return NAN;
    return (float)(44330.0 * (1.0 - pow((double)pressHpa / refHpa, 0.1903)));
}

void ScalarStats::reset() {
    n_ = 0;
    mean_ = 0;
    m2_ = 0;
}

void ScalarStats::add(double v) {
    n_++;
    const double d = v - mean_;
    mean_ += d / n_;
    m2_ += d * (v - mean_);
}

double ScalarStats::stddev() const { return n_ > 0 ? sqrt(m2_ / n_) : 0.0; }

BaroZeroResult evaluateZeroWindow(const ScalarStats &pressPa, const ScalarStats &tempC, uint32_t minSamples,
                                  float maxStdPa, float minHpa, float maxHpa) {
    BaroZeroResult r;
    r.refHpa = (float)(pressPa.mean() / 100.0);
    r.stdPa = (float)pressPa.stddev();
    r.tempC = (float)tempC.mean();
    if (pressPa.count() < minSamples) r.status = BaroZeroCheck::TooFewSamples;
    else if (!isfinite(r.refHpa) || !isfinite(r.stdPa) || !isfinite(r.tempC)) r.status = BaroZeroCheck::Invalid;
    else if (r.refHpa < minHpa || r.refHpa > maxHpa) r.status = BaroZeroCheck::OutOfRange;
    else if (r.stdPa > maxStdPa) r.status = BaroZeroCheck::Unstable;
    else r.status = BaroZeroCheck::Ok;
    return r;
}

void BaroZero::start(bool settle, uint32_t nowMs) {
    state_ = settle ? State::Settling : State::Measuring;
    untilMs_ = nowMs + p_.settleMs;
    restartWindow(nowMs);
}

void BaroZero::restartWindow(uint32_t tMs) {
    startMs_ = tMs;
    press_.reset();
    temp_.reset();
}

BaroZero::Event BaroZero::feed(float pressHpa, float tempC, uint32_t tMs) {
    if (state_ == State::Off) return Event::None;
    if (state_ == State::Settling) {
        if ((int32_t)(tMs - untilMs_) < 0) return Event::None;
        state_ = State::Measuring;
        restartWindow(tMs);
    }
    press_.add(pressHpa * 100.0);   // Pa: o limite de desvio está em Pa
    temp_.add(tempC);
    if (tMs - startMs_ < p_.windowMs) return Event::None;

    result_ = evaluateZeroWindow(press_, temp_, p_.minSamples, p_.maxStdPa, p_.minHpa, p_.maxHpa);
    count_ = press_.count();
    if (result_.status == BaroZeroCheck::Ok) {
        refValid_ = true;
        refHpa_ = result_.refHpa;
        refTempC_ = result_.tempC;
        state_ = State::Off;
        return Event::Accepted;
    }
    restartWindow(tMs);
    return Event::Rejected;
}
