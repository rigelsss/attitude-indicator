// ESP A — avião. *** main.cpp PROVISÓRIO (etapa de bancada, antes do RTOS) ***
// Lê o MPU6050 a cada pulso de "data ready", cuida da calibração (docs/calibracao.md) e roda o
// filtro complementar de roll e pitch (docs/fusao.md). Lê também o BME280 e calcula a altitude
// relativa (docs/barometro.md), e o HMC5883L para a proa compensada (docs/magnetometro.md):
//   boot: carrega da NVS os coeficientes do acelerômetro, a montagem e a calibração do magnetômetro,
//         mede o bias do giroscópio e zera a altitude;
//   serial: c = estado | 6 = seis posições | g = calibrar giroscópio | z = calibrar montagem |
//           m = calibrar magnetômetro | a = fluxo de atitude | b = fluxo de altitude | p = fluxo de proa |
//           0 = zerar altitude | r = resumo por segundo.
// A lógica está em lib/imu_cal, lib/cal_flow, lib/attitude, lib/fusion, lib/baro, lib/mag e lib/sched (puras,
// testadas com pio test -e native).
// As calibrações do MPU (giroscópio, seis posições e montagem) e a carga da NVS ficam em lib/cal_service;
// o barômetro e o magnetômetro são atendidos por lib/baro_service e lib/mag_service.
// A saída de texto passa por lib/logger. Refatoração em andamento: docs/refatoracao.md.
// Na semana 3 este arquivo passa a só inicializar o hardware e criar as tasks (src/tasks/):
//   sensor_task  (prio 4, core 1) — acordada pela INT do MPU a SAMPLE_RATE_HZ; dona do barramento: lê o
//                MPU e, intercalados, o BME280 e o HMC5883L; calibração e fusão (roll/pitch/proa).
//                É o laço de aquisição deste loop() (docs/plano.md, "Decisões para a semana 3")
//   radio_task   (prio 2, core 0) — monta TelemetryPacket e envia via ESP-NOW
//   status_task  (prio 1, core 0) — OLED de status (Wire1) + LED
//   e uma task de baixa prioridade para a serial (lib/logger e console)
#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include "config.h"
#include "telemetry.h"
#include "mpu6050.h"
#include "imu_cal.h"
#include "attitude.h"
#include "fusion.h"
#include "cal_service.h"
#include "baro_service.h"
#include "mag_service.h"
#include "telemetry_flags.h"
#include "logger.h"
#include "sched.h"

extern const char *const BUILD_STAMP;   // build_info.cpp: data e hora da compilação

static Mpu6050 g_mpu;
static QueueHandle_t g_intQueue = nullptr;   // instantes (micros) dos pulsos de "data ready"
// Pulsos que não couberam na fila (cheia: o laço ficou parado mais de MPU_INT_QUEUE_LEN períodos).
// Só a ISR escreve; o laço só lê. Total desde o boot. O pulso perdido também aparece em "perdidas"
// (intervalo entre pulsos), e a leitura seguinte sai pela checagem de atraso, sem dt errado na fusão.
static volatile uint32_t g_intOverflow = 0;
static bool g_mpuReady = false;
static uint32_t g_mpuRetryMs = 0;            // MPU ausente: próxima tentativa de configuração

// ---------- Resumo por segundo (validação do driver) ----------

// Acúmulo de uma janela de impressão. As janelas têm limites contínuos, medidos pelos instantes
// dos pulsos: [startUs, startUs + WINDOW_US). Assim o tempo gasto imprimindo não distorce a taxa.
struct Window {
    uint32_t startUs = 0;
    uint32_t pulses = 0;       // pulsos de INT com instante dentro da janela
    uint32_t samples = 0;      // leituras válidas
    uint32_t lost = 0;         // pulsos que faltaram (intervalo maior que 1,5 período)
    uint32_t stale = 0;        // pulsos atendidos atrasados: a amostra já tinha sido sobrescrita
    uint32_t satAcc = 0;       // amostras com algum eixo do acelerômetro no limite da faixa
    uint32_t satGyro = 0;      // idem, giroscópio
    double   sum[6] = {0}, sq[6] = {0};   // ax ay az gx gy gz
    float    peakGyro = 0;     // maior |g| em qualquer eixo
    float    tempSum = 0;
    AngleStats att[2];         // roll, pitch com a fusão inicializada (sem o salto de ±180°)
};
static const uint32_t WINDOW_US = MAIN_PRINT_MS * 1000UL;
static Window g_win;
static Window g_lastWin;          // última janela completa: base do comando c
static bool g_summaryOn = false;  // resumo por segundo (comando r)
static uint32_t g_lastIntUs = 0;

