// Testes do magnetômetro em lógica pura (lib/mag). Rodar no PC: pio test -e native
#include <math.h>
#include <stdlib.h>
#include <unity.h>
#include "mag.h"

void setUp() {}
void tearDown() {}

static const float D2R = (float)M_PI / 180.0f;
// Campo de referência da bancada (WMM2025, João Pessoa): H = 22,2 µT, V = −13,8 µT (para cima)
static const float H_UT = 22.2f;
static const float V_UT = -13.8f;
// Mesmos limites do config.h
static const MagFitLimits LIM = {10.0f, 60.0f, 3.0f, 0.7f, 1.3f, 20, 5};
static const HeadingParams HP = {1.0f, 70.0f, 0.2f};

// Ruído determinístico: xorshift32 + aproximação gaussiana (soma de 12 uniformes − 6)
static uint32_t g_seed = 12345;
static float uniform01() {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return (g_seed & 0xFFFFFF) / 16777216.0f;
}
static float gauss() {
    float s = 0;
    for (int i = 0; i < 12; i++) s += uniform01();
    return s - 6.0f;
}

// Campo nos eixos do modelo para proa ψ, roll φ e pitch θ (sequência ZYX): m = R1(φ)·R2(θ)·R3(ψ)·(H, 0, V)
static Vec3 bodyField(float headingDeg, float rollDeg, float pitchDeg) {
    const float ps = headingDeg * D2R, th = pitchDeg * D2R, ph = rollDeg * D2R;
    // R3(ψ): Terra → proa
    const float x1 = cosf(ps) * H_UT, y1 = -sinf(ps) * H_UT, z1 = V_UT;
    // R2(θ)
    const float x2 = cosf(th) * x1 - sinf(th) * z1, y2 = y1, z2 = sinf(th) * x1 + cosf(th) * z1;
    // R1(φ)
    return {x2, cosf(ph) * y2 + sinf(ph) * z2, -sinf(ph) * y2 + cosf(ph) * z2};
}

// Pontos espalhados uniformemente na esfera unitária (espiral de Fibonacci)
static Vec3 fib(int i, int n) {
    const float z = 1.0f - 2.0f * (i + 0.5f) / n;
    const float r = sqrtf(1.0f - z * z);
    const float a = i * 2.39996323f;   // ângulo áureo
    return {r * cosf(a), r * sinf(a), z};
}

// Distorção sintética: bruto = offset + D·(R·u) + ruído, D diagonal
static int makeCloud(Vec3 *pts, int n, float radius, Vec3 off, Vec3 d, float noise, float minZ = -2.0f) {
    int k = 0;
    for (int i = 0; i < n; i++) {
        const Vec3 u = fib(i, n);
        if (u.z > minZ && minZ > -2.0f) continue;   // remove a calota acima de minZ
        pts[k++] = {off.x + d.x * radius * u.x + noise * gauss(), off.y + d.y * radius * u.y + noise * gauss(),
                    off.z + d.z * radius * u.z + noise * gauss()};
    }
    return k;
}

// ---------- HMC5883L ----------

static void test_cra_crb_codes() {
    uint8_t cra, crb;
    float g;
    TEST_ASSERT_TRUE(hmcCraCode(8, 30.0f, cra));
    TEST_ASSERT_EQUAL_HEX8(0x74, cra);   // média 8 (11), 30 Hz (101), sem polarização
    TEST_ASSERT_TRUE(hmcCraCode(8, 15.0f, cra));
    TEST_ASSERT_EQUAL_HEX8(0x70, cra);   // configuração do diagnóstico
    TEST_ASSERT_TRUE(hmcCraCode(1, 0.75f, cra));
    TEST_ASSERT_EQUAL_HEX8(0x00, cra);
    TEST_ASSERT_FALSE(hmcCraCode(3, 30.0f, cra));
    TEST_ASSERT_FALSE(hmcCraCode(8, 20.0f, cra));
    TEST_ASSERT_TRUE(hmcCrbCode(1300, crb, g));
    TEST_ASSERT_EQUAL_HEX8(0x20, crb);
    TEST_ASSERT_EQUAL_FLOAT(1090.0f, g);
    TEST_ASSERT_TRUE(hmcCrbCode(880, crb, g));
    TEST_ASSERT_EQUAL_HEX8(0x00, crb);
    TEST_ASSERT_FALSE(hmcCrbCode(1000, crb, g));
}

static void test_decode_order_and_sign() {
    // X = +300 (0x012C), Z = −500 (0xFE0C), Y = +1090 (0x0442): ordem X, Z, Y no chip
    const uint8_t raw[HMC_BURST_LEN] = {0x74, 0x20, 0x00, 0x01, 0x2C, 0xFE, 0x0C, 0x04, 0x42};
    int16_t c[3];
    TEST_ASSERT_EQUAL(HmcFrame::Ok, hmcDecode(raw, 0x74, 0x20, 0x00, c));
    TEST_ASSERT_EQUAL_INT16(300, c[0]);
    TEST_ASSERT_EQUAL_INT16(1090, c[1]);
    TEST_ASSERT_EQUAL_INT16(-500, c[2]);
    const Vec3 ut = hmcCountsToUt(c, 1090.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 100.0f, ut.y);   // 1090 LSB = 1 Ga = 100 µT
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 27.523f, ut.x);
}

static void test_decode_saturation_each_axis() {
    for (int ax = 0; ax < 3; ax++) {
        uint8_t raw[HMC_BURST_LEN] = {0x74, 0x20, 0x00, 0, 10, 0, 20, 0, 30};
        const int pos = ax == 0 ? 3 : (ax == 1 ? 7 : 5);   // X, Y, Z na rajada
        raw[pos] = 0xF0;   // −4096 = 0xF000
        raw[pos + 1] = 0x00;
        int16_t c[3];
        TEST_ASSERT_EQUAL(HmcFrame::Saturated, hmcDecode(raw, 0x74, 0x20, 0x00, c));
        TEST_ASSERT_EQUAL_INT16(HMC_OVERFLOW, c[ax]);
    }
}

static void test_decode_config_lost() {
    int16_t c[3];
    // Padrão de fábrica após queda de alimentação: CRA 0x10, CRB 0x20, modo 0x01 (medição única)
    const uint8_t reset[HMC_BURST_LEN] = {0x10, 0x20, 0x01, 0, 0, 0, 0, 0, 0};
    TEST_ASSERT_EQUAL(HmcFrame::ConfigLost, hmcDecode(reset, 0x74, 0x20, 0x00, c));
    // Medição única concluída: modo ocioso (11)
    const uint8_t idle[HMC_BURST_LEN] = {0x74, 0x20, 0x03, 0, 0, 0, 0, 0, 0};
    TEST_ASSERT_EQUAL(HmcFrame::ConfigLost, hmcDecode(idle, 0x74, 0x20, 0x00, c));
    // Bits reservados não contam: CRA bit 7, CRB bits 4:0 e modo bits 7:2 (bit HS)
    const uint8_t hs[HMC_BURST_LEN] = {0xF4, 0x3F, 0x80, 0, 0, 0, 0, 0, 0};
    TEST_ASSERT_EQUAL(HmcFrame::Ok, hmcDecode(hs, 0x74, 0x20, 0x00, c));
}

