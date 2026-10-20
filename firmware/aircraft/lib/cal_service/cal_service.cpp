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

void CalService::sample(const ImuSample &s) {
    if (!coord_.window().active) return;
    const Vec3 a = {s.ax, s.ay, s.az};
    switch (coord_.feed({s.gx, s.gy, s.gz}, a, cal_.accelValid ? applyAccelCal(cal_.accel, a) : a, s.tempC, s.tUs,
                        cal_.accelValid)) {
    case CalCoordinator::Done::None:  return;
    case CalCoordinator::Done::Gyro:  onGyroEvent(); break;
    case CalCoordinator::Done::Six:   onSixEvent(); break;
    case CalCoordinator::Done::Level: onMountEvent(); break;
    }
    coord_.startPendingWindow(micros());   // depois das mensagens, como antes da refatoração
}

// ---------- Seis posições (6) ----------

void CalService::startSix() {
    coord_.startSix();
    logPrintln("[cal] seis posições: cada medida leva 2 s com a caixa apoiada e parada");
    promptSixPose();
}

void CalService::promptSixPose() const {
    const int i = coord_.six().index();
    logPrintf("[cal] posição %d/%d: %s. Posicione a caixa, solte e aperte Enter (x cancela)\n", i + 1, POSE_COUNT,
              poseName((Pose)i));
}

void CalService::onSixEvent() {
    const SixPoseFlow &f = coord_.six();
    const Vec3 &m = f.lastMean();
    switch (coord_.lastSixEvent()) {
    case SixPoseFlow::Event::Incomplete:
        logPrintf("[cal] janela incompleta (%lu amostras): repita\n", (unsigned long)f.lastCount());
        promptSixPose();
        return;
    case SixPoseFlow::Event::Moved:
        logPrintf("[cal] a caixa se mexeu (desvio do giroscópio %.3f °/s): repita\n", f.lastGyroStd());
        promptSixPose();
        return;
    case SixPoseFlow::Event::WrongPose: {
        const Pose pose = (Pose)f.index();
        logPrintf("[cal] a média (%+.3f %+.3f %+.3f g) não corresponde a \"%s\" (%s): confira e repita\n", m.x,
                  m.y, m.z, poseName(pose), f.lastPoseCheck() == PoseCheck::WrongAxis ? "outro eixo" : "inclinada demais");
        promptSixPose();
        return;
    }
    case SixPoseFlow::Event::PoseOk:
        logPrintf("[cal] ok: %+.4f %+.4f %+.4f g\n", m.x, m.y, m.z);
        promptSixPose();
        return;
    case SixPoseFlow::Event::ScaleFail:
    case SixPoseFlow::Event::ValidationFail:
    case SixPoseFlow::Event::ReadyToSave:
        logPrintf("[cal] ok: %+.4f %+.4f %+.4f g\n", m.x, m.y, m.z);
        break;
    }

    if (coord_.lastSixEvent() == SixPoseFlow::Event::ScaleFail) {
        logPrintln("[cal] escalas fora de 0,5..1,5: posições trocadas? Calibração descartada");
        if (coord_.gyroRestartPending()) printGyroStart();
        return;
    }
    const AccelCal &c = f.result();
    logPrintln("[cal] resultado das seis posições:");
    logPrintf("  offset (mg): %+.1f %+.1f %+.1f | escala: %+.2f %% %+.2f %% %+.2f %%\n", c.offset.x * 1000,
              c.offset.y * 1000, c.offset.z * 1000, (c.scale.x - 1) * 100, (c.scale.y - 1) * 100,
              (c.scale.z - 1) * 100);
    for (int i = 0; i < POSE_COUNT; i++)
        logPrintf("  %-26s |a| corrigido %.4f g %s\n", poseName((Pose)i), f.poseNorm(i),
                  f.normOk(i) ? "" : "FORA DA FAIXA");
    if (coord_.lastSixEvent() == SixPoseFlow::Event::ValidationFail) {
        logPrintf("[cal] validação FALHOU (|a| fora de %.2f..%.2f g): resultado descartado, calibração "
                  "anterior mantida. Apoie a caixa bem no esquadro e refaça com 6\n",
                  cfg_.flow.six.validMinG, cfg_.flow.six.validMaxG);
        if (coord_.gyroRestartPending()) printGyroStart();
        return;
    }
    logPrintln("[cal] validação ok (|a| entre 0,98 e 1,02 g). s = gravar na NVS | x = descartar");
}