// ---------- Calibração (lib/cal_service, lib/cal_flow, docs/calibracao.md) ----------

static const uint32_t CAL_EXPECTED_SAMPLES = (uint32_t)CAL_WINDOW_MS * SAMPLE_RATE_HZ / 1000;
static const uint32_t CAL_MIN_SAMPLES = CAL_EXPECTED_SAMPLES * CAL_MIN_SAMPLES_PCT / 100;
static const PoseLimits POSE_LIMITS = {CAL_POSE_MIN_G, CAL_POSE_MAX_G};

static const CalServiceConfig CAL_CFG = {
    {CAL_WINDOW_MS * 1000UL,
     {CAL_MIN_SAMPLES,
      {CAL_REST_GYRO_MAX_STD_DPS, CAL_REST_ACC_MIN_G, CAL_REST_ACC_MAX_G},
      {CAL_REST_GYRO_MAX_STD_DPS, CAL_REST_ACC_RAW_MIN_G, CAL_REST_ACC_RAW_MAX_G},
      CAL_BIAS_MAX_DELTA_DPS, CAL_BIAS_MAX_TILT_DEG},
     {CAL_MIN_SAMPLES, CAL_REST_GYRO_MAX_STD_DPS, POSE_LIMITS, CAL_VALID_MIN_G, CAL_VALID_MAX_G},
     {CAL_MIN_SAMPLES, {CAL_REST_GYRO_MAX_STD_DPS, CAL_REST_ACC_MIN_G, CAL_REST_ACC_MAX_G},
      LEVEL_REST_GYRO_MAX_MEAN_DPS, LEVEL_MAX_TILT_DEG}},
    MPU_ACCEL_RANGE_G, CAL_STORE_ACC_OFFSET_MAX_G, CAL_STORE_GYRO_BIAS_MAX_DPS, LEVEL_MAX_TILT_DEG, CAL_TEMP_WARN_C,
};
static CalService g_calSvc(CAL_CFG);

// Após configurar o MPU, a calibração descarta amostras até g_settleUntilUs. O estado explícito
// evita comparar o prazo depois de vencido: a diferença em int32_t volta a ficar negativa ~36 min
// depois, e todas as amostras passariam a ser descartadas.
static bool g_settling = false;
static uint32_t g_settleUntilUs = 0;
static uint32_t g_lastRecoveries = 0;

// ---------- Fusão (lib/fusion, lib/attitude, docs/fusao.md) ----------

static const FusionParams FUSION_PARAMS = {COMP_FILTER_TAU_S, FUSION_ACC_FULL_DEV_G, FUSION_ACC_ZERO_DEV_G,
                                           FUSION_ROLL_MIN_G, FUSION_MAX_DT_MS / 1000.0f, FUSION_INIT_SAMPLES,
                                           FUSION_INIT_MAX_RATE_DPS};
static const HeadingParams HEADING_PARAMS = {MAG_HEADING_TAU_S, MAG_MAX_PITCH_DEG, MAG_DISTURB_PCT,
                                             MAG_MAX_DT_MS / 1000.0f};
// Atitude, diagnóstico só de giroscópio e proa (predição aqui, correção pelo serviço do magnetômetro)
static FusionCore g_fusion(FUSION_PARAMS, HEADING_PARAMS);
static const AttitudeFilter &g_att = g_fusion.att();

// Fluxo de atitude (comando a): estatística da aquisição entre duas linhas; a do filtro fica em
// g_fusion.stats()
struct AttStream {
    bool     on = false;
    Periodic print;            // próxima linha
    uint32_t lagMaxUs = 0;     // maior atraso pulso → início da leitura
    uint32_t ambiguous = 0;    // quadros fora da calibração e da fusão: pulso mais novo durante a leitura (ou atraso excessivo)
    uint32_t dropped = 0;      // pulsos sem leitura (amostra sobrescrita)
};
static AttStream g_as;
static const uint32_t ATT_PRINT_US = 1000000UL / ATT_PRINT_HZ;

