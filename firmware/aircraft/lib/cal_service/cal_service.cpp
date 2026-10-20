// cal_service.cpp — ver cal_service.h
#include "cal_service.h"
#include "logger.h"

static const char *restText(RestCheck r) {
    switch (r) {
    case RestCheck::Ok:            return "ok";
    case RestCheck::TooFewSamples: return "janela incompleta";
    case RestCheck::Moving:        return "placa em movimento";
    case RestCheck::AccOutOfRange: return "|a| fora da faixa de repouso";
    case RestCheck::Invalid:       return "leitura inválida (não finita) na janela";
    }
    return "?";
}

void CalService::loadFromNvs() {
    imuStoreLoad(cal_, cfg_.accelRangeG, cfg_.storeAccOffsetMaxG, cfg_.storeGyroBiasMaxDps);
    if (cal_.accelRejected)
        logPrintln("[A] NVS: calibração do acelerômetro com valores impossíveis, ignorada. Refaça com 6");
    if (cal_.gyroRejected) logPrintln("[A] NVS: bias do giroscópio com valores impossíveis, ignorado");
    if (cal_.gyroValid) coord_.gyro().setProvisional(cal_.gyroBias, cal_.gyroTempC);
    levelStoreLoad(level_, cfg_.levelMaxTiltDeg);
    if (level_.rejected) logPrintln("[A] NVS: calibração de montagem com valores impossíveis, ignorada. Refaça com z");
    if (level_.valid && !sameAccelCal(level_.accelCalValid, level_.accelCal, cal_.accelValid, cal_.accel)) {
        level_ = LevelData();
        logPrintln("[A] NVS: montagem medida com outra calibração do acelerômetro, ignorada. Refaça com z");
    }
}

void CalService::printNvsSummary(bool magCalibrated) const {
    logPrintf("[A] NVS: acelerômetro %s | giroscópio %s | montagem %s | magnetômetro %s\n",
              cal_.accelValid ? "calibrado" : "sem calibração", cal_.gyroValid ? "com bias salvo" : "sem bias salvo",
              level_.valid ? "calibrada" : "sem calibração",
              magCalibrated ? "calibrado" : "sem calibração (proa com offset zero; use m)");
}

void CalService::printGyroStart() const {
    logPrintf("[cal] giroscópio: mantenha a placa parada (duas janelas de %lu ms)...\n",
              (unsigned long)(cfg_.flow.windowUs / 1000));
}

void CalService::startGyro(GyroCalKind kind) {
    printGyroStart();
    coord_.startGyro(kind, micros());
}

CalCoordinator::Done CalService::sample(const ImuSample &s) {
    if (!coord_.window().active) return CalCoordinator::Done::None;
    const Vec3 a = {s.ax, s.ay, s.az};
    const CalCoordinator::Done d = coord_.feed({s.gx, s.gy, s.gz}, a, cal_.accelValid ? applyAccelCal(cal_.accel, a) : a,
                                               s.tempC, s.tUs, cal_.accelValid);
    if (d == CalCoordinator::Done::Gyro) {
        onGyroEvent();
        coord_.startPendingWindow(micros());   // depois das mensagens, como antes da refatoração
    }
    return d;
}

void CalService::onGyroEvent() {
    const GyroCalFlow &g = coord_.gyro();
    const GyroBiasResult &r = g.lastWindow();
    switch (coord_.lastGyroEvent()) {
    case GyroCalFlow::Event::Accepted: {
        const Vec3 b = g.bias();
        const float t = g.biasTempC();
        tempWarned_ = false;
        const bool saved = imuStoreSaveGyro(b, t);
        if (saved) {
            cal_.gyroBias = b;
            cal_.gyroTempC = t;
            cal_.gyroValid = true;
        }
        logPrintf("[cal] giroscópio calibrado: bias %+.3f %+.3f %+.3f °/s a %.1f °C (diferença entre "
                  "janelas %.3f °/s e %.3f° na gravidade, desvio máx. %.3f °/s)%s\n",
                  b.x, b.y, b.z, t, g.lastDelta(), g.lastTilt(), r.gyroMaxStd,
                  saved ? "" : " | FALHA ao gravar na NVS");
        return;
    }
    case GyroCalFlow::Event::Candidate:
        return;
    case GyroCalFlow::Event::Inconsistent:
        logPrintf("[cal] giroscópio: entre janelas, bias mudou %.3f °/s (limite %.3f) e gravidade %.3f° "
                  "(limite %.2f): caixa se acomodando ou girando; confirmando de novo\n",
                  g.lastDelta(), cfg_.flow.gyro.maxDeltaDps, g.lastTilt(), cfg_.flow.gyro.maxTiltDeg);
        return;
    case GyroCalFlow::Event::RejectedRetry:
    case GyroCalFlow::Event::RejectedKept:
    case GyroCalFlow::Event::RejectedKeptBootRestart:
        break;
    }

    logPrintf("[cal] giroscópio recusado: %s (desvio máx. %.3f °/s, |a| %.3f g, %lu amostras)\n",
              restText(r.status), r.gyroMaxStd, r.accNorm, (unsigned long)g.lastCount());
    if (coord_.lastGyroEvent() != GyroCalFlow::Event::RejectedRetry) logPrintln("[cal] mantido o bias anterior");
    if (coord_.lastGyroEvent() != GyroCalFlow::Event::RejectedKept) printGyroStart();
}

void CalService::onMpuReconfigured() { coord_.onMpuReconfigured(micros()); }

void CalService::checkTempDrift(float nowC) {
    if (!gyroDefinitive() || tempWarned_) return;
    if (tempDrifted(gyroBiasTempC(), nowC, cfg_.tempWarnC)) {
        logPrintf("[cal] AVISO: chip a %.1f °C, calibração do giroscópio feita a %.1f °C. Recalibre com g\n",
                  nowC, gyroBiasTempC());
        tempWarned_ = true;
    }
}

void CalService::endSix() {
    if (coord_.endSix()) printGyroStart();
    coord_.startPendingWindow(micros());
}

bool CalService::saveAccel(const AccelCal &cal, float tempC) {
    if (!imuStoreSaveAccel(cal, tempC, cfg_.accelRangeG)) return false;
    cal_.accel = cal;
    cal_.accelTempC = tempC;
    cal_.accelRangeG = cfg_.accelRangeG;
    cal_.accelValid = true;
    return true;
}

bool CalService::saveLevel(const LevelData &d) {
    if (!levelStoreSave(d)) return false;
    level_ = d;
    return true;
}
