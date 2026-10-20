// fusion.cpp — ver fusion.h
#include "fusion.h"
#include <math.h>

FusionCore::FusionCore(const FusionParams &fp, const HeadingParams &hp)
    : att_(fp), gyroOnly_(fp, false), hdg_(hp) {}

void FusionCore::reset() {
    att_.reset();
    gyroOnly_.reset();
    hdg_.reset();   // a proa depende da atitude
    prevValid_ = false;
}

AttitudeFilter::Step FusionCore::update(const Vec3 &acc, const Vec3 &gyro, const ImuFlags &flags, uint32_t intUs,
                                        bool headingHold) {
    const float dt = prevValid_ ? elapsedUs(prevIntUs_, intUs) * 1e-6f : 0.0f;
    prevIntUs_ = intUs;
    prevValid_ = true;

    const uint32_t t0 = clock_ ? clock_() : 0;
    const AttitudeFilter::Step st = att_.update(acc, gyro, dt, flags);
    const uint32_t cost = clock_ ? clock_() - t0 : 0;
    if (st == AttitudeFilter::Step::Reset) stats_.resets++;
    // Diagnóstico só de giroscópio: parte da atitude do filtro e depois só integra
    if (gyroOnly_.initialized()) gyroOnly_.update(acc, gyro, dt, flags);
    else if (att_.initialized()) gyroOnly_.initFrom(att_.rollDeg(), att_.pitchDeg());
    // Proa: predição pelo giroscópio a cada amostra; sem atitude (ou na coleta da calibração), recomeça
    if (att_.initialized() && !headingHold) hdg_.predict(gyro, att_.rollDeg(), att_.pitchDeg(), dt);
    else hdg_.reset();

    if (st == AttitudeFilter::Step::Updated) {
        stats_.n++;
        stats_.dtSum += dt;
        stats_.dtMin = fminf(stats_.dtMin, dt);
        stats_.dtMax = fmaxf(stats_.dtMax, dt);
        stats_.costSumUs += cost;
        if (cost > stats_.costMaxUs) stats_.costMaxUs = cost;
    }
    return st;
}
