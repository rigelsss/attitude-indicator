// console.cpp — ver console.h
#include "console.h"
#include <math.h>
#include "config.h"
#include "logger.h"

static const uint32_t ATT_PRINT_US = 1000000UL / ATT_PRINT_HZ;
static const ValidityLimits VALIDITY_LIMITS = {FUSION_MAX_DT_MS, BARO_STALE_MS, MAG_STALE_MS};

void Console::printHelp() const {
    logPrintln("[A] comandos: c = estado | 6 = seis posições | g = calibrar giroscópio | z = calibrar montagem | "
               "m = calibrar magnetômetro | a = fluxo de atitude | b = fluxo de altitude | p = fluxo de proa | "
               "0 = zerar altitude | r = resumo por segundo");
}

void Console::poll() {
    while (Serial.available()) {
        const char ch = (char)Serial.read();
        const MagKey mk = d_.mag.handleKey(ch);
        // d parou o laço por alguns segundos: a referência de tempo da fusão não vale mais
        if (mk == MagKey::HandledLoopStalled) d_.fusionReset();
        if (mk != MagKey::NotHandled) continue;
        const CalKey ck = d_.cal.handleKey(ch);   // seis posições e montagem em andamento
        if (ck == CalKey::HandledFusionReset) d_.fusionReset();
        if (ck != CalKey::NotHandled) continue;
        switch (ch) {
        case 'c': printStatus(); break;
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
        default: break;
        }
    }
}

// ---------- Resumo por segundo (r) ----------

void Console::windowClosed(const SampleWindow &w) const {
    if (!summaryOn_) return;
    printWindow(w);
    d_.baro.printSummary();
    d_.mag.printSummary();
}

void Console::printWindow(const SampleWindow &w) const {
    const WindowSummary s = summarize(w, d_.acq.windowUs());
    if (w.samples == 0) {
        logPrintf("[A] %.0f Hz | nenhuma leitura válida | falhas %lu (seguidas %lu, recuperações %lu)%s\n",
                  s.rateHz, (unsigned long)d_.mpu.failTotal(), (unsigned long)d_.mpu.failStreak(),
                  (unsigned long)d_.mpu.recoveries(), w.pulses == 0 ? " | sem pulsos do INT" : "");
        return;
    }
    const double *mean = s.mean;
    logPrintf("[A] %.0f Hz | a (g) %+.3f %+.3f %+.3f  |a| %.3f | g (°/s) %+.2f %+.2f %+.2f | T %.1f °C\n",
              s.rateHz, mean[0], mean[1], mean[2], s.accNorm, mean[3], mean[4], mean[5], s.tempC);
    logPrintf("    ruído: a %.1f mg, g %.3f °/s | pico |g| %.1f °/s | saturadas a %lu g %lu | perdidas %lu | "
              "descartadas %lu | falhas %lu (seguidas %lu, recuperações %lu) | fila cheia %lu\n",
              s.accMaxStdG * 1000.0, s.gyroMaxStdDps, w.peakGyro, (unsigned long)w.satAcc, (unsigned long)w.satGyro,
              (unsigned long)w.lost, (unsigned long)w.stale, (unsigned long)d_.mpu.failTotal(),
              (unsigned long)d_.mpu.failStreak(), (unsigned long)d_.mpu.recoveries(),
              (unsigned long)d_.intOverflow);
    const AngleStats *att = w.att;
    if (att[0].count() == 0) {
        logPrintln("    atitude: não inicializada");
        return;
    }
    logPrintf("    atitude: roll %+.2f ± %.2f° | pitch %+.2f ± %.2f° (%lu amostras)\n", att[0].mean(),
              att[0].stddev(), att[1].mean(), att[1].stddev(), (unsigned long)att[0].count());
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
    printAttitudeLine(acc, tempC);
    d_.acqStats = AcqStats();
    attPrint_.restart(intUs, ATT_PRINT_US);
    d_.fusion.resetStats();
}

