// cal_flow.h — fluxos da calibração do MPU6050 em lógica pura (sem Arduino.h), testáveis com
// pio test -e native. Não imprimem, não leem o relógio e não acessam a NVS: os instantes (micros)
// vêm de quem chama, e o serviço (lib/cal_service) imprime e grava a partir dos eventos.
// Docs: docs/calibracao.md e docs/refatoracao.md (passo 6a).
#pragma once
#include <stdint.h>
#include "imu_cal.h"
#include "attitude.h"

// Modo da calibração: no máximo um fluxo por vez, porque todos usam a mesma janela
enum class CalMode : uint8_t {
    None, GyroBoot, GyroCmd, SixWait, SixMeasure, SixReview, LevelWait, LevelMeasure, LevelReview
};

// Janela de calibração em andamento (giroscópio, uma das seis posições ou uma medida da montagem)
struct CalWindow {
    bool        active = false;
    uint32_t    startUs = 0;
    WindowStats gyro, accRaw, accCorr;
    float       tempSum = 0;
};

struct GyroCalParams {
    uint32_t   minSamples;    // amostras mínimas para a janela valer
    RestLimits rest;          // repouso com o acelerômetro calibrado
    RestLimits restRaw;       // idem, sem calibração (|a| ≈ 1,10 g em repouso: faixa larga)
    float      maxDeltaDps;   // confirmação: diferença máxima do bias entre as duas janelas
    float      maxTiltDeg;    // e da direção da gravidade
};

enum class GyroCalKind : uint8_t { Boot, Cmd };

// Calibração do giroscópio: janelas avaliadas pelo critério de repouso e bias confirmado por duas
// janelas consistentes (BiasConfirmer). Na recusa, a do boot tenta de novo; a do g mantém o bias
// anterior e volta para a do boot se ainda não houver bias deste boot. Guarda o bias em uso.
class GyroCalFlow {
public:
    enum class Event : uint8_t {
        Accepted,                  // bias deste boot: bias(), biasTempC()
        Candidate,                 // primeira janela ok: a próxima confirma
        Inconsistent,              // janelas diferentes: a última vira a candidata
        RejectedRetry,             // boot recusado: tenta de novo
        RejectedKept,              // g recusado: bias anterior (deste boot) mantido
        RejectedKeptBootRestart,   // g recusado sem bias deste boot: volta a calibração do boot
    };

    explicit GyroCalFlow(const GyroCalParams &p) : p_(p) {}
    // Bias salvo de antes (NVS), em uso até a calibração deste boot ser aceita
    void setProvisional(const Vec3 &bias, float tempC);
    void start(GyroCalKind kind);
    // Uma janela completa. Em Candidate, Inconsistent e nas recusas que continuam, vem outra janela.
    Event finish(const CalWindow &w, bool accelValid);
    // A candidata é de antes da reconfiguração do MPU
    void resetConfirmation() { confirm_.reset(); }

    GyroCalKind kind() const { return kind_; }
    const Vec3 &bias() const { return bias_; }
    float biasTempC() const { return biasTempC_; }
    bool definitive() const { return definitive_; }   // bias aceito neste boot (equivale a ST_GYRO_CAL)
    // Última janela avaliada (diagnóstico das mensagens)
    const GyroBiasResult &lastWindow() const { return last_; }
    uint32_t lastCount() const { return lastCount_; }
    float lastDelta() const { return confirm_.lastDelta(); }
    float lastTilt() const { return confirm_.lastTilt(); }

private:
    GyroCalParams p_;
    GyroCalKind   kind_ = GyroCalKind::Boot;
    BiasConfirmer confirm_;
    float         candTempC_ = 0;   // temperatura da janela candidata
    Vec3          bias_{0, 0, 0};
    float         biasTempC_ = 0;
    bool          definitive_ = false;
    GyroBiasResult last_ = {RestCheck::TooFewSamples, {0, 0, 0}, 0, 0, {0, 0, 0}};
    uint32_t      lastCount_ = 0;
};

struct SixPoseParams {
    uint32_t   minSamples;      // amostras mínimas para a janela valer
    float      gyroMaxStdDps;   // caixa parada: desvio de cada eixo do giroscópio abaixo disto
    PoseLimits pose;            // eixo esperado de cada posição
    float      validMinG;       // validação: |a| corrigido em cada posição
    float      validMaxG;
};

