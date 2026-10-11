// ESP A — avião. *** main.cpp PROVISÓRIO (etapa de bancada, antes do RTOS) ***
// Lê o MPU6050 a cada pulso de "data ready" e cuida da calibração (docs/calibracao.md):
//   boot: carrega da NVS os coeficientes do acelerômetro e mede o bias do giroscópio em repouso;
//   serial: c = estado | 6 = seis posições | g = calibrar giroscópio | r = resumo por segundo.
// A lógica de calibração está em lib/imu_cal (pura, testada com pio test -e native).
// Na semana 3 este arquivo passa a só inicializar o hardware e criar as tasks (src/tasks/):
//   sensor_task  (prio 4, core 1) — acordada pela INT do MPU, lê IMU a SAMPLE_RATE_HZ
//   fusion_task  (prio 3, core 1) — filtro complementar → roll/pitch/heading
//   radio_task   (prio 2, core 0) — monta TelemetryPacket e envia via ESP-NOW
//   status_task  (prio 1, core 0) — OLED de status + LED
#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include "config.h"
#include "telemetry.h"
#include "mpu6050.h"
#include "imu_cal.h"
#include "imu_store.h"

static Mpu6050 g_mpu;
static QueueHandle_t g_intQueue = nullptr;   // instantes (micros) dos pulsos de "data ready"
static bool g_mpuReady = false;

// ---------- Resumo por segundo (validação do driver) ----------

// Acúmulo de uma janela de impressão. As janelas têm limites contínuos, medidos pelos instantes
// dos pulsos: [startUs, startUs + WINDOW_US). Assim o tempo gasto imprimindo não distorce a taxa.
struct Window {
    uint32_t startUs = 0;
    uint32_t pulses = 0;       // pulsos de INT com instante dentro da janela
    uint32_t samples = 0;      // leituras válidas
    uint32_t lost = 0;         // pulsos que faltaram (intervalo maior que 1,5 período)
    uint32_t stale = 0;        // pulsos atendidos atrasados: a amostra já tinha sido sobrescrita
    uint32_t saturated = 0;    // amostras com algum eixo no limite da faixa
    double   sum[6] = {0}, sq[6] = {0};   // ax ay az gx gy gz
    float    peakGyro = 0;     // maior |g| em qualquer eixo
    float    tempSum = 0;
};
static const uint32_t WINDOW_US = MAIN_PRINT_MS * 1000UL;
static Window g_win;
static Window g_lastWin;          // última janela completa: base do comando c
static bool g_summaryOn = false;  // resumo por segundo (comando r)
static uint32_t g_lastIntUs = 0;

// ---------- Estado da calibração ----------

static ImuCalData g_cal;                  // o que está na NVS (e em uso para o acelerômetro)
static Vec3 g_gyroBias{0, 0, 0};          // bias em uso
static float g_gyroBiasTempC = 0;
static bool g_gyroDefinitive = false;     // bias aceito neste boot (equivale a ST_GYRO_CAL)
static bool g_tempWarned = false;
static BiasConfirmer g_biasConfirm;       // bias só é aceito com duas janelas consistentes
static float g_candTempC = 0;             // temperatura da janela candidata

enum class CalMode : uint8_t { None, GyroBoot, GyroCmd, SixWait, SixMeasure, SixReview };
static CalMode g_mode = CalMode::None;

// Janela de calibração em andamento (giroscópio ou uma das seis posições)
struct CalWindow {
    bool        active = false;
    uint32_t    startUs = 0;
    WindowStats gyro, accRaw, accCorr;
    float       tempSum = 0;
};
static CalWindow g_cw;
// Após configurar o MPU, a calibração descarta amostras até g_settleUntilUs. O estado explícito
// evita comparar o prazo depois de vencido: a diferença em int32_t volta a ficar negativa ~36 min
// depois, e todas as amostras passariam a ser descartadas.
static bool g_settling = false;
static uint32_t g_settleUntilUs = 0;
static uint32_t g_lastRecoveries = 0;

static int g_sixIdx = 0;
static Vec3 g_sixMeans[POSE_COUNT];
static float g_sixTempSum = 0;
static AccelCal g_sixResult;

