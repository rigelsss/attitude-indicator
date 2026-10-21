// console_view.cpp — ver console_view.h
#include "console_view.h"
#include <math.h>
#include "config.h"
#include "logger.h"
#include "text_buf.h"
#include "imu_cal.h"
#include "telemetry_flags.h"

static const ValidityLimits VALIDITY_LIMITS = {FUSION_MAX_DT_MS, BARO_STALE_MS, MAG_STALE_MS};

// Buffer de formatação: o maior bloco é o c (~1,4 kB). Só a serial_task usa
static char s_text[CONSOLE_VIEW_BUFFER_BYTES];

// Entrega o bloco ao logger numa escrita só
static void emit(TextBuf &t) {
    logPrint(t.c_str());
    if (t.truncated()) logPrintln("[A] AVISO: saída do console cortada (buffer de formatação cheio)");
}

// ---------- Linhas dos sensores no r e no c ----------

static void baroLine(TextBuf &t, const BaroSummary &b) {
    if (!b.ready) {
        t.printf("    baro: barômetro indisponível (%s)\n", b.statusText);
        return;
    }
    t.printf("    baro: %s | ", b.ok ? "ok" : "FALHANDO");
    if (!b.have) t.print("sem leitura");
    else if (isfinite(b.altM))
        t.printf("%.2f hPa | alt %+.2f m | %.1f °C (ΔT %+.1f desde o zero)", b.pressHpa, b.altM, b.tempC,
                 b.tempC - b.zeroTempC);
    else if (b.zeroValid)
        t.printf("%.2f hPa | alt inválida | %.1f °C", b.pressHpa, b.tempC);
    else t.printf("%.2f hPa | sem zero | %.1f °C", b.pressHpa, b.tempC);
    t.printf(" | falhas %lu (seguidas %lu, recuperações %lu)\n", (unsigned long)b.failTotal,
             (unsigned long)b.failStreak, (unsigned long)b.recoveries);
}

static void magLine(TextBuf &t, const MagSummary &m) {
    if (!m.ready) {
        t.printf("    mag: HMC5883L indisponível (%s)\n", m.statusText);
        return;
    }
    t.printf("    mag: %s | %lu leituras no último 1 s | ", m.ok ? "ok" : "FALHANDO", (unsigned long)m.winReads);
    if (!m.have) t.print("sem leitura");
    else {
        t.printf("|m| %.1f µT", m.normUt);
        if (m.hdgInitialized) t.printf(" | proa %.1f°", m.hdgDeg);
        t.printf(" | %s%s", m.state, m.calValid ? "" : " SEM_CAL");
    }
    t.printf(" | falhas %lu (seguidas %lu, recuperações %lu) | saturadas %lu\n", (unsigned long)m.failTotal,
             (unsigned long)m.failStreak, (unsigned long)m.recoveries, (unsigned long)m.saturations);
}

// ---------- Resumo por segundo (r) ----------

static void printWindow(const WindowReport &r) {
    TextBuf t(s_text, sizeof(s_text));
    const SampleWindow &w = r.w;
    const WindowSummary s = summarize(w, r.windowUs);
    if (w.samples == 0) {
        t.printf("[A] %.0f Hz | nenhuma leitura válida | falhas %lu (seguidas %lu, recuperações %lu)%s\n", s.rateHz,
                 (unsigned long)r.mpu.failTotal, (unsigned long)r.mpu.failStreak, (unsigned long)r.mpu.recoveries,
                 w.pulses == 0 ? " | sem pulsos do INT" : "");
    } else {
        const double *mean = s.mean;
        t.printf("[A] %.0f Hz | a (g) %+.3f %+.3f %+.3f  |a| %.3f | g (°/s) %+.2f %+.2f %+.2f | T %.1f °C\n",
                 s.rateHz, mean[0], mean[1], mean[2], s.accNorm, mean[3], mean[4], mean[5], s.tempC);
        t.printf("    ruído: a %.1f mg, g %.3f °/s | pico |g| %.1f °/s | saturadas a %lu g %lu | perdidas %lu | "
                 "descartadas %lu | falhas %lu (seguidas %lu, recuperações %lu) | fila cheia %lu\n",
                 s.accMaxStdG * 1000.0, s.gyroMaxStdDps, w.peakGyro, (unsigned long)w.satAcc,
                 (unsigned long)w.satGyro, (unsigned long)w.lost, (unsigned long)w.stale,
                 (unsigned long)r.mpu.failTotal, (unsigned long)r.mpu.failStreak, (unsigned long)r.mpu.recoveries,
                 (unsigned long)r.mpu.intOverflow);
        const AngleStats *att = w.att;
        if (att[0].count() == 0)
            t.println("    atitude: não inicializada");
        else
            t.printf("    atitude: roll %+.2f ± %.2f° | pitch %+.2f ± %.2f° (%lu amostras)\n", att[0].mean(),
                     att[0].stddev(), att[1].mean(), att[1].stddev(), (unsigned long)att[0].count());
    }
    baroLine(t, r.baro);
    magLine(t, r.mag);
    emit(t);
}

