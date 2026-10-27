// console.cpp — ver console.h
#include "console.h"
#include <math.h>
#include "config.h"
#include "logger.h"
#include "console_report.h"
#include "report_queue.h"
#include "pulse_test.h"

static const uint32_t ATT_PRINT_US = 1000000UL / ATT_PRINT_HZ;

void Console::printHelp() const {
    logPrintln("[A] comandos: c = estado | 6 = seis posições | g = calibrar giroscópio | z = calibrar montagem | "
               "m = calibrar magnetômetro | a = fluxo de atitude | b = fluxo de altitude | p = fluxo de proa | "
               "0 = zerar altitude | r = resumo por segundo");
}

void Console::handleKey(char ch) {
    const MagKey mk = d_.mag.handleKey(ch);
    // d parou o laço por alguns segundos: a referência de tempo da fusão não vale mais
    if (mk == MagKey::HandledLoopStalled) d_.fusionReset();
    if (mk != MagKey::NotHandled) return;
    const CalKey ck = d_.cal.handleKey(ch);   // seis posições e montagem em andamento
    if (ck == CalKey::HandledFusionReset) d_.fusionReset();
    if (ck != CalKey::NotHandled) return;
    switch (ch) {
    case 'c': sendStatus(); break;
    case 'r':
        summaryOn_ = !summaryOn_;
        logPrintf("[A] resumo por segundo %s\n", summaryOn_ ? "ligado" : "desligado");
        break;
    case 'g': d_.cal.startGyro(GyroCalKind::Cmd); break;
    case 'z': d_.cal.startMount(); break;
    case 'a': toggleAttitude(); break;
    case 'b': d_.baro.toggleStream(micros()); break;
    case 'm': d_.mag.startCalibration(!d_.cal.magAllowed()); break;
    case 'p': d_.mag.toggleStream(micros()); break;
    case '0': d_.baro.zeroAltitude(); break;
    case '6': d_.cal.startSix(); break;
    case 'h':
    case '?': printHelp(); break;
#ifdef PULSE_TEST
    case 'F': pulseTestStart(); break;   // só no ambiente aircraft_pulsetest
#endif
#ifdef FUSION_RESET_TEST
    case 'R':   // só no ambiente aircraft_resettest: o mesmo reinício de uma recuperação do MPU ou lacuna
        logPrintln("[teste] fusão reiniciada pela tecla R");
        d_.fusionReset();
        break;
#endif
    default: break;
    }
}

// ---------- Resumo por segundo (r) ----------

void Console::windowClosed(const SampleWindow &w) const {
    const RadioCounters radio = radioCounters();
    const RadioCounters prev = lastRadio_;
    lastRadio_ = radio;   // a cada janela, com ou sem o r: a primeira linha depois do r já é de 1 s
    if (!summaryOn_) return;
    WindowReport r;
    r.radio = radio;
    r.radioPrev = prev;
    r.w = w;
    r.windowUs = d_.acq.windowUs();
    r.mpu = {d_.mpu.failTotal(), d_.mpu.failStreak(), d_.mpu.recoveries(), d_.intOverflow};
    r.baro = d_.baro.summary();
    r.mag = d_.mag.summary();
    reportSend(ReportKind::Window, r);
}

// ---------- Fluxo de atitude (a) ----------

void Console::toggleAttitude() {
    attOn_ = !attOn_;
    d_.acqStats = AcqStats();
    attPrint_.start(micros());
    d_.fusion.resetStats();
    d_.fusion.resetGyroOnly();   // o diagnóstico só de giroscópio recomeça da atitude atual
    logPrintf("[att] fluxo de atitude %s (%d Hz): filtro | só acelerômetro | só giroscópio desde o a\n",
              attOn_ ? "ligado" : "desligado", ATT_PRINT_HZ);
}

void Console::attitudeSample(const Vec3 &acc, float tempC, uint32_t intUs) {
    if (!attOn_ || !attPrint_.due(intUs)) return;
    const AttitudeFilter &att = d_.fusion.att();
    const AttitudeFilter &go = d_.fusion.gyroOnly();
    AttReport r;
    r.initialized = att.initialized();
    r.rollDeg = att.rollDeg();
    r.pitchDeg = att.pitchDeg();
    r.degraded = att.degraded();
    r.accWeight = att.lastAccWeight();
    r.accOnly = accelRollPitch(acc);
    r.gyroOnlyInit = go.initialized();
    r.gyroOnlyRoll = go.rollDeg();
    r.gyroOnlyPitch = go.pitchDeg();
    r.accNorm = norm(acc);
    r.tempC = tempC;
    r.acq = d_.acqStats;
    r.fusion = d_.fusion.stats();
    reportSend(ReportKind::Att, r);
    d_.acqStats = AcqStats();
    attPrint_.restart(intUs, ATT_PRINT_US);
    d_.fusion.resetStats();
}

// ---------- Estado (c) ----------

void Console::sendStatus() const {
    StatusReport r;
    r.validity = d_.validityInputs();   // primeiro: as idades valem para o instante do pedido
    const AttitudeFilter &att = d_.fusion.att();
    r.imu = d_.cal.imuCal();
    r.level = d_.cal.level();
    r.attInitialized = att.initialized();
    r.rollDeg = att.rollDeg();
    r.pitchDeg = att.pitchDeg();
    r.attDegraded = att.degraded();
    r.gyroBias = d_.cal.gyroBias();
    r.gyroBiasTempC = d_.cal.gyroBiasTempC();
    r.gyroDefinitive = d_.cal.gyroDefinitive();
    const BaroService &baro = d_.baro;
    r.zeroValid = baro.zeroValid();
    r.zeroActive = baro.zeroActive();
    r.zeroRefHpa = baro.zeroRefHpa();
    r.zeroRefTempC = baro.zeroRefTempC();
    r.baro = baro.summary();
    r.magCal = d_.mag.calData();
    r.mag = d_.mag.summary();
    r.last = d_.acq.last();
    r.intOverflow = d_.intOverflow;
    r.radio = radioCounters();
    r.oled = statusCounters();
    reportSend(ReportKind::Status, r);   // fila cheia: a serial_task avisa o descarte
}
