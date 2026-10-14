// mag.h — magnetômetro em lógica pura (sem Arduino.h), testável com pio test -e native.
// Decodificação da rajada do HMC5883L, mapeamento de eixos, calibração hard/soft iron por ajuste de
// elipsoide alinhado aos eixos, cobertura da esfera, proa compensada pela inclinação e filtro da proa
// com o giroscópio. Estratégia: docs/magnetometro.md. Evidência de bancada: docs/hardware.md.
//
// Eixos do modelo como em lib/attitude: x para o nariz, y para a asa direita, z para baixo.
// Proa positiva com o nariz virando para a direita: 0° no norte magnético, 90° no leste.
// Campo magnético em µT fora do driver.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "attitude.h"   // Vec3, Mat3, wrap180

// ---------- HMC5883L: registradores ----------

// Rajada lida a partir de 0x00: CRA, CRB, modo e os dados em X, Z, Y (big-endian, nessa ordem).
// A configuração vem junto com os dados, na mesma transação.
static const uint8_t HMC_BURST_LEN = 9;
static const int16_t HMC_OVERFLOW = -4096;   // valor gravado no eixo saturado

// Códigos dos registradores a partir dos parâmetros físicos; false se o valor não for suportado.
// CRA: média de 1, 2, 4 ou 8 amostras; saída de 0,75, 1,5, 3, 7,5, 15, 30 ou 75 Hz; sem polarização.
bool hmcCraCode(uint8_t avgSamples, float outputHz, uint8_t &cra);
// CRB: faixa em mGa (880, 1300, 1900, 2500, 4000, 4700, 5600 ou 8100) e o ganho correspondente
bool hmcCrbCode(uint16_t rangeMga, uint8_t &crb, float &lsbPerGa);
static const uint8_t HMC_MODE_CONTINUOUS = 0x00;

enum class HmcFrame : uint8_t {
    Ok,
    ConfigLost,   // CRA, CRB ou modo diferentes do configurado: o chip reiniciou (queda de alimentação)
    Saturated,    // algum eixo em −4096 (campo acima da faixa)
};

// Decodifica a rajada. counts em X, Y, Z do chip, em LSB (preenchido também quando saturado).
HmcFrame hmcDecode(const uint8_t raw[HMC_BURST_LEN], uint8_t cra, uint8_t crb, uint8_t mode, int16_t counts[3]);

// Contagens → µT (1 Ga = 100 µT), ainda nos eixos do chip
Vec3 hmcCountsToUt(const int16_t counts[3], float lsbPerGa);

// Eixos do chip do GY-273 (X nariz, Y asa esquerda, Z para cima) → eixos do modelo: x = X, y = −Y, z = −Z.
// Levantado na bancada (hardware.md, mapeamento de eixos do GY-273).
Vec3 magChipToBody(const Vec3 &chip);

// ---------- Calibração: hard iron e soft iron ----------

// Correção m = W · (bruto − offset), nos eixos do modelo. No ajuste alinhado aos eixos, W é diagonal;
// a matriz completa fica para um ajuste de elipsoide com acoplamentos (docs/magnetometro.md, D4).
struct MagCal {
    Vec3  offset{0, 0, 0};    // µT
    Mat3  w = mat3Identity();
    float radiusUt = 0;       // |m| corrigido esperado; 0 = sem calibração (sem detecção de distúrbio)
};

Vec3 magApplyCal(const MagCal &cal, const Vec3 &raw);

// Cobertura: a esfera de direções é dividida em 24 setores. A face do cubo do eixo dominante
// (+x, −x, +y, −y, +z, −z) é dividida em 4 quadrantes pelos sinais dos outros dois eixos.
static const int MAG_SECTORS = 24;
static const int MAG_FACES = 6;

// Setor da direção d (0..23), ou −1 para um vetor nulo ou não finito
int magSector(const Vec3 &d);
const char *magFaceName(int face);   // "+x", "-x", "+y", "-y", "+z", "-z"

// Pontos por setor, com a direção tomada em relação a center
void magCountSectors(const Vec3 *pts, size_t n, const Vec3 &center, uint16_t counts[MAG_SECTORS]);
// Setores com pelo menos minPts pontos
int magSectorsCovered(const uint16_t counts[MAG_SECTORS], uint16_t minPts);
// Setores ainda sem cobertura em cada face (0 a 4)
void magMissingByFace(const uint16_t counts[MAG_SECTORS], uint16_t minPts, uint8_t missing[MAG_FACES]);

// Centro provisório: meio do mín./máx. de cada eixo (converge depressa mesmo com hard iron grande)
class MagMinMax {
public:
    void reset();
    void add(const Vec3 &p);
    bool empty() const { return n_ == 0; }
    Vec3 center() const;

private:
    uint32_t n_ = 0;
    Vec3     lo_{0, 0, 0}, hi_{0, 0, 0};
};