bool CalService::saveSix() {
    const SixPoseFlow &f = coord_.six();
    if (!imuStoreSaveAccel(f.result(), f.tempC(), cfg_.accelRangeG)) {
        logPrintln("[cal] FALHA ao gravar na NVS; calibração anterior mantida");
        return false;
    }
    cal_.accel = f.result();
    cal_.accelTempC = f.tempC();
    cal_.accelRangeG = cfg_.accelRangeG;
    cal_.accelValid = true;
    logPrintln("[cal] calibração do acelerômetro gravada na NVS");
    // A montagem foi medida com o acelerômetro corrigido pela calibração anterior. O registro
    // fica na NVS, mas não bate mais com a calibração e é ignorado também nos próximos boots.
    if (level_.valid) {
        level_ = LevelData();
        logPrintln("[mont] montagem descartada: foi medida com a calibração anterior do "
                   "acelerômetro. Refaça com z");
    }
    return true;
}

// ---------- Montagem (z) ----------

void CalService::startMount() {
    switch (coord_.startMount(cal_.accelValid)) {
    case MountStart::Busy:
        logPrintln("[mont] espere a calibração em andamento terminar");
        return;
    case MountStart::NoAccel:
        logPrintln("[mont] acelerômetro sem calibração: faça antes as seis posições (6)");
        return;
    case MountStart::GyroProvisional:
        logPrintln("[mont] bias do giroscópio ainda provisório: espere a calibração (ou use g)");
        return;
    case MountStart::Ok:
        break;
    }
    logPrintf("[mont] calibração de montagem: duas medidas de %lu ms, a segunda com a caixa girada "
              "180°\n", (unsigned long)(cfg_.flow.windowUs / 1000));
    promptMount();
}

void CalService::promptMount() const {
    static const char *const STEP[2] = {
        "caixa em pé, na posição normal (fundo apoiado na mesa, sem virar), nariz encostado numa referência "
        "(borda da mesa ou esquadro)",
        "caixa ainda em pé, girada 180° no plano da mesa (sem virar), com a cauda encostada na mesma referência",
    };
    const int i = coord_.mount().index();
    logPrintf("[mont] posição %d/2: %s. Solte a caixa e aperte Enter (x cancela)\n", i + 1, STEP[i]);
}

static const char *levelRestText(LevelRest r) {
    switch (r) {
    case LevelRest::Ok:            return "ok";
    case LevelRest::TooFewSamples: return "janela incompleta";
    case LevelRest::Moving:        return "caixa em movimento";
    case LevelRest::Rotating:      return "giroscópio corrigido longe de zero (rotação lenta)";
    case LevelRest::AccOutOfRange: return "|a| fora da faixa de repouso";
    case LevelRest::Invalid:       return "leitura inválida (não finita) na janela";
    }
    return "?";
}

void CalService::onMountEvent() {
    const MountFlow &f = coord_.mount();
    switch (coord_.lastMountEvent()) {
    case MountFlow::Event::Rejected: {
        const Vec3 &gm = f.lastGyroMean();
        logPrintf("[mont] medida recusada: %s (desvio máx. %.3f °/s, giro médio %+.3f %+.3f %+.3f °/s, "
                  "|a| %.3f g): repita\n",
                  levelRestText(f.lastRest()), f.lastGyroStd(), gm.x, gm.y, gm.z, f.lastAccNorm());
        promptMount();
        return;
    }
    case MountFlow::Event::PoseOk:
    case MountFlow::Event::BadInput:
    case MountFlow::Event::TooTilted:
    case MountFlow::Event::ReadyToSave:
        logPrintf("[mont] ok: roll %+.2f° pitch %+.2f° (sem correção de montagem)\n", f.lastAngles().rollDeg,
                  f.lastAngles().pitchDeg);
        break;
    }
    if (coord_.lastMountEvent() == MountFlow::Event::PoseOk) {
        promptMount();
        return;
    }
    if (coord_.lastMountEvent() == MountFlow::Event::BadInput) {
        logPrintln("[mont] medidas incoerentes (a mesma face estava apoiada nas duas?): calibração descartada");
        return;
    }
    const MountResult &m = f.result();
    logPrintf("[mont] desalinhamento do módulo: roll %+.2f° pitch %+.2f° | inclinação da mesa: roll %+.2f° "
              "pitch %+.2f°\n",
              m.mountRollDeg, m.mountPitchDeg, m.tableRollDeg, m.tablePitchDeg);
    if (coord_.lastMountEvent() == MountFlow::Event::TooTilted) {
        logPrintf("[mont] acima de %.0f°: calibração descartada. Confira se a caixa estava em pé (sem virar), a mesa e "
                  "a fixação do módulo\n",
                  cfg_.flow.mount.maxTiltDeg);
        return;
    }
    logPrintln("[mont] s = gravar na NVS | x = descartar");
}