// ---------- Fluxo de atitude (a) ----------

static void printAttitudeLine(const AttReport &r) {
    TextBuf t(s_text, sizeof(s_text));
    char gyroOnly[32] = "  --      --  ";
    if (r.gyroOnlyInit) snprintf(gyroOnly, sizeof(gyroOnly), "%+7.2f %+7.2f", r.gyroOnlyRoll, r.gyroOnlyPitch);
    if (r.initialized)
        t.printf("[att] roll %+7.2f pitch %+7.2f | acc %+7.2f %+7.2f | giro %s | |a| %.3f w %.2f | %.2f °C%s\n",
                 r.rollDeg, r.pitchDeg, r.accOnly.rollDeg, r.accOnly.pitchDeg, gyroOnly, r.accNorm, r.accWeight,
                 r.tempC, r.degraded ? " | DEGRADADO" : "");
    else
        t.printf("[att] aguardando referência | acc %+7.2f %+7.2f | |a| %.3f w %.2f | %.2f °C\n", r.accOnly.rollDeg,
                 r.accOnly.pitchDeg, r.accNorm, r.accWeight, r.tempC);
    const AcqStats &a = r.acq;
    const FusionStats &f = r.fusion;
    t.printf("      dt %.2f/%.2f/%.2f ms (%lu) | atraso máx %lu µs | filtro méd/máx %.0f/%lu µs | ambíguos %lu | "
             "descartados %lu | reinícios %lu\n",
             f.n ? f.dtMin * 1e3 : 0.0, f.n ? f.dtSum / f.n * 1e3 : 0.0, f.dtMax * 1e3, (unsigned long)f.n,
             (unsigned long)a.lagMaxUs, f.n ? (double)f.costSumUs / f.n : 0.0, (unsigned long)f.costMaxUs,
             (unsigned long)a.ambiguous, (unsigned long)a.dropped, (unsigned long)f.resets);
    emit(t);
}

// ---------- Estado (c) ----------

// Uma linha no estado (c): cada flag com o motivo quando está desligada
static void validityLine(TextBuf &t, const StatusReport &r) {
    const ValidityInputs &in = r.validity;
    const uint16_t v = telemetryValidFlags(in, VALIDITY_LIMITS);
    t.print("  validade (telemetria): ATT ");
    if (v & VAL_ATT) t.print("ok");
    else if (!in.attInitialized) t.print("não (filtro não inicializado)");
    else t.printf("não (MPU sem amostra há %lu ms)", (unsigned long)in.attAgeMs);
    t.printf(" | ATT_DEGRADED %s | ALT ", (v & VAL_ATT_DEGRADED) ? "sim" : "não");
    if (v & VAL_ALT) t.print("ok");
    else if (!in.baroOk) t.print("não (sem barômetro)");
    else if (!in.baroZeroValid) t.print("não (sem zero)");
    else if (!in.altFinite) t.print("não (valor inválido)");
    else t.printf("não (leitura de %lu ms atrás)", (unsigned long)in.baroAgeMs);
    t.print(" | HDG ");
    if (v & VAL_HDG) t.print("ok");
    else if (!(v & VAL_ATT)) t.print("não (sem atitude)");
    else if (!in.magCalibrated) t.print("não (magnetômetro sem calibração)");
    else t.print("não (filtro da proa não inicializado)");
    t.print(" | HDG_GYRO_ONLY ");
    if (!(v & VAL_HDG_GYRO_ONLY)) t.print("não");
    else if (in.attDegraded) t.print("sim (ATITUDE_DEGRADADA)");
    else if (in.magAgeMs > VALIDITY_LIMITS.magMaxAgeMs) t.print("sim (magnetômetro sem leitura recente)");
    else t.printf("sim (%s)", r.mag.state);
    t.printf(" | valid = 0x%04X\n", v);
}