static const uint32_t CAL_EXPECTED_SAMPLES = (uint32_t)CAL_WINDOW_MS * SAMPLE_RATE_HZ / 1000;
static const uint32_t CAL_MIN_SAMPLES = CAL_EXPECTED_SAMPLES * CAL_MIN_SAMPLES_PCT / 100;
static const PoseLimits POSE_LIMITS = {CAL_POSE_MIN_G, CAL_POSE_MAX_G};

static void IRAM_ATTR onMpuInt() {
    uint32_t t = micros();
    BaseType_t woke = pdFALSE;
    xQueueSendFromISR(g_intQueue, &t, &woke);
    if (woke) portYIELD_FROM_ISR();
}

static void startMpu() {
    const Mpu6050Config cfg = {
        &Wire, PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ, ADDR_MPU6050,
        MPU_ACCEL_RANGE_G, MPU_GYRO_RANGE_DPS, MPU_DLPF_HZ, SAMPLE_RATE_HZ, MPU_MAX_FALHAS_SEGUIDAS,
    };
    Mpu6050Status st = g_mpu.begin(cfg);
    if (st != Mpu6050Status::Ok) {
        Serial.printf("[A] MPU6050: %s (WHO_AM_I=0x%02X); nova tentativa em 2 s\n", mpu6050StatusText(st),
                      g_mpu.whoAmI());
        return;
    }
    Serial.printf("[A] MPU6050 configurado: ±%d g, ±%d °/s, DLPF %d Hz, %.1f Hz, I2C %lu Hz\n",
                  MPU_ACCEL_RANGE_G, MPU_GYRO_RANGE_DPS, MPU_DLPF_HZ, g_mpu.actualSampleRateHz(),
                  (unsigned long)I2C_FREQ_HZ);
    xQueueReset(g_intQueue);
    pinMode(PIN_MPU_INT, INPUT_PULLDOWN);
    attachInterrupt(digitalPinToInterrupt(PIN_MPU_INT), onMpuInt, RISING);
    g_mpuReady = true;
    g_lastIntUs = 0;
    // O giroscópio tem um transitório logo após a configuração: a calibração ignora esse trecho
    g_settling = true;
    g_settleUntilUs = micros() + CAL_MPU_SETTLE_MS * 1000UL;
    g_lastRecoveries = g_mpu.recoveries();
    g_win = Window();
    g_win.startUs = micros();
}

// ---------- Resumo por segundo ----------

static void accumulate(const ImuSample &s) {
    const float v[6] = {s.ax, s.ay, s.az, s.gx, s.gy, s.gz};
    for (int i = 0; i < 6; i++) {
        g_win.sum[i] += v[i];
        g_win.sq[i] += (double)v[i] * v[i];
    }
    for (int i = 3; i < 6; i++) g_win.peakGyro = fmaxf(g_win.peakGyro, fabsf(v[i]));
    g_win.tempSum += s.tempC;
    if (s.saturated) g_win.saturated++;
    g_win.samples++;
}

static void printWindow() {
    const uint32_t n = g_win.samples;
    const float rate = g_win.pulses * 1e6f / WINDOW_US;
    if (n == 0) {
        Serial.printf("[A] %.0f Hz | nenhuma leitura válida | falhas %lu (seguidas %lu, recuperações %lu)%s\n",
                      rate, (unsigned long)g_mpu.failTotal(), (unsigned long)g_mpu.failStreak(),
                      (unsigned long)g_mpu.recoveries(), g_win.pulses == 0 ? " | sem pulsos do INT" : "");
        return;
    }
    double mean[6], sd[6];
    for (int i = 0; i < 6; i++) {
        mean[i] = g_win.sum[i] / n;
        sd[i] = sqrt(fmax(0.0, g_win.sq[i] / n - mean[i] * mean[i]));
    }
    const double amod = sqrt(mean[0] * mean[0] + mean[1] * mean[1] + mean[2] * mean[2]);
    const double sdA = fmax(sd[0], fmax(sd[1], sd[2]));
    const double sdG = fmax(sd[3], fmax(sd[4], sd[5]));
    Serial.printf("[A] %.0f Hz | a (g) %+.3f %+.3f %+.3f  |a| %.3f | g (°/s) %+.2f %+.2f %+.2f | T %.1f °C\n",
                  rate, mean[0], mean[1], mean[2], amod, mean[3], mean[4], mean[5], g_win.tempSum / n);
    Serial.printf("    ruído: a %.1f mg, g %.3f °/s | pico |g| %.1f °/s | saturadas %lu | perdidas %lu | "
                  "descartadas %lu | falhas %lu (seguidas %lu, recuperações %lu)\n",
                  sdA * 1000.0, sdG, g_win.peakGyro, (unsigned long)g_win.saturated, (unsigned long)g_win.lost,
                  (unsigned long)g_win.stale, (unsigned long)g_mpu.failTotal(), (unsigned long)g_mpu.failStreak(),
                  (unsigned long)g_mpu.recoveries());
}