void Console::printAttitudeLine(const Vec3 &acc, float tempC) const {
    const AttitudeFilter &att = d_.fusion.att();
    const Angles am = accelRollPitch(acc);
    const AttitudeFilter &go = d_.fusion.gyroOnly();
    char gyroOnly[32] = "  --      --  ";
    if (go.initialized()) snprintf(gyroOnly, sizeof(gyroOnly), "%+7.2f %+7.2f", go.rollDeg(), go.pitchDeg());
    if (att.initialized())
        logPrintf("[att] roll %+7.2f pitch %+7.2f | acc %+7.2f %+7.2f | giro %s | |a| %.3f w %.2f | %.2f °C%s\n",
                  att.rollDeg(), att.pitchDeg(), am.rollDeg, am.pitchDeg, gyroOnly, norm(acc),
                  att.lastAccWeight(), tempC, att.degraded() ? " | DEGRADADO" : "");
    else
        logPrintf("[att] aguardando referência | acc %+7.2f %+7.2f | |a| %.3f w %.2f | %.2f °C\n", am.rollDeg,
                  am.pitchDeg, norm(acc), att.lastAccWeight(), tempC);
    const AcqStats &a = d_.acqStats;
    const FusionStats &f = d_.fusion.stats();
    logPrintf("      dt %.2f/%.2f/%.2f ms (%lu) | atraso máx %lu µs | filtro méd/máx %.0f/%lu µs | ambíguos %lu | "
              "descartados %lu | reinícios %lu\n",
              f.n ? f.dtMin * 1e3 : 0.0, f.n ? f.dtSum / f.n * 1e3 : 0.0, f.dtMax * 1e3, (unsigned long)f.n,
              (unsigned long)a.lagMaxUs, f.n ? (double)f.costSumUs / f.n : 0.0, (unsigned long)f.costMaxUs,
              (unsigned long)a.ambiguous,
              (unsigned long)a.dropped, (unsigned long)f.resets);
}

// ---------- Estado (c) ----------

// Uma linha no estado (c): cada flag com o motivo quando está desligada. in é capturado no início do c:
// as linhas anteriores levam ~100 ms na serial, e as idades medidas depois delas refletiriam a impressão
void Console::printValidity(const ValidityInputs &in) const {
    const uint16_t v = telemetryValidFlags(in, VALIDITY_LIMITS);
    logPrint("  validade (telemetria): ATT ");
    if (v & VAL_ATT) logPrint("ok");
    else if (!in.attInitialized) logPrint("não (filtro não inicializado)");
    else logPrintf("não (MPU sem amostra há %lu ms)", (unsigned long)in.attAgeMs);
    logPrintf(" | ATT_DEGRADED %s | ALT ", (v & VAL_ATT_DEGRADED) ? "sim" : "não");
    if (v & VAL_ALT) logPrint("ok");
    else if (!in.baroOk) logPrint("não (sem barômetro)");
    else if (!in.baroZeroValid) logPrint("não (sem zero)");
    else if (!in.altFinite) logPrint("não (valor inválido)");
    else logPrintf("não (leitura de %lu ms atrás)", (unsigned long)in.baroAgeMs);
    logPrint(" | HDG ");
    if (v & VAL_HDG) logPrint("ok");
    else if (!(v & VAL_ATT)) logPrint("não (sem atitude)");
    else if (!in.magCalibrated) logPrint("não (magnetômetro sem calibração)");
    else logPrint("não (filtro da proa não inicializado)");
    logPrint(" | HDG_GYRO_ONLY ");
    if (!(v & VAL_HDG_GYRO_ONLY)) logPrint("não");
    else if (in.attDegraded) logPrint("sim (ATITUDE_DEGRADADA)");
    else if (in.magAgeMs > VALIDITY_LIMITS.magMaxAgeMs) logPrint("sim (magnetômetro sem leitura recente)");
    else logPrintf("sim (%s)", d_.mag.stateText());
    logPrintf(" | valid = 0x%04X\n", v);
}

