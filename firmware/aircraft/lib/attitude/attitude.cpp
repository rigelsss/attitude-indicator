// attitude.cpp — filtro complementar de roll e pitch em lógica pura (sem dependência de Arduino)
#include "attitude.h"
#include <math.h>

static const float PI_F = (float)M_PI;
static const float DEG2RAD = PI_F / 180.0f;
static const float RAD2DEG = 180.0f / PI_F;
// Guarda numérica da cinemática de Euler: tanθ limitado ao valor de |θ| = 85°. Não remove a
// singularidade em 90°; a faixa validada é |pitch| ≤ 70° (docs/fusao.md).
static const float COS_PITCH_MIN = 0.0871557f;   // cos 85°

static bool finite3(const Vec3 &v) {
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

static float wrapPi(float a) {
    return atan2f(sinf(a), cosf(a));
}

static Vec3 scaled(const Vec3 &v, float k) {
    return {v.x * k, v.y * k, v.z * k};
}

static float dot(const Vec3 &a, const Vec3 &b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static Vec3 cross(const Vec3 &a, const Vec3 &b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Vec3 chipToBody(const Vec3 &chip) {
    return {chip.x, -chip.y, -chip.z};
}

uint32_t elapsedUs(uint32_t fromUs, uint32_t toUs) {
    return toUs - fromUs;   // aritmética módulo 2³²
}

float wrap180(float deg) {
    float r = fmodf(deg, 360.0f);
    if (r > 180.0f) r -= 360.0f;
    else if (r <= -180.0f) r += 360.0f;
    return r;
}

Angles accelRollPitch(const Vec3 &f) {
    return {atan2f(-f.y, -f.z) * RAD2DEG, atan2f(f.x, sqrtf(f.y * f.y + f.z * f.z)) * RAD2DEG};
}

float accWeight(float accNormG, bool accSaturated, const FusionParams &p) {
    if (accSaturated || !isfinite(accNormG)) return 0.0f;
    const float dev = fabsf(accNormG - 1.0f);
    if (dev <= p.accFullDevG) return 1.0f;
    if (dev >= p.accZeroDevG) return 0.0f;
    return (p.accZeroDevG - dev) / (p.accZeroDevG - p.accFullDevG);
}

// ---------- AttitudeFilter ----------

void AttitudeFilter::reset() {
    init_ = false;
    roll_ = pitch_ = 0;
    degraded_ = false;
    corrSinceSatS_ = 0;
    initN_ = 0;
}

void AttitudeFilter::initFrom(float rollDeg, float pitchDeg) {
    roll_ = wrapPi(rollDeg * DEG2RAD);
    pitch_ = pitchDeg * DEG2RAD;
    init_ = true;
    degraded_ = false;
    corrSinceSatS_ = 0;
    initN_ = 0;
}

float AttitudeFilter::rollDeg() const {
    return roll_ * RAD2DEG;
}

float AttitudeFilter::pitchDeg() const {
    return pitch_ * RAD2DEG;
}

AttitudeFilter::Step AttitudeFilter::update(const Vec3 &acc, const Vec3 &gyro, float dtS, const ImuFlags &flags) {
    if (!finite3(acc) || !finite3(gyro) || !isfinite(dtS)) {
        if (!init_) initN_ = 0;   // a média parcial não atravessa um dado inválido
        return Step::BadData;
    }

    const float w = useAccel_ ? accWeight(norm(acc), flags.accSaturated, p_) : 0.0f;
    lastW_ = w;
    const float proj = sqrtf(acc.y * acc.y + acc.z * acc.z);
    const bool rollObservable = proj >= p_.rollMinG;

    // Inicialização pela média do acelerômetro em initSamples amostras seguidas, todas com uma
    // referência completa: peso pleno, roll observável (uma amostra vertical pode ter |a| = 1 g sem
    // dar roll), giroscópio não saturado e caixa sem girar. Uma amostra só carregaria o ruído do
    // acelerômetro (~0,1°) para a atitude inicial, que levaria ~3τ para sair.
    if (!init_) {
        const bool usable = w >= 1.0f && rollObservable && !flags.gyroSaturated && norm(gyro) <= p_.initMaxRateDps;
        if (!usable) {
            initN_ = 0;   // a média só vale para amostras seguidas
            return Step::Waiting;
        }
        // A primeira amostra da média entra com qualquer dt (depois de um reset ele é 0 ou indefinido).
        // Nas seguintes, dt ≤ 0 não entra, e uma lacuna acima do limite recomeça a média: a caixa
        // pode ter se movido sem que o filtro visse; a amostra atual vira a primeira da nova média.
        if (initN_ > 0) {
            if (dtS <= 0) return Step::Skipped;
            if (dtS > p_.maxDtS) initN_ = 0;
        }
        if (initN_ == 0) initSum_ = {0, 0, 0};
        initSum_ = {initSum_.x + acc.x, initSum_.y + acc.y, initSum_.z + acc.z};
        if (++initN_ < p_.initSamples) return Step::Waiting;
        const Vec3 m = scaled(initSum_, 1.0f / initN_);
        roll_ = atan2f(-m.y, -m.z);
        pitch_ = atan2f(m.x, sqrtf(m.y * m.y + m.z * m.z));
        initN_ = 0;
        init_ = true;
        degraded_ = false;
        corrSinceSatS_ = 0;
        return Step::Initialized;
    }
    if (dtS <= 0) return Step::Skipped;
    if (dtS > p_.maxDtS) {
        reset();
        return Step::Reset;
    }

    // Predição pela cinemática de Euler (p, q, r → φ̇, θ̇)
    const float p = gyro.x * DEG2RAD, q = gyro.y * DEG2RAD, r = gyro.z * DEG2RAD;
    const float sr = sinf(roll_), cr = cosf(roll_);
    float cp = cosf(pitch_);
    if (fabsf(cp) < COS_PITCH_MIN) cp = copysignf(COS_PITCH_MIN, cp);
    const float tp = sinf(pitch_) / cp;
    roll_ += (p + (q * sr + r * cr) * tp) * dtS;
    pitch_ += (q * cr - r * sr) * dtS;
    // Passou da vertical: a mesma atitude com pitch em [−90°, 90°] tem o roll girado de 180°
    if (pitch_ > PI_F / 2) {
        pitch_ = PI_F - pitch_;
        roll_ += PI_F;
    } else if (pitch_ < -PI_F / 2) {
        pitch_ = -PI_F - pitch_;
        roll_ += PI_F;
    }
    roll_ = wrapPi(roll_);

    // Correção pelo acelerômetro: ganho dt/(τ + dt) = 1 − α, reduzido pelo peso
    if (w > 0) {
        const float k = dtS / (p_.tauS + dtS) * w;
        pitch_ += k * (atan2f(acc.x, proj) - pitch_);
        if (rollObservable) roll_ = wrapPi(roll_ + k * wrapPi(atan2f(-acc.y, -acc.z) - roll_));
    }

    if (flags.gyroSaturated) {
        degraded_ = true;
        corrSinceSatS_ = 0;
    } else if (degraded_) {
        corrSinceSatS_ += w * dtS;
        if (corrSinceSatS_ >= 3 * p_.tauS) degraded_ = false;
    }
    return Step::Updated;
}

// ---------- Calibração de montagem ----------

Mat3 mat3Identity() {
    return {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
}

Vec3 mat3Apply(const Mat3 &r, const Vec3 &v) {
    return {r.m[0][0] * v.x + r.m[0][1] * v.y + r.m[0][2] * v.z,
            r.m[1][0] * v.x + r.m[1][1] * v.y + r.m[1][2] * v.z,
            r.m[2][0] * v.x + r.m[2][1] * v.y + r.m[2][2] * v.z};
}

bool mat3IsRotation(const Mat3 &r, float tol) {
    Vec3 row[3];
    for (int i = 0; i < 3; i++) {
        row[i] = {r.m[i][0], r.m[i][1], r.m[i][2]};
        if (!finite3(row[i])) return false;
    }
    for (int i = 0; i < 3; i++) {
        if (fabsf(dot(row[i], row[i]) - 1.0f) > tol) return false;
        if (fabsf(dot(row[i], row[(i + 1) % 3])) > tol) return false;
    }
    return fabsf(dot(row[0], cross(row[1], row[2])) - 1.0f) <= tol;
}

// Ângulo (graus) entre a força específica unitária u e a de repouso nivelado (0, 0, −1)
static float tiltDeg(const Vec3 &u) {
    return acosf(fminf(1.0f, fmaxf(-1.0f, -u.z))) * RAD2DEG;
}

MountCheck mountFromTwoPoses(const Vec3 &f1, const Vec3 &f2, float maxTiltDeg, MountResult &out) {
    if (!finite3(f1) || !finite3(f2)) return MountCheck::BadInput;
    const float n1 = norm(f1), n2 = norm(f2);
    if (n1 < 0.5f || n2 < 0.5f) return MountCheck::BadInput;
    const Vec3 u1 = scaled(f1, 1 / n1), u2 = scaled(f2, 1 / n2);
    const Vec3 s = {u1.x + u2.x, u1.y + u2.y, u1.z + u2.z};
    const float ns = norm(s);
    if (ns < 1.0f) return MountCheck::BadInput;   // mais de 120° entre as medidas: não é a mesma face

    // Eixo z (para baixo) da caixa, nos eixos nominais
    const Vec3 z = scaled(s, -1 / ns);
    // Nariz nominal (1, 0, 0) sem a componente em z (Gram-Schmidt): preserva a direção do nariz
    Vec3 x = {1 - z.x * z.x, -z.x * z.y, -z.x * z.z};
    const float nx = norm(x);
    if (nx < 0.5f) return MountCheck::BadInput;
    x = scaled(x, 1 / nx);
    const Vec3 y = cross(z, x);
    out.r = {{{x.x, x.y, x.z}, {y.x, y.y, y.z}, {z.x, z.y, z.z}}};

    // Caixa nivelada: força específica −z (caixa) vista nos eixos nominais
    const Vec3 level = scaled(z, -1);
    const Angles m = accelRollPitch(level);
    out.mountRollDeg = m.rollDeg;
    out.mountPitchDeg = m.pitchDeg;
    const Vec3 t1 = mat3Apply(out.r, u1);
    const Angles t = accelRollPitch(t1);
    out.tableRollDeg = t.rollDeg;
    out.tablePitchDeg = t.pitchDeg;
    if (tiltDeg(level) > maxTiltDeg || tiltDeg(t1) > maxTiltDeg) return MountCheck::TooTilted;
    return MountCheck::Ok;
}

// ---------- AngleStats ----------

void AngleStats::reset() {
    n_ = 0;
    ref_ = 0;
    sum_ = sq_ = 0;
}

void AngleStats::add(float deg) {
    if (n_ == 0) ref_ = deg;
    const double d = wrap180(deg - ref_);
    sum_ += d;
    sq_ += d * d;
    n_++;
}

float AngleStats::mean() const {
    return n_ ? wrap180(ref_ + (float)(sum_ / n_)) : 0.0f;
}

float AngleStats::stddev() const {
    if (n_ == 0) return 0.0f;
    const double m = sum_ / n_;
    return (float)sqrt(fmax(0.0, sq_ / n_ - m * m));
}

LevelRest checkLevelWindow(const WindowStats &gyroRaw, const Vec3 &gyroBias, const WindowStats &accCorrected,
                           uint32_t minSamples, const RestLimits &lim, float maxMeanDps) {
    const GyroBiasResult r = evaluateGyroWindow(gyroRaw, accCorrected, minSamples, lim);
    switch (r.status) {
    case RestCheck::TooFewSamples: return LevelRest::TooFewSamples;
    case RestCheck::Moving:        return LevelRest::Moving;
    case RestCheck::AccOutOfRange: return LevelRest::AccOutOfRange;
    case RestCheck::Ok:            break;
    }
    const Vec3 m = applyGyroBias(gyroRaw.mean(), gyroBias);
    if (fabsf(m.x) >= maxMeanDps || fabsf(m.y) >= maxMeanDps || fabsf(m.z) >= maxMeanDps) return LevelRest::Rotating;
    return LevelRest::Ok;
}