// ---------- Barômetro (lib/baro_service, docs/barometro.md) ----------

static const BaroServiceConfig BARO_CFG = {
    {&Wire, ADDR_BME280, BARO_OSRS_T, BARO_OSRS_P, BARO_IIR_COEF, BARO_STANDBY_US, BARO_MAX_FAIL_STREAK},
    {BARO_SETTLE_MS, BARO_ZERO_WINDOW_MS,
     (uint32_t)BARO_ZERO_WINDOW_MS * BARO_READ_HZ / 1000 * BARO_ZERO_MIN_SAMPLES_PCT / 100, BARO_ZERO_MAX_STD_PA,
     BARO_REF_MIN_HPA, BARO_REF_MAX_HPA},
    BARO_READ_HZ, BARO_RETRY_MS, BARO_FAIL_RETRY_MS, BARO_TEMP_WARN_C, BARO_PRINT_HZ,
};
static BaroService g_baroSvc(BARO_CFG);

// ---------- Magnetômetro (lib/mag_service, docs/magnetometro.md) ----------

static const MagServiceConfig MAG_CFG = {
    {&Wire, ADDR_HMC5883L, MAG_AVG_SAMPLES, MAG_OUTPUT_HZ, MAG_RANGE_MGA, MAG_MAX_FAIL_STREAK},
    {MAG_CAL_MS, MAG_CAL_PROGRESS_MS, (size_t)MAG_CAL_MS / 1000 * MAG_READ_HZ,
     {MAG_CAL_MIN_RADIUS_UT, MAG_CAL_MAX_RADIUS_UT, MAG_CAL_MAX_RESID_PCT, MAG_CAL_MIN_SCALE, MAG_CAL_MAX_SCALE,
      MAG_CAL_MIN_SECTORS, MAG_CAL_MIN_PTS_SECTOR}},
    MAG_READ_HZ, MAG_RETRY_MS, MAG_FAIL_RETRY_MS, MAG_DISTURB_PCT, MAG_REF_FIELD_UT, MAG_PRINT_HZ,
};
static MagService g_magSvc(MAG_CFG, g_fusion.att(), g_fusion.heading());

static void fusionReset() { g_fusion.reset(); }

// Relógio do custo do filtro no fluxo a (o micros() devolve unsigned long, não uint32_t)
static uint32_t clockUs() { return micros(); }

// Após configurar o MPU, calibração e fusão descartam amostras por CAL_MPU_SETTLE_MS
static void beginSettle() {
    g_settling = true;
    g_settleUntilUs = micros() + CAL_MPU_SETTLE_MS * 1000UL;
}

static void IRAM_ATTR onMpuInt() {
    uint32_t t = micros();
    BaseType_t woke = pdFALSE;
    if (xQueueSendFromISR(g_intQueue, &t, &woke) != pdTRUE) g_intOverflow++;
    if (woke) portYIELD_FROM_ISR();
}

static void startMpu() {
    const Mpu6050Config cfg = {
        &Wire, PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ, ADDR_MPU6050,
        MPU_ACCEL_RANGE_G, MPU_GYRO_RANGE_DPS, MPU_DLPF_HZ, SAMPLE_RATE_HZ, MPU_MAX_FALHAS_SEGUIDAS,
    };
    Mpu6050Status st = g_mpu.begin(cfg);
    if (st != Mpu6050Status::Ok) {
        logPrintf("[A] MPU6050: %s (WHO_AM_I=0x%02X); nova tentativa em %d s\n", mpu6050StatusText(st),
                  g_mpu.whoAmI(), MPU_RETRY_MS / 1000);
        g_mpuRetryMs = millis() + MPU_RETRY_MS;
        return;
    }
    logPrintf("[A] MPU6050 configurado: ±%d g, ±%d °/s, DLPF %d Hz, %.1f Hz, I2C %lu Hz\n",
              MPU_ACCEL_RANGE_G, MPU_GYRO_RANGE_DPS, MPU_DLPF_HZ, g_mpu.actualSampleRateHz(),
              (unsigned long)I2C_FREQ_HZ);
    xQueueReset(g_intQueue);
    pinMode(PIN_MPU_INT, INPUT_PULLDOWN);
    attachInterrupt(digitalPinToInterrupt(PIN_MPU_INT), onMpuInt, RISING);
    g_mpuReady = true;
    g_lastIntUs = 0;
    // O giroscópio tem um transitório logo após a configuração: calibração e fusão ignoram esse trecho
    beginSettle();
    fusionReset();
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
    if (s.accelSaturated) g_win.satAcc++;
    if (s.gyroSaturated) g_win.satGyro++;
    g_win.samples++;
}