// Mapeamento levantado na bancada (13/10/2026): com o nariz em 85° verdadeiros (~106° magnéticos), o
// chip mediu X ≈ −6, Y ≈ +21, Z ≈ +14 µT (sem calibração); nos eixos do modelo isso é o campo dessa proa
static void test_chip_to_body_mapping() {
    const Vec3 b = magChipToBody({0.1f, 0.2f, 0.3f});
    TEST_ASSERT_EQUAL_FLOAT(0.1f, b.x);
    TEST_ASSERT_EQUAL_FLOAT(-0.2f, b.y);
    TEST_ASSERT_EQUAL_FLOAT(-0.3f, b.z);
    const Vec3 chipP1 = {-6.0f, 21.4f, 13.8f};
    const Vec3 m = magChipToBody(chipP1);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 106.0f, magFlatHeadingDeg(m));
    TEST_ASSERT_TRUE(m.z < 0);   // campo apontando para cima: z do modelo (para baixo) negativo
    // Giro horário (nariz para a direita): a proa cresce
    const Vec3 chipP2 = {-21.4f, -6.0f, 13.8f};
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 196.0f, magFlatHeadingDeg(magChipToBody(chipP2)));
}

// ---------- Setores e cobertura ----------

static void test_sector_assignment() {
    TEST_ASSERT_EQUAL_INT(-1, magSector({0, 0, 0}));
    TEST_ASSERT_EQUAL_INT(-1, magSector({NAN, 0, 1}));
    TEST_ASSERT_EQUAL_INT(0 * 4 + 0, magSector({1.0f, 0.1f, 0.1f}));    // +x, y+ z+
    TEST_ASSERT_EQUAL_INT(0 * 4 + 3, magSector({1.0f, -0.1f, -0.1f}));  // +x, y− z−
    TEST_ASSERT_EQUAL_INT(1 * 4 + 1, magSector({-1.0f, -0.2f, 0.3f}));  // −x, y− z+
    TEST_ASSERT_EQUAL_INT(2 * 4 + 1, magSector({0.1f, 1.0f, -0.5f}));   // +y: outros eixos na ordem (z, x): z− x+
    TEST_ASSERT_EQUAL_INT(5 * 4 + 0, magSector({0.2f, 0.3f, -0.9f}));   // −z, x+ y+
    // Esfera uniforme: todos os setores com pontos, e a soma confere
    static Vec3 pts[960];
    for (int i = 0; i < 960; i++) pts[i] = fib(i, 960);
    uint16_t counts[MAG_SECTORS];
    magCountSectors(pts, 960, {0, 0, 0}, counts);
    uint32_t sum = 0;
    for (int i = 0; i < MAG_SECTORS; i++) {
        TEST_ASSERT_TRUE(counts[i] > 20);
        sum += counts[i];
    }
    TEST_ASSERT_EQUAL_UINT32(960, sum);
    TEST_ASSERT_EQUAL_INT(24, magSectorsCovered(counts, 5));
}

static void test_coverage_minimum_points_and_faces() {
    uint16_t counts[MAG_SECTORS];
    for (int i = 0; i < MAG_SECTORS; i++) counts[i] = 10;
    counts[20] = 4;   // −z: um quadrante abaixo do mínimo
    counts[21] = 0;
    counts[9] = 5;    // +y: exatamente no mínimo, conta
    TEST_ASSERT_EQUAL_INT(22, magSectorsCovered(counts, 5));
    uint8_t miss[MAG_FACES];
    magMissingByFace(counts, 5, miss);
    TEST_ASSERT_EQUAL_UINT8(2, miss[5]);
    TEST_ASSERT_EQUAL_UINT8(0, miss[2]);
    TEST_ASSERT_EQUAL_STRING("-z", magFaceName(5));
}

static void test_minmax_center() {
    MagMinMax mm;
    TEST_ASSERT_TRUE(mm.empty());
    mm.add({10, -5, 3});
    mm.add({-20, 15, 7});
    mm.add({NAN, 0, 0});
    const Vec3 c = mm.center();
    TEST_ASSERT_EQUAL_FLOAT(-5.0f, c.x);
    TEST_ASSERT_EQUAL_FLOAT(5.0f, c.y);
    TEST_ASSERT_EQUAL_FLOAT(5.0f, c.z);
}

// ---------- Ajuste do elipsoide ----------

static Vec3 g_cloud[2400];

static void test_fit_recovers_offset_and_scale() {
    g_seed = 777;
    const Vec3 off = {15.0f, -8.0f, 30.0f}, d = {1.1f, 0.9f, 1.05f};
    const int n = makeCloud(g_cloud, 1500, 26.2f, off, d, 0.1f);
    MagFitResult r;
    TEST_ASSERT_EQUAL(MagFitStatus::Ok, magFitEllipsoid(g_cloud, n, LIM, r));
    TEST_ASSERT_FLOAT_WITHIN(0.1f, off.x, r.cal.offset.x);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, off.y, r.cal.offset.y);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, off.z, r.cal.offset.z);
    const float mean = (d.x + d.y + d.z) / 3;
    TEST_ASSERT_FLOAT_WITHIN(0.005f, d.x / mean, r.scale.x);
    TEST_ASSERT_FLOAT_WITHIN(0.005f, d.y / mean, r.scale.y);
    TEST_ASSERT_FLOAT_WITHIN(0.005f, d.z / mean, r.scale.z);
    TEST_ASSERT_FLOAT_WITHIN(0.3f, 26.2f * cbrtf(d.x * d.y * d.z), r.cal.radiusUt);
    TEST_ASSERT_TRUE(r.residPct < 1.0f);
    TEST_ASSERT_EQUAL_INT(24, r.sectors);
    // Depois da correção, |m| fica no raio em qualquer direção
    for (int i = 0; i < n; i += 97)
        TEST_ASSERT_FLOAT_WITHIN(0.02f * r.cal.radiusUt, r.cal.radiusUt, norm(magApplyCal(r.cal, g_cloud[i])));
}

