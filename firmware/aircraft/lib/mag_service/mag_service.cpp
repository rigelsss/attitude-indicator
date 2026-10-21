// mag_service.cpp — ver mag_service.h
#include "mag_service.h"
#include <math.h>
#include <stdlib.h>
#include "logger.h"

MagService::MagService(const MagServiceConfig &cfg, const AttitudeFilter &att, HeadingFilter &hdg)
    : cfg_(cfg), att_(att), hdg_(hdg), periodUs_(1000000UL / cfg.readHz), printUs_(1000000UL / cfg.printHz),
      calib_(cfg.cal) {}

void MagService::loadCalibration() {
    uint8_t crb;
    float lsb;
    if (hmcCrbCode(cfg_.chip.rangeMga, crb, lsb)) magStoreLoad(cal_, crb);
    if (cal_.rejected) logPrintln("[A] NVS: calibração do magnetômetro com valores impossíveis, ignorada");
    if (cal_.otherAxisMap)
        logPrintln("[A] NVS: calibração do magnetômetro feita com outro mapeamento de eixos, ignorada. Refaça com m");
    if (cal_.otherGain)
        logPrintln("[A] NVS: calibração do magnetômetro feita com outro ganho, ignorada. Refaça com m");
}

void MagService::begin() { start(); }

void MagService::start() {
    const Hmc5883lStatus st = mag_.begin(cfg_.chip);
    if (st != Hmc5883lStatus::Ok) {
        if (st != lastStatus_)
            logPrintf("[mag] HMC5883L: %s; sem proa, nova tentativa a cada %lu s\n", hmc5883lStatusText(st),
                      (unsigned long)(cfg_.retryMs / 1000));
        lastStatus_ = st;
        retryAtMs_ = millis() + cfg_.retryMs;
        return;
    }
    lastStatus_ = st;
    ready_ = true;
    lastRecoveries_ = mag_.recoveries();
    sched_.start(micros());
    prevValid_ = false;
    logPrintf("[mag] HMC5883L configurado: ±%.1f Ga (%.0f LSB/Ga), %.0f Hz, média de %d, modo contínuo\n",
              cfg_.chip.rangeMga / 1000.0f, mag_.lsbPerGa(), cfg_.chip.outputHz, cfg_.chip.avgSamples);
}

const char *MagService::stateText() const {
    if (calib_.state() == MagCalibrator::State::Collect) return "CALIBRANDO";
    if (!haveTilt_) return "SEM_ATITUDE";
    switch (step_) {
    case HeadingFilter::Mag::Disturbed: return "DISTURBIO";
    case HeadingFilter::Mag::Tilted:    return "INCL_ALTA";
    case HeadingFilter::Mag::AttitudeDegraded: return "ATITUDE_DEGRADADA";
    case HeadingFilter::Mag::BadData:   return "DADO_INVALIDO";
    default:                            return "ok";
    }
}

bool MagService::correctionSuspended() const {
    return step_ == HeadingFilter::Mag::Tilted || step_ == HeadingFilter::Mag::Disturbed ||
           step_ == HeadingFilter::Mag::AttitudeDegraded || step_ == HeadingFilter::Mag::BadData;
}

// Fluxo de proa (p): filtrada | magnetômetro compensada | deitada (sem compensação) | |m| corrigido |
// inclinação magnética medida | roll e pitch | estado
void MagService::printLine() const {
    char hdg[12] = "  --  ";
    if (hdg_.initialized()) snprintf(hdg, sizeof(hdg), "%6.1f", hdg_.headingDeg());
    char mag[12] = "  --  ", dip[12] = "  --  ";
    if (haveTilt_) {
        snprintf(mag, sizeof(mag), "%6.1f", tilt_.headingDeg);
        snprintf(dip, sizeof(dip), "%+6.1f", tilt_.dipDeg);
    }
    logPrintf("[mag] proa %s | mag %s | deitada %6.1f | |m| %5.1f µT | incl %s° | roll %+6.1f pitch %+6.1f | %s%s\n",
              hdg, mag, magFlatHeadingDeg(corr_), norm(corr_), dip,
              att_.initialized() ? att_.rollDeg() : 0.0f, att_.initialized() ? att_.pitchDeg() : 0.0f,
              stateText(), cal_.valid ? "" : " SEM_CAL");
}

