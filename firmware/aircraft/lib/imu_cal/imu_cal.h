// imu_cal.h — calibração do MPU6050 em lógica pura (sem Arduino.h), testável com pio test -e native.
// Trabalha nos eixos do chip e em unidades físicas: aceleração em g, velocidade angular em °/s.
// Estratégia: docs/calibracao.md. Evidência de bancada: docs/hardware.md.
#pragma once
#include <stdint.h>

struct Vec3 {
    float x, y, z;
};

float norm(const Vec3 &v);

// Média e desvio padrão (populacional) de 3 eixos, calculados de forma incremental (Welford)
class WindowStats {
public:
    void     reset();
    void     add(const Vec3 &v);
    uint32_t count() const { return n_; }
    Vec3     mean() const;
    Vec3     stddev() const;
    float    maxStddev() const;   // maior desvio entre os três eixos

private:
    uint32_t n_ = 0;
    double   mean_[3] = {0, 0, 0};
    double   m2_[3] = {0, 0, 0};
};

// ---------- Acelerômetro: seis posições ----------

// Correção por eixo: a = (bruto − offset) / escala. offset em g, escala adimensional (≈ 1)
struct AccelCal {
    Vec3 offset{0, 0, 0};
    Vec3 scale{1, 1, 1};
};

Vec3 applyAccelCal(const AccelCal &cal, const Vec3 &raw);

// Faixa de escala aceita: fora dela, posições trocadas, dados inválidos ou registro corrompido
static const float ACCEL_SCALE_MIN = 0.5f;
static const float ACCEL_SCALE_MAX = 1.5f;

// Coeficientes utilizáveis: todos finitos, escala em ACCEL_SCALE_MIN..MAX e |offset| ≤ offsetMaxG
// em cada eixo. Conferido ao carregar da NVS: uma escala zero ou um NaN inutilizaria a fusão.
bool accelCalPlausible(const AccelCal &cal, float offsetMaxG);

// Mesma calibração do acelerômetro (comparação exata: os valores vêm do mesmo registro da NVS).
// Duas ausências também são a mesma; uma presente e outra ausente, não.
bool sameAccelCal(bool aValid, const AccelCal &a, bool bValid, const AccelCal &b);

// Posições na ordem em que o firmware as pede; cada uma aponta um eixo do chip para cima (+1 g)
enum class Pose : uint8_t { ZUp, ZDown, XUp, XDown, YUp, YDown, Count };
static const int POSE_COUNT = (int)Pose::Count;

const char *poseName(Pose p);   // descrição para o operador (eixos do modelo)

struct PoseLimits {
    float dominantMinG;   // leitura do eixo esperado, já com o sinal da posição
    float dominantMaxG;
};

enum class PoseCheck : uint8_t {
    Ok,
    WrongAxis,   // outro eixo domina, ou o eixo esperado está com o sinal invertido
    WeakAxis,    // eixo certo, mas fora da faixa esperada (inclinação grande ou sensor com defeito)
};

// Confere se a média medida corresponde à posição pedida
PoseCheck checkPose(Pose p, const Vec3 &mean, const PoseLimits &lim);

// Calcula offset e escala a partir das médias das seis posições (índice = Pose).
// Para cada eixo: offset = (a₊ + a₋) / 2, escala = (a₊ − a₋) / 2. Devolve false se alguma
// escala sair de ACCEL_SCALE_MIN..MAX (posições trocadas ou dados inválidos).
bool sixPositionCal(const Vec3 means[POSE_COUNT], AccelCal &out);

// Validação antes de gravar: |a| corrigido de cada posição dentro de minG..maxG.
// Pega posições muito inclinadas, que passam por checkPose e pelas escalas permitidas, mas
// subestimam a escala (inclinação de 20° → |a| ≈ 1,064 g). normsOut recebe |a| de cada posição.
bool validateAccelCal(const AccelCal &cal, const Vec3 means[POSE_COUNT], float minG, float maxG,
                      float normsOut[POSE_COUNT]);