struct MagFitLimits {
    float    minRadiusUt, maxRadiusUt;   // raio ajustado aceito
    float    maxResidPct;                // resíduo RMS de |m| corrigido, em % do raio
    float    minScale, maxScale;         // escalas normalizadas para média 1
    uint16_t minSectors;                 // setores cobertos, recontados com o centro ajustado
    uint16_t minPtsSector;               // pontos para um setor contar
};

enum class MagFitStatus : uint8_t {
    Ok,
    TooFewPoints,
    Singular,           // sistema 6×6 sem solução (pontos num plano, por exemplo)
    NotEllipsoid,       // coeficiente quadrático não positivo: a forma ajustada não é um elipsoide
    Coverage,           // menos setores cobertos que o mínimo
    RadiusOutOfRange,
    ScaleOutOfRange,
    ResidualTooHigh,
};

const char *magFitStatusText(MagFitStatus s);

struct MagFitResult {
    MagFitStatus status = MagFitStatus::TooFewPoints;
    MagCal   cal;                 // válido só com status Ok (preenchido sempre que o ajuste existe)
    Vec3     scale{1, 1, 1};      // semieixos normalizados para média 1 (informativo)
    float    residPct = 0;        // resíduo RMS de |m| corrigido, % do raio
    int      sectors = 0;         // setores cobertos em relação ao centro ajustado
};

// Ajuste de A·x² + B·y² + C·z² + D·x + E·y + F·z = 1 por mínimos quadrados, com todos os pontos
// (µT, eixos do modelo, sem calibração). Os pontos são centralizados e normalizados antes do ajuste
// para o sistema não ficar mal condicionado. W = diag(R/rᵢ), com R = média geométrica dos semieixos.
MagFitStatus magFitEllipsoid(const Vec3 *pts, size_t n, const MagFitLimits &lim, MagFitResult &out);

// ---------- Proa ----------

float wrap360(float deg);   // [0, 360)

// Proa sem compensação (caixa considerada nivelada): atan2(−m_y, m_x)
float magFlatHeadingDeg(const Vec3 &m);

struct MagTilt {
    float headingDeg;   // proa magnética compensada, 0..360
    float dipDeg;       // inclinação magnética medida, positiva com o campo apontando para baixo
};

// Compensação de inclinação: m nos eixos do modelo, já calibrado; roll e pitch da fusão.
// Xh = mx·cosθ + my·sinφ·sinθ + mz·cosφ·sinθ, Yh = my·cosφ − mz·sinφ, proa = atan2(−Yh, Xh).
MagTilt magTiltCompensate(const Vec3 &m, float rollDeg, float pitchDeg);

// |m| afastado do raio da calibração mais que pct %. Sem calibração (raio 0), nunca.
bool magDisturbed(float normUt, float refRadiusUt, float pct);

struct HeadingParams {
    float tauS;          // constante de tempo da correção pelo magnetômetro
    float maxPitchDeg;   // acima disto, sem correção (proa mal definida perto de ±90°)
    float disturbPct;    // desvio de |m| que suspende a correção
    float maxDtS;        // maior intervalo integrável na predição; na correção, limita o passo
};

// Filtro complementar da proa: predição pelo giroscópio a cada amostra do MPU e correção lenta pela
// proa do magnetômetro, pelo caminho curto (359° → 1° são +2°).
class HeadingFilter {
public:
    enum class Mag : uint8_t {
        Initialized,   // primeira proa válida: o filtro parte dela
        Corrected,
        Tilted,        // |pitch| acima do limite: correção suspensa
        Disturbed,     // |m| fora do raio da calibração: correção suspensa
        BadData,       // valor não finito: ignorado
    };

    explicit HeadingFilter(const HeadingParams &p) : p_(p) {}

    void reset();
    // Taxa de guinada no referencial da Terra: ψ̇ = (q·sinφ + r·cosφ)/cosθ, com gyroBodyDps nos eixos
    // do modelo. dt acima de maxDtS (ou não finito) reinicia o filtro; dt ≤ 0 é ignorado.
    void predict(const Vec3 &gyroBodyDps, float rollDeg, float pitchDeg, float dtS);
    // dtS: intervalo desde a leitura anterior do magnetômetro (limitado a maxDtS no ganho)
    Mag correct(float magHeadingDeg, float normUt, float refRadiusUt, float pitchDeg, float dtS);

    bool  initialized() const { return init_; }
    float headingDeg() const { return psi_; }

private:
    HeadingParams p_;
    bool  init_ = false;
    float psi_ = 0;   // graus, 0..360
};
