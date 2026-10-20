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

// ---------- SixPoseFlow ----------

void SixPoseFlow::start() {
    idx_ = 0;
    tempSum_ = 0;
}

SixPoseFlow::Event SixPoseFlow::finish(const CalWindow &w) {
    const Pose pose = (Pose)idx_;
    lastMean_ = w.accRaw.mean();
    lastCount_ = w.accRaw.count();
    lastGyroStd_ = w.gyro.maxStddev();
    if (lastCount_ < p_.minSamples) return Event::Incomplete;
    if (lastGyroStd_ >= p_.gyroMaxStdDps) return Event::Moved;
    lastPose_ = checkPose(pose, lastMean_, p_.pose);
    if (lastPose_ != PoseCheck::Ok) return Event::WrongPose;
    means_[idx_] = lastMean_;
    tempSum_ += w.tempSum / lastCount_;
    if (++idx_ < POSE_COUNT) return Event::PoseOk;

    if (!sixPositionCal(means_, result_)) return Event::ScaleFail;
    // Resultado reprovado não pode ser gravado: provavelmente posições inclinadas demais
    return validateAccelCal(result_, means_, p_.validMinG, p_.validMaxG, norms_) ? Event::ReadyToSave
                                                                                 : Event::ValidationFail;
}

// ---------- MountFlow ----------

void MountFlow::start() {
    idx_ = 0;
    tempSum_ = 0;
}

MountFlow::Event MountFlow::finish(const CalWindow &w, const Vec3 &gyroBias) {
    lastRest_ = checkLevelWindow(w.gyro, gyroBias, w.accCorr, p_.minSamples, p_.rest, p_.gyroMaxMeanDps);
    lastGyroStd_ = w.gyro.maxStddev();
    lastGyroMean_ = applyGyroBias(w.gyro.mean(), gyroBias);
    lastAccNorm_ = norm(w.accCorr.mean());
    if (lastRest_ != LevelRest::Ok) return Event::Rejected;
    f_[idx_] = chipToBody(w.accCorr.mean());
    tempSum_ += w.tempSum / w.accCorr.count();
    lastAngles_ = accelRollPitch(f_[idx_]);
    if (++idx_ < 2) return Event::PoseOk;

    switch (mountFromTwoPoses(f_[0], f_[1], p_.maxTiltDeg, result_)) {
    case MountCheck::BadInput:  return Event::BadInput;
    case MountCheck::TooTilted: return Event::TooTilted;
    case MountCheck::Ok:        break;
    }
    return Event::ReadyToSave;
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
    case CalMode::SixMeasure:   return finishSix();
    case CalMode::LevelMeasure: return finishMount();
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

CalCoordinator::Done CalCoordinator::finishSix() {
    lastSix_ = six_.finish(w_);
    switch (lastSix_) {
    case SixPoseFlow::Event::Incomplete:
    case SixPoseFlow::Event::Moved:
    case SixPoseFlow::Event::WrongPose:
    case SixPoseFlow::Event::PoseOk:         mode_ = CalMode::SixWait; break;
    case SixPoseFlow::Event::ScaleFail:
    case SixPoseFlow::Event::ValidationFail: endSix(); break;
    case SixPoseFlow::Event::ReadyToSave:    mode_ = CalMode::SixReview; break;
    }
    return Done::Six;
}

CalCoordinator::Done CalCoordinator::finishMount() {
    lastMount_ = mount_.finish(w_, gyro_.bias());
    switch (lastMount_) {
    case MountFlow::Event::Rejected:
    case MountFlow::Event::PoseOk:      mode_ = CalMode::LevelWait; break;
    case MountFlow::Event::BadInput:
    case MountFlow::Event::TooTilted:   mode_ = CalMode::None; break;
    case MountFlow::Event::ReadyToSave: mode_ = CalMode::LevelReview; break;
    }
    return Done::Level;
}

void CalCoordinator::startSix() {
    w_.active = false;   // interrompe a calibração do giroscópio em andamento
    pending_ = false;
    six_.start();
    mode_ = CalMode::SixWait;
}

MountStart CalCoordinator::startMount(bool accelValid) {
    const MountStart s = checkMount(accelValid);
    if (s != MountStart::Ok) return s;
    mount_.start();
    mode_ = CalMode::LevelWait;
    return s;
}

bool CalCoordinator::confirm() {
    if (mode_ == CalMode::SixWait) mode_ = CalMode::SixMeasure;
    else if (mode_ == CalMode::LevelWait) mode_ = CalMode::LevelMeasure;
    else return false;
    pending_ = true;
    return true;
}

void CalCoordinator::exitFlow() {
    switch (mode_) {
    case CalMode::SixWait:
    case CalMode::SixMeasure:
    case CalMode::SixReview:
        w_.active = false;
        pending_ = false;
        endSix();
        break;
    case CalMode::LevelWait:
    case CalMode::LevelMeasure:
    case CalMode::LevelReview:
        w_.active = false;
        pending_ = false;
        mode_ = CalMode::None;
        break;
    default:
        break;
    }
}

// Depois das seis posições (gravando ou não), retoma a calibração do giroscópio se ela ainda falta
void CalCoordinator::endSix() {
    if (gyro_.definitive()) {
        mode_ = CalMode::None;
        return;
    }
    mode_ = CalMode::GyroBoot;
    gyro_.start(GyroCalKind::Boot);
    pending_ = true;
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