// Elipsoide inclinado: bruto = offset + D·(raio·u) + ruído, com D simétrica e acoplamento y–z
// (como na bancada, 15/10/2026: termo fora da diagonal de ~0,07)
static void test_fit_recovers_coupled_soft_iron() {
    g_seed = 4242;
    const Vec3 off = {8.3f, 6.5f, 9.9f};
    const float D[3][3] = {{1.02f, 0.02f, 0.01f}, {0.02f, 1.10f, 0.08f}, {0.01f, 0.08f, 0.88f}};
    int n = 0;
    for (int i = 0; i < 1500; i++) {
        const Vec3 u = fib(i, 1500);
        const float p[3] = {25.9f * u.x, 25.9f * u.y, 25.9f * u.z};
        g_cloud[n++] = {off.x + D[0][0] * p[0] + D[0][1] * p[1] + D[0][2] * p[2] + 0.1f * gauss(),
                        off.y + D[1][0] * p[0] + D[1][1] * p[1] + D[1][2] * p[2] + 0.1f * gauss(),
                        off.z + D[2][0] * p[0] + D[2][1] * p[1] + D[2][2] * p[2] + 0.1f * gauss()};
    }
    MagFitResult r;
    TEST_ASSERT_EQUAL(MagFitStatus::Ok, magFitEllipsoid(g_cloud, n, LIM, r));
    TEST_ASSERT_FLOAT_WITHIN(0.1f, off.x, r.cal.offset.x);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, off.y, r.cal.offset.y);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, off.z, r.cal.offset.z);
    TEST_ASSERT_TRUE(r.residPct < 1.0f);
    // W simétrica e W·D proporcional à identidade (D simétrica: a raiz simétrica desfaz D sem rotação)
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            TEST_ASSERT_FLOAT_WITHIN(1e-4f, r.cal.w.m[i][j], r.cal.w.m[j][i]);
            float wd = 0;
            for (int k = 0; k < 3; k++) wd += r.cal.w.m[i][k] * D[k][j];
            const float k0 = r.cal.radiusUt / 25.9f;
            TEST_ASSERT_FLOAT_WITHIN(0.01f, i == j ? k0 : 0.0f, wd);
        }
    // Semieixos na ordem dos eixos do modelo: y é o maior e z o menor
    TEST_ASSERT_TRUE(r.scale.y > r.scale.x && r.scale.x > r.scale.z);
    for (int i = 0; i < n; i += 97)
        TEST_ASSERT_FLOAT_WITHIN(0.02f * r.cal.radiusUt, r.cal.radiusUt, norm(magApplyCal(r.cal, g_cloud[i])));
}

// Distorção com matriz cheia: bruto = offset + D·(raio·u) + ruído
static int makeCloudMat(Vec3 *pts, int n, float radius, Vec3 off, const float D[3][3], float noise) {
    for (int i = 0; i < n; i++) {
        const Vec3 u = fib(i, n);
        const float p[3] = {radius * u.x, radius * u.y, radius * u.z};
        pts[i] = {off.x + D[0][0] * p[0] + D[0][1] * p[1] + D[0][2] * p[2] + noise * gauss(),
                  off.y + D[1][0] * p[0] + D[1][1] * p[1] + D[1][2] * p[2] + noise * gauss(),
                  off.z + D[2][0] * p[0] + D[2][1] * p[1] + D[2][2] * p[2] + noise * gauss()};
    }
    return n;
}

// Ajuste aceito, offset recuperado, W simétrica e W·D = (R/raio)·I (D simétrica)
static void checkFitInvertsD(const Vec3 *pts, int n, float radius, Vec3 off, const float D[3][3]) {
    MagFitResult r;
    TEST_ASSERT_EQUAL(MagFitStatus::Ok, magFitEllipsoid(pts, n, LIM, r));
    TEST_ASSERT_FLOAT_WITHIN(0.1f, off.x, r.cal.offset.x);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, off.y, r.cal.offset.y);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, off.z, r.cal.offset.z);
    TEST_ASSERT_TRUE(r.residPct < 1.0f);
    TEST_ASSERT_TRUE(magWPlausible(r.cal.w));
    const float k0 = r.cal.radiusUt / radius;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            TEST_ASSERT_FLOAT_WITHIN(1e-4f, r.cal.w.m[i][j], r.cal.w.m[j][i]);
            float wd = 0;
            for (int k = 0; k < 3; k++) wd += r.cal.w.m[i][k] * D[k][j];
            TEST_ASSERT_FLOAT_WITHIN(0.01f, i == j ? k0 : 0.0f, wd);
        }
}

