// attitude.h — fusão de atitude (roll e pitch) por filtro complementar, em lógica pura (sem Arduino.h),
// testável com pio test -e native. Estratégia, limites e decisões: docs/fusao.md.
//
// Eixos do modelo (aeronáuticos): x para o nariz, y para a asa direita, z para baixo.
// Roll positivo com a asa direita descendo, pitch positivo com o nariz subindo (docs/hardware.md).
// A API usa unidades físicas (g, °/s, graus, segundos); por dentro, o filtro trabalha em radianos.
#pragma once
#include <stdint.h>
#include "imu_cal.h"   // Vec3, WindowStats, RestLimits

// Eixos do chip na montagem final (X asa direita, Y nariz, Z para cima; hardware.md, 14/10/2026) →
// eixos do modelo: x = Y, y = X, z = −Z. Vale para o acelerômetro e para o giroscópio.
// (Montagem 5, descontinuada: X nariz, Y asa esquerda, Z para cima → x = X, y = −Y, z = −Z.)
Vec3 chipToBody(const Vec3 &chip);

// Intervalo entre dois instantes de micros(), atravessando a virada dos 32 bits (~71 min)
uint32_t elapsedUs(uint32_t fromUs, uint32_t toUs);

// Ângulo em graus no intervalo (−180, 180]
float wrap180(float deg);

struct Angles {
    float rollDeg;
    float pitchDeg;
};

// Roll e pitch só pelo acelerômetro. f = força específica nos eixos do modelo, em g
// (em repouso nivelado, f = (0, 0, −1)). roll = atan2(−f_y, −f_z), pitch = atan2(f_x, √(f_y² + f_z²)).
Angles accelRollPitch(const Vec3 &f);

struct FusionParams {
    float tauS;          // constante de tempo do filtro; α = τ/(τ + dt) a cada amostra
    float accFullDevG;   // ||a| − 1 g| até este valor: peso pleno do acelerômetro
    float accZeroDevG;   // a partir deste valor: peso zero (interpolação linear entre os dois)
    float rollMinG;      // projeção mínima √(f_y² + f_z²) para o roll ser observável (0,3 g ≈ |pitch| 72,5°)
    float maxDtS;        // maior intervalo integrável; acima dele o filtro é reiniciado
    uint16_t initSamples;    // amostras seguidas utilizáveis cuja média define a atitude inicial
    float    initMaxRateDps; // |giroscópio| acima disto durante a inicialização: caixa girando, recomeça
};

// Peso do acelerômetro na correção (0 a 1), pelo afastamento de |a| em relação a 1 g.
// Acelerômetro saturado: peso zero. Não detecta aceleração perpendicular à gravidade
// (0,1 g horizontal dá |a| = 1,005 g e ~5,7° de inclinação aparente): ver docs/fusao.md.
float accWeight(float accNormG, bool accSaturated, const FusionParams &p);

struct ImuFlags {
    bool accSaturated;    // suspende a correção pelo acelerômetro nesta amostra
    bool gyroSaturated;   // a predição perdeu confiança: o filtro fica "degradado"
};

class AttitudeFilter {
public:
    enum class Step : uint8_t {
        Initialized,   // atitude inicial tirada do acelerômetro nesta amostra
        Updated,
        Waiting,       // ainda sem referência (peso não pleno, roll não observável, giroscópio saturado
                       // ou girando) ou acumulando as amostras da média inicial
        Skipped,       // dt ≤ 0: amostra ignorada
        Reset,         // dt acima do limite: estado descartado, espera nova referência
        BadData,       // NaN ou infinito na entrada: amostra ignorada, estado intacto
    };

    // useAccel = false: só integra o giroscópio (diagnóstico de deriva); inicializa só por initFrom()
    explicit AttitudeFilter(const FusionParams &p, bool useAccel = true) : p_(p), useAccel_(useAccel) {}

    void reset();
    void initFrom(float rollDeg, float pitchDeg);

    // accBodyG: força específica (g) e gyroBodyDps: velocidade angular (°/s), nos eixos do modelo,
    // já calibrados e com a rotação de montagem aplicada. dtS: intervalo medido desde a amostra anterior.
    Step update(const Vec3 &accBodyG, const Vec3 &gyroBodyDps, float dtS, const ImuFlags &flags);