// Aviso único quando o chip se afasta da temperatura da calibração do giroscópio
static void checkTempDrift(float nowC) {
    if (!g_gyroDefinitive || g_tempWarned) return;
    if (tempDrifted(g_gyroBiasTempC, nowC, CAL_TEMP_WARN_C)) {
        Serial.printf("[cal] AVISO: chip a %.1f °C, calibração do giroscópio feita a %.1f °C. Recalibre com g\n",
                      nowC, g_gyroBiasTempC);
        g_tempWarned = true;
    }
}

// Fecha (imprime) todas as janelas que terminaram antes de nowUs e abre a seguinte sem lacuna.
// Diferença com sinal: um instante anterior ao início da janela dá negativo (e não um intervalo
// enorme), e a virada do micros() a cada ~71 min continua tratada.
static void closeWindowsUntil(uint32_t nowUs) {
    while ((int32_t)(nowUs - g_win.startUs) >= (int32_t)WINDOW_US) {
        if (g_summaryOn) printWindow();
        if (g_win.samples > 0) checkTempDrift(g_win.tempSum / g_win.samples);
        digitalWrite(PIN_LED_STATUS, !digitalRead(PIN_LED_STATUS));
        g_lastWin = g_win;
        const uint32_t next = g_win.startUs + WINDOW_US;
        g_win = Window();
        g_win.startUs = next;
    }
}

// Conta um pulso na janela do seu instante e detecta pulsos que faltaram
static void notePulse(uint32_t tInt) {
    closeWindowsUntil(tInt);
    // Pulso anterior ao início da janela (ela avançou pelo relógio enquanto ele esperava na fila):
    // pertence a uma janela já impressa e não entra na taxa da atual
    if ((int32_t)(tInt - g_win.startUs) >= 0) g_win.pulses++;
    const float periodUs = 1e6f / g_mpu.actualSampleRateHz();
    if (g_lastIntUs != 0) {
        const uint32_t dt = tInt - g_lastIntUs;
        if (dt > 1.5f * periodUs) g_win.lost += (uint32_t)lroundf(dt / periodUs) - 1;
    }
    g_lastIntUs = tInt;
}

// ---------- Calibração ----------

static const char *restText(RestCheck r) {
    switch (r) {
    case RestCheck::Ok:            return "ok";
    case RestCheck::TooFewSamples: return "janela incompleta";
    case RestCheck::Moving:        return "placa em movimento";
    case RestCheck::AccOutOfRange: return "|a| fora da faixa de repouso";
    }
    return "?";
}

static void startCalWindow() {
    g_cw = CalWindow();
    g_cw.active = true;
    g_cw.startUs = micros();
}

static void startGyroCal(CalMode mode) {
    g_mode = mode;
    g_biasConfirm.reset();
    Serial.printf("[cal] giroscópio: mantenha a placa parada (duas janelas de %d ms)...\n", CAL_WINDOW_MS);
    startCalWindow();
}

static void promptSixPose() {
    Serial.printf("[cal] posição %d/%d: %s. Posicione a caixa, solte e aperte Enter (x cancela)\n", g_sixIdx + 1,
                  POSE_COUNT, poseName((Pose)g_sixIdx));
    g_mode = CalMode::SixWait;
}