static void printWindow() {
    const uint32_t n = g_win.samples;
    const float rate = g_win.pulses * 1e6f / WINDOW_US;
    if (n == 0) {
        logPrintf("[A] %.0f Hz | nenhuma leitura válida | falhas %lu (seguidas %lu, recuperações %lu)%s\n",
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
    logPrintf("[A] %.0f Hz | a (g) %+.3f %+.3f %+.3f  |a| %.3f | g (°/s) %+.2f %+.2f %+.2f | T %.1f °C\n",
              rate, mean[0], mean[1], mean[2], amod, mean[3], mean[4], mean[5], g_win.tempSum / n);
    logPrintf("    ruído: a %.1f mg, g %.3f °/s | pico |g| %.1f °/s | saturadas a %lu g %lu | perdidas %lu | "
              "descartadas %lu | falhas %lu (seguidas %lu, recuperações %lu) | fila cheia %lu\n",
              sdA * 1000.0, sdG, g_win.peakGyro, (unsigned long)g_win.satAcc, (unsigned long)g_win.satGyro,
              (unsigned long)g_win.lost, (unsigned long)g_win.stale, (unsigned long)g_mpu.failTotal(),
              (unsigned long)g_mpu.failStreak(), (unsigned long)g_mpu.recoveries(),
              (unsigned long)g_intOverflow);
    const AngleStats *att = g_win.att;
    if (att[0].count() == 0) {
        logPrintln("    atitude: não inicializada");
        return;
    }
    logPrintf("    atitude: roll %+.2f ± %.2f° | pitch %+.2f ± %.2f° (%lu amostras)\n", att[0].mean(),
              att[0].stddev(), att[1].mean(), att[1].stddev(), (unsigned long)att[0].count());
}

// Fecha (imprime) todas as janelas que terminaram antes de nowUs e abre a seguinte sem lacuna.
// Diferença com sinal: um instante anterior ao início da janela dá negativo (e não um intervalo
// enorme), e a virada do micros() a cada ~71 min continua tratada.
static void closeWindowsUntil(uint32_t nowUs) {
    while ((int32_t)(nowUs - g_win.startUs) >= (int32_t)WINDOW_US) {
        g_magSvc.closeWindow();
        if (g_summaryOn) {
            printWindow();
            g_baroSvc.printSummary();
            g_magSvc.printSummary();
        }
        if (g_win.samples > 0) g_calSvc.checkTempDrift(g_win.tempSum / g_win.samples);
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


// Reconfiguração do MPU (recuperação em read() ou em checkAlive()): o transitório de partida é
// descartado, a janela de calibração em andamento recomeça e a fusão perde a referência de tempo
static void checkRecovery() {
    if (g_mpu.recoveries() == g_lastRecoveries) return;
    g_lastRecoveries = g_mpu.recoveries();
    xQueueReset(g_intQueue);   // pulsos enfileirados são de antes da reconfiguração
    beginSettle();
    g_calSvc.onMpuReconfigured();   // a candidata é de antes da reconfiguração; a janela recomeça
    fusionReset();
}

// ---------- Fusão ----------

static void printAttitudeLine(const Vec3 &acc, float tempC) {
    const Angles am = accelRollPitch(acc);
    const AttitudeFilter &go = g_fusion.gyroOnly();
    char gyroOnly[32] = "  --      --  ";
    if (go.initialized()) snprintf(gyroOnly, sizeof(gyroOnly), "%+7.2f %+7.2f", go.rollDeg(), go.pitchDeg());
    if (g_att.initialized())
        logPrintf("[att] roll %+7.2f pitch %+7.2f | acc %+7.2f %+7.2f | giro %s | |a| %.3f w %.2f | %.2f °C%s\n",
                  g_att.rollDeg(), g_att.pitchDeg(), am.rollDeg, am.pitchDeg, gyroOnly, norm(acc),
                  g_att.lastAccWeight(), tempC, g_att.degraded() ? " | DEGRADADO" : "");
    else
        logPrintf("[att] aguardando referência | acc %+7.2f %+7.2f | |a| %.3f w %.2f | %.2f °C\n", am.rollDeg,
                  am.pitchDeg, norm(acc), g_att.lastAccWeight(), tempC);
    const AttStream &a = g_as;
    const FusionStats &f = g_fusion.stats();
    logPrintf("      dt %.2f/%.2f/%.2f ms (%lu) | atraso máx %lu µs | filtro méd/máx %.0f/%lu µs | ambíguos %lu | "
              "descartados %lu | reinícios %lu\n",
              f.n ? f.dtMin * 1e3 : 0.0, f.n ? f.dtSum / f.n * 1e3 : 0.0, f.dtMax * 1e3, (unsigned long)f.n,
              (unsigned long)a.lagMaxUs, f.n ? (double)f.costSumUs / f.n : 0.0, (unsigned long)f.costMaxUs,
              (unsigned long)a.ambiguous,
              (unsigned long)a.dropped, (unsigned long)f.resets);
}

// Uma amostra válida, fora do descarte: calibração nos eixos do chip → eixos do modelo →
// rotação de montagem → filtro. intUs: instante do pulso de "data ready" desta amostra, que já
// passou pela validade temporal no loop().
static void fusionSample(const ImuSample &s, uint32_t intUs) {
    const ImuCalData &cal = g_calSvc.imuCal();
    const BodyImu b = imuToBody({s.ax, s.ay, s.az}, {s.gx, s.gy, s.gz}, cal.accelValid, cal.accel, g_calSvc.gyroBias(),
                                g_calSvc.level().r);
    const ImuFlags flags = {s.accelSaturated, s.gyroSaturated};
    g_fusion.update(b.acc, b.gyro, flags, intUs, g_magSvc.calibrating());
    if (g_att.initialized()) {
        g_win.att[0].add(g_att.rollDeg());
        g_win.att[1].add(g_att.pitchDeg());
    }

    if (g_as.on && g_as.print.due(intUs)) {
        printAttitudeLine(b.acc, s.tempC);
        const bool on = g_as.on;
        g_as = AttStream();
        g_as.on = on;
        g_as.print.restart(intUs, ATT_PRINT_US);
        g_fusion.resetStats();
    }
}

// ---------- Validade dos dados (campo valid do TelemetryPacket, lib/telemetry_flags) ----------

static const ValidityLimits VALIDITY_LIMITS = {FUSION_MAX_DT_MS, BARO_STALE_MS, MAG_STALE_MS};

static uint32_t ageMs(bool have, uint32_t tMs) {
    return have ? millis() - tMs : AGE_NEVER;
}

static ValidityInputs validityInputs() {
    ValidityInputs in;
    in.attInitialized = g_att.initialized();
    in.attDegraded = g_att.degraded();
    in.attAgeMs = g_fusion.havePrev() ? (micros() - g_fusion.prevIntUs()) / 1000UL : AGE_NEVER;
    in.baroOk = g_baroSvc.ok();
    in.baroZeroValid = g_baroSvc.zeroValid();
    in.altFinite = isfinite(g_baroSvc.altitudeM());
    in.baroAgeMs = ageMs(g_baroSvc.have(), g_baroSvc.last().tMs);
    in.hdgInitialized = g_fusion.heading().initialized();
    in.magCalibrated = g_magSvc.calData().valid;
    in.hdgMagSuspended = g_magSvc.correctionSuspended();
    in.magAgeMs = ageMs(g_magSvc.lastReadValid(), g_magSvc.lastReadMs());
    return in;
}

// Uma linha no estado (c): cada flag com o motivo quando está desligada. in é capturado no início do c:
// as linhas anteriores levam ~100 ms na serial, e as idades medidas depois delas refletiriam a impressão
static void printValidity(const ValidityInputs &in) {
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
    else logPrintf("sim (%s)", g_magSvc.stateText());
    logPrintf(" | valid = 0x%04X\n", v);
}

static void printStatus() {
    const ValidityInputs validity = validityInputs();   // antes de imprimir (ver printValidity)
    const ImuCalData &cal = g_calSvc.imuCal();
    const LevelData &level = g_calSvc.level();
    const Vec3 &bias = g_calSvc.gyroBias();
    const float biasTempC = g_calSvc.gyroBiasTempC();
    const bool definitive = g_calSvc.gyroDefinitive();
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
    if (g_att.initialized())
        logPrintf("  atitude: roll %+.2f° pitch %+.2f°%s\n", g_att.rollDeg(), g_att.pitchDeg(),
                  g_att.degraded() ? " (DEGRADADA: giroscópio saturou)" : "");
    else
        logPrintln("  atitude: aguardando referência (caixa parada, |a| perto de 1 g)");
    logPrintf("  giroscópio: %s | bias %+.3f %+.3f %+.3f °/s | calibrado a %.1f °C\n",
              definitive ? "DEFINITIVO (deste boot)"
                               : (cal.gyroValid ? "PROVISÓRIO (bias salvo de antes)" : "PROVISÓRIO (sem bias salvo)"),
              bias.x, bias.y, bias.z, biasTempC);

    if (g_baroSvc.zeroValid())
        logPrintf("  altitude: zero p0 = %.2f hPa a %.1f °C%s\n", g_baroSvc.zeroRefHpa(),
                  g_baroSvc.zeroRefTempC(), g_baroSvc.zeroActive() ? " (novo zero em andamento)" : "");
    else
        logPrintf("  altitude: %s\n", g_baroSvc.zeroActive() ? "zero em andamento" : "sem zero");
    g_baroSvc.printSummary();

    const MagCalData &magCal = g_magSvc.calData();
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
    g_magSvc.printSummary();
    printValidity(validity);

    const uint32_t n = g_lastWin.samples;
    if (n == 0) {
        logPrintln("  último 1 s: nenhuma leitura válida");
        return;
    }
    const Vec3 aRaw = {(float)(g_lastWin.sum[0] / n), (float)(g_lastWin.sum[1] / n), (float)(g_lastWin.sum[2] / n)};
    const Vec3 gRaw = {(float)(g_lastWin.sum[3] / n), (float)(g_lastWin.sum[4] / n), (float)(g_lastWin.sum[5] / n)};
    const Vec3 aCor = cal.accelValid ? applyAccelCal(cal.accel, aRaw) : aRaw;
    const Vec3 gCor = applyGyroBias(gRaw, bias);
    const float tNow = g_lastWin.tempSum / n;
    logPrintf("  pulsos do INT: no último 1 s, perdidos %lu e descartados %lu | fila cheia %lu desde o boot\n",
              (unsigned long)g_lastWin.lost, (unsigned long)g_lastWin.stale, (unsigned long)g_intOverflow);
    logPrintf("  último 1 s (%lu amostras, %.1f °C):\n", (unsigned long)n, tNow);
    logPrintf("    a bruto     %+.4f %+.4f %+.4f g  |a| %.4f\n", aRaw.x, aRaw.y, aRaw.z, norm(aRaw));
    logPrintf("    a corrigido %+.4f %+.4f %+.4f g  |a| %.4f\n", aCor.x, aCor.y, aCor.z, norm(aCor));
    logPrintf("    g bruto     %+.3f %+.3f %+.3f °/s\n", gRaw.x, gRaw.y, gRaw.z);
    logPrintf("    g corrigido %+.3f %+.3f %+.3f °/s\n", gCor.x, gCor.y, gCor.z);
    if (tempDrifted(biasTempC, tNow, CAL_TEMP_WARN_C) && (definitive || cal.gyroValid))
        logPrintf("  AVISO: temperatura a mais de %.1f °C da calibração do giroscópio: recalibre com g\n",
                  CAL_TEMP_WARN_C);
}

static void printHelp() {
    logPrintln("[A] comandos: c = estado | 6 = seis posições | g = calibrar giroscópio | z = calibrar montagem | "
               "m = calibrar magnetômetro | a = fluxo de atitude | b = fluxo de altitude | p = fluxo de proa | "
               "0 = zerar altitude | r = resumo por segundo");
}

static void handleSerial() {
    while (Serial.available()) {
        const char ch = (char)Serial.read();
        const MagKey mk = g_magSvc.handleKey(ch);
        // d parou o laço por alguns segundos: a referência de tempo da fusão não vale mais
        if (mk == MagKey::HandledLoopStalled) fusionReset();
        if (mk != MagKey::NotHandled) continue;
        const CalKey ck = g_calSvc.handleKey(ch);   // seis posições e montagem em andamento
        if (ck == CalKey::HandledFusionReset) fusionReset();
        if (ck != CalKey::NotHandled) continue;
        switch (ch) {
        case 'c': printStatus(); break;
        case 'r':
            g_summaryOn = !g_summaryOn;
            logPrintf("[A] resumo por segundo %s\n", g_summaryOn ? "ligado" : "desligado");
            break;
        case 'g': g_calSvc.startGyro(GyroCalKind::Cmd); break;
        case 'z': g_calSvc.startMount(); break;
        case 'a': {
            const bool on = !g_as.on;
            g_as = AttStream();
            g_as.on = on;
            g_as.print.start(micros());
            g_fusion.resetStats();
            g_fusion.resetGyroOnly();   // o diagnóstico só de giroscópio recomeça da atitude atual
            logPrintf("[att] fluxo de atitude %s (%d Hz): filtro | só acelerômetro | só giroscópio desde o a\n",
                      on ? "ligado" : "desligado", ATT_PRINT_HZ);
            break;
        }
        case 'b': g_baroSvc.toggleStream(micros()); break;
        case 'm': g_magSvc.startCalibration(!g_calSvc.magAllowed()); break;
        case 'p': g_magSvc.toggleStream(micros()); break;
        case '0': g_baroSvc.zeroAltitude(); break;
        case '6': g_calSvc.startSix(); break;
        case 'h':
        case '?': printHelp(); break;
        default: break;
        }
    }
}

void setup() {
    // Buffer de transmissão em software (antes do begin): sem ele, cada impressão bloqueia o laço
    // até sobrar só a FIFO de 128 bytes da UART (~10 ms por linha longa a 115200) e descarta amostras
    Serial.setTxBufferSize(SERIAL_TX_BUFFER_BYTES);
    Serial.begin(115200);
    delay(500);
    pinMode(PIN_LED_STATUS, OUTPUT);
    logPrintf("[A] boot — compilado em %s — TelemetryPacket = %u bytes (main provisório: MPU6050 e "
              "calibração)\n", BUILD_STAMP, (unsigned)sizeof(TelemetryPacket));
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
    Wire.setTimeOut(I2C_TIMEOUT_MS);   // vale para o barramento inteiro e sobrevive a Wire.end()/begin()
    g_intQueue = xQueueCreate(MPU_INT_QUEUE_LEN, sizeof(uint32_t));
    if (g_intQueue == nullptr) {   // a ISR do INT usa a fila: sem ela não há como seguir
        logPrintln("[A] ERRO: sem memória para a fila do INT; reiniciando em 1 s");
        logFlush();
        delay(1000);
        ESP.restart();
    }

    g_calSvc.loadFromNvs();
    g_magSvc.loadCalibration();
    g_calSvc.printNvsSummary(g_magSvc.calData().valid);

    g_fusion.setClock(clockUs);
    startMpu();
    g_baroSvc.begin();
    g_magSvc.begin();
    printHelp();
    g_calSvc.startGyro(GyroCalKind::Boot);
}

// Espera o próximo pulso do INT por até MPU_INT_TIMEOUT_MS, em fatias de MPU_INT_WAIT_SLICE_MS.
// Com o MPU funcionando, o pulso chega (a cada 5 ms) dentro da primeira fatia, e o barômetro segue
// atendido logo depois da leitura do MPU. Se os pulsos pararem, ele é atendido entre as fatias.
static bool waitPulse(uint32_t &tInt) {
    const uint32_t t0 = millis();
    for (;;) {
        if (xQueueReceive(g_intQueue, &tInt, pdMS_TO_TICKS(MPU_INT_WAIT_SLICE_MS)) == pdTRUE) return true;
        if (millis() - t0 >= MPU_INT_TIMEOUT_MS) return false;
        g_baroSvc.service(micros());
        g_magSvc.service(micros());
    }
}

void loop() {
    handleSerial();
    if (!g_mpuReady) {
        // Sem MPU, o barômetro continua sendo atendido; a configuração é tentada a cada MPU_RETRY_MS
        if ((int32_t)(millis() - g_mpuRetryMs) >= 0) startMpu();
        g_baroSvc.service(micros());
        g_magSvc.service(micros());
        delay(1);
        return;
    }

    uint32_t tInt;
    if (!waitPulse(tInt)) {
        // Pulsos pararam (chip sem alimentação, reiniciado em sleep ou fio do INT solto)
        g_mpu.checkAlive();
        checkRecovery();   // checkAlive() pode ter reconfigurado o chip
        // Pulsos que chegaram durante a verificação são processados antes de avançar pelo relógio
        if (uxQueueMessagesWaiting(g_intQueue) == 0) closeWindowsUntil(micros());
        return;
    }

    // Esvazia a fila num buffer local e lê o MPU antes de contar as janelas: a contagem pode imprimir
    // o resumo, e uma leitura feita depois disso seria de um pulso posterior ao último visto.
    // Só o pulso mais recente é lido, porque o MPU guarda apenas a última amostra.
    uint32_t pulses[MPU_INT_QUEUE_LEN];
    size_t n = 0;
    pulses[n++] = tInt;
    while (n < MPU_INT_QUEUE_LEN && xQueueReceive(g_intQueue, &pulses[n], 0) == pdTRUE) n++;
    const uint32_t latest = pulses[n - 1];

    // A borda de descida do pulso do INT induz ruído no SDA/SCL; lê só depois dela
    const uint32_t sinceInt = micros() - latest;
    if (sinceInt < MPU_INT_READ_DELAY_US) delayMicroseconds(MPU_INT_READ_DELAY_US - sinceInt);
    ImuSample s;
    const bool ok = g_mpu.read(s);
    // Um pulso mais novo que o último visto, chegado antes do fim da leitura, torna o quadro ambíguo:
    // ele pode ser desse pulso, e não de latest. O primeiro da fila é o mais antigo depois de latest.
    const uint32_t readEndUs = micros();
    uint32_t nextInt;
    const bool newerPulse = xQueuePeek(g_intQueue, &nextInt, 0) == pdTRUE && (int32_t)(nextInt - readEndUs) <= 0;
    g_baroSvc.service(readEndUs);
    g_magSvc.service(micros());

    // Todos os pulsos contam para a taxa e as perdas; os anteriores ao último ficam sem leitura
    notePulse(pulses[0]);
    for (size_t i = 1; i < n; i++) {
        g_win.stale++;   // o pulso anterior fica sem leitura; conta na janela dele, antes de avançar
        notePulse(pulses[i]);
    }
    g_as.dropped += n - 1;

    checkRecovery();   // read() pode ter reconfigurado o chip (e então devolveu false)
    if (!ok) return;
    accumulate(s);
    // Descarte de partida: a comparação só acontece durante ele (no máximo CAL_MPU_SETTLE_MS), longe da virada
    if (g_settling) {
        if ((int32_t)(s.tUs - g_settleUntilUs) < 0) {
            g_calSvc.holdWindowStart(s.tUs);   // a janela só começa a contar depois do descarte
            return;
        }
        g_settling = false;
    }

    // Validade temporal, comum à calibração e à fusão. Sem FIFO, o quadro lido é o mais recente no
    // instante da leitura. Se um pulso mais novo chegou antes do fim da leitura, o quadro pode ser
    // dele, e o próximo ciclo (que atende esse pulso) leria o mesmo quadro de novo: ele ficaria
    // duplicado na janela de calibração e com o instante errado na fusão. O ciclo fica fora das duas;
    // na fusão, o dt seguinte cobre a lacuna (a referência de tempo não avança). O teto de atraso é
    // só uma rede de segurança (latência da ISR): um atraso de alguns ms, depois de uma impressão, é
    // normal e o quadro é válido.
    const uint32_t lag = elapsedUs(latest, s.tUs);
    if (newerPulse || lag > FUSION_MAX_INT_LAG_US) {
        g_as.ambiguous++;
        return;
    }
    if (lag > g_as.lagMaxUs) g_as.lagMaxUs = lag;
    g_calSvc.sample(s);
    fusionSample(s, latest);
}