static void printStatus(const StatusReport &r) {
    TextBuf t(s_text, sizeof(s_text));
    const ImuCalData &cal = r.imu;
    const LevelData &level = r.level;
    const Vec3 &bias = r.gyroBias;
    t.println("[A] estado da calibração");
    if (cal.accelValid) {
        const AccelCal &c = cal.accel;
        t.printf("  acelerômetro: calibrado (±%d g, %.1f °C) | offset (mg) %+.1f %+.1f %+.1f | "
                 "escala %+.2f %% %+.2f %% %+.2f %%\n",
                 cal.accelRangeG, cal.accelTempC, c.offset.x * 1000, c.offset.y * 1000, c.offset.z * 1000,
                 (c.scale.x - 1) * 100, (c.scale.y - 1) * 100, (c.scale.z - 1) * 100);
    } else {
        t.println("  acelerômetro: NÃO calibrado (sem dados na NVS para esta faixa): use 6");
    }
    if (level.valid)
        t.printf("  montagem: calibrada | módulo em roll %+.2f° pitch %+.2f° em relação à caixa (%.1f °C)\n",
                 level.mountRollDeg, level.mountPitchDeg, level.tempC);
    else
        t.println("  montagem: NÃO calibrada (eixos nominais): use z");
    if (r.attInitialized)
        t.printf("  atitude: roll %+.2f° pitch %+.2f°%s\n", r.rollDeg, r.pitchDeg,
                 r.attDegraded ? " (DEGRADADA: giroscópio saturou)" : "");
    else
        t.println("  atitude: aguardando referência (caixa parada, |a| perto de 1 g)");
    t.printf("  giroscópio: %s | bias %+.3f %+.3f %+.3f °/s | calibrado a %.1f °C\n",
             r.gyroDefinitive ? "DEFINITIVO (deste boot)"
                              : (cal.gyroValid ? "PROVISÓRIO (bias salvo de antes)" : "PROVISÓRIO (sem bias salvo)"),
             bias.x, bias.y, bias.z, r.gyroBiasTempC);

    if (r.zeroValid)
        t.printf("  altitude: zero p0 = %.2f hPa a %.1f °C%s\n", r.zeroRefHpa, r.zeroRefTempC,
                 r.zeroActive ? " (novo zero em andamento)" : "");
    else
        t.printf("  altitude: %s\n", r.zeroActive ? "zero em andamento" : "sem zero");
    baroLine(t, r.baro);

    const MagCalData &magCal = r.magCal;
    if (magCal.valid) {
        const MagCal &mc = magCal.cal;
        t.printf("  magnetômetro: calibrado | offset %+.1f %+.1f %+.1f µT | raio %.1f µT | resíduo %.1f %% | "
                 "%d setores\n",
                 mc.offset.x, mc.offset.y, mc.offset.z, mc.radiusUt, magCal.residPct, magCal.sectors);
        t.printf("  magnetômetro: W = [%+.3f %+.3f %+.3f; %+.3f %+.3f %+.3f; %+.3f %+.3f %+.3f]\n", mc.w.m[0][0],
                 mc.w.m[0][1], mc.w.m[0][2], mc.w.m[1][0], mc.w.m[1][1], mc.w.m[1][2], mc.w.m[2][0],
                 mc.w.m[2][1], mc.w.m[2][2]);
    } else {
        t.println("  magnetômetro: NÃO calibrado (offset zero, escala 1, sem detecção de distúrbio): use m");
    }
    t.printf("  magnetômetro: ±%.1f Ga, %.0f Hz, média de %d, leitura a %d Hz\n", MAG_RANGE_MGA / 1000.0f,
             MAG_OUTPUT_HZ, MAG_AVG_SAMPLES, MAG_READ_HZ);
    magLine(t, r.mag);
    validityLine(t, r);

    const SampleWindow &last = r.last;
    const uint32_t n = last.samples;
    if (n == 0) {
        t.println("  último 1 s: nenhuma leitura válida");
        emit(t);
        return;
    }
    const Vec3 aRaw = last.meanAcc();
    const Vec3 gRaw = last.meanGyro();
    const Vec3 aCor = cal.accelValid ? applyAccelCal(cal.accel, aRaw) : aRaw;
    const Vec3 gCor = applyGyroBias(gRaw, bias);
    const float tNow = last.meanTempC();
    t.printf("  pulsos do INT: no último 1 s, perdidos %lu e descartados %lu | fila cheia %lu desde o boot\n",
             (unsigned long)last.lost, (unsigned long)last.stale, (unsigned long)r.intOverflow);
    t.printf("  último 1 s (%lu amostras, %.1f °C):\n", (unsigned long)n, tNow);
    t.printf("    a bruto     %+.4f %+.4f %+.4f g  |a| %.4f\n", aRaw.x, aRaw.y, aRaw.z, norm(aRaw));
    t.printf("    a corrigido %+.4f %+.4f %+.4f g  |a| %.4f\n", aCor.x, aCor.y, aCor.z, norm(aCor));
    t.printf("    g bruto     %+.3f %+.3f %+.3f °/s\n", gRaw.x, gRaw.y, gRaw.z);
    t.printf("    g corrigido %+.3f %+.3f %+.3f °/s\n", gCor.x, gCor.y, gCor.z);
    if (tempDrifted(r.gyroBiasTempC, tNow, CAL_TEMP_WARN_C) && (r.gyroDefinitive || cal.gyroValid))
        t.printf("  AVISO: temperatura a mais de %.1f °C da calibração do giroscópio: recalibre com g\n",
                 CAL_TEMP_WARN_C);
    emit(t);
}

void consoleViewPrint(const ReportMsg &msg) {
    switch (msg.kind) {
    case ReportKind::Window: printWindow(msg.as<WindowReport>()); break;
    case ReportKind::Att:    printAttitudeLine(msg.as<AttReport>()); break;
    case ReportKind::Status: printStatus(msg.as<StatusReport>()); break;
    }
}