// Depois das seis posições (gravando ou não), retoma a calibração do giroscópio se ela ainda falta
static void endSix() {
    if (!g_gyroDefinitive) startGyroCal(CalMode::GyroBoot);
    else g_mode = CalMode::None;
}

static void finishGyroWindow() {
    // Sem calibração do acelerômetro, |a| ≈ 1,10 g em repouso: o critério de |a| não se aplica
    const RestLimits lim = g_cal.accelValid
                               ? RestLimits{CAL_REST_GYRO_MAX_STD_DPS, CAL_REST_ACC_MIN_G, CAL_REST_ACC_MAX_G}
                               : RestLimits{CAL_REST_GYRO_MAX_STD_DPS, 0.0f, 1e9f};
    const GyroBiasResult r = evaluateGyroWindow(g_cw.gyro, g_cw.accCorr, CAL_MIN_SAMPLES, lim);
    const float tempC = g_cw.tempSum / fmaxf(1, g_cw.gyro.count());
    const BiasConfirmer::Result res = g_biasConfirm.feed(r, CAL_BIAS_MAX_DELTA_DPS);

    if (res == BiasConfirmer::Result::Accepted) {
        const Vec3 b = g_biasConfirm.bias();
        const float t = (g_candTempC + tempC) / 2;
        g_gyroBias = b;
        g_gyroBiasTempC = t;
        g_gyroDefinitive = true;
        g_tempWarned = false;
        const bool saved = imuStoreSaveGyro(b, t);
        if (saved) {
            g_cal.gyroBias = b;
            g_cal.gyroTempC = t;
            g_cal.gyroValid = true;
        }
        Serial.printf("[cal] giroscópio calibrado: bias %+.3f %+.3f %+.3f °/s a %.1f °C (diferença entre "
                      "janelas %.3f °/s, desvio máx. %.3f °/s)%s\n",
                      b.x, b.y, b.z, t, g_biasConfirm.lastDelta(), r.gyroMaxStd,
                      saved ? "" : " | FALHA ao gravar na NVS");
        g_mode = CalMode::None;
        return;
    }
    if (res == BiasConfirmer::Result::Candidate || res == BiasConfirmer::Result::Inconsistent) {
        if (res == BiasConfirmer::Result::Inconsistent)
            Serial.printf("[cal] giroscópio: bias mudou %.3f °/s entre janelas (placa se acomodando); confirmando "
                          "de novo\n", g_biasConfirm.lastDelta());
        g_candTempC = tempC;
        startCalWindow();   // a janela que acabou é a candidata; a próxima confirma ou não
        return;
    }

    Serial.printf("[cal] giroscópio recusado: %s (desvio máx. %.3f °/s, |a| %.3f g, %lu amostras)\n",
                  restText(r.status), r.gyroMaxStd, r.accNorm, (unsigned long)g_cw.gyro.count());
    if (g_mode == CalMode::GyroBoot) {
        startGyroCal(CalMode::GyroBoot);   // tenta de novo, sem bloquear; o bias provisório segue em uso
    } else {
        Serial.println("[cal] mantido o bias anterior");
        g_mode = CalMode::None;
        if (!g_gyroDefinitive) startGyroCal(CalMode::GyroBoot);
    }
}