// Autovalores iguais (esfera): o Jacobi não tem direção preferida, e W tem de sair ∝ identidade
static void test_fit_sphere_equal_eigenvalues() {
    g_seed = 31;
    const float D[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const int n = makeCloudMat(g_cloud, 1500, 25.7f, {-5, 12, 3}, D, 0.1f);
    checkFitInvertsD(g_cloud, n, 25.7f, {-5, 12, 3}, D);
}

// Dois semieixos iguais (autovalor duplo) e um diferente: D = Rx(45°)·diag(1,15, 1,15, 0,85)·Rx(45°)ᵀ,
// com o autovalor duplo no plano de x e (0, 1, 1)
static void test_fit_repeated_eigenvalue() {
    g_seed = 32;
    const float a = 1.15f, b = 0.85f;
    const float D[3][3] = {{a, 0, 0}, {0, (a + b) / 2, (a - b) / 2}, {0, (a - b) / 2, (a + b) / 2}};
    const int n = makeCloudMat(g_cloud, 1500, 25.7f, {8, 6, 10}, D, 0.1f);
    checkFitInvertsD(g_cloud, n, 25.7f, {8, 6, 10}, D);
}

// Elipsoide fortemente girado: D = R·diag(1,25, 1,0, 0,8)·Rᵀ, R = Rz(45°)·Rx(45°)
static void test_fit_strongly_rotated() {
    g_seed = 33;
    const float s = 0.70710678f;
    const float R[3][3] = {{s, -s * s, s * s}, {s, s * s, -s * s}, {0, s, s}};   // Rz(45°)·Rx(45°)
    const float L[3] = {1.25f, 1.0f, 0.8f};
    float D[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            D[i][j] = 0;
            for (int k = 0; k < 3; k++) D[i][j] += R[i][k] * L[k] * R[j][k];
        }
    const int n = makeCloudMat(g_cloud, 1500, 25.7f, {-15, 20, 5}, D, 0.1f);
    checkFitInvertsD(g_cloud, n, 25.7f, {-15, 20, 5}, D);
}

static void test_w_plausible() {
    TEST_ASSERT_TRUE(magWPlausible(mat3Identity()));
    // W gravada na bancada em 15/10/2026
    const Mat3 bench = {{{0.9741f, 0.0188f, -0.0071f}, {0.0188f, 0.9268f, -0.0742f}, {-0.0071f, -0.0742f, 1.1141f}}};
    TEST_ASSERT_TRUE(magWPlausible(bench));
    Mat3 w = bench;
    w.m[1][2] = 0.05f;   // assimétrica
    TEST_ASSERT_FALSE(magWPlausible(w));
    // Diagonal positiva, mas indefinida: autovalores 1 + 2 e 1 − 2
    const Mat3 indef = {{{1, 2, 0}, {2, 1, 0}, {0, 0, 1}}};
    TEST_ASSERT_FALSE(magWPlausible(indef));
    // Positiva definida, mas fora da faixa (autovalor 10)
    const Mat3 big = {{{10, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    TEST_ASSERT_FALSE(magWPlausible(big));
    w = bench;
    w.m[0][0] = NAN;
    TEST_ASSERT_FALSE(magWPlausible(w));
}

static void test_fit_partial_coverage() {
    g_seed = 99;
    // Sem a calota de z > 0,75 (~12 % da esfera): ainda acerta, com 20 ou mais setores
    const Vec3 off = {-12.0f, 4.0f, -6.0f}, d = {1.0f, 1.08f, 0.95f};
    const int n = makeCloud(g_cloud, 1500, 26.2f, off, d, 0.1f, 0.75f);
    MagFitResult r;
    TEST_ASSERT_EQUAL(MagFitStatus::Ok, magFitEllipsoid(g_cloud, n, LIM, r));
    TEST_ASSERT_TRUE(r.sectors >= 20);
    TEST_ASSERT_FLOAT_WITHIN(0.3f, off.x, r.cal.offset.x);
    TEST_ASSERT_FLOAT_WITHIN(0.3f, off.z, r.cal.offset.z);
    // Só meia esfera (z < 0): cobertura insuficiente
    const int nh = makeCloud(g_cloud, 1500, 26.2f, off, d, 0.1f, 0.0f);
    TEST_ASSERT_EQUAL(MagFitStatus::Coverage, magFitEllipsoid(g_cloud, nh, LIM, r));
}

static void test_fit_rejects_planar_and_few() {
    // Giro só sobre a mesa: um círculo no plano (z constante)
    int n = 0;
    for (int i = 0; i < 600; i++) {
        const float a = i * 2.0f * (float)M_PI / 600;
        g_cloud[n++] = {5 + 22.2f * cosf(a), -3 + 22.2f * sinf(a), -13.8f};
    }
    MagFitResult r;
    const MagFitStatus s = magFitEllipsoid(g_cloud, n, LIM, r);
    TEST_ASSERT_TRUE(s == MagFitStatus::Singular || s == MagFitStatus::NotEllipsoid || s == MagFitStatus::Coverage);
    TEST_ASSERT_TRUE(s != MagFitStatus::Ok);
    TEST_ASSERT_EQUAL(MagFitStatus::TooFewPoints, magFitEllipsoid(g_cloud, 10, LIM, r));
}

static void test_fit_rejects_bad_results() {
    MagFitResult r;
    g_seed = 5;
    // Raio de 5 µT
    int n = makeCloud(g_cloud, 1200, 5.0f, {0, 0, 0}, {1, 1, 1}, 0.05f);
    TEST_ASSERT_EQUAL(MagFitStatus::RadiusOutOfRange, magFitEllipsoid(g_cloud, n, LIM, r));
    // Escala de 1,5 num eixo (normalizada: 1,5/1,167 ≈ 1,29 passa; 1,8 não)
    n = makeCloud(g_cloud, 1200, 26.2f, {0, 0, 0}, {1.8f, 1.0f, 1.0f}, 0.1f);
    TEST_ASSERT_EQUAL(MagFitStatus::ScaleOutOfRange, magFitEllipsoid(g_cloud, n, LIM, r));
    // Distúrbio no meio da coleta: metade dos pontos com +20 µT em x (ferramenta perto)
    n = makeCloud(g_cloud, 1200, 26.2f, {0, 0, 0}, {1, 1, 1}, 0.1f);
    for (int i = 0; i < n; i += 2) g_cloud[i].x += 20.0f;
    TEST_ASSERT_TRUE(magFitEllipsoid(g_cloud, n, LIM, r) != MagFitStatus::Ok);
    TEST_ASSERT_TRUE(r.residPct > 3.0f || r.status == MagFitStatus::NotEllipsoid);
    // Ponto não finito é ignorado
    n = makeCloud(g_cloud, 1200, 26.2f, {3, 3, 3}, {1, 1, 1}, 0.1f);
    g_cloud[7] = {NAN, 0, 0};
    TEST_ASSERT_EQUAL(MagFitStatus::Ok, magFitEllipsoid(g_cloud, n, LIM, r));
}

// ---------- Proa ----------

static void test_tilt_compensation_exact() {
    const float dip = atan2f(V_UT, H_UT) / D2R;   // ≈ −31,9°: o campo aponta para cima
    for (int h = 0; h < 360; h += 15) {
        for (int roll = -45; roll <= 45; roll += 15) {
            for (int pitch = -45; pitch <= 45; pitch += 15) {
                const Vec3 m = bodyField((float)h, (float)roll, (float)pitch);
                const MagTilt t = magTiltCompensate(m, (float)roll, (float)pitch);
                TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, wrap180(t.headingDeg - h));
                TEST_ASSERT_FLOAT_WITHIN(0.01f, dip, t.dipDeg);
                TEST_ASSERT_TRUE(t.headingDeg >= 0.0f && t.headingDeg < 360.0f);
            }
        }
    }
    // Conferência do doc: nariz para o norte nivelado e nariz para o leste
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, wrap180(magTiltCompensate({H_UT, 0, V_UT}, 0, 0).headingDeg));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 90.0f, magTiltCompensate({0, -H_UT, V_UT}, 0, 0).headingDeg);
}

static void test_flat_heading_error_with_tilt() {
    // Sem compensação, 20° de roll na proa 0° já dão dezenas de graus de erro (o campo é inclinado)
    const Vec3 m = bodyField(0, 20, 0);
    TEST_ASSERT_TRUE(fabsf(wrap180(magFlatHeadingDeg(m))) > 5.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, wrap180(magTiltCompensate(m, 20, 0).headingDeg));
}

static void test_wrap360() {
    TEST_ASSERT_EQUAL_FLOAT(0.0f, wrap360(360.0f));
    TEST_ASSERT_EQUAL_FLOAT(359.0f, wrap360(-1.0f));
    TEST_ASSERT_EQUAL_FLOAT(10.0f, wrap360(730.0f));
    const float w = wrap360(-1e-6f);
    TEST_ASSERT_TRUE(w >= 0.0f && w < 360.0f);
}

// Distúrbio com histerese, sem tempo mínimo: entra acima de 15 %, sai em 10 % ou menos
static void test_disturbed() {
    MagDisturbance d(15.0f, 10.0f, 0);
    TEST_ASSERT_FALSE(d.update(26.0f, 26.2f, 0));
    TEST_ASSERT_TRUE(d.update(31.0f, 26.2f, 0));
    TEST_ASSERT_TRUE(d.update(29.0f, 26.2f, 0));    // 10,7 %: entre a saída e a entrada, continua
    TEST_ASSERT_FALSE(d.update(28.5f, 26.2f, 0));   // 8,8 %: sai
    TEST_ASSERT_TRUE(d.update(21.0f, 26.2f, 0));    // do lado de baixo também
    TEST_ASSERT_FALSE(d.update(24.0f, 26.2f, 0));
    // Limite de saída exato: raio 20, saída com desvio de 2,0 µT
    TEST_ASSERT_TRUE(d.update(23.5f, 20.0f, 0));
    TEST_ASSERT_TRUE(d.update(22.1f, 20.0f, 0));
    TEST_ASSERT_FALSE(d.update(22.0f, 20.0f, 0));
    // Limite de entrada exato (15 % de 20 = 3,0 µT): igual não entra
    TEST_ASSERT_FALSE(d.update(23.0f, 20.0f, 0));
    // Sem calibração (raio 0): nunca, e um distúrbio em curso termina
    TEST_ASSERT_TRUE(d.update(30.0f, 20.0f, 0));
    TEST_ASSERT_FALSE(d.update(80.0f, 0.0f, 0));
    // |m| inválido conta como distúrbio, e só sai com um valor finito dentro da faixa de saída
    TEST_ASSERT_TRUE(d.update(NAN, 26.2f, 0));
    TEST_ASSERT_TRUE(d.update(INFINITY, 26.2f, 0));
    TEST_ASSERT_TRUE(d.update(29.5f, 26.2f, 0));
    TEST_ASSERT_FALSE(d.update(26.0f, 26.2f, 0));
    // reset volta a sem distúrbio
    TEST_ASSERT_TRUE(d.update(40.0f, 26.2f, 0));
    d.reset();
    TEST_ASSERT_FALSE(d.disturbed());
}

// Os valores de 20/10/2026 (raio 25,7 µT; limites de entrada 21,8 e 29,6 µT): sem a histerese, o aviso
// alternava; com ela, uma entrada só, e a saída só com |m| de volta a 23,1–28,3 µT
static void test_disturbed_hysteresis_bench_values() {
    const float R = 25.7f;
    MagDisturbance d(15.0f, 10.0f, 0);
    const float upper[] = {28.0f, 31.0f, 28.9f, 30.0f, 28.5f, 29.9f, 28.4f};
    int changes = 0;
    bool prev = false;
    for (float v : upper) {
        const bool now = d.update(v, R, 0);
        if (now != prev) changes++;
        prev = now;
    }
    TEST_ASSERT_EQUAL(1, changes);   // entrou em 31,0 e não saiu
    TEST_ASSERT_FALSE(d.update(28.0f, R, 0));
    const float lower[] = {21.7f, 21.9f, 21.8f, 21.9f, 22.5f, 23.0f};
    changes = 0;
    prev = false;
    for (float v : lower) {
        const bool now = d.update(v, R, 0);
        if (now != prev) changes++;
        prev = now;
    }
    TEST_ASSERT_EQUAL(1, changes);   // entrou em 21,7 e não saiu
    TEST_ASSERT_FALSE(d.update(23.2f, R, 0));
}

// Tempo mínimo na faixa de saída: só termina depois de exitHoldMs seguidos dentro dela; uma leitura fora
// recomeça a contagem
static void test_disturbed_exit_hold() {
    const float R = 25.7f;
    MagDisturbance d(15.0f, 10.0f, 1000);
    TEST_ASSERT_TRUE(d.update(31.0f, R, 0));
    TEST_ASSERT_TRUE(d.update(27.0f, R, 40));     // entrou na faixa de saída aos 40 ms
    TEST_ASSERT_TRUE(d.update(26.0f, R, 1000));   // 960 ms: ainda não
    TEST_ASSERT_FALSE(d.update(26.0f, R, 1040));  // 1000 ms: termina
    // Uma leitura fora da faixa no meio recomeça a contagem
    TEST_ASSERT_TRUE(d.update(31.0f, R, 2000));
    TEST_ASSERT_TRUE(d.update(27.0f, R, 2040));
    TEST_ASSERT_TRUE(d.update(28.9f, R, 2800));   // entre a saída e a entrada: fora da faixa de saída
    TEST_ASSERT_TRUE(d.update(27.0f, R, 2840));
    TEST_ASSERT_TRUE(d.update(27.0f, R, 3800));   // 960 ms desde 2840
    TEST_ASSERT_FALSE(d.update(27.0f, R, 3840));
    // Virada do millis() durante a contagem
    TEST_ASSERT_TRUE(d.update(31.0f, R, 0xFFFFFE00u));
    TEST_ASSERT_TRUE(d.update(27.0f, R, 0xFFFFFF00u));
    TEST_ASSERT_TRUE(d.update(27.0f, R, 0x000002E7u));   // 999 ms
    TEST_ASSERT_FALSE(d.update(27.0f, R, 0x000002E8u));  // 1000 ms
}

// Leitura inválida ou pausa no meio da contagem: interruptExit() zera a contagem sem tirar o distúrbio
static void test_disturbed_exit_interrupted() {
    const float R = 25.7f;
    MagDisturbance d(15.0f, 10.0f, 1000);
    TEST_ASSERT_TRUE(d.update(31.0f, R, 0));
    TEST_ASSERT_TRUE(d.update(27.0f, R, 40));   // a contagem começa aos 40 ms
    d.interruptExit();                          // leitura inválida aos 500 ms (saturação, por exemplo)
    TEST_ASSERT_TRUE(d.disturbed());
    // 2 s sem leituras válidas; a primeira válida na faixa não encerra: a contagem recomeça nela
    TEST_ASSERT_TRUE(d.update(27.0f, R, 2500));
    TEST_ASSERT_TRUE(d.update(27.0f, R, 3480));
    TEST_ASSERT_FALSE(d.update(27.0f, R, 3500));
    // Sem a interrupção, a mesma sequência terminaria na primeira leitura depois da pausa
    MagDisturbance e(15.0f, 10.0f, 1000);
    e.update(31.0f, R, 0);
    e.update(27.0f, R, 40);
    TEST_ASSERT_FALSE(e.update(27.0f, R, 2500));
}

// Calibração nova: depois do reset(), o estado antigo não passa adiante. Um |m| entre 10 e 15 % do raio
// novo mantinha o distúrbio decidido com o raio antigo; com o detector reiniciado, não entra
static void test_disturbed_reset_on_new_calibration() {
    MagDisturbance d(15.0f, 10.0f, 1000);
    TEST_ASSERT_TRUE(d.update(31.0f, 25.7f, 0));     // distúrbio com o raio antigo
    TEST_ASSERT_TRUE(d.update(26.0f, 23.0f, 100));   // raio novo, 13 %: continuaria suspenso
    d.reset();
    TEST_ASSERT_FALSE(d.disturbed());
    TEST_ASSERT_FALSE(d.update(26.0f, 23.0f, 200));
    TEST_ASSERT_FALSE(d.update(26.4f, 23.0f, 300));  // 14,8 %: abaixo da entrada
}

// A segunda aproximação da bancada de 21/10/2026 (log 261021-141352): |m| das linhas do fluxo p, a ~10 Hz,
// com a chave de fenda mexendo perto do módulo. Sem tempo mínimo, 4 entradas; com 1 s, 2, e o fim sem distúrbio
static void test_disturbed_exit_hold_bench_values() {
    static const float M[] = {
        26.4f, 26.7f, 27.1f, 27.5f, 27.3f, 27.3f, 27.3f, 27.7f, 28.1f, 28.3f, 28.4f, 28.8f, 30.1f, 30.3f, 28.8f,
        28.5f, 29.1f, 29.4f, 29.8f, 29.2f, 30.0f, 29.2f, 30.7f, 30.6f, 31.8f, 30.3f, 30.1f, 30.3f, 31.3f, 32.4f,
        34.0f, 37.0f, 37.5f, 36.0f, 33.5f, 32.4f, 32.4f, 30.9f, 29.4f, 29.2f, 29.2f, 27.9f, 26.6f, 27.1f, 28.6f,
        32.6f, 29.1f, 24.3f, 23.3f, 23.2f, 24.2f, 24.5f, 25.4f, 26.0f, 27.3f, 27.7f, 27.0f, 27.0f, 27.6f, 29.9f,
        34.4f, 36.4f, 26.8f, 25.4f, 36.2f, 35.8f, 33.9f, 37.4f, 43.5f, 28.3f, 27.2f, 25.8f, 25.5f, 25.4f, 25.3f,
        25.2f, 25.3f, 25.3f, 25.1f, 25.0f, 25.0f, 24.9f,
    };
    const int N = sizeof(M) / sizeof(M[0]);
    const uint32_t holds[] = {0, 1000};
    const int expectedEntries[] = {4, 2};
    for (int h = 0; h < 2; h++) {
        MagDisturbance d(15.0f, 10.0f, holds[h]);
        int entries = 0;
        bool prev = false;
        for (int i = 0; i < N; i++) {
            const bool now = d.update(M[i], 25.7f, 100u * (uint32_t)i);
            if (now && !prev) entries++;
            prev = now;
        }
        TEST_ASSERT_EQUAL(expectedEntries[h], entries);
        TEST_ASSERT_FALSE(prev);
    }
}

// O correct() recebe o estado pronto; a ordem das suspensões continua: dado inválido, atitude degradada,
// inclinação e distúrbio
static void test_heading_correct_disturbed_flag_order() {
    HeadingFilter f(HP);
    TEST_ASSERT_EQUAL(HeadingFilter::Mag::BadData, f.correct(NAN, true, 0, 0.04f, true));
    TEST_ASSERT_EQUAL(HeadingFilter::Mag::AttitudeDegraded, f.correct(40, true, 75, 0.04f, true));
    TEST_ASSERT_EQUAL(HeadingFilter::Mag::Tilted, f.correct(40, true, 75, 0.04f));
    TEST_ASSERT_EQUAL(HeadingFilter::Mag::Disturbed, f.correct(40, true, 0, 0.04f));
    TEST_ASSERT_FALSE(f.initialized());
    TEST_ASSERT_EQUAL(HeadingFilter::Mag::Initialized, f.correct(40, false, 0, 0.04f));
}

// ---------- Filtro da proa ----------

static void test_heading_init_from_first_valid() {
    HeadingFilter f(HP);
    TEST_ASSERT_FALSE(f.initialized());
    f.predict({0, 0, 10}, 0, 0, 0.005f);   // sem referência: nada a integrar
    TEST_ASSERT_FALSE(f.initialized());
    TEST_ASSERT_EQUAL(HeadingFilter::Mag::Tilted, f.correct(40, false, 75, 0.04f));
    TEST_ASSERT_EQUAL(HeadingFilter::Mag::Disturbed, f.correct(40, true, 0, 0.04f));
    TEST_ASSERT_FALSE(f.initialized());
    TEST_ASSERT_EQUAL(HeadingFilter::Mag::Initialized, f.correct(40, false, 0, 0.04f));
    TEST_ASSERT_EQUAL_FLOAT(40.0f, f.headingDeg());
    TEST_ASSERT_EQUAL(HeadingFilter::Mag::BadData, f.correct(NAN, false, 0, 0.04f));
}

static void test_heading_step_converges_with_tau() {
    HeadingFilter f(HP);
    f.correct(10, false, 0, 0.04f);
    // Degrau de +30° no magnetômetro, correções a 25 Hz durante τ = 1 s: resta ~e^−1 do erro
    for (int i = 0; i < 25; i++) TEST_ASSERT_EQUAL(HeadingFilter::Mag::Corrected, f.correct(40, false, 0, 0.04f));
    const float k = 0.04f / (1.0f + 0.04f);
    const float expected = 40.0f - 30.0f * powf(1 - k, 25);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, expected, f.headingDeg());
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 40.0f - 30.0f * expf(-1.0f), f.headingDeg());
    for (int i = 0; i < 250; i++) f.correct(40, false, 0, 0.04f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 40.0f, f.headingDeg());
}

