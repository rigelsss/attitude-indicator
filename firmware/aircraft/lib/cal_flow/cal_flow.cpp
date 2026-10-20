// cal_flow.cpp — fluxos da calibração do MPU6050 (lógica pura). Ver cal_flow.h.
#include "cal_flow.h"
#include <math.h>

// ---------- GyroCalFlow ----------

void GyroCalFlow::setProvisional(const Vec3 &bias, float tempC) {
    bias_ = bias;
    biasTempC_ = tempC;
}

void GyroCalFlow::start(GyroCalKind kind) {
    kind_ = kind;
    confirm_.reset();
}

GyroCalFlow::Event GyroCalFlow::finish(const CalWindow &w, bool accelValid) {
    // Sem calibração do acelerômetro, |a| ≈ 1,10 g em repouso: vale uma faixa larga, que ainda recusa
    // um vetor nulo (sem direção da gravidade para comparar entre as janelas)
    last_ = evaluateGyroWindow(w.gyro, w.accCorr, p_.minSamples, accelValid ? p_.rest : p_.restRaw);
    lastCount_ = w.gyro.count();
    const float tempC = w.tempSum / fmaxf(1, w.gyro.count());

    switch (confirm_.feed(last_, p_.maxDeltaDps, p_.maxTiltDeg)) {
    case BiasConfirmer::Result::Accepted:
        bias_ = confirm_.bias();
        biasTempC_ = (candTempC_ + tempC) / 2;
        definitive_ = true;
        return Event::Accepted;
    case BiasConfirmer::Result::Candidate:
        candTempC_ = tempC;   // a janela que acabou é a candidata; a próxima confirma ou não
        return Event::Candidate;
    case BiasConfirmer::Result::Inconsistent:
        candTempC_ = tempC;
        return Event::Inconsistent;
    case BiasConfirmer::Result::Rejected:
        break;
    }

    if (kind_ == GyroCalKind::Boot) {
        start(GyroCalKind::Boot);   // tenta de novo, sem bloquear; o bias provisório segue em uso
        return Event::RejectedRetry;
    }
    if (definitive_) return Event::RejectedKept;
    start(GyroCalKind::Boot);
    return Event::RejectedKeptBootRestart;
}

// ---------- CalCoordinator ----------

void CalCoordinator::startWindow(uint32_t nowUs) {
    w_ = CalWindow();
    w_.active = true;
    w_.startUs = nowUs;
    pending_ = false;
}

void CalCoordinator::startGyro(GyroCalKind kind, uint32_t nowUs) {
    mode_ = kind == GyroCalKind::Boot ? CalMode::GyroBoot : CalMode::GyroCmd;
    gyro_.start(kind);
    startWindow(nowUs);
}

CalCoordinator::Done CalCoordinator::feed(const Vec3 &gyro, const Vec3 &accRaw, const Vec3 &accCorr, float tempC,
                                          uint32_t tUs, bool accelValid) {
    if (!w_.active) return Done::None;
    w_.gyro.add(gyro);
    w_.accRaw.add(accRaw);
    w_.accCorr.add(accCorr);
    w_.tempSum += tempC;
    if ((uint32_t)(tUs - w_.startUs) < p_.windowUs) return Done::None;

    w_.active = false;
    switch (mode_) {
    case CalMode::GyroBoot:
    case CalMode::GyroCmd:      break;
    case CalMode::SixMeasure:   return Done::Six;
    case CalMode::LevelMeasure: return Done::Level;
    default:                    return Done::None;
    }

    lastEvent_ = gyro_.finish(w_, accelValid);
    switch (lastEvent_) {
    case GyroCalFlow::Event::Accepted:
    case GyroCalFlow::Event::RejectedKept:
        mode_ = CalMode::None;
        break;
    case GyroCalFlow::Event::Candidate:
    case GyroCalFlow::Event::Inconsistent:
        pending_ = true;
        break;
    case GyroCalFlow::Event::RejectedRetry:
    case GyroCalFlow::Event::RejectedKeptBootRestart:
        mode_ = CalMode::GyroBoot;
        pending_ = true;
        break;
    }
    return Done::Gyro;
}

bool CalCoordinator::endSix() {
    if (gyro_.definitive()) {
        mode_ = CalMode::None;
        return false;
    }
    mode_ = CalMode::GyroBoot;
    gyro_.start(GyroCalKind::Boot);
    pending_ = true;
    return true;
}

void CalCoordinator::onMpuReconfigured(uint32_t nowUs) {
    gyro_.resetConfirmation();
    if (w_.active) startWindow(nowUs);   // uma janela pendente abre normalmente depois
}

MountStart CalCoordinator::checkMount(bool accelValid) const {
    if (mode_ != CalMode::None) return MountStart::Busy;
    if (!accelValid) return MountStart::NoAccel;
    if (!gyro_.definitive()) return MountStart::GyroProvisional;
    return MountStart::Ok;
}
