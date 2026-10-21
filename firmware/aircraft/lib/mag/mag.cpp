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

void magCountSectors(const Vec3 *pts, size_t n, const Vec3 &center, uint16_t counts[MAG_SECTORS],
                     float minDistUt) {
    for (int i = 0; i < MAG_SECTORS; i++) counts[i] = 0;
    const float minDist2 = minDistUt * minDistUt;
    for (size_t k = 0; k < n; k++) {
        const Vec3 d = {pts[k].x - center.x, pts[k].y - center.y, pts[k].z - center.z};
        if (minDistUt > 0 && !(d.x * d.x + d.y * d.y + d.z * d.z >= minDist2)) continue;
        const int s = magSector(d);
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

// Resolve M·x = v (n×n, n ≤ 9) por eliminação de Gauss com pivoteamento parcial. false se singular.
// M e v são alterados.
static const int FIT_N = 9;
static bool solveN(double M[FIT_N][FIT_N], double v[FIT_N], double x[FIT_N], int n) {
    double scale = 0;
    for (int i = 0; i < n; i++) scale = fmax(scale, fabs(M[i][i]));
    if (!(scale > 0)) return false;
    const double eps = 1e-10 * scale;
    for (int c = 0; c < n; c++) {
        int p = c;
        for (int r = c + 1; r < n; r++)
            if (fabs(M[r][c]) > fabs(M[p][c])) p = r;
        if (!(fabs(M[p][c]) > eps)) return false;
        if (p != c) {
            for (int k = 0; k < n; k++) {
                const double t = M[c][k];
                M[c][k] = M[p][k];
                M[p][k] = t;
            }
            const double t = v[c];
            v[c] = v[p];
            v[p] = t;
        }
        for (int r = c + 1; r < n; r++) {
            const double f = M[r][c] / M[c][c];
            for (int k = c; k < n; k++) M[r][k] -= f * M[c][k];
            v[r] -= f * v[c];
        }
    }
    for (int r = n - 1; r >= 0; r--) {
        double s = v[r];
        for (int k = r + 1; k < n; k++) s -= M[r][k] * x[k];
        x[r] = s / M[r][r];
    }
    return true;
}

// Autovalores e autovetores de uma matriz simétrica 3×3 (Jacobi cíclico). A é destruída; ao fim,
// lam[k] é o autovalor da coluna k de V.
static void jacobi3(double A[3][3], double lam[3], double V[3][3]) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) V[i][j] = (i == j) ? 1 : 0;
    for (int sweep = 0; sweep < 50; sweep++) {
        const double off = A[0][1] * A[0][1] + A[0][2] * A[0][2] + A[1][2] * A[1][2];
        const double diag = A[0][0] * A[0][0] + A[1][1] * A[1][1] + A[2][2] * A[2][2];
        if (!(off > 1e-30 * diag)) break;
        for (int p = 0; p < 2; p++) {
            for (int q = p + 1; q < 3; q++) {
                if (A[p][q] == 0) continue;
                const double th = (A[q][q] - A[p][p]) / (2 * A[p][q]);
                const double t = (th >= 0 ? 1 : -1) / (fabs(th) + sqrt(th * th + 1));
                const double c = 1 / sqrt(t * t + 1), s = t * c;
                for (int k = 0; k < 3; k++) {   // A·J
                    const double akp = A[k][p], akq = A[k][q];
                    A[k][p] = c * akp - s * akq;
                    A[k][q] = s * akp + c * akq;
                }
                for (int k = 0; k < 3; k++) {   // Jᵀ·A
                    const double apk = A[p][k], aqk = A[q][k];
                    A[p][k] = c * apk - s * aqk;
                    A[q][k] = s * apk + c * aqk;
                }
                for (int k = 0; k < 3; k++) {   // V·J
                    const double vkp = V[k][p], vkq = V[k][q];
                    V[k][p] = c * vkp - s * vkq;
                    V[k][q] = s * vkp + c * vkq;
                }
            }
        }
    }
    for (int k = 0; k < 3; k++) lam[k] = A[k][k];
}