void MagService::printSummary() const {
    if (!ready_) {
        logPrintf("    mag: HMC5883L indisponível (%s)\n", hmc5883lStatusText(lastStatus_));
        return;
    }
    logPrintf("    mag: %s | %lu leituras no último 1 s | ", mag_.ok() ? "ok" : "FALHANDO",
              (unsigned long)lastWinReads_);
    if (!have_) logPrint("sem leitura");
    else {
        logPrintf("|m| %.1f µT", norm(corr_));
        if (hdg_.initialized()) logPrintf(" | proa %.1f°", hdg_.headingDeg());
        logPrintf(" | %s%s", stateText(), cal_.valid ? "" : " SEM_CAL");
    }
    logPrintf(" | falhas %lu (seguidas %lu, recuperações %lu) | saturadas %lu\n", (unsigned long)mag_.failTotal(),
              (unsigned long)mag_.failStreak(), (unsigned long)mag_.recoveries(),
              (unsigned long)mag_.saturations());
}

void MagService::closeWindow() {
    lastWinReads_ = winReads_;
    winReads_ = 0;
}

void MagService::freeCalibration() {
    calib_.stop();
    free(pts_);
    pts_ = nullptr;
    prevValid_ = false;   // a correção recomeça sem passo acumulado
}

void MagService::startCalibration(bool otherCalRunning) {
    // A coleta não usa a janela do giroscópio: pode começar durante a calibração do boot (que só confirma
    // com a caixa parada e, até lá, usa o bias salvo). As outras calibrações continuam bloqueando o m
    if (otherCalRunning) {
        logPrintln("[mag] espere a calibração em andamento terminar");
        return;
    }
    // O console já impede (as teclas vão para a calibração), mas um comando de outra origem não: uma
    // segunda alocação perderia o buffer em uso
    if (calibrating()) {
        logPrintln("[mag] calibração do magnetômetro já em andamento");
        return;
    }
    if (!ready_ || !mag_.ok()) {
        logPrintln("[mag] HMC5883L indisponível ou falhando: calibração não iniciada");
        return;
    }
    pts_ = (Vec3 *)malloc(cfg_.cal.maxPoints * sizeof(Vec3));
    if (pts_ == nullptr) {
        logPrintln("[mag] ERRO: sem memória para os pontos da calibração");
        return;
    }
    calib_.start(pts_, millis());
    keyHinted_ = false;
    hdg_.reset();
    logPrintf("[mag] calibração: gire a caixa devagar em TODAS as orientações por até %lu s (x cancela).\n",
              (unsigned long)(cfg_.cal.maxMs / 1000));
    logPrintln("[mag] para cada face voltada para cima (topo, fundo, asas, nariz, cauda), um giro em torno da "
               "vertical. Termina sozinha com os 24 setores cobertos");
}

void MagService::printFinish() {
    const MagFitResult &r = calib_.fit();
    const MagFitStatus st = r.status;
    const MagCal &c = r.cal;
    logPrintf("[mag] coleta encerrada: %lu pontos em %.0f s\n", (unsigned long)calib_.count(),
              (millis() - calib_.startMs()) / 1000.0f);
    if (st != MagFitStatus::TooFewPoints && st != MagFitStatus::Singular && st != MagFitStatus::NotEllipsoid) {
        logPrintf("  offset %+.2f %+.2f %+.2f µT | semieixos %.3f %.3f %.3f\n", c.offset.x, c.offset.y,
                  c.offset.z, r.scale.x, r.scale.y, r.scale.z);
        for (int i = 0; i < 3; i++)
            logPrintf("  W %s %+.4f %+.4f %+.4f\n", i == 0 ? "=" : " ", c.w.m[i][0], c.w.m[i][1], c.w.m[i][2]);
        logPrintf("  raio %.2f µT (WMM na bancada: %.1f µT) | resíduo %.2f %% | cobertura %d/%d setores\n",
                  c.radiusUt, cfg_.refFieldUt, r.residPct, r.sectors, MAG_SECTORS);
    }
    const MagFitLimits &lim = cfg_.cal.fit;
    if (st == MagFitStatus::Ok) {
        logPrintln("[mag] calibração ok. s = gravar na NVS | x = descartar | d = pontos em CSV");
    } else {
        logPrintf("[mag] calibração RECUSADA: %s (limites: raio %.0f–%.0f µT, resíduo ≤ %.1f %%, semieixos "
                  "%.2f–%.2f, ≥ %d setores). x = sair | d = pontos em CSV\n",
                  magFitStatusText(st), lim.minRadiusUt, lim.maxRadiusUt, lim.maxResidPct, lim.minScale, lim.maxScale,
                  lim.minSectors);
    }
}