// Calibração do acelerômetro em seis posições (docs/calibracao.md): uma janela por posição, com o
// acelerômetro bruto; depois da sexta, o ajuste e a validação. Uma posição recusada é pedida de novo.
class SixPoseFlow {
public:
    enum class Event : uint8_t {
        Incomplete,       // janela com poucas amostras: repete a posição
        Moved,            // caixa mexida: repete
        WrongPose,        // média não corresponde à posição (lastPoseCheck()): repete
        PoseOk,           // posição aceita; index() é a próxima
        ScaleFail,        // sexta aceita, mas escalas fora da faixa: descartada
        ValidationFail,   // sexta aceita, mas |a| corrigido fora da faixa (poseNorm()): descartada
        ReadyToSave,      // sexta aceita e validada: result(), poseNorm() e tempC() para gravar
    };

    explicit SixPoseFlow(const SixPoseParams &p) : p_(p) {}
    void start();
    Event finish(const CalWindow &w);

    int index() const { return idx_; }   // posição pedida (0..POSE_COUNT-1)
    // Última janela avaliada (diagnóstico das mensagens)
    const Vec3 &lastMean() const { return lastMean_; }
    uint32_t lastCount() const { return lastCount_; }
    float lastGyroStd() const { return lastGyroStd_; }
    PoseCheck lastPoseCheck() const { return lastPose_; }
    // Resultado (depois da sexta posição)
    const AccelCal &result() const { return result_; }
    float poseNorm(int pose) const { return norms_[pose]; }   // |a| corrigido de cada posição
    bool normOk(int pose) const { return norms_[pose] >= p_.validMinG && norms_[pose] <= p_.validMaxG; }
    float tempC() const { return tempSum_ / POSE_COUNT; }

private:
    SixPoseParams p_;
    int      idx_ = 0;
    Vec3     means_[POSE_COUNT] = {};
    float    tempSum_ = 0;
    Vec3     lastMean_{0, 0, 0};
    uint32_t lastCount_ = 0;
    float    lastGyroStd_ = 0;
    PoseCheck lastPose_ = PoseCheck::Ok;
    AccelCal result_ = {};
    float    norms_[POSE_COUNT] = {};
};

struct MountParams {
    uint32_t   minSamples;
    RestLimits rest;              // repouso, com o acelerômetro corrigido
    float      gyroMaxMeanDps;    // média do giroscópio corrigido abaixo disto (rotação lenta)
    float      maxTiltDeg;        // desalinhamento do módulo e inclinação da mesa aceitos
};

// Calibração de montagem (z, docs/fusao.md): duas medidas com a caixa em pé, a segunda girada 180°
// no plano da mesa. Uma medida recusada é pedida de novo.
class MountFlow {
public:
    enum class Event : uint8_t {
        Rejected,      // caixa fora do repouso (lastRest()): repete a medida
        PoseOk,        // medida aceita (lastAngles()); index() é a próxima
        BadInput,      // segunda aceita, mas medidas incoerentes: descartada
        TooTilted,     // segunda aceita, mas acima do limite (result() para a mensagem): descartada
        ReadyToSave,   // result() e tempC() para gravar
    };

    explicit MountFlow(const MountParams &p) : p_(p) {}
    void start();
    // gyroBias: bias em uso (o critério de rotação lenta usa o giroscópio corrigido)
    Event finish(const CalWindow &w, const Vec3 &gyroBias);

    int index() const { return idx_; }   // medida pedida (0 ou 1)
    LevelRest lastRest() const { return lastRest_; }
    float lastGyroStd() const { return lastGyroStd_; }
    const Vec3 &lastGyroMean() const { return lastGyroMean_; }   // corrigido
    float lastAccNorm() const { return lastAccNorm_; }
    const Angles &lastAngles() const { return lastAngles_; }     // sem correção de montagem
    const MountResult &result() const { return result_; }
    float tempC() const { return tempSum_ / 2; }

private:
    MountParams p_;
    int       idx_ = 0;
    Vec3      f_[2] = {};
    float     tempSum_ = 0;
    LevelRest lastRest_ = LevelRest::Ok;
    float     lastGyroStd_ = 0;
    Vec3      lastGyroMean_{0, 0, 0};
    float     lastAccNorm_ = 0;
    Angles    lastAngles_ = {0, 0};
    MountResult result_ = {};
};