// Ordem dos autovetores pelo eixo do modelo em que cada um mais pesa, para mostrar os semieixos como
// x, y, z quando o elipsoide está quase alinhado. perm[eixo] = coluna de V.
static void axisOrder(const double V[3][3], int perm[3]) {
    static const int PERMS[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    double best = -1;
    for (int k = 0; k < 6; k++) {
        double s = 0;
        for (int i = 0; i < 3; i++) s += fabs(V[i][PERMS[k][i]]);
        if (s > best) {
            best = s;
            for (int i = 0; i < 3; i++) perm[i] = PERMS[k][i];
        }
    }
}

bool magWPlausible(const Mat3 &w) {
    static const float SYM_TOL = 1e-3f;                     // o ajuste grava W simétrica (erro de arredondamento ~1e-7)
    static const double EIG_MIN = 0.25, EIG_MAX = 4.0;      // faixa larga: o ajuste já limita os semieixos a 0,7..1,3
    double A[3][3], lam[3], V[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            if (!isfinite(w.m[i][j])) return false;
            if (fabsf(w.m[i][j] - w.m[j][i]) > SYM_TOL) return false;
            A[i][j] = 0.5 * ((double)w.m[i][j] + w.m[j][i]);
        }
    jacobi3(A, lam, V);
    for (int k = 0; k < 3; k++)
        if (!(lam[k] > EIG_MIN && lam[k] < EIG_MAX)) return false;   // inclui a positividade definida
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

    // u·Q·u + L·u = 1, com Q simétrica: 9 incógnitas (a, b, c, d, e, f e os três termos de L)
    double M[FIT_N][FIT_N] = {{0}}, v[FIT_N] = {0};
    for (size_t k = 0; k < n; k++) {
        if (!finite3(pts[k])) continue;
        const double u[3] = {(pts[k].x - c0[0]) / s0, (pts[k].y - c0[1]) / s0, (pts[k].z - c0[2]) / s0};
        const double phi[FIT_N] = {u[0] * u[0],     u[1] * u[1],     u[2] * u[2], 2 * u[0] * u[1], 2 * u[0] * u[2],
                                   2 * u[1] * u[2], u[0],            u[1],        u[2]};
        for (int i = 0; i < FIT_N; i++) {
            v[i] += phi[i];
            for (int j = 0; j < FIT_N; j++) M[i][j] += phi[i] * phi[j];
        }
    }
    double th[FIT_N];
    if (!solveN(M, v, th, FIT_N)) return out.status = MagFitStatus::Singular;
    const double Q[3][3] = {{th[0], th[3], th[4]}, {th[3], th[1], th[5]}, {th[4], th[5], th[2]}};

    // Centro: u0 = −½·Q⁻¹·L; então (u − u0)·Q·(u − u0) = G, com G = 1 + u0·Q·u0
    double Qs[FIT_N][FIT_N] = {{0}}, rhs[FIT_N] = {0}, u0[FIT_N];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) Qs[i][j] = Q[i][j];
        rhs[i] = -0.5 * th[6 + i];
    }
    if (!solveN(Qs, rhs, u0, 3)) return out.status = MagFitStatus::NotEllipsoid;
    double G = 1;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) G += u0[i] * Q[i][j] * u0[j];
    if (!isfinite(G) || G == 0) return out.status = MagFitStatus::NotEllipsoid;

    // Q/G = V·diag(λ)·Vᵀ: elipsoide só com os três autovalores positivos. Semieixos rₖ = s0/√λₖ (µT).
    double A[3][3], lam[3], V[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) A[i][j] = Q[i][j] / G;
    jacobi3(A, lam, V);
    double r[3];
    for (int k = 0; k < 3; k++) {
        if (!(lam[k] > 0)) return out.status = MagFitStatus::NotEllipsoid;
        r[k] = s0 / sqrt(lam[k]);
    }
    const double R = cbrt(r[0] * r[1] * r[2]);
    const double rMean = (r[0] + r[1] + r[2]) / 3;

    // W = V·diag(R/rₖ)·Vᵀ: raiz simétrica que leva o elipsoide à esfera de raio R
    MagCal &cal = out.cal;
    cal.offset = {(float)(c0[0] + s0 * u0[0]), (float)(c0[1] + s0 * u0[1]), (float)(c0[2] + s0 * u0[2])};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            double s = 0;
            for (int k = 0; k < 3; k++) s += V[i][k] * (R / r[k]) * V[j][k];
            cal.w.m[i][j] = (float)s;
        }
    cal.radiusUt = (float)R;
    // Rede de segurança numérica: as checagens acima já excluem os casos conhecidos
    bool finite = isfinite(cal.radiusUt) && finite3(cal.offset);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) finite = finite && isfinite(cal.w.m[i][j]);
    if (!finite) return out.status = MagFitStatus::NotEllipsoid;
    int perm[3];
    axisOrder(V, perm);
    out.scale = {(float)(r[perm[0]] / rMean), (float)(r[perm[1]] / rMean), (float)(r[perm[2]] / rMean)};

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

// ---------- Calibração (comando m) ----------

void MagCalibrator::start(Vec3 *buf, uint32_t nowMs) {
    pts_ = buf;
    n_ = 0;
    mm_.reset();
    startMs_ = nowMs;
    lastMs_ = nowMs;
    nextProgMs_ = nowMs + p_.progressMs;
    covered_ = 0;
    for (int f = 0; f < MAG_FACES; f++) miss_[f] = 0;
    fit_ = MagFitResult();
    state_ = State::Collect;
}

MagCalibrator::Event MagCalibrator::feed(const Vec3 &raw, uint32_t tMs) {
    if (state_ != State::Collect) return Event::None;
    if (n_ < p_.maxPoints) {
        pts_[n_++] = raw;
        mm_.add(raw);
    }
    const bool timeUp = tMs - startMs_ >= p_.maxMs || n_ >= p_.maxPoints;
    if ((int32_t)(tMs - nextProgMs_) < 0 && !timeUp) return Event::None;
    nextProgMs_ += p_.progressMs;
    lastMs_ = tMs;
    uint16_t counts[MAG_SECTORS];
    magCountSectors(pts_, n_, mm_.center(), counts, p_.coverMinUt);
    covered_ = magSectorsCovered(counts, p_.fit.minPtsSector);
    magMissingByFace(counts, p_.fit.minPtsSector, miss_);
    if (covered_ < MAG_SECTORS && !timeUp) return Event::Progress;
    magFitEllipsoid(pts_, n_, p_.fit, fit_);
    state_ = State::Review;
    return Event::Finished;
}

void MagCalibrator::stop() {
    state_ = State::Off;
    pts_ = nullptr;
    n_ = 0;
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
    if (!isfinite(normUt)) return true;   // |m| inválido não pode ser tratado como campo limpo
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
                                          float dtS, bool attitudeDegraded) {
    if (!isfinite(magHeadingDeg) || !isfinite(normUt) || !isfinite(pitchDeg)) return Mag::BadData;
    if (attitudeDegraded) return Mag::AttitudeDegraded;
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