// Pontos da última coleta em CSV, para tools/mag_fit.py. Bloqueia o laço por alguns segundos
// (~25 bytes por ponto a 115200): quem chama reinicia a fusão depois (MagKey::HandledLoopStalled).
void MagService::dumpPoints() {
    const MagFitResult &f = calib_.fit();
    const MagCal &c = f.cal;
    const Vec3 *p = calib_.points();
    const size_t n = calib_.count();
    logWaitRoom(512);   // cabeçalho (~300 bytes) inteiro, mesmo depois de outras mensagens
    logPrintf("# mag_cal inicio: %lu pontos (µT, eixos do modelo, sem calibração)\n", (unsigned long)n);
    logPrintf("# firmware status=%s b=%.4f,%.4f,%.4f w=%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f R=%.4f "
              "resid=%.3f setores=%d\n",
              magFitStatusText(f.status), c.offset.x, c.offset.y, c.offset.z, c.w.m[0][0], c.w.m[0][1],
              c.w.m[0][2], c.w.m[1][0], c.w.m[1][1], c.w.m[1][2], c.w.m[2][0], c.w.m[2][1], c.w.m[2][2],
              c.radiusUt, f.residPct, f.sectors);
    logPrintln("x,y,z");
    // ~45 kB, bem mais que o buffer do logger: cada linha espera espaço, em vez de ser descartada.
    // O laço fica parado, como antes (MagKey::HandledLoopStalled)
    for (size_t i = 0; i < n; i++) {
        logWaitRoom(64);
        logPrintf("%.3f,%.3f,%.3f\n", p[i].x, p[i].y, p[i].z);
    }
    logPrintln("# mag_cal fim");
    logFlush();
}

void MagService::service(uint32_t nowUs) {
    if (!ready_) {
        if ((int32_t)(millis() - retryAtMs_) >= 0) start();
        return;
    }
    if (!sched_.due(nowUs)) return;

    MagSample ms;
    const bool ok = mag_.read(ms);
    // Com o HMC5883L falhando, tenta só a cada failRetryMs (cada transação sem resposta atrasa o laço)
    sched_.advance(nowUs, mag_.ok() ? periodUs_ : cfg_.failRetryMs * 1000UL);
    if (mag_.recoveries() != lastRecoveries_) {
        lastRecoveries_ = mag_.recoveries();
        logPrintf("[mag] HMC5883L perdeu a configuração (queda de alimentação?): %s\n",
                  mag_.configured() ? "reconfigurado" : "reconfiguração falhou, nova tentativa a cada leitura");
    }
    if (!ok) return;
    winReads_++;
    const Vec3 raw = magChipToBody(ms.chipUt);
    have_ = true;
    const float dt = prevValid_ ? (ms.tMs - prevMs_) / 1000.0f : 0.0f;
    prevMs_ = ms.tMs;
    prevValid_ = true;

    if (calib_.state() == MagCalibrator::State::Collect) {
        // Ponto da coleta (eixos do modelo, sem calibração), com o progresso da cobertura
        const MagCalibrator::Event ev = calib_.feed(raw, ms.tMs);
        if (ev == MagCalibrator::Event::None) return;
        logPrintf("[mag] %2lu s | %4lu pontos | cobertura %2d/%d", (unsigned long)(calib_.elapsedMs() / 1000),
                  (unsigned long)calib_.count(), calib_.covered(), MAG_SECTORS);
        if (calib_.covered() < MAG_SECTORS) {
            logPrint(" | faltam:");
            for (int f = 0; f < MAG_FACES; f++)
                if (calib_.missing(f)) logPrintf(" %s(%d)", magFaceName(f), calib_.missing(f));
        }
        logPrintln();
        if (ev == MagCalibrator::Event::Finished) printFinish();
        return;
    }
    if (calib_.state() == MagCalibrator::State::Review) return;   // proa suspensa até gravar ou descartar

    corr_ = magApplyCal(cal_.cal, raw);
    if (att_.initialized()) {
        tilt_ = magTiltCompensate(corr_, att_.rollDeg(), att_.pitchDeg());
        haveTilt_ = true;
        step_ = hdg_.correct(tilt_.headingDeg, norm(corr_), cal_.cal.radiusUt, att_.pitchDeg(), dt, att_.degraded());
    } else {
        haveTilt_ = false;
    }
    // Aviso de distúrbio pelo próprio |m|, e não pelo resultado de correct(): com outra suspensão ativa
    // (atitude degradada, inclinação alta), correct() devolve outro estado mesmo com o campo fora do limite
    const bool disturbed = magDisturbed(norm(corr_), cal_.cal.radiusUt, cfg_.disturbPct);
    if (disturbed != disturbWarned_) {
        disturbWarned_ = disturbed;
        if (disturbed)
            logPrintf("[mag] DISTÚRBIO: |m| %.1f µT, raio da calibração %.1f µT (limite %.0f %%): correção "
                      "pelo magnetômetro suspensa\n", norm(corr_), cal_.cal.radiusUt, cfg_.disturbPct);
        else
            logPrintf("[mag] distúrbio terminou: |m| %.1f µT de volta ao raio da calibração\n", norm(corr_));
    }
    if (streamOn_ && streamPrint_.due(nowUs)) {
        streamPrint_.advance(nowUs, printUs_);
        printLine();
    }
}