static void finishSixWindow() {
    const Pose pose = (Pose)g_sixIdx;
    const Vec3 m = g_cw.accRaw.mean();
    if (g_cw.accRaw.count() < CAL_MIN_SAMPLES) {
        Serial.printf("[cal] janela incompleta (%lu amostras): repita\n", (unsigned long)g_cw.accRaw.count());
        promptSixPose();
        return;
    }
    if (g_cw.gyro.maxStddev() >= CAL_REST_GYRO_MAX_STD_DPS) {
        Serial.printf("[cal] a caixa se mexeu (desvio do giroscópio %.3f °/s): repita\n", g_cw.gyro.maxStddev());
        promptSixPose();
        return;
    }
    const PoseCheck pc = checkPose(pose, m, POSE_LIMITS);
    if (pc != PoseCheck::Ok) {
        Serial.printf("[cal] a média (%+.3f %+.3f %+.3f g) não corresponde a \"%s\" (%s): confira e repita\n", m.x,
                      m.y, m.z, poseName(pose), pc == PoseCheck::WrongAxis ? "outro eixo" : "inclinada demais");
        promptSixPose();
        return;
    }
    g_sixMeans[g_sixIdx] = m;
    g_sixTempSum += g_cw.tempSum / g_cw.accRaw.count();
    Serial.printf("[cal] ok: %+.4f %+.4f %+.4f g\n", m.x, m.y, m.z);
    if (++g_sixIdx < POSE_COUNT) {
        promptSixPose();
        return;
    }

    if (!sixPositionCal(g_sixMeans, g_sixResult)) {
        Serial.println("[cal] escalas fora de 0,5..1,5: posições trocadas? Calibração descartada");
        endSix();
        return;
    }
    const AccelCal &c = g_sixResult;
    Serial.println("[cal] resultado das seis posições:");
    Serial.printf("  offset (mg): %+.1f %+.1f %+.1f | escala: %+.2f %% %+.2f %% %+.2f %%\n", c.offset.x * 1000,
                  c.offset.y * 1000, c.offset.z * 1000, (c.scale.x - 1) * 100, (c.scale.y - 1) * 100,
                  (c.scale.z - 1) * 100);
    float norms[POSE_COUNT];
    const bool valid = validateAccelCal(c, g_sixMeans, CAL_VALID_MIN_G, CAL_VALID_MAX_G, norms);
    for (int i = 0; i < POSE_COUNT; i++) {
        const bool ok = norms[i] >= CAL_VALID_MIN_G && norms[i] <= CAL_VALID_MAX_G;
        Serial.printf("  %-26s |a| corrigido %.4f g %s\n", poseName((Pose)i), norms[i], ok ? "" : "FORA DA FAIXA");
    }
    if (!valid) {
        // Resultado reprovado não pode ser gravado: provavelmente posições inclinadas demais
        Serial.printf("[cal] validação FALHOU (|a| fora de %.2f..%.2f g): resultado descartado, calibração "
                      "anterior mantida. Apoie a caixa bem no esquadro e refaça com 6\n",
                      CAL_VALID_MIN_G, CAL_VALID_MAX_G);
        endSix();
        return;
    }
    Serial.println("[cal] validação ok (|a| entre 0,98 e 1,02 g). s = gravar na NVS | x = descartar");
    g_mode = CalMode::SixReview;
}

static void finishCalWindow() {
    g_cw.active = false;
    switch (g_mode) {
    case CalMode::GyroBoot:
    case CalMode::GyroCmd:    finishGyroWindow(); break;
    case CalMode::SixMeasure: finishSixWindow(); break;
    default:                  break;
    }
}

// Alimenta a janela de calibração com uma amostra válida
static void calSample(const ImuSample &s) {
    // Reconfiguração do MPU (recuperação): transitório de partida; descarta e recomeça a janela
    if (g_mpu.recoveries() != g_lastRecoveries) {
        g_lastRecoveries = g_mpu.recoveries();
        g_settling = true;
        g_settleUntilUs = s.tUs + CAL_MPU_SETTLE_MS * 1000UL;
        g_biasConfirm.reset();   // a candidata é de antes da reconfiguração
        if (g_cw.active) startCalWindow();
    }
    // A comparação só acontece durante o descarte (no máximo CAL_MPU_SETTLE_MS), longe da virada
    if (g_settling) {
        if ((int32_t)(s.tUs - g_settleUntilUs) < 0) {
            if (g_cw.active) g_cw.startUs = s.tUs;   // a janela só começa a contar depois do descarte
            return;
        }
        g_settling = false;
    }
    if (!g_cw.active) return;

    const Vec3 a = {s.ax, s.ay, s.az};
    g_cw.gyro.add({s.gx, s.gy, s.gz});
    g_cw.accRaw.add(a);
    g_cw.accCorr.add(g_cal.accelValid ? applyAccelCal(g_cal.accel, a) : a);
    g_cw.tempSum += s.tempC;
    if ((uint32_t)(s.tUs - g_cw.startUs) >= CAL_WINDOW_MS * 1000UL) finishCalWindow();
}

