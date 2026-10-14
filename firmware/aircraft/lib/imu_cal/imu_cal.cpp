// imu_cal.cpp — implementação da calibração em lógica pura (sem dependência de Arduino)
#include "imu_cal.h"
#include <math.h>

float norm(const Vec3 &v) {
    return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

static float axis(const Vec3 &v, int i) {
    return i == 0 ? v.x : (i == 1 ? v.y : v.z);
}

// ---------- WindowStats ----------

void WindowStats::reset() {
    n_ = 0;
    for (int i = 0; i < 3; i++) mean_[i] = m2_[i] = 0;
}

void WindowStats::add(const Vec3 &v) {
    n_++;
    for (int i = 0; i < 3; i++) {
        const double x = axis(v, i);
        const double d = x - mean_[i];
        mean_[i] += d / n_;
        m2_[i] += d * (x - mean_[i]);
    }
}

Vec3 WindowStats::mean() const {
    return {(float)mean_[0], (float)mean_[1], (float)mean_[2]};
}

Vec3 WindowStats::stddev() const {
    if (n_ == 0) return {0, 0, 0};
    return {(float)sqrt(m2_[0] / n_), (float)sqrt(m2_[1] / n_), (float)sqrt(m2_[2] / n_)};
}

float WindowStats::maxStddev() const {
    const Vec3 s = stddev();
    return fmaxf(s.x, fmaxf(s.y, s.z));
}

// ---------- Acelerômetro ----------

Vec3 applyAccelCal(const AccelCal &cal, const Vec3 &raw) {
    return {(raw.x - cal.offset.x) / cal.scale.x,
            (raw.y - cal.offset.y) / cal.scale.y,
            (raw.z - cal.offset.z) / cal.scale.z};
}

// Eixo (0 = X, 1 = Y, 2 = Z) e sinal que a posição aponta para cima
static void poseAxis(Pose p, int &ax, float &sign) {
    switch (p) {
    case Pose::ZUp:   ax = 2; sign = +1; break;
    case Pose::ZDown: ax = 2; sign = -1; break;
    case Pose::XUp:   ax = 0; sign = +1; break;
    case Pose::XDown: ax = 0; sign = -1; break;
    case Pose::YUp:   ax = 1; sign = +1; break;
    default:          ax = 1; sign = -1; break;   // YDown
    }
}

// Mapeamento da montagem final (docs/hardware.md, 14/10/2026): X+ asa direita, Y+ nariz, Z+ para cima.
// Cada posição aponta um eixo do chip para cima.
const char *poseName(Pose p) {
    switch (p) {
    case Pose::ZUp:   return "nivelada, face para cima";
    case Pose::ZDown: return "nivelada, face para baixo";
    case Pose::XUp:   return "asa esquerda para baixo";
    case Pose::XDown: return "asa direita para baixo";
    case Pose::YUp:   return "nariz para cima";
    case Pose::YDown: return "nariz para baixo";
    default:          return "?";
    }
}

PoseCheck checkPose(Pose p, const Vec3 &mean, const PoseLimits &lim) {
    int ax;
    float sign;
    poseAxis(p, ax, sign);
    // O eixo esperado precisa ser o de maior módulo e ter o sinal da posição
    int dom = 0;
    for (int i = 1; i < 3; i++)
        if (fabsf(axis(mean, i)) > fabsf(axis(mean, dom))) dom = i;
    const float v = sign * axis(mean, ax);
    if (dom != ax || v <= 0) return PoseCheck::WrongAxis;
    if (v < lim.dominantMinG || v > lim.dominantMaxG) return PoseCheck::WeakAxis;
    return PoseCheck::Ok;
}

bool sixPositionCal(const Vec3 means[POSE_COUNT], AccelCal &out) {
    const Pose up[3] = {Pose::XUp, Pose::YUp, Pose::ZUp};
    const Pose down[3] = {Pose::XDown, Pose::YDown, Pose::ZDown};
    float off[3], sc[3];
    for (int i = 0; i < 3; i++) {
        const float aUp = axis(means[(int)up[i]], i);
        const float aDown = axis(means[(int)down[i]], i);
        off[i] = (aUp + aDown) / 2;
        sc[i] = (aUp - aDown) / 2;
        if (!(sc[i] > ACCEL_SCALE_MIN && sc[i] < ACCEL_SCALE_MAX)) return false;
    }
    out.offset = {off[0], off[1], off[2]};
    out.scale = {sc[0], sc[1], sc[2]};
    return true;
}

bool validateAccelCal(const AccelCal &cal, const Vec3 means[POSE_COUNT], float minG, float maxG,
                      float normsOut[POSE_COUNT]) {
    bool ok = true;
    for (int i = 0; i < POSE_COUNT; i++) {
        normsOut[i] = norm(applyAccelCal(cal, means[i]));
        if (!(normsOut[i] >= minG && normsOut[i] <= maxG)) ok = false;
    }
    return ok;
}

static bool finite3(const Vec3 &v) {
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

// |v| ≤ max em cada eixo (falso para NaN)
static bool within3(const Vec3 &v, float max) {
    return fabsf(v.x) <= max && fabsf(v.y) <= max && fabsf(v.z) <= max;
}

bool accelCalPlausible(const AccelCal &cal, float offsetMaxG) {
    if (!finite3(cal.offset) || !finite3(cal.scale) || !within3(cal.offset, offsetMaxG)) return false;
    const float sc[3] = {cal.scale.x, cal.scale.y, cal.scale.z};
    for (float v : sc)
        if (!(v > ACCEL_SCALE_MIN && v < ACCEL_SCALE_MAX)) return false;
    return true;
}

bool sameAccelCal(bool aValid, const AccelCal &a, bool bValid, const AccelCal &b) {
    if (aValid != bValid) return false;
    if (!aValid) return true;
    return a.offset.x == b.offset.x && a.offset.y == b.offset.y && a.offset.z == b.offset.z &&
           a.scale.x == b.scale.x && a.scale.y == b.scale.y && a.scale.z == b.scale.z;
}

// ---------- Giroscópio ----------

GyroBiasResult evaluateGyroWindow(const WindowStats &gyro, const WindowStats &accCorrected,
                                  uint32_t minSamples, const RestLimits &lim) {
    GyroBiasResult r;
    r.bias = gyro.mean();
    r.gyroMaxStd = gyro.maxStddev();
    r.accMean = accCorrected.mean();
    r.accNorm = norm(r.accMean);
    if (gyro.count() < minSamples || accCorrected.count() < minSamples)
        r.status = RestCheck::TooFewSamples;
    else if (!finite3(r.bias) || !finite3(gyro.stddev()) || !finite3(r.accMean) || !finite3(accCorrected.stddev()))
        r.status = RestCheck::Invalid;
    else if (r.gyroMaxStd >= lim.gyroMaxStdDps)
        r.status = RestCheck::Moving;
    else if (r.accNorm < lim.accMinG || r.accNorm > lim.accMaxG)
        r.status = RestCheck::AccOutOfRange;
    else
        r.status = RestCheck::Ok;
    return r;
}

BiasConfirmer::Result BiasConfirmer::feed(const GyroBiasResult &window, float maxDeltaDps, float maxTiltDeg) {
    if (window.status != RestCheck::Ok || !finite3(window.bias) || !finite3(window.accMean)) {
        hasCandidate_ = false;
        return Result::Rejected;
    }
    if (!hasCandidate_) {
        candidate_ = window.bias;
        candidateAcc_ = window.accMean;
        hasCandidate_ = true;
        return Result::Candidate;
    }
    const Vec3 &b = window.bias;
    lastDelta_ = fmaxf(fabsf(b.x - candidate_.x), fmaxf(fabsf(b.y - candidate_.y), fabsf(b.z - candidate_.z)));
    lastTiltDeg_ = angleBetweenDeg(window.accMean, candidateAcc_);
    if (!(lastDelta_ <= maxDeltaDps && lastTiltDeg_ <= maxTiltDeg)) {   // NaN também cai aqui
        candidate_ = b;
        candidateAcc_ = window.accMean;
        return Result::Inconsistent;
    }
    accepted_ = {(b.x + candidate_.x) / 2, (b.y + candidate_.y) / 2, (b.z + candidate_.z) / 2};
    hasCandidate_ = false;
    return Result::Accepted;
}

Vec3 applyGyroBias(const Vec3 &raw, const Vec3 &bias) {
    return {raw.x - bias.x, raw.y - bias.y, raw.z - bias.z};
}

float angleBetweenDeg(const Vec3 &a, const Vec3 &b) {
    if (!(norm(a) > 0 && norm(b) > 0)) return NAN;   // também falso para NaN
    const Vec3 c = {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    const float dot = a.x * b.x + a.y * b.y + a.z * b.z;
    return atan2f(norm(c), dot) * 180.0f / (float)M_PI;
}

bool gyroBiasPlausible(const Vec3 &bias, float maxDps) {
    return finite3(bias) && within3(bias, maxDps);
}

bool tempDrifted(float calTempC, float nowTempC, float maxDeltaC) {
    return fabsf(nowTempC - calTempC) > maxDeltaC;
}