struct CalCoordinatorParams {
    uint32_t      windowUs;   // duração de cada janela
    GyroCalParams gyro;
    SixPoseParams six;
    MountParams   mount;
};

// Pré-requisitos da calibração de montagem (z)
enum class MountStart : uint8_t {
    Ok,
    Busy,              // outra calibração em andamento
    NoAccel,           // acelerômetro sem calibração das seis posições
    GyroProvisional,   // sem bias do giroscópio deste boot
};

// Coordenador das calibrações: o modo, a janela compartilhada, os comandos dos fluxos e as regras de
// bloqueio (m liberado durante a calibração do boot; z com o acelerômetro calibrado e o bias deste boot).
// A janela que começa depois de uma mensagem fica pendente: quem chama imprime e então a abre com
// startPendingWindow(), para o tempo da impressão não sair da janela (como antes da refatoração).
class CalCoordinator {
public:
    enum class Done : uint8_t { None, Gyro, Six, Level };

    explicit CalCoordinator(const CalCoordinatorParams &p)
        : p_(p), gyro_(p.gyro), six_(p.six), mount_(p.mount) {}

    CalMode mode() const { return mode_; }
    const CalWindow &window() const { return w_; }
    bool windowPending() const { return pending_; }
    void startPendingWindow(uint32_t nowUs) {
        if (pending_) startWindow(nowUs);
    }
    // A janela pendente é a calibração do boot recomeçando (depois das seis posições)
    bool gyroRestartPending() const { return pending_ && mode_ == CalMode::GyroBoot; }

    // Comandos
    void startGyro(GyroCalKind kind, uint32_t nowUs);   // boot e g (janela aberta já)
    void startSix();                                    // 6: interrompe a calibração do giroscópio em andamento
    MountStart startMount(bool accelValid);             // z: começa só com Ok
    // Enter na espera de uma posição ou medida: passa a medir (janela pendente). false fora da espera.
    bool confirm();
    // Sai das seis posições ou da montagem (x em qualquer etapa; s ou x na revisão). Das seis posições,
    // retoma a calibração do boot se ainda não há bias deste boot (gyroRestartPending()).
    void exitFlow();

    // Uma amostra válida (fora do descarte), com o acelerômetro bruto e o corrigido. tUs: instante da
    // amostra (fecha a janela). O evento do fluxo cuja janela terminou fica em lastGyroEvent(),
    // lastSixEvent() ou lastMountEvent(), e a janela seguinte, se houver, fica pendente.
    Done feed(const Vec3 &gyro, const Vec3 &accRaw, const Vec3 &accCorr, float tempC, uint32_t tUs, bool accelValid);
    GyroCalFlow::Event lastGyroEvent() const { return lastEvent_; }
    SixPoseFlow::Event lastSixEvent() const { return lastSix_; }
    MountFlow::Event lastMountEvent() const { return lastMount_; }
    // MPU reconfigurado: a candidata do giroscópio é descartada e a janela ativa recomeça
    void onMpuReconfigured(uint32_t nowUs);
    // Descarte de partida do MPU: a janela só começa a contar depois dele
    void holdWindowStart(uint32_t tUs) {
        if (w_.active) w_.startUs = tUs;
    }

    bool magAllowed() const { return mode_ == CalMode::None || mode_ == CalMode::GyroBoot; }
    MountStart checkMount(bool accelValid) const;

    GyroCalFlow &gyro() { return gyro_; }
    const GyroCalFlow &gyro() const { return gyro_; }
    const SixPoseFlow &six() const { return six_; }
    const MountFlow &mount() const { return mount_; }

private:
    void startWindow(uint32_t nowUs);
    Done finishSix();
    Done finishMount();
    void endSix();

    CalCoordinatorParams p_;
    GyroCalFlow gyro_;
    SixPoseFlow six_;
    MountFlow   mount_;
    CalMode     mode_ = CalMode::None;
    CalWindow   w_;
    bool        pending_ = false;
    GyroCalFlow::Event lastEvent_ = GyroCalFlow::Event::Candidate;
    SixPoseFlow::Event lastSix_ = SixPoseFlow::Event::PoseOk;
    MountFlow::Event   lastMount_ = MountFlow::Event::PoseOk;
};