bool CalService::saveMount() {
    const MountFlow &f = coord_.mount();
    LevelData d;
    d.valid = true;
    d.r = f.result().r;
    d.mountRollDeg = f.result().mountRollDeg;
    d.mountPitchDeg = f.result().mountPitchDeg;
    d.tempC = f.tempC();
    d.accelCalValid = cal_.accelValid;   // as medidas usaram o acelerômetro com esta calibração
    d.accelCal = cal_.accel;
    if (!levelStoreSave(d)) {
        logPrintln("[mont] FALHA ao gravar na NVS; montagem anterior mantida");
        return false;
    }
    level_ = d;
    logPrintln("[mont] calibração de montagem gravada na NVS");
    return true;
}

// ---------- Teclas das seis posições e da montagem ----------

void CalService::exitFlow() {
    coord_.exitFlow();
    if (coord_.gyroRestartPending()) printGyroStart();
    coord_.startPendingWindow(micros());
}

CalKey CalService::handleKey(char ch) {
    const bool enter = ch == '\r' || ch == '\n';
    switch (coord_.mode()) {
    case CalMode::SixWait:
        if (enter) {
            logPrintf("[cal] medindo %lu ms, não toque na caixa...\n", (unsigned long)(cfg_.flow.windowUs / 1000));
            coord_.confirm();
            coord_.startPendingWindow(micros());
        } else if (ch == 'x') {
            logPrintln("[cal] seis posições canceladas");
            exitFlow();
        }
        return CalKey::Handled;
    case CalMode::SixMeasure:
        if (ch == 'x') {
            logPrintln("[cal] seis posições canceladas");
            exitFlow();
        }
        return CalKey::Handled;   // Enter extra (\n do CR+LF) e outras teclas são ignorados durante a medida
    case CalMode::SixReview:
        if (ch == 's') {
            const bool saved = saveSix();
            exitFlow();
            return saved ? CalKey::HandledFusionReset : CalKey::Handled;   // a referência do acelerômetro mudou
        }
        if (ch == 'x') {
            logPrintln("[cal] resultado descartado; calibração anterior mantida");
            exitFlow();
        }
        return CalKey::Handled;
    case CalMode::LevelWait:
        if (enter) {
            logPrintf("[mont] medindo %lu ms, não toque na caixa...\n", (unsigned long)(cfg_.flow.windowUs / 1000));
            coord_.confirm();
            coord_.startPendingWindow(micros());
        } else if (ch == 'x') {
            logPrintln("[mont] calibração de montagem cancelada");
            exitFlow();
        }
        return CalKey::Handled;
    case CalMode::LevelMeasure:
        if (ch == 'x') {
            logPrintln("[mont] calibração de montagem cancelada");
            exitFlow();
        }
        return CalKey::Handled;
    case CalMode::LevelReview:
        if (ch == 's') {
            const bool saved = saveMount();
            exitFlow();
            return saved ? CalKey::HandledFusionReset : CalKey::Handled;   // a rotação de montagem mudou
        }
        if (ch == 'x') {
            logPrintln("[mont] resultado descartado; montagem anterior mantida");
            exitFlow();
        }
        return CalKey::Handled;
    default:
        return CalKey::NotHandled;
    }
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
