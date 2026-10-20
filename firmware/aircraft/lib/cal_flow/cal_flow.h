// cal_flow.h — fluxos da calibração do MPU6050 em lógica pura (sem Arduino.h), testáveis com
// pio test -e native. Não imprimem, não leem o relógio e não acessam a NVS: os instantes (micros)
// vêm de quem chama, e o serviço (lib/cal_service) imprime e grava a partir dos eventos.
// Docs: docs/calibracao.md e docs/refatoracao.md (passo 6a).
#pragma once
#include <stdint.h>
#include "imu_cal.h"

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

struct CalCoordinatorParams {
    uint32_t      windowUs;   // duração de cada janela
    GyroCalParams gyro;
};

// Pré-requisitos da calibração de montagem (z)
enum class MountStart : uint8_t {
    Ok,
    Busy,              // outra calibração em andamento
    NoAccel,           // acelerômetro sem calibração das seis posições
    GyroProvisional,   // sem bias do giroscópio deste boot
};

// Coordenador das calibrações: o modo, a janela compartilhada e as regras de bloqueio
// (m liberado durante a calibração do boot; z com o acelerômetro calibrado e o bias deste boot).
// Até o passo 6b, as seis posições e a montagem usam setMode()/startWindow()/cancelWindow() e
// tratam o fim da janela fora daqui.
class CalCoordinator {
public:
    enum class Done : uint8_t { None, Gyro, Six, Level };

    explicit CalCoordinator(const CalCoordinatorParams &p) : p_(p), gyro_(p.gyro) {}

    CalMode mode() const { return mode_; }
    void setMode(CalMode m) { mode_ = m; }
    const CalWindow &window() const { return w_; }
    void startWindow(uint32_t nowUs);
    void cancelWindow() {
        w_.active = false;
        pending_ = false;
    }
    // Próxima janela do giroscópio, deixada pendente por feed() e endSix(): quem chama a abre depois de
    // imprimir as mensagens, para o tempo da impressão não sair da janela (como antes da refatoração)
    bool windowPending() const { return pending_; }
    void startPendingWindow(uint32_t nowUs) {
        if (pending_) startWindow(nowUs);
    }
    // Descarte de partida do MPU: a janela só começa a contar depois dele
    void holdWindowStart(uint32_t tUs) {
        if (w_.active) w_.startUs = tUs;
    }

    void startGyro(GyroCalKind kind, uint32_t nowUs);
    // Uma amostra válida (fora do descarte), com o acelerômetro bruto e o corrigido. tUs: instante da
    // amostra (fecha a janela). Em Done::Gyro, lastGyroEvent() diz o que aconteceu e, se o fluxo
    // continua, a próxima janela fica pendente (startPendingWindow); em Six e Level, window() tem a
    // janela que terminou.
    Done feed(const Vec3 &gyro, const Vec3 &accRaw, const Vec3 &accCorr, float tempC, uint32_t tUs, bool accelValid);
    GyroCalFlow::Event lastGyroEvent() const { return lastEvent_; }
    // Fim das seis posições (gravando ou não): true se a calibração do boot recomeçou (janela pendente)
    bool endSix();
    // MPU reconfigurado: a candidata do giroscópio é descartada e a janela ativa recomeça
    void onMpuReconfigured(uint32_t nowUs);

    bool magAllowed() const { return mode_ == CalMode::None || mode_ == CalMode::GyroBoot; }
    MountStart checkMount(bool accelValid) const;

    GyroCalFlow &gyro() { return gyro_; }
    const GyroCalFlow &gyro() const { return gyro_; }

private:
    CalCoordinatorParams p_;
    GyroCalFlow gyro_;
    CalMode     mode_ = CalMode::None;
    CalWindow   w_;
    bool        pending_ = false;
    GyroCalFlow::Event lastEvent_ = GyroCalFlow::Event::Candidate;
};
