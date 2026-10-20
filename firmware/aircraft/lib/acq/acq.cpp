// acq.cpp — contabilidade da aquisição (lógica pura). Ver acq.h.
#include "acq.h"
#include <math.h>

// ---------- SampleWindow ----------

void SampleWindow::add(const Vec3 &acc, const Vec3 &gyro, float tempC, bool accSat, bool gyroSat) {
    const float v[6] = {acc.x, acc.y, acc.z, gyro.x, gyro.y, gyro.z};
    for (int i = 0; i < 6; i++) {
        sum[i] += v[i];
        sq[i] += (double)v[i] * v[i];
    }
    for (int i = 3; i < 6; i++) peakGyro = fmaxf(peakGyro, fabsf(v[i]));
    tempSum += tempC;
    if (accSat) satAcc++;
    if (gyroSat) satGyro++;
    samples++;
}

void SampleWindow::addAttitude(float rollDeg, float pitchDeg) {
    att[0].add(rollDeg);
    att[1].add(pitchDeg);
}

Vec3 SampleWindow::meanAcc() const {
    return {(float)(sum[0] / samples), (float)(sum[1] / samples), (float)(sum[2] / samples)};
}

Vec3 SampleWindow::meanGyro() const {
    return {(float)(sum[3] / samples), (float)(sum[4] / samples), (float)(sum[5] / samples)};
}

WindowSummary summarize(const SampleWindow &w, uint32_t windowUs) {
    WindowSummary s = {w.pulses * 1e6f / windowUs, {0, 0, 0, 0, 0, 0}, 0, 0, 0, 0};
    const uint32_t n = w.samples;
    if (n == 0) return s;
    double sd[6];
    for (int i = 0; i < 6; i++) {
        s.mean[i] = w.sum[i] / n;
        sd[i] = sqrt(fmax(0.0, w.sq[i] / n - s.mean[i] * s.mean[i]));
    }
    s.accNorm = sqrt(s.mean[0] * s.mean[0] + s.mean[1] * s.mean[1] + s.mean[2] * s.mean[2]);
    s.accMaxStdG = fmax(sd[0], fmax(sd[1], sd[2]));
    s.gyroMaxStdDps = fmax(sd[3], fmax(sd[4], sd[5]));
    s.tempC = w.tempSum / n;
    return s;
}

// ---------- PulseTracker ----------

void PulseTracker::begin(uint32_t nowUs) {
    cur_ = SampleWindow();
    cur_.startUs = nowUs;
    havePrev_ = false;
}

void PulseTracker::rotate() {
    last_ = cur_;
    const uint32_t next = cur_.startUs + windowUs_;
    cur_ = SampleWindow();
    cur_.startUs = next;
}

void PulseTracker::notePulse(uint32_t tInt, float periodUs) {
    if ((int32_t)(tInt - cur_.startUs) >= 0) cur_.pulses++;
    if (havePrev_) {
        const uint32_t dt = tInt - lastIntUs_;
        if (dt > 1.5f * periodUs) cur_.lost += (uint32_t)lroundf(dt / periodUs) - 1;
    }
    lastIntUs_ = tInt;
    havePrev_ = true;
}

// ---------- Validade temporal ----------

FrameCheck checkFrame(uint32_t pulseUs, uint32_t sampleUs, bool newerPulse, uint32_t maxLagUs) {
    const uint32_t lag = sampleUs - pulseUs;   // aritmética módulo 2³²
    return {!newerPulse && lag <= maxLagUs, lag};
}