// Tecla ignorada na calibração: diz o que vale agora, no máximo uma vez por keyHintMs, para o terminal não
// parecer travado (refatoracao.md, pendência 1). Enter e espaço não contam. O intervalo usa a diferença
// sem sinal do millis(), e não o Periodic (diferença com sinal em µs, válida só até ~35 min de atraso)
void MagService::hintIgnoredKey(char ch, const char *msg) {
    if (ch == '\r' || ch == '\n' || ch == ' ') return;
    const uint32_t now = millis();
    if (keyHinted_ && now - keyHintMs_ < cfg_.keyHintMs) return;
    logPrintln(msg);
    keyHinted_ = true;
    keyHintMs_ = now;
}

MagKey MagService::handleKey(char ch) {
    const MagCalibrator::State st = calib_.state();
    if (st == MagCalibrator::State::Collect) {
        if (ch == 'x') {
            logPrintln("[mag] calibração cancelada; calibração anterior mantida");
            freeCalibration();
        } else {
            hintIgnoredKey(ch, "[mag] calibração em andamento: x cancela");
        }
        return MagKey::Handled;   // outras teclas são ignoradas durante a coleta
    }
    if (st != MagCalibrator::State::Review) return MagKey::NotHandled;
    if (ch != 's' && ch != 'x' && ch != 'd') {
        // Recusada, o s não grava: a dica não o oferece
        hintIgnoredKey(ch, calib_.fit().status == MagFitStatus::Ok
                               ? "[mag] revisão da calibração: s = gravar | x = sair | d = pontos em CSV"
                               : "[mag] revisão da calibração recusada: x = sair | d = pontos em CSV");
        return MagKey::Handled;
    }
    if (ch == 'd') {
        dumpPoints();
        return MagKey::HandledLoopStalled;
    }
    if (ch == 'x') {
        logPrintln("[mag] resultado descartado; calibração anterior mantida");
        freeCalibration();
    } else if (ch == 's') {
        const MagFitResult &f = calib_.fit();
        if (f.status != MagFitStatus::Ok) {
            logPrintln("[mag] calibração recusada não pode ser gravada (x = sair | d = pontos em CSV)");
        } else if (magStoreSave(f.cal, f.residPct, (uint8_t)f.sectors, mag_.crb())) {
            cal_.valid = true;
            cal_.cal = f.cal;
            cal_.residPct = f.residPct;
            cal_.sectors = (uint8_t)f.sectors;
            hdg_.reset();
            logPrintln("[mag] calibração do magnetômetro gravada na NVS");
            freeCalibration();
        } else {
            logPrintln("[mag] FALHA ao gravar na NVS; calibração anterior mantida");
            freeCalibration();
        }
    }
    return MagKey::Handled;
}

void MagService::toggleStream(uint32_t nowUs) {
    streamOn_ = !streamOn_;
    streamPrint_.start(nowUs);
    logPrintf("[mag] fluxo de proa %s (%lu Hz): filtrada | magnetômetro compensada | deitada | |m| | "
              "inclinação | roll pitch | estado\n", streamOn_ ? "ligado" : "desligado", (unsigned long)cfg_.printHz);
}