// ---------- Giroscópio: bias em repouso ----------

struct RestLimits {
    float gyroMaxStdDps;   // desvio máximo de cada eixo do giroscópio
    float accMinG;         // faixa de |a| corrigido
    float accMaxG;
};

enum class RestCheck : uint8_t {
    Ok,
    TooFewSamples,   // janela incompleta (falhas de leitura ou descarte)
    Moving,          // desvio do giroscópio acima do limite
    AccOutOfRange,   // |a| fora da faixa: placa acelerando ou acelerômetro sem calibração
};

struct GyroBiasResult {
    RestCheck status;
    Vec3      bias;          // média do giroscópio na janela (válida mesmo se recusada, para diagnóstico)
    float     gyroMaxStd;    // °/s
    float     accNorm;       // |média do acelerômetro corrigido|, g
    Vec3      accMean;       // média do acelerômetro corrigido: direção da gravidade na janela
};

// Avalia uma janela: estatísticas do giroscópio bruto e do acelerômetro já corrigido
GyroBiasResult evaluateGyroWindow(const WindowStats &gyro, const WindowStats &accCorrected,
                                  uint32_t minSamples, const RestLimits &lim);

// Confirmação do bias por duas janelas seguidas em repouso com médias parecidas.
// O desvio padrão sozinho não basta: vibração (oscila em torno de zero) aumenta o desvio sem
// deslocar a média, e a acomodação lenta da caixa desloca a média com desvio baixo
// (bancada, 11/10/2026: janela com desvio 0,065 °/s e erro de bias ~0,08 °/s).
// Uma rotação constante passa pelos dois critérios do giroscópio (desvio baixo, média estável) e
// seria aceita como bias. Em torno de um eixo não vertical, ela inclina a gravidade de ω·T entre
// as janelas (T = duração da janela), e por isso a direção da média do acelerômetro também é
// comparada. Rotação em torno do eixo vertical não muda a gravidade: a IMU sozinha não a distingue
// de bias (revisão de 13/10/2026; docs/calibracao.md).
class BiasConfirmer {
public:
    enum class Result : uint8_t {
        Rejected,       // janela reprovada no critério de repouso; candidata descartada
        Candidate,      // primeira janela ok: aguarda a próxima para comparar
        Inconsistent,   // médias do giroscópio ou direções da gravidade diferem mais que o limite:
                        // a mais recente vira a nova candidata
        Accepted,       // duas janelas consistentes: bias = média das duas
    };
    void   reset() { hasCandidate_ = false; }
    Result feed(const GyroBiasResult &window, float maxDeltaDps, float maxTiltDeg);
    Vec3   bias() const { return accepted_; }         // válido depois de Accepted
    float  lastDelta() const { return lastDelta_; }    // maior diferença entre eixos, °/s
    float  lastTilt() const { return lastTiltDeg_; }   // ângulo entre as direções da gravidade, graus

private:
    bool  hasCandidate_ = false;
    Vec3  candidate_{0, 0, 0};
    Vec3  candidateAcc_{0, 0, 0};
    Vec3  accepted_{0, 0, 0};
    float lastDelta_ = 0;
    float lastTiltDeg_ = 0;
};

Vec3 applyGyroBias(const Vec3 &raw, const Vec3 &bias);

// Ângulo entre dois vetores, em graus (atan2 do produto vetorial e escalar: preciso também para
// ângulos pequenos, onde acos perde resolução em float). 0 se algum for nulo.
float angleBetweenDeg(const Vec3 &a, const Vec3 &b);

// Bias utilizável: finito e com |bias| ≤ maxDps em cada eixo (conferido ao carregar da NVS)
bool gyroBiasPlausible(const Vec3 &bias, float maxDps);

// Temperatura atual longe da temperatura da calibração?
bool tempDrifted(float calTempC, float nowTempC, float maxDeltaC);