    bool  initialized() const { return init_; }
    float rollDeg() const;
    float pitchDeg() const;
    float lastAccWeight() const { return lastW_; }
    // O giroscópio saturou e a correção acumulada desde então (∑ peso·dt) ainda não chegou a 3τ
    bool  degraded() const { return degraded_; }

private:
    FusionParams p_;
    bool  useAccel_;
    bool  init_ = false;
    float roll_ = 0, pitch_ = 0;   // rad
    float lastW_ = 0;
    bool  degraded_ = false;
    float corrSinceSatS_ = 0;
    uint16_t initN_ = 0;           // amostras acumuladas para a média inicial
    Vec3     initSum_{0, 0, 0};
};

// Média e desvio padrão de um ângulo (graus) numa janela curta, sem o salto de ±180°: cada valor
// entra como desvio em relação ao primeiro da janela (wrap180), e a média volta a (−180, 180].
// Vale enquanto a janela variar menos de 180° (+179,9° e −179,9° dão média 180° e desvio 0,1°).
class AngleStats {
public:
    void     reset();
    void     add(float deg);
    uint32_t count() const { return n_; }
    float    mean() const;
    float    stddev() const;

private:
    uint32_t n_ = 0;
    float    ref_ = 0;
    double   sum_ = 0, sq_ = 0;   // desvios em relação a ref_
};

// ---------- Calibração de montagem ----------

// Matriz 3×3 por linhas
struct Mat3 {
    float m[3][3];
};

Mat3 mat3Identity();
Vec3 mat3Apply(const Mat3 &r, const Vec3 &v);
// Linhas unitárias e ortogonais e determinante +1, dentro de tol
bool mat3IsRotation(const Mat3 &r, float tol);
// Ângulo da rotação r (graus, 0..180), pelo traço: acos((tr − 1)/2). Só faz sentido com mat3IsRotation(r);
// NaN se r não for finita
float mat3RotationAngleDeg(const Mat3 &r);

struct MountResult {
    Mat3  r;                 // eixos nominais do modelo (chipToBody) → eixos da caixa
    float mountRollDeg;      // desalinhamento do módulo em relação à caixa
    float mountPitchDeg;
    float tableRollDeg;      // inclinação da mesa, vista com a caixa na posição 1
    float tablePitchDeg;
};

enum class MountCheck : uint8_t {
    Ok,
    BadInput,    // NaN, vetor curto ou as duas medidas apontando em direções muito diferentes
    TooTilted,   // montagem ou mesa inclinadas mais que o limite
};

// Calibração de montagem em duas posições: f1 com a caixa apoiada numa face e f2 com a mesma
// face apoiada, girada 180° no plano da mesa (forças específicas médias, eixos nominais, em g).
// Nos eixos da caixa, f1 = (gx, gy, gz) e f2 = (−gx, −gy, gz): a soma dos vetores unitários é
// paralela ao eixo z da caixa, qualquer que seja a inclinação da mesa. O nariz é o nominal projetado
// no plano perpendicular a esse eixo; o desalinhamento em yaw não é observável pela gravidade.
MountCheck mountFromTwoPoses(const Vec3 &f1, const Vec3 &f2, float maxTiltDeg, MountResult &out);

enum class LevelRest : uint8_t {
    Ok,
    TooFewSamples,
    Moving,          // desvio do giroscópio acima do limite
    Rotating,        // desvio baixo, mas média do giroscópio corrigido longe de zero (rotação lenta)
    AccOutOfRange,   // |a| corrigido fora da faixa de repouso
    Invalid,         // média ou desvio não finitos
};

// Repouso para a calibração de montagem: o critério da calibração do giroscópio mais a média do
// giroscópio corrigido. Desvio baixo e médias estáveis também aceitariam uma rotação lenta constante.
LevelRest checkLevelWindow(const WindowStats &gyroRaw, const Vec3 &gyroBias, const WindowStats &accCorrected,
                           uint32_t minSamples, const RestLimits &lim, float maxMeanDps);