static void printStatus() {
    Serial.println("[A] estado da calibração");
    if (g_cal.accelValid) {
        const AccelCal &c = g_cal.accel;
        Serial.printf("  acelerômetro: calibrado (±%d g, %.1f °C) | offset (mg) %+.1f %+.1f %+.1f | "
                      "escala %+.2f %% %+.2f %% %+.2f %%\n",
                      g_cal.accelRangeG, g_cal.accelTempC, c.offset.x * 1000, c.offset.y * 1000, c.offset.z * 1000,
                      (c.scale.x - 1) * 100, (c.scale.y - 1) * 100, (c.scale.z - 1) * 100);
    } else {
        Serial.println("  acelerômetro: NÃO calibrado (sem dados na NVS para esta faixa): use 6");
    }
    Serial.printf("  giroscópio: %s | bias %+.3f %+.3f %+.3f °/s | calibrado a %.1f °C\n",
                  g_gyroDefinitive ? "DEFINITIVO (deste boot)"
                                   : (g_cal.gyroValid ? "PROVISÓRIO (bias salvo de antes)" : "PROVISÓRIO (sem bias salvo)"),
                  g_gyroBias.x, g_gyroBias.y, g_gyroBias.z, g_gyroBiasTempC);

    const uint32_t n = g_lastWin.samples;
    if (n == 0) {
        Serial.println("  último 1 s: nenhuma leitura válida");
        return;
    }
    const Vec3 aRaw = {(float)(g_lastWin.sum[0] / n), (float)(g_lastWin.sum[1] / n), (float)(g_lastWin.sum[2] / n)};
    const Vec3 gRaw = {(float)(g_lastWin.sum[3] / n), (float)(g_lastWin.sum[4] / n), (float)(g_lastWin.sum[5] / n)};
    const Vec3 aCor = g_cal.accelValid ? applyAccelCal(g_cal.accel, aRaw) : aRaw;
    const Vec3 gCor = applyGyroBias(gRaw, g_gyroBias);
    const float tNow = g_lastWin.tempSum / n;
    Serial.printf("  último 1 s (%lu amostras, %.1f °C):\n", (unsigned long)n, tNow);
    Serial.printf("    a bruto     %+.4f %+.4f %+.4f g  |a| %.4f\n", aRaw.x, aRaw.y, aRaw.z, norm(aRaw));
    Serial.printf("    a corrigido %+.4f %+.4f %+.4f g  |a| %.4f\n", aCor.x, aCor.y, aCor.z, norm(aCor));
    Serial.printf("    g bruto     %+.3f %+.3f %+.3f °/s\n", gRaw.x, gRaw.y, gRaw.z);
    Serial.printf("    g corrigido %+.3f %+.3f %+.3f °/s\n", gCor.x, gCor.y, gCor.z);
    if (tempDrifted(g_gyroBiasTempC, tNow, CAL_TEMP_WARN_C) && (g_gyroDefinitive || g_cal.gyroValid))
        Serial.printf("  AVISO: temperatura a mais de %.1f °C da calibração do giroscópio: recalibre com g\n",
                      CAL_TEMP_WARN_C);
}

static void printHelp() {
    Serial.println("[A] comandos: c = estado | 6 = seis posições | g = calibrar giroscópio | r = resumo por segundo");
}

