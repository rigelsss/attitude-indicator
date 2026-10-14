// mag.cpp — magnetômetro em lógica pura (sem dependência de Arduino). Registradores conforme o
// datasheet do HMC5883L (Honeywell, rev. E).
#include "mag.h"
#include <math.h>

static const float PI_F = (float)M_PI;
static const float DEG2RAD = PI_F / 180.0f;
static const float RAD2DEG = 180.0f / PI_F;
static const float COS_PITCH_MIN = 0.0871557f;   // cos 85°: mesma guarda numérica da fusão
static const size_t FIT_MIN_POINTS = 30;

static bool finite3(const Vec3 &v) {
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

static float axisOf(const Vec3 &v, int i) {
    return i == 0 ? v.x : (i == 1 ? v.y : v.z);
}

// ---------- HMC5883L ----------

bool hmcCraCode(uint8_t avgSamples, float outputHz, uint8_t &cra) {
    uint8_t ma;
    switch (avgSamples) {
    case 1: ma = 0; break;
    case 2: ma = 1; break;
    case 4: ma = 2; break;
    case 8: ma = 3; break;
    default: return false;
    }
    static const float RATES[] = {0.75f, 1.5f, 3.0f, 7.5f, 15.0f, 30.0f, 75.0f};
    for (uint8_t d = 0; d < sizeof(RATES) / sizeof(RATES[0]); d++) {
        if (fabsf(outputHz - RATES[d]) < 0.01f) {
            cra = (uint8_t)((ma << 5) | (d << 2));   // bits 1:0 = 00: sem polarização
            return true;
        }
    }
    return false;
}

bool hmcCrbCode(uint16_t rangeMga, uint8_t &crb, float &lsbPerGa) {
    static const uint16_t RANGES[] = {880, 1300, 1900, 2500, 4000, 4700, 5600, 8100};
    static const float GAINS[] = {1370, 1090, 820, 660, 440, 390, 330, 230};
    for (uint8_t g = 0; g < 8; g++) {
        if (RANGES[g] == rangeMga) {
            crb = (uint8_t)(g << 5);
            lsbPerGa = GAINS[g];
            return true;
        }
    }
    return false;
}

HmcFrame hmcDecode(const uint8_t raw[HMC_BURST_LEN], uint8_t cra, uint8_t crb, uint8_t mode, int16_t counts[3]) {
    counts[0] = (int16_t)((raw[3] << 8) | raw[4]);   // X
    counts[2] = (int16_t)((raw[5] << 8) | raw[6]);   // Z
    counts[1] = (int16_t)((raw[7] << 8) | raw[8]);   // Y
    // Bits reservados fora da comparação: CRA bit 7, CRB bits 4:0, modo bits 7:2
    if ((raw[0] & 0x7F) != (cra & 0x7F) || (raw[1] & 0xE0) != (crb & 0xE0) || (raw[2] & 0x03) != (mode & 0x03))
        return HmcFrame::ConfigLost;
    for (int i = 0; i < 3; i++)
        if (counts[i] == HMC_OVERFLOW) return HmcFrame::Saturated;
    return HmcFrame::Ok;
}

Vec3 hmcCountsToUt(const int16_t counts[3], float lsbPerGa) {
    const float k = 100.0f / lsbPerGa;
    return {counts[0] * k, counts[1] * k, counts[2] * k};
}

Vec3 magChipToBody(const Vec3 &chip) {
    return {chip.x, -chip.y, -chip.z};
}

// ---------- Calibração ----------

Vec3 magApplyCal(const MagCal &cal, const Vec3 &raw) {
    return mat3Apply(cal.w, {raw.x - cal.offset.x, raw.y - cal.offset.y, raw.z - cal.offset.z});
}

int magSector(const Vec3 &d) {
    if (!finite3(d)) return -1;
    const float a[3] = {fabsf(d.x), fabsf(d.y), fabsf(d.z)};
    int dom = 0;
    for (int i = 1; i < 3; i++)
        if (a[i] > a[dom]) dom = i;
    if (a[dom] == 0.0f) return -1;
    const int face = 2 * dom + (axisOf(d, dom) < 0 ? 1 : 0);
    const int o1 = (dom + 1) % 3, o2 = (dom + 2) % 3;
    const int q = (axisOf(d, o1) < 0 ? 1 : 0) | (axisOf(d, o2) < 0 ? 2 : 0);
    return face * 4 + q;
}

const char *magFaceName(int face) {
    static const char *const NAMES[MAG_FACES] = {"+x", "-x", "+y", "-y", "+z", "-z"};
    return (face >= 0 && face < MAG_FACES) ? NAMES[face] : "?";
}

void magCountSectors(const Vec3 *pts, size_t n, const Vec3 &center, uint16_t counts[MAG_SECTORS]) {
    for (int i = 0; i < MAG_SECTORS; i++) counts[i] = 0;
    for (size_t k = 0; k < n; k++) {
        const int s = magSector({pts[k].x - center.x, pts[k].y - center.y, pts[k].z - center.z});
        if (s >= 0 && counts[s] < UINT16_MAX) counts[s]++;
    }
}

int magSectorsCovered(const uint16_t counts[MAG_SECTORS], uint16_t minPts) {
    int c = 0;
    for (int i = 0; i < MAG_SECTORS; i++)
        if (counts[i] >= minPts) c++;
    return c;
}

void magMissingByFace(const uint16_t counts[MAG_SECTORS], uint16_t minPts, uint8_t missing[MAG_FACES]) {
    for (int f = 0; f < MAG_FACES; f++) {
        missing[f] = 0;
        for (int q = 0; q < 4; q++)
            if (counts[f * 4 + q] < minPts) missing[f]++;
    }
}

void MagMinMax::reset() {
    n_ = 0;
}

void MagMinMax::add(const Vec3 &p) {
    if (!finite3(p)) return;
    if (n_ == 0) {
        lo_ = hi_ = p;
    } else {
        lo_ = {fminf(lo_.x, p.x), fminf(lo_.y, p.y), fminf(lo_.z, p.z)};
        hi_ = {fmaxf(hi_.x, p.x), fmaxf(hi_.y, p.y), fmaxf(hi_.z, p.z)};
    }
    n_++;
}

Vec3 MagMinMax::center() const {
    return {(lo_.x + hi_.x) / 2, (lo_.y + hi_.y) / 2, (lo_.z + hi_.z) / 2};
}

const char *magFitStatusText(MagFitStatus s) {
    switch (s) {
    case MagFitStatus::Ok:               return "ok";
    case MagFitStatus::TooFewPoints:     return "pontos insuficientes";
    case MagFitStatus::Singular:         return "sistema singular (pontos num plano?)";
    case MagFitStatus::NotEllipsoid:     return "forma ajustada não é um elipsoide";
    case MagFitStatus::Coverage:         return "cobertura insuficiente";
    case MagFitStatus::RadiusOutOfRange: return "raio fora da faixa";
    case MagFitStatus::ScaleOutOfRange:  return "escala fora da faixa";
    case MagFitStatus::ResidualTooHigh:  return "resíduo acima do limite";
    }
    return "?";
}

// Resolve M·x = v (6×6) por eliminação de Gauss com pivoteamento parcial. false se singular.
static bool solve6(double M[6][6], double v[6], double x[6]) {
    double scale = 0;
    for (int i = 0; i < 6; i++) scale = fmax(scale, fabs(M[i][i]));
    if (!(scale > 0)) return false;
    const double eps = 1e-10 * scale;
    for (int c = 0; c < 6; c++) {
        int p = c;
        for (int r = c + 1; r < 6; r++)
            if (fabs(M[r][c]) > fabs(M[p][c])) p = r;
        if (!(fabs(M[p][c]) > eps)) return false;
        if (p != c) {
            for (int k = 0; k < 6; k++) {
                const double t = M[c][k];
                M[c][k] = M[p][k];
                M[p][k] = t;
            }
            const double t = v[c];
            v[c] = v[p];
            v[p] = t;
        }
        for (int r = c + 1; r < 6; r++) {
            const double f = M[r][c] / M[c][c];
            for (int k = c; k < 6; k++) M[r][k] -= f * M[c][k];
            v[r] -= f * v[c];
        }
    }
    for (int r = 5; r >= 0; r--) {
        double s = v[r];
        for (int k = r + 1; k < 6; k++) s -= M[r][k] * x[k];
        x[r] = s / M[r][r];
    }
    return true;
}

MagFitStatus magFitEllipsoid(const Vec3 *pts, size_t n, const MagFitLimits &lim, MagFitResult &out) {
    out = MagFitResult();
    // Normalização: centro e escala dos pontos, para os termos quadráticos e lineares terem a mesma ordem
    double c0[3] = {0, 0, 0};
    size_t m = 0;
    for (size_t k = 0; k < n; k++) {
        if (!finite3(pts[k])) continue;
        c0[0] += pts[k].x;
        c0[1] += pts[k].y;
        c0[2] += pts[k].z;
        m++;
    }
    if (m < FIT_MIN_POINTS) return out.status = MagFitStatus::TooFewPoints;
    for (int i = 0; i < 3; i++) c0[i] /= m;
    double s2 = 0;
    for (size_t k = 0; k < n; k++) {
        if (!finite3(pts[k])) continue;
        const double d[3] = {pts[k].x - c0[0], pts[k].y - c0[1], pts[k].z - c0[2]};
        s2 += d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    }
    const double s0 = sqrt(s2 / m);
    if (!(s0 > 1e-6)) return out.status = MagFitStatus::Singular;

    double M[6][6] = {{0}}, v[6] = {0};
    for (size_t k = 0; k < n; k++) {
        if (!finite3(pts[k])) continue;
        const double u[3] = {(pts[k].x - c0[0]) / s0, (pts[k].y - c0[1]) / s0, (pts[k].z - c0[2]) / s0};
        const double phi[6] = {u[0] * u[0], u[1] * u[1], u[2] * u[2], u[0], u[1], u[2]};
        for (int i = 0; i < 6; i++) {
            v[i] += phi[i];
            for (int j = 0; j < 6; j++) M[i][j] += phi[i] * phi[j];
        }
    }
    double th[6];
    if (!solve6(M, v, th)) return out.status = MagFitStatus::Singular;
    const double A[3] = {th[0], th[1], th[2]}, L[3] = {th[3], th[4], th[5]};
    if (!(A[0] > 0 && A[1] > 0 && A[2] > 0)) return out.status = MagFitStatus::NotEllipsoid;
    // A·(u − u0)² = G, com u0 = −L/(2A) e G = 1 + Σ L²/(4A)
    double G = 1;
    for (int i = 0; i < 3; i++) G += L[i] * L[i] / (4 * A[i]);
    if (!(G > 0)) return out.status = MagFitStatus::NotEllipsoid;

    double ctr[3], r[3];
    for (int i = 0; i < 3; i++) {
        ctr[i] = c0[i] + s0 * (-L[i] / (2 * A[i]));
        r[i] = s0 * sqrt(G / A[i]);
    }
    const double R = cbrt(r[0] * r[1] * r[2]);
    const double rMean = (r[0] + r[1] + r[2]) / 3;

    MagCal &cal = out.cal;
    cal.offset = {(float)ctr[0], (float)ctr[1], (float)ctr[2]};
    cal.w = mat3Identity();
    for (int i = 0; i < 3; i++) cal.w.m[i][i] = (float)(R / r[i]);
    cal.radiusUt = (float)R;
    out.scale = {(float)(r[0] / rMean), (float)(r[1] / rMean), (float)(r[2] / rMean)};

    double e2 = 0;
    for (size_t k = 0; k < n; k++) {
        if (!finite3(pts[k])) continue;
        const double e = norm(magApplyCal(cal, pts[k])) - R;
        e2 += e * e;
    }
    out.residPct = (float)(sqrt(e2 / m) / R * 100.0);

    uint16_t counts[MAG_SECTORS];
    magCountSectors(pts, n, cal.offset, counts);
    out.sectors = magSectorsCovered(counts, lim.minPtsSector);

    if (out.sectors < lim.minSectors) return out.status = MagFitStatus::Coverage;
    if (!(R >= lim.minRadiusUt && R <= lim.maxRadiusUt)) return out.status = MagFitStatus::RadiusOutOfRange;
    const float sc[3] = {out.scale.x, out.scale.y, out.scale.z};
    for (int i = 0; i < 3; i++)
        if (!(sc[i] >= lim.minScale && sc[i] <= lim.maxScale)) return out.status = MagFitStatus::ScaleOutOfRange;
    if (!(out.residPct <= lim.maxResidPct)) return out.status = MagFitStatus::ResidualTooHigh;
    return out.status = MagFitStatus::Ok;
}

// ---------- Proa ----------

float wrap360(float deg) {
    float r = fmodf(deg, 360.0f);
    if (r < 0) r += 360.0f;
    if (r >= 360.0f) r -= 360.0f;   // −0,00001 + 360 arredonda para 360
    return r;
}

float magFlatHeadingDeg(const Vec3 &m) {
    return wrap360(atan2f(-m.y, m.x) * RAD2DEG);
}

MagTilt magTiltCompensate(const Vec3 &m, float rollDeg, float pitchDeg) {
    const float phi = rollDeg * DEG2RAD, th = pitchDeg * DEG2RAD;
    const float sf = sinf(phi), cf = cosf(phi), st = sinf(th), ct = cosf(th);
    const float xh = m.x * ct + m.y * sf * st + m.z * cf * st;
    const float yh = m.y * cf - m.z * sf;
    const float zh = -m.x * st + m.y * sf * ct + m.z * cf * ct;
    return {wrap360(atan2f(-yh, xh) * RAD2DEG), atan2f(zh, sqrtf(xh * xh + yh * yh)) * RAD2DEG};
}

bool magDisturbed(float normUt, float refRadiusUt, float pct) {
    if (!(refRadiusUt > 0)) return false;
    return fabsf(normUt - refRadiusUt) > refRadiusUt * pct / 100.0f;
}

void HeadingFilter::reset() {
    init_ = false;
    psi_ = 0;
}

void HeadingFilter::predict(const Vec3 &gyroBodyDps, float rollDeg, float pitchDeg, float dtS) {
    if (!init_) return;
    if (!isfinite(dtS) || dtS > p_.maxDtS) {
        reset();   // lacuna: a proa volta a partir do magnetômetro
        return;
    }
    if (!(dtS > 0) || !finite3(gyroBodyDps) || !isfinite(rollDeg) || !isfinite(pitchDeg)) return;
    const float phi = rollDeg * DEG2RAD;
    float ct = cosf(pitchDeg * DEG2RAD);
    if (fabsf(ct) < COS_PITCH_MIN) ct = copysignf(COS_PITCH_MIN, ct);
    const float rate = (gyroBodyDps.y * sinf(phi) + gyroBodyDps.z * cosf(phi)) / ct;
    psi_ = wrap360(psi_ + rate * dtS);
}

HeadingFilter::Mag HeadingFilter::correct(float magHeadingDeg, float normUt, float refRadiusUt, float pitchDeg,
                                          float dtS) {
    if (!isfinite(magHeadingDeg) || !isfinite(normUt) || !isfinite(pitchDeg)) return Mag::BadData;
    if (fabsf(pitchDeg) > p_.maxPitchDeg) return Mag::Tilted;
    if (magDisturbed(normUt, refRadiusUt, p_.disturbPct)) return Mag::Disturbed;
    if (!init_) {
        psi_ = wrap360(magHeadingDeg);
        init_ = true;
        return Mag::Initialized;
    }
    const float dt = isfinite(dtS) ? fminf(fmaxf(dtS, 0.0f), p_.maxDtS) : 0.0f;
    const float k = dt / (p_.tauS + dt);
    psi_ = wrap360(psi_ + k * wrap180(magHeadingDeg - psi_));
    return Mag::Corrected;
}