static void test_heading_gyro_follows_rotation() {
    HeadingFilter f(HP);
    f.correct(350, false, 0, 0.04f);
    // Nariz para a direita a 90 °/s por 1 s, nivelado (r = +90 °/s): 350° → 80°, passando por 0
    for (int i = 0; i < 200; i++) f.predict({0, 0, 90}, 0, 0, 0.005f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 80.0f, f.headingDeg());
    // Com 30° de roll, a guinada na Terra vem de q·sinφ + r·cosφ: giro só em r dá r·cos30°
    HeadingFilter g(HP);
    g.correct(0, false, 0, 0.04f);
    for (int i = 0; i < 200; i++) g.predict({0, 0, 10}, 30, 0, 0.005f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f * cosf(30 * D2R), g.headingDeg());
}

static void test_heading_short_path() {
    HeadingFilter f(HP);
    f.correct(359, false, 0, 0.04f);
    f.correct(1, false, 0, 0.2f);   // passo grande: k = 0,2/1,2
    const float d = wrap180(f.headingDeg() - 359.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 2.0f * 0.2f / 1.2f, d);   // andou para frente (+), não −358°
    TEST_ASSERT_TRUE(f.headingDeg() < 1.0f || f.headingDeg() > 359.0f);
}

static void test_heading_suspensions_keep_gyro() {
    HeadingFilter f(HP);
    f.correct(100, false, 0, 0.04f);
    // Distúrbio: o magnetômetro diz 160°, mas a correção fica suspensa
    for (int i = 0; i < 50; i++) TEST_ASSERT_EQUAL(HeadingFilter::Mag::Disturbed, f.correct(160, true, 0, 0.04f));
    TEST_ASSERT_EQUAL_FLOAT(100.0f, f.headingDeg());
    // Inclinação alta: idem; o giroscópio continua valendo
    f.predict({0, 0, 20}, 0, 0, 0.1f);
    TEST_ASSERT_EQUAL(HeadingFilter::Mag::Tilted, f.correct(160, false, -72, 0.04f));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 102.0f, f.headingDeg());
}