void Console::printStatus() const {
    const ValidityInputs validity = d_.validityInputs();   // antes de imprimir (ver printValidity)
    const AttitudeFilter &att = d_.fusion.att();
    const ImuCalData &cal = d_.cal.imuCal();
    const LevelData &level = d_.cal.level();
    const Vec3 &bias = d_.cal.gyroBias();
    const float biasTempC = d_.cal.gyroBiasTempC();
    const bool definitive = d_.cal.gyroDefinitive();
    logPrintln("[A] estado da calibração");
    if (cal.accelValid) {
        const AccelCal &c = cal.accel;
        logPrintf("  acelerômetro: calibrado (±%d g, %.1f °C) | offset (mg) %+.1f %+.1f %+.1f | "
                  "escala %+.2f %% %+.2f %% %+.2f %%\n",
                  cal.accelRangeG, cal.accelTempC, c.offset.x * 1000, c.offset.y * 1000, c.offset.z * 1000,
                  (c.scale.x - 1) * 100, (c.scale.y - 1) * 100, (c.scale.z - 1) * 100);
    } else {
        logPrintln("  acelerômetro: NÃO calibrado (sem dados na NVS para esta faixa): use 6");
    }
    if (level.valid)
        logPrintf("  montagem: calibrada | módulo em roll %+.2f° pitch %+.2f° em relação à caixa (%.1f °C)\n",
                  level.mountRollDeg, level.mountPitchDeg, level.tempC);
    else
        logPrintln("  montagem: NÃO calibrada (eixos nominais): use z");
    if (att.initialized())
        logPrintf("  atitude: roll %+.2f° pitch %+.2f°%s\n", att.rollDeg(), att.pitchDeg(),
                  att.degraded() ? " (DEGRADADA: giroscópio saturou)" : "");
    else
        logPrintln("  atitude: aguardando referência (caixa parada, |a| perto de 1 g)");
    logPrintf("  giroscópio: %s | bias %+.3f %+.3f %+.3f °/s | calibrado a %.1f °C\n",
              definitive ? "DEFINITIVO (deste boot)"
                               : (cal.gyroValid ? "PROVISÓRIO (bias salvo de antes)" : "PROVISÓRIO (sem bias salvo)"),
              bias.x, bias.y, bias.z, biasTempC);

    const BaroService &baro = d_.baro;
    if (baro.zeroValid())
        logPrintf("  altitude: zero p0 = %.2f hPa a %.1f °C%s\n", baro.zeroRefHpa(), baro.zeroRefTempC(),
                  baro.zeroActive() ? " (novo zero em andamento)" : "");
    else
        logPrintf("  altitude: %s\n", baro.zeroActive() ? "zero em andamento" : "sem zero");
    baro.printSummary();

    const MagCalData &magCal = d_.mag.calData();
    if (magCal.valid) {
        const MagCal &mc = magCal.cal;
        logPrintf("  magnetômetro: calibrado | offset %+.1f %+.1f %+.1f µT | raio %.1f µT | resíduo %.1f %% | "
                  "%d setores\n",
                  mc.offset.x, mc.offset.y, mc.offset.z, mc.radiusUt, magCal.residPct, magCal.sectors);
        logPrintf("  magnetômetro: W = [%+.3f %+.3f %+.3f; %+.3f %+.3f %+.3f; %+.3f %+.3f %+.3f]\n", mc.w.m[0][0],
                  mc.w.m[0][1], mc.w.m[0][2], mc.w.m[1][0], mc.w.m[1][1], mc.w.m[1][2], mc.w.m[2][0],
                  mc.w.m[2][1], mc.w.m[2][2]);
    } else {
        logPrintln("  magnetômetro: NÃO calibrado (offset zero, escala 1, sem detecção de distúrbio): use m");
    }
    logPrintf("  magnetômetro: ±%.1f Ga, %.0f Hz, média de %d, leitura a %d Hz\n", MAG_RANGE_MGA / 1000.0f,
              MAG_OUTPUT_HZ, MAG_AVG_SAMPLES, MAG_READ_HZ);
    d_.mag.printSummary();
    printValidity(validity);

    const SampleWindow &last = d_.acq.last();
    const uint32_t n = last.samples;
    if (n == 0) {
        logPrintln("  último 1 s: nenhuma leitura válida");
        return;
    }
    const Vec3 aRaw = last.meanAcc();
    const Vec3 gRaw = last.meanGyro();
    const Vec3 aCor = cal.accelValid ? applyAccelCal(cal.accel, aRaw) : aRaw;
    const Vec3 gCor = applyGyroBias(gRaw, bias);
    const float tNow = last.meanTempC();
    logPrintf("  pulsos do INT: no último 1 s, perdidos %lu e descartados %lu | fila cheia %lu desde o boot\n",
              (unsigned long)last.lost, (unsigned long)last.stale, (unsigned long)d_.intOverflow);
    logPrintf("  último 1 s (%lu amostras, %.1f °C):\n", (unsigned long)n, tNow);
    logPrintf("    a bruto     %+.4f %+.4f %+.4f g  |a| %.4f\n", aRaw.x, aRaw.y, aRaw.z, norm(aRaw));
    logPrintf("    a corrigido %+.4f %+.4f %+.4f g  |a| %.4f\n", aCor.x, aCor.y, aCor.z, norm(aCor));
    logPrintf("    g bruto     %+.3f %+.3f %+.3f °/s\n", gRaw.x, gRaw.y, gRaw.z);
    logPrintf("    g corrigido %+.3f %+.3f %+.3f °/s\n", gCor.x, gCor.y, gCor.z);
    if (tempDrifted(biasTempC, tNow, CAL_TEMP_WARN_C) && (definitive || cal.gyroValid))
        logPrintf("  AVISO: temperatura a mais de %.1f °C da calibração do giroscópio: recalibre com g\n",
                  CAL_TEMP_WARN_C);
}