static void handleSerial() {
    while (Serial.available()) {
        const char ch = (char)Serial.read();
        switch (g_mode) {
        case CalMode::SixWait:
            if (ch == '\r' || ch == '\n') {
                Serial.printf("[cal] medindo %d ms, não toque na caixa...\n", CAL_WINDOW_MS);
                g_mode = CalMode::SixMeasure;
                startCalWindow();
            } else if (ch == 'x') {
                Serial.println("[cal] seis posições canceladas");
                endSix();
            }
            continue;
        case CalMode::SixMeasure:
            if (ch == 'x') {
                g_cw.active = false;
                Serial.println("[cal] seis posições canceladas");
                endSix();
            }
            continue;   // Enter extra (\n do CR+LF) e outras teclas são ignorados durante a medida
        case CalMode::SixReview:
            if (ch == 's') {
                const float tempC = g_sixTempSum / POSE_COUNT;
                if (imuStoreSaveAccel(g_sixResult, tempC, MPU_ACCEL_RANGE_G)) {
                    g_cal.accel = g_sixResult;
                    g_cal.accelTempC = tempC;
                    g_cal.accelRangeG = MPU_ACCEL_RANGE_G;
                    g_cal.accelValid = true;
                    Serial.println("[cal] calibração do acelerômetro gravada na NVS");
                } else {
                    Serial.println("[cal] FALHA ao gravar na NVS; calibração anterior mantida");
                }
                endSix();
            } else if (ch == 'x') {
                Serial.println("[cal] resultado descartado; calibração anterior mantida");
                endSix();
            }
            continue;
        default:
            break;
        }
        switch (ch) {
        case 'c': printStatus(); break;
        case 'r':
            g_summaryOn = !g_summaryOn;
            Serial.printf("[A] resumo por segundo %s\n", g_summaryOn ? "ligado" : "desligado");
            break;
        case 'g': startGyroCal(CalMode::GyroCmd); break;
        case '6':
            g_cw.active = false;
            g_sixIdx = 0;
            g_sixTempSum = 0;
            Serial.println("[cal] seis posições: cada medida leva 2 s com a caixa apoiada e parada");
            promptSixPose();
            break;
        case 'h':
        case '?': printHelp(); break;
        default: break;
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);
    pinMode(PIN_LED_STATUS, OUTPUT);
    Serial.printf("[A] boot — TelemetryPacket = %u bytes (main provisório: MPU6050 e calibração)\n",
                  (unsigned)sizeof(TelemetryPacket));
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
    Wire.setTimeOut(I2C_TIMEOUT_MS);   // vale para o barramento inteiro e sobrevive a Wire.end()/begin()
    g_intQueue = xQueueCreate(MPU_INT_QUEUE_LEN, sizeof(uint32_t));

    imuStoreLoad(g_cal, MPU_ACCEL_RANGE_G);
    if (g_cal.gyroValid) {
        g_gyroBias = g_cal.gyroBias;   // provisório até a janela deste boot ser aceita
        g_gyroBiasTempC = g_cal.gyroTempC;
    }
    Serial.printf("[A] NVS: acelerômetro %s | giroscópio %s\n", g_cal.accelValid ? "calibrado" : "sem calibração",
                  g_cal.gyroValid ? "com bias salvo" : "sem bias salvo");

    startMpu();
    printHelp();
    startGyroCal(CalMode::GyroBoot);
}

void loop() {
    handleSerial();
    if (!g_mpuReady) {
        delay(2000);
        startMpu();
        return;
    }

    uint32_t tInt;
    if (xQueueReceive(g_intQueue, &tInt, pdMS_TO_TICKS(MPU_INT_TIMEOUT_MS)) != pdTRUE) {
        // Pulsos pararam (chip sem alimentação, reiniciado em sleep ou fio do INT solto)
        g_mpu.checkAlive();
        // Pulsos que chegaram durante a verificação são processados antes de avançar pelo relógio
        if (uxQueueMessagesWaiting(g_intQueue) == 0) closeWindowsUntil(micros());
        return;
    }

    // Esvazia a fila: todos os pulsos contam para a taxa e as perdas, mas só o mais recente é lido,
    // porque o MPU guarda apenas a última amostra (as anteriores já foram sobrescritas)
    notePulse(tInt);
    uint32_t latest = tInt, t;
    while (xQueueReceive(g_intQueue, &t, 0) == pdTRUE) {
        g_win.stale++;   // o pulso anterior fica sem leitura; conta na janela dele, antes de avançar
        notePulse(t);
        latest = t;
    }

    // A borda de descida do pulso do INT induz ruído no SDA/SCL; lê só depois dela
    const uint32_t sinceInt = micros() - latest;
    if (sinceInt < MPU_INT_READ_DELAY_US) delayMicroseconds(MPU_INT_READ_DELAY_US - sinceInt);

    ImuSample s;
    if (g_mpu.read(s)) {
        accumulate(s);
        calSample(s);
    }
}