static void test_heading_suspended_with_degraded_attitude() {
    HeadingFilter f(HP);
    // Sem referência e com a atitude degradada: não inicializa por uma proa compensada com roll/pitch errados
    TEST_ASSERT_EQUAL(HeadingFilter::Mag::AttitudeDegraded, f.correct(40, false, 0, 0.04f, true));
    TEST_ASSERT_FALSE(f.initialized());
    TEST_ASSERT_EQUAL(HeadingFilter::Mag::Initialized, f.correct(40, false, 0, 0.04f, false));
    // Degradada: o magnetômetro diz 90°, a correção fica suspensa e o giroscópio continua valendo
    for (int i = 0; i < 50; i++) TEST_ASSERT_EQUAL(HeadingFilter::Mag::AttitudeDegraded, f.correct(90, false, 0, 0.04f, true));
    f.predict({0, 0, 10}, 0, 0, 0.1f);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 41.0f, f.headingDeg());
    // Atitude recuperada: a correção volta
    TEST_ASSERT_EQUAL(HeadingFilter::Mag::Corrected, f.correct(90, false, 0, 0.04f, false));
    TEST_ASSERT_TRUE(f.headingDeg() > 41.0f);
}

static void test_heading_gap_resets() {
    HeadingFilter f(HP);
    f.correct(100, false, 0, 0.04f);
    f.predict({0, 0, 10}, 0, 0, 0.0f);   // dt nulo: ignorado
    TEST_ASSERT_EQUAL_FLOAT(100.0f, f.headingDeg());
    f.predict({0, 0, 10}, 0, 0, 0.5f);   // lacuna acima de maxDtS: reinicia
    TEST_ASSERT_FALSE(f.initialized());
    TEST_ASSERT_EQUAL(HeadingFilter::Mag::Initialized, f.correct(200, false, 0, 0.04f));
    f.predict({0, 0, 10}, 0, 89.9f, 0.005f);   // perto da vertical: finito (guarda de 85°)
    TEST_ASSERT_TRUE(isfinite(f.headingDeg()));
}

// ---------- Calibração (MagCalibrator) ----------

static const MagCalParams CP = {180000, 2000, 4500, LIM, 10.0f};
static Vec3 g_calBuf[4500];

struct CalOut {
    MagCalibrator::Event ev;   // primeiro evento diferente de None
    uint32_t tMs;
    int samples;               // pontos entregues até o evento
};

// Entrega pontos a 25 Hz (40 ms) a partir de t0 até o primeiro evento (ou maxSamples). pt(i) dá o i-ésimo ponto.
template <typename F>
static CalOut feedUntilEvent(MagCalibrator &c, uint32_t t0, int maxSamples, F pt, int i0 = 0) {
    for (int i = 0; i < maxSamples; i++) {
        const uint32_t t = t0 + 40u * (uint32_t)i;
        const MagCalibrator::Event e = c.feed(pt(i0 + i), t);
        if (e != MagCalibrator::Event::None) return {e, t, i + 1};
    }
    return {MagCalibrator::Event::None, 0, maxSamples};
}

static const Vec3 OFF = {8.0f, 6.0f, 10.0f};
static Vec3 fixedPoint(int) { return {OFF.x + 26.0f, OFF.y, OFF.z}; }
static Vec3 spherePoint(int i) {
    const Vec3 u = fib(i % 600, 600);
    return {OFF.x + 26.0f * u.x, OFF.y + 26.0f * u.y, OFF.z + 26.0f * u.z};
}

static void test_calib_off_ignores_points() {
    MagCalibrator c(CP);
    TEST_ASSERT_TRUE(c.feed({1, 2, 3}, 0) == MagCalibrator::Event::None);
    TEST_ASSERT_TRUE(c.state() == MagCalibrator::State::Off);
    TEST_ASSERT_EQUAL_UINT32(0, c.count());
}

// Uma linha de progresso a cada 2 s, com a contagem de pontos e o que falta por face
static void test_calib_progress_every_interval() {
    MagCalibrator c(CP);
    c.start(g_calBuf, 1000);
    const CalOut o = feedUntilEvent(c, 1000, 500, fixedPoint);
    TEST_ASSERT_TRUE(o.ev == MagCalibrator::Event::Progress);
    TEST_ASSERT_EQUAL_UINT32(3000, o.tMs);
    TEST_ASSERT_EQUAL_UINT32(51, c.count());
    TEST_ASSERT_EQUAL_UINT32(2000, c.elapsedMs());
    TEST_ASSERT_EQUAL(0, c.covered());   // um ponto só: direção nula em relação ao centro
    for (int f = 0; f < MAG_FACES; f++) TEST_ASSERT_EQUAL_UINT8(4, c.missing(f));
    TEST_ASSERT_TRUE(c.state() == MagCalibrator::State::Collect);
    const CalOut o2 = feedUntilEvent(c, 3040, 500, fixedPoint);
    TEST_ASSERT_TRUE(o2.ev == MagCalibrator::Event::Progress);
    TEST_ASSERT_EQUAL_UINT32(5000, o2.tMs);
}

// Com a esfera inteira coberta, a coleta termina sozinha e o ajuste recupera o offset e o raio
static void test_calib_finishes_by_coverage() {
    MagCalibrator c(CP);
    c.start(g_calBuf, 0);
    CalOut o = {MagCalibrator::Event::None, 0, 0};
    int fed = 0;
    while (fed < 2000) {
        o = feedUntilEvent(c, 40u * (uint32_t)fed, 2000 - fed, spherePoint, fed);
        fed += o.samples;
        if (o.ev != MagCalibrator::Event::Progress) break;
    }
    TEST_ASSERT_TRUE(o.ev == MagCalibrator::Event::Finished);
    TEST_ASSERT_TRUE(c.state() == MagCalibrator::State::Review);
    TEST_ASSERT_EQUAL(MAG_SECTORS, c.covered());
    // Fim numa linha de progresso (51 pontos aos 2 s, mais 50 a cada 2 s), antes da volta completa (600)
    TEST_ASSERT_EQUAL_UINT32(1, c.count() % 50);
    TEST_ASSERT_TRUE(c.count() < 600);
    TEST_ASSERT_EQUAL(MagFitStatus::Ok, c.fit().status);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, OFF.x, c.fit().cal.offset.x);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, OFF.y, c.fit().cal.offset.y);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, OFF.z, c.fit().cal.offset.z);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 26.0f, c.fit().cal.radiusUt);
    TEST_ASSERT_TRUE(c.feed(spherePoint(0), o.tMs + 40) == MagCalibrator::Event::None);   // revisão: ignorado
}

// Sem cobertura, a coleta termina pelo tempo, e o ajuste é recusado
static void test_calib_finishes_by_time() {
    const MagCalParams p = {10000, 2000, 4500, LIM, 10.0f};
    MagCalibrator c(p);
    c.start(g_calBuf, 0);
    int progress = 0;
    CalOut o = {MagCalibrator::Event::None, 0, 0};
    int fed = 0;
    for (;;) {
        o = feedUntilEvent(c, 40u * (uint32_t)fed, 1000, fixedPoint);
        fed += o.samples;
        if (o.ev != MagCalibrator::Event::Progress) break;
        progress++;
    }
    TEST_ASSERT_EQUAL(4, progress);   // 2, 4, 6 e 8 s
    TEST_ASSERT_TRUE(o.ev == MagCalibrator::Event::Finished);
    TEST_ASSERT_EQUAL_UINT32(10000, o.tMs);
    TEST_ASSERT_TRUE(c.state() == MagCalibrator::State::Review);
    TEST_ASSERT_TRUE(c.fit().status != MagFitStatus::Ok);
}

// Buffer cheio encerra a coleta antes da próxima linha de progresso
static void test_calib_finishes_when_buffer_full() {
    const MagCalParams p = {180000, 2000, 30, LIM, 10.0f};
    MagCalibrator c(p);
    c.start(g_calBuf, 0);
    const CalOut o = feedUntilEvent(c, 0, 100, spherePoint);
    TEST_ASSERT_TRUE(o.ev == MagCalibrator::Event::Finished);
    TEST_ASSERT_EQUAL(30, o.samples);
    TEST_ASSERT_EQUAL_UINT32(30, c.count());
}

static void test_calib_stop() {
    MagCalibrator c(CP);
    c.start(g_calBuf, 0);
    feedUntilEvent(c, 0, 10, spherePoint);
    c.stop();
    TEST_ASSERT_TRUE(c.state() == MagCalibrator::State::Off);
    TEST_ASSERT_EQUAL_UINT32(0, c.count());
    TEST_ASSERT_TRUE(c.feed(spherePoint(0), 400) == MagCalibrator::Event::None);
    // Uma nova coleta começa do zero
    c.start(g_calBuf, 1000);
    TEST_ASSERT_TRUE(c.feed(spherePoint(0), 1000) == MagCalibrator::Event::None);
    TEST_ASSERT_EQUAL_UINT32(1, c.count());
}

// Virada do millis() (~49,7 dias) durante a coleta
static void test_calib_millis_wraparound() {
    MagCalibrator c(CP);
    const uint32_t t0 = 0xFFFFF830u;   // 2000 ms antes da virada: a primeira linha de progresso cai em 0
    c.start(g_calBuf, t0);
    const CalOut o = feedUntilEvent(c, t0, 500, fixedPoint);
    TEST_ASSERT_TRUE(o.ev == MagCalibrator::Event::Progress);
    TEST_ASSERT_EQUAL_UINT32(0, o.tMs);
    TEST_ASSERT_EQUAL_UINT32(2000, c.elapsedMs());
    TEST_ASSERT_EQUAL_UINT32(51, c.count());
}

// Distância mínima ao centro na contagem dos setores: abaixo dela o ponto não conta; no limite, conta
static void test_count_sectors_min_distance() {
    const Vec3 pts[] = {{5, 0, 0}, {10, 0, 0}, {-20, 0, 0}, {0, 0, 9.99f}, {NAN, 0, 0}};
    uint16_t counts[MAG_SECTORS];
    magCountSectors(pts, 5, {0, 0, 0}, counts, 10.0f);
    uint32_t sum = 0;
    for (int i = 0; i < MAG_SECTORS; i++) sum += counts[i];
    TEST_ASSERT_EQUAL_UINT32(2, sum);
    TEST_ASSERT_EQUAL_UINT16(1, counts[magSector({1, 0, 0})]);
    TEST_ASSERT_EQUAL_UINT16(1, counts[magSector({-1, 0, 0})]);
    // Sem distância mínima (a recontagem do ajuste), todos os finitos e não nulos contam
    magCountSectors(pts, 5, {0, 0, 0}, counts);
    sum = 0;
    for (int i = 0; i < MAG_SECTORS; i++) sum += counts[i];
    TEST_ASSERT_EQUAL_UINT32(4, sum);
}

// Caixa parada: ruído de ±0,5 µT em torno do offset (gerador linear congruente, determinístico)
static uint32_t g_noiseState = 1;
static float noise05() {
    g_noiseState = g_noiseState * 1664525u + 1013904223u;
    return ((g_noiseState >> 8) / 16777216.0f - 0.5f);   // [−0,5, 0,5)
}
static Vec3 stillPoint(int) { return {OFF.x + noise05(), OFF.y + noise05(), OFF.z + noise05()}; }

// O caso da pendência 3: com a caixa parada, o ruído cobria setores pelo centro provisório. Com a
// distância mínima, a cobertura fica em 0 em todas as linhas, e a coleta só termina pelo tempo
static void test_calib_still_box_no_coverage() {
    // Sem a distância mínima, o mesmo ruído cobre setores logo na primeira linha (o defeito de antes)
    {
        const MagCalParams p0 = {180000, 2000, 4500, LIM, 0.0f};
        MagCalibrator c(p0);
        g_noiseState = 1;
        c.start(g_calBuf, 0);
        const CalOut o = feedUntilEvent(c, 0, 500, stillPoint);
        TEST_ASSERT_TRUE(o.ev == MagCalibrator::Event::Progress);
        TEST_ASSERT_TRUE(c.covered() > 0);
    }
    MagCalibrator c(CP);
    g_noiseState = 1;
    c.start(g_calBuf, 0);
    int progress = 0;
    int fed = 0;
    CalOut o = {MagCalibrator::Event::None, 0, 0};
    for (;;) {
        o = feedUntilEvent(c, 40u * (uint32_t)fed, 5000, stillPoint, fed);
        fed += o.samples;
        if (o.ev != MagCalibrator::Event::Progress) break;
        progress++;
        TEST_ASSERT_EQUAL(0, c.covered());
        for (int f = 0; f < MAG_FACES; f++) TEST_ASSERT_EQUAL_UINT8(4, c.missing(f));
    }
    TEST_ASSERT_TRUE(o.ev == MagCalibrator::Event::Finished);
    TEST_ASSERT_EQUAL(89, progress);              // 2, 4, …, 178 s
    TEST_ASSERT_EQUAL_UINT32(4500, c.count());    // até o fim do tempo ou do buffer
    TEST_ASSERT_TRUE(c.fit().status != MagFitStatus::Ok);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_cra_crb_codes);
    RUN_TEST(test_decode_order_and_sign);
    RUN_TEST(test_decode_saturation_each_axis);
    RUN_TEST(test_decode_config_lost);
    RUN_TEST(test_chip_to_body_mapping);
    RUN_TEST(test_sector_assignment);
    RUN_TEST(test_coverage_minimum_points_and_faces);
    RUN_TEST(test_minmax_center);
    RUN_TEST(test_fit_recovers_offset_and_scale);
    RUN_TEST(test_fit_recovers_coupled_soft_iron);
    RUN_TEST(test_fit_sphere_equal_eigenvalues);
    RUN_TEST(test_fit_repeated_eigenvalue);
    RUN_TEST(test_fit_strongly_rotated);
    RUN_TEST(test_w_plausible);
    RUN_TEST(test_fit_partial_coverage);
    RUN_TEST(test_fit_rejects_planar_and_few);
    RUN_TEST(test_fit_rejects_bad_results);
    RUN_TEST(test_tilt_compensation_exact);
    RUN_TEST(test_flat_heading_error_with_tilt);
    RUN_TEST(test_wrap360);
    RUN_TEST(test_disturbed);
    RUN_TEST(test_disturbed_hysteresis_bench_values);
    RUN_TEST(test_disturbed_exit_hold);
    RUN_TEST(test_disturbed_exit_hold_bench_values);
    RUN_TEST(test_disturbed_exit_interrupted);
    RUN_TEST(test_disturbed_reset_on_new_calibration);
    RUN_TEST(test_heading_correct_disturbed_flag_order);
    RUN_TEST(test_heading_init_from_first_valid);
    RUN_TEST(test_heading_step_converges_with_tau);
    RUN_TEST(test_heading_gyro_follows_rotation);
    RUN_TEST(test_heading_short_path);
    RUN_TEST(test_heading_suspensions_keep_gyro);
    RUN_TEST(test_heading_suspended_with_degraded_attitude);
    RUN_TEST(test_heading_gap_resets);
    RUN_TEST(test_calib_off_ignores_points);
    RUN_TEST(test_calib_progress_every_interval);
    RUN_TEST(test_calib_finishes_by_coverage);
    RUN_TEST(test_calib_finishes_by_time);
    RUN_TEST(test_calib_finishes_when_buffer_full);
    RUN_TEST(test_calib_stop);
    RUN_TEST(test_calib_millis_wraparound);
    RUN_TEST(test_count_sectors_min_distance);
    RUN_TEST(test_calib_still_box_no_coverage);
    return UNITY_END();
}
