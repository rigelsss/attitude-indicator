// ESP A — avião. *** main.cpp PROVISÓRIO (etapa de bancada, antes do RTOS) ***
// Lê o MPU6050 a cada pulso de "data ready", cuida da calibração (docs/calibracao.md) e roda o
// filtro complementar de roll e pitch (docs/fusao.md). Lê também o BME280 e calcula a altitude
// relativa (docs/barometro.md):
//   boot: carrega da NVS os coeficientes do acelerômetro e a montagem, mede o bias do giroscópio
//         e zera a altitude;
//   serial: c = estado | 6 = seis posições | g = calibrar giroscópio | z = calibrar montagem |
//           a = fluxo de atitude | b = fluxo de altitude | 0 = zerar altitude | r = resumo por segundo.
// A lógica está em lib/imu_cal, lib/attitude e lib/baro (puras, testadas com pio test -e native).
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
#include "attitude.h"
#include "level_store.h"
#include "bme280.h"
#include "baro.h"

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

// ---------- Estado da calibração ----------

static ImuCalData g_cal;                  // o que está na NVS (e em uso para o acelerômetro)
static Vec3 g_gyroBias{0, 0, 0};          // bias em uso
static float g_gyroBiasTempC = 0;
static bool g_gyroDefinitive = false;     // bias aceito neste boot (equivale a ST_GYRO_CAL)
static bool g_tempWarned = false;
static BiasConfirmer g_biasConfirm;       // bias só é aceito com duas janelas consistentes
static float g_candTempC = 0;             // temperatura da janela candidata

enum class CalMode : uint8_t {
    None, GyroBoot, GyroCmd, SixWait, SixMeasure, SixReview, LevelWait, LevelMeasure, LevelReview
};
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

// Calibração de montagem (comando z): duas medidas, a segunda com a caixa girada 180° na mesa
static int g_levelIdx = 0;
static Vec3 g_levelF[2];
static float g_levelTempSum = 0;
static MountResult g_levelResult;

// ---------- Fusão (lib/attitude, docs/fusao.md) ----------

static const FusionParams FUSION_PARAMS = {COMP_FILTER_TAU_S, FUSION_ACC_FULL_DEV_G, FUSION_ACC_ZERO_DEV_G,
                                           FUSION_ROLL_MIN_G, FUSION_MAX_DT_MS / 1000.0f, FUSION_INIT_SAMPLES,
                                           FUSION_INIT_MAX_RATE_DPS};
static AttitudeFilter g_att(FUSION_PARAMS);
static AttitudeFilter g_gyroOnly(FUSION_PARAMS, false);   // diagnóstico: deriva só do giroscópio
static LevelData g_level;                // rotação de montagem em uso (identidade sem calibração)
static bool g_fusPrevValid = false;      // há um pulso anterior aceito pela fusão (referência do dt)
static uint32_t g_fusPrevIntUs = 0;

// Fluxo de atitude (comando a): estatística acumulada entre duas linhas
struct AttStream {
    bool     on = false;
    uint32_t nextUs = 0;
    uint32_t n = 0;            // intervalos integrados
    float    dtMin = 1e9f, dtMax = 0;
    double   dtSum = 0;
    uint32_t lagMaxUs = 0;     // maior atraso pulso → início da leitura
    uint32_t costSumUs = 0;    // custo de update() (só o filtro, não o ciclo inteiro): soma e máximo
    uint32_t costMaxUs = 0;
    uint32_t ambiguous = 0;    // quadros fora da fusão: pulso mais novo durante a leitura (ou atraso excessivo)
    uint32_t dropped = 0;      // pulsos sem leitura (amostra sobrescrita)
    uint32_t resets = 0;       // reinícios por intervalo acima de FUSION_MAX_DT_MS
};
static AttStream g_as;
static const uint32_t ATT_PRINT_US = 1000000UL / ATT_PRINT_HZ;

// ---------- Barômetro (lib/bme280, lib/baro, docs/barometro.md) ----------

static Bme280 g_baro;
static bool g_baroReady = false;          // begin() aceito; depois disso o driver se recupera sozinho
static Bme280Status g_baroLastStatus = Bme280Status::Ok;   // evita repetir a mesma mensagem a cada tentativa
static uint32_t g_baroRetryMs = 0;
static uint32_t g_baroNextUs = 0;         // próxima leitura
static uint32_t g_baroLastRecoveries = 0;
static BaroSample g_baroLast;             // última leitura válida
static bool g_baroHave = false;

// Zero da altitude: espera o filtro IIR do chip assentar e tira a média de uma janela parada
enum class BaroZeroState : uint8_t { Off, Settling, Measuring };
static BaroZeroState g_bzState = BaroZeroState::Off;
static uint32_t g_bzUntilMs = 0;          // fim da espera (Settling)
static uint32_t g_bzStartMs = 0;          // início da janela (Measuring)
static ScalarStats g_bzPress, g_bzTemp;
static bool g_baroRefValid = false;       // há um zero: altitude disponível
static float g_baroRefHpa = 0;
static float g_baroRefTempC = 0;
static bool g_baroTempWarned = false;
static const uint32_t BARO_PERIOD_US = 1000000UL / BARO_READ_HZ;
static const uint32_t BARO_ZERO_MIN_SAMPLES =
    (uint32_t)BARO_ZERO_WINDOW_MS * BARO_READ_HZ / 1000 * BARO_ZERO_MIN_SAMPLES_PCT / 100;

// Fluxo de altitude (comando b)
static bool g_bsOn = false;
static uint32_t g_bsNextUs = 0;
static const uint32_t BARO_PRINT_US = 1000000UL / BARO_PRINT_HZ;

static void baroZeroStart(bool settle) {
    g_bzState = settle ? BaroZeroState::Settling : BaroZeroState::Measuring;
    g_bzUntilMs = millis() + BARO_SETTLE_MS;
    g_bzStartMs = millis();
    g_bzPress.reset();
    g_bzTemp.reset();
    if (settle)
        Serial.printf("[baro] zero da altitude: aguardando %d ms o filtro do chip e depois %d ms com a caixa "
                      "parada...\n", BARO_SETTLE_MS, BARO_ZERO_WINDOW_MS);
    else
        Serial.printf("[baro] zero da altitude: medindo %d ms, mantenha a caixa parada...\n", BARO_ZERO_WINDOW_MS);
}

static void baroZeroSample(const BaroSample &b) {
    if (g_bzState == BaroZeroState::Off) return;
    if (g_bzState == BaroZeroState::Settling) {
        if ((int32_t)(b.tMs - g_bzUntilMs) < 0) return;
        g_bzState = BaroZeroState::Measuring;
        g_bzStartMs = b.tMs;
        g_bzPress.reset();
        g_bzTemp.reset();
    }
    g_bzPress.add(b.pressHpa * 100.0);   // Pa: o limite de desvio está em Pa
    g_bzTemp.add(b.tempC);
    if (b.tMs - g_bzStartMs < BARO_ZERO_WINDOW_MS) return;

    const BaroZeroResult r = evaluateZeroWindow(g_bzPress, g_bzTemp, BARO_ZERO_MIN_SAMPLES, BARO_ZERO_MAX_STD_PA);
    if (r.status == BaroZeroCheck::Ok) {
        g_baroRefValid = true;
        g_baroRefHpa = r.refHpa;
        g_baroRefTempC = r.tempC;
        g_baroTempWarned = false;
        g_bzState = BaroZeroState::Off;
        Serial.printf("[baro] zero aceito: p0 = %.2f hPa | desvio %.2f Pa | %lu leituras | %.1f °C\n", r.refHpa,
                      r.stdPa, (unsigned long)g_bzPress.count(), r.tempC);
        return;
    }
    if (r.status == BaroZeroCheck::Unstable)
        Serial.printf("[baro] zero recusado: desvio %.2f Pa acima de %.1f Pa (caixa mexida ou ar agitado); "
                      "medindo de novo\n", r.stdPa, BARO_ZERO_MAX_STD_PA);
    else
        Serial.printf("[baro] zero recusado: %lu leituras, mínimo %lu; medindo de novo\n",
                      (unsigned long)g_bzPress.count(), (unsigned long)BARO_ZERO_MIN_SAMPLES);
    g_bzStartMs = b.tMs;
    g_bzPress.reset();
    g_bzTemp.reset();
}

static void startBaro() {
    const Bme280Config cfg = {&Wire, ADDR_BME280, BARO_OSRS_T, BARO_OSRS_P, BARO_IIR_COEF, BARO_STANDBY_US,
                              BARO_MAX_FAIL_STREAK};
    const Bme280Status st = g_baro.begin(cfg);
    if (st != Bme280Status::Ok) {
        if (st != g_baroLastStatus)
            Serial.printf("[baro] BME280: %s (chip_id=0x%02X); sem altitude, nova tentativa a cada %d s\n",
                          bme280StatusText(st), g_baro.chipId(), BARO_RETRY_MS / 1000);
        g_baroLastStatus = st;
        g_baroRetryMs = millis() + BARO_RETRY_MS;
        return;
    }
    g_baroLastStatus = st;
    g_baroReady = true;
    g_baroLastRecoveries = g_baro.recoveries();
    g_baroNextUs = micros();
    Serial.printf("[baro] BME280 configurado: pressão x%d, temperatura x%d, IIR %d, modo normal\n", BARO_OSRS_P,
                  BARO_OSRS_T, BARO_IIR_COEF);
    baroZeroStart(true);
}

static void printBaroLine(const BaroSample &b) {
    if (g_baroRefValid)
        Serial.printf("[baro] %6.2f °C | %8.2f hPa | alt %+6.2f m | ΔT %+5.2f °C desde o zero\n", b.tempC, b.pressHpa,
                      baroAltitudeM(b.pressHpa, g_baroRefHpa), b.tempC - g_baroRefTempC);
    else
        Serial.printf("[baro] %6.2f °C | %8.2f hPa | alt: sem zero\n", b.tempC, b.pressHpa);
}

// Uma linha no resumo por segundo (r) e no estado (c)
static void printBaroSummary() {
    if (!g_baroReady) {
        Serial.printf("    baro: BME280 indisponível (%s)\n", bme280StatusText(g_baroLastStatus));
        return;
    }
    Serial.printf("    baro: %s | ", g_baro.ok() ? "ok" : "FALHANDO");
    if (!g_baroHave) Serial.print("sem leitura");
    else if (g_baroRefValid)
        Serial.printf("%.2f hPa | alt %+.2f m | %.1f °C (ΔT %+.1f desde o zero)", g_baroLast.pressHpa,
                      baroAltitudeM(g_baroLast.pressHpa, g_baroRefHpa), g_baroLast.tempC,
                      g_baroLast.tempC - g_baroRefTempC);
    else Serial.printf("%.2f hPa | sem zero | %.1f °C", g_baroLast.pressHpa, g_baroLast.tempC);
    Serial.printf(" | falhas %lu (seguidas %lu, recuperações %lu)\n", (unsigned long)g_baro.failTotal(),
                  (unsigned long)g_baro.failStreak(), (unsigned long)g_baro.recoveries());
}

// Chamada logo depois de cada leitura do MPU: a transação do BME280 (~0,3 ms) fica longe do
// próximo pulso do INT, cujas bordas induzem ruído no SDA/SCL
static void baroService(uint32_t nowUs) {
    if (!g_baroReady) {
        if ((int32_t)(millis() - g_baroRetryMs) >= 0) startBaro();
        return;
    }
    if ((int32_t)(nowUs - g_baroNextUs) < 0) return;

    BaroSample b;
    const bool ok = g_baro.read(b);
    // Com o BME280 falhando, tenta só a cada BARO_FAIL_RETRY_MS: cada transação sem resposta imprime um
    // erro do I2C e atrasa o laço, e o MPU descartava amostras (hardware.md, 12/10/2026)
    const uint32_t period = g_baro.ok() ? BARO_PERIOD_US : BARO_FAIL_RETRY_MS * 1000UL;
    g_baroNextUs += period;
    if ((int32_t)(nowUs - g_baroNextUs) >= 0) g_baroNextUs = nowUs + period;   // atrasou: ressincroniza
    if (g_baro.recoveries() != g_baroLastRecoveries) {
        g_baroLastRecoveries = g_baro.recoveries();
        Serial.printf("[baro] BME280 perdeu a configuração (queda de alimentação?): %s\n",
                      g_baro.configured() ? "reconfigurado; o filtro do chip recomeçou"
                                          : "reconfiguração falhou, nova tentativa a cada leitura");
        if (g_bzState != BaroZeroState::Off) baroZeroStart(true);
    }
    if (!ok) return;
    g_baroLast = b;
    g_baroHave = true;
    baroZeroSample(b);
    if (g_baroRefValid && !g_baroTempWarned && fabsf(b.tempC - g_baroRefTempC) > BARO_TEMP_WARN_C) {
        Serial.printf("[baro] AVISO: chip a %.1f °C, zero feito a %.1f °C: a altitude deriva ~13 cm/°C. "
                      "Zere de novo com 0\n", b.tempC, g_baroRefTempC);
        g_baroTempWarned = true;
    }
    if (g_bsOn && (int32_t)(nowUs - g_bsNextUs) >= 0) {
        g_bsNextUs = nowUs + BARO_PRINT_US;
        printBaroLine(b);
    }
}

static void fusionReset() {
    g_att.reset();
    g_gyroOnly.reset();
    g_fusPrevValid = false;
}

// Após configurar o MPU, calibração e fusão descartam amostras por CAL_MPU_SETTLE_MS
static void beginSettle() {
    g_settling = true;
    g_settleUntilUs = micros() + CAL_MPU_SETTLE_MS * 1000UL;
}

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
    Serial.printf("    ruído: a %.1f mg, g %.3f °/s | pico |g| %.1f °/s | saturadas a %lu g %lu | perdidas %lu | "
                  "descartadas %lu | falhas %lu (seguidas %lu, recuperações %lu)\n",
                  sdA * 1000.0, sdG, g_win.peakGyro, (unsigned long)g_win.satAcc, (unsigned long)g_win.satGyro,
                  (unsigned long)g_win.lost, (unsigned long)g_win.stale, (unsigned long)g_mpu.failTotal(),
                  (unsigned long)g_mpu.failStreak(), (unsigned long)g_mpu.recoveries());
    const AngleStats *att = g_win.att;
    if (att[0].count() == 0) {
        Serial.println("    atitude: não inicializada");
        return;
    }
    Serial.printf("    atitude: roll %+.2f ± %.2f° | pitch %+.2f ± %.2f° (%lu amostras)\n", att[0].mean(),
                  att[0].stddev(), att[1].mean(), att[1].stddev(), (unsigned long)att[0].count());
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
        if (g_summaryOn) {
            printWindow();
            printBaroSummary();
        }
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

// ---------- Calibração de montagem (z) ----------

static void promptLevel() {
    static const char *const STEP[2] = {
        "caixa em pé, na posição normal (fundo apoiado na mesa, sem virar), nariz encostado numa referência "
        "(borda da mesa ou esquadro)",
        "caixa ainda em pé, girada 180° no plano da mesa (sem virar), com a cauda encostada na mesma referência",
    };
    Serial.printf("[mont] posição %d/2: %s. Solte a caixa e aperte Enter (x cancela)\n", g_levelIdx + 1,
                  STEP[g_levelIdx]);
    g_mode = CalMode::LevelWait;
}

static const char *levelRestText(LevelRest r) {
    switch (r) {
    case LevelRest::Ok:            return "ok";
    case LevelRest::TooFewSamples: return "janela incompleta";
    case LevelRest::Moving:        return "caixa em movimento";
    case LevelRest::Rotating:      return "giroscópio corrigido longe de zero (rotação lenta)";
    case LevelRest::AccOutOfRange: return "|a| fora da faixa de repouso";
    }
    return "?";
}

static void finishLevelWindow() {
    const RestLimits lim = {CAL_REST_GYRO_MAX_STD_DPS, CAL_REST_ACC_MIN_G, CAL_REST_ACC_MAX_G};
    const LevelRest r = checkLevelWindow(g_cw.gyro, g_gyroBias, g_cw.accCorr, CAL_MIN_SAMPLES, lim,
                                         LEVEL_REST_GYRO_MAX_MEAN_DPS);
    if (r != LevelRest::Ok) {
        const Vec3 gm = applyGyroBias(g_cw.gyro.mean(), g_gyroBias);
        Serial.printf("[mont] medida recusada: %s (desvio máx. %.3f °/s, giro médio %+.3f %+.3f %+.3f °/s, "
                      "|a| %.3f g): repita\n",
                      levelRestText(r), g_cw.gyro.maxStddev(), gm.x, gm.y, gm.z, norm(g_cw.accCorr.mean()));
        promptLevel();
        return;
    }
    g_levelF[g_levelIdx] = chipToBody(g_cw.accCorr.mean());
    g_levelTempSum += g_cw.tempSum / g_cw.accCorr.count();
    const Angles a = accelRollPitch(g_levelF[g_levelIdx]);
    Serial.printf("[mont] ok: roll %+.2f° pitch %+.2f° (sem correção de montagem)\n", a.rollDeg, a.pitchDeg);
    if (++g_levelIdx < 2) {
        promptLevel();
        return;
    }

    const MountCheck mc = mountFromTwoPoses(g_levelF[0], g_levelF[1], LEVEL_MAX_TILT_DEG, g_levelResult);
    if (mc == MountCheck::BadInput) {
        Serial.println("[mont] medidas incoerentes (a mesma face estava apoiada nas duas?): calibração descartada");
        g_mode = CalMode::None;
        return;
    }
    const MountResult &m = g_levelResult;
    Serial.printf("[mont] desalinhamento do módulo: roll %+.2f° pitch %+.2f° | inclinação da mesa: roll %+.2f° "
                  "pitch %+.2f°\n",
                  m.mountRollDeg, m.mountPitchDeg, m.tableRollDeg, m.tablePitchDeg);
    if (mc == MountCheck::TooTilted) {
        Serial.printf("[mont] acima de %.0f°: calibração descartada. Confira se a caixa estava em pé (sem virar), a mesa e "
                      "a fixação do módulo\n",
                      LEVEL_MAX_TILT_DEG);
        g_mode = CalMode::None;
        return;
    }
    Serial.println("[mont] s = gravar na NVS | x = descartar");
    g_mode = CalMode::LevelReview;
}

static void finishCalWindow() {
    g_cw.active = false;
    switch (g_mode) {
    case CalMode::GyroBoot:
    case CalMode::GyroCmd:      finishGyroWindow(); break;
    case CalMode::SixMeasure:   finishSixWindow(); break;
    case CalMode::LevelMeasure: finishLevelWindow(); break;
    default:                    break;
    }
}

// Alimenta a janela de calibração com uma amostra válida (já fora do descarte de partida)
static void calSample(const ImuSample &s) {
    if (!g_cw.active) return;

    const Vec3 a = {s.ax, s.ay, s.az};
    g_cw.gyro.add({s.gx, s.gy, s.gz});
    g_cw.accRaw.add(a);
    g_cw.accCorr.add(g_cal.accelValid ? applyAccelCal(g_cal.accel, a) : a);
    g_cw.tempSum += s.tempC;
    if ((uint32_t)(s.tUs - g_cw.startUs) >= CAL_WINDOW_MS * 1000UL) finishCalWindow();
}

// Reconfiguração do MPU (recuperação em read() ou em checkAlive()): o transitório de partida é
// descartado, a janela de calibração em andamento recomeça e a fusão perde a referência de tempo
static void checkRecovery() {
    if (g_mpu.recoveries() == g_lastRecoveries) return;
    g_lastRecoveries = g_mpu.recoveries();
    xQueueReset(g_intQueue);   // pulsos enfileirados são de antes da reconfiguração
    beginSettle();
    g_biasConfirm.reset();     // a candidata é de antes da reconfiguração
    if (g_cw.active) startCalWindow();
    fusionReset();
}

// ---------- Fusão ----------

static void printAttitudeLine(const Vec3 &acc, float tempC) {
    const Angles am = accelRollPitch(acc);
    char gyroOnly[32] = "  --      --  ";
    if (g_gyroOnly.initialized()) snprintf(gyroOnly, sizeof(gyroOnly), "%+7.2f %+7.2f", g_gyroOnly.rollDeg(),
                                           g_gyroOnly.pitchDeg());
    if (g_att.initialized())
        Serial.printf("[att] roll %+7.2f pitch %+7.2f | acc %+7.2f %+7.2f | giro %s | |a| %.3f w %.2f | %.2f °C%s\n",
                      g_att.rollDeg(), g_att.pitchDeg(), am.rollDeg, am.pitchDeg, gyroOnly, norm(acc),
                      g_att.lastAccWeight(), tempC, g_att.degraded() ? " | DEGRADADO" : "");
    else
        Serial.printf("[att] aguardando referência | acc %+7.2f %+7.2f | |a| %.3f w %.2f | %.2f °C\n", am.rollDeg,
                      am.pitchDeg, norm(acc), g_att.lastAccWeight(), tempC);
    const AttStream &a = g_as;
    Serial.printf("      dt %.2f/%.2f/%.2f ms (%lu) | atraso máx %lu µs | filtro méd/máx %.0f/%lu µs | ambíguos %lu | "
                  "descartados %lu | reinícios %lu\n",
                  a.n ? a.dtMin * 1e3 : 0.0, a.n ? a.dtSum / a.n * 1e3 : 0.0, a.dtMax * 1e3, (unsigned long)a.n,
                  (unsigned long)a.lagMaxUs, a.n ? (double)a.costSumUs / a.n : 0.0, (unsigned long)a.costMaxUs,
                  (unsigned long)a.ambiguous,
                  (unsigned long)a.dropped, (unsigned long)a.resets);
}

// Uma amostra válida, fora do descarte: calibração nos eixos do chip → eixos do modelo →
// rotação de montagem → filtro. intUs: instante do pulso de "data ready" desta amostra;
// newerPulse: chegou um pulso mais novo antes do fim da leitura.
static void fusionSample(const ImuSample &s, uint32_t intUs, bool newerPulse) {
    // Sem FIFO, o quadro lido é o mais recente no instante da leitura. Se um pulso mais novo chegou
    // antes do fim da leitura, o quadro pode ser dele: o ciclo fica fora da fusão, e o dt seguinte
    // cobre a lacuna (a referência de tempo não avança). O teto de atraso é só uma rede de segurança
    // (latência da ISR): um atraso de alguns ms, depois de uma impressão, é normal e o quadro é válido.
    const uint32_t lag = elapsedUs(intUs, s.tUs);
    if (newerPulse || lag > FUSION_MAX_INT_LAG_US) {
        g_as.ambiguous++;
        return;
    }
    if (lag > g_as.lagMaxUs) g_as.lagMaxUs = lag;

    const Vec3 aRaw = {s.ax, s.ay, s.az};
    const Vec3 aChip = g_cal.accelValid ? applyAccelCal(g_cal.accel, aRaw) : aRaw;
    const Vec3 gChip = applyGyroBias({s.gx, s.gy, s.gz}, g_gyroBias);
    const Vec3 acc = mat3Apply(g_level.r, chipToBody(aChip));
    const Vec3 gyro = mat3Apply(g_level.r, chipToBody(gChip));
    const float dt = g_fusPrevValid ? elapsedUs(g_fusPrevIntUs, intUs) * 1e-6f : 0.0f;
    g_fusPrevIntUs = intUs;
    g_fusPrevValid = true;

    const ImuFlags flags = {s.accelSaturated, s.gyroSaturated};
    const uint32_t t0 = micros();
    const AttitudeFilter::Step st = g_att.update(acc, gyro, dt, flags);
    const uint32_t cost = micros() - t0;
    if (st == AttitudeFilter::Step::Reset) g_as.resets++;
    // Diagnóstico só de giroscópio: parte da atitude do filtro e depois só integra
    if (g_gyroOnly.initialized()) g_gyroOnly.update(acc, gyro, dt, flags);
    else if (g_att.initialized()) g_gyroOnly.initFrom(g_att.rollDeg(), g_att.pitchDeg());

    if (st == AttitudeFilter::Step::Updated) {
        g_as.n++;
        g_as.dtSum += dt;
        g_as.dtMin = fminf(g_as.dtMin, dt);
        g_as.dtMax = fmaxf(g_as.dtMax, dt);
        g_as.costSumUs += cost;
        if (cost > g_as.costMaxUs) g_as.costMaxUs = cost;
    }
    if (g_att.initialized()) {
        g_win.att[0].add(g_att.rollDeg());
        g_win.att[1].add(g_att.pitchDeg());
    }

    if (g_as.on && (int32_t)(intUs - g_as.nextUs) >= 0) {
        printAttitudeLine(acc, s.tempC);
        const bool on = g_as.on;
        g_as = AttStream();
        g_as.on = on;
        g_as.nextUs = intUs + ATT_PRINT_US;
    }
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
    if (g_level.valid)
        Serial.printf("  montagem: calibrada | módulo em roll %+.2f° pitch %+.2f° em relação à caixa (%.1f °C)\n",
                      g_level.mountRollDeg, g_level.mountPitchDeg, g_level.tempC);
    else
        Serial.println("  montagem: NÃO calibrada (eixos nominais): use z");
    if (g_att.initialized())
        Serial.printf("  atitude: roll %+.2f° pitch %+.2f°%s\n", g_att.rollDeg(), g_att.pitchDeg(),
                      g_att.degraded() ? " (DEGRADADA: giroscópio saturou)" : "");
    else
        Serial.println("  atitude: aguardando referência (caixa parada, |a| perto de 1 g)");
    Serial.printf("  giroscópio: %s | bias %+.3f %+.3f %+.3f °/s | calibrado a %.1f °C\n",
                  g_gyroDefinitive ? "DEFINITIVO (deste boot)"
                                   : (g_cal.gyroValid ? "PROVISÓRIO (bias salvo de antes)" : "PROVISÓRIO (sem bias salvo)"),
                  g_gyroBias.x, g_gyroBias.y, g_gyroBias.z, g_gyroBiasTempC);

    if (g_baroRefValid)
        Serial.printf("  altitude: zero p0 = %.2f hPa a %.1f °C%s\n", g_baroRefHpa, g_baroRefTempC,
                      g_bzState != BaroZeroState::Off ? " (novo zero em andamento)" : "");
    else
        Serial.printf("  altitude: %s\n", g_bzState != BaroZeroState::Off ? "zero em andamento" : "sem zero");
    printBaroSummary();

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
    Serial.println("[A] comandos: c = estado | 6 = seis posições | g = calibrar giroscópio | z = calibrar montagem | "
                   "a = fluxo de atitude | b = fluxo de altitude | 0 = zerar altitude | r = resumo por segundo");
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
                    fusionReset();   // a referência do acelerômetro mudou
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
        case CalMode::LevelWait:
            if (ch == '\r' || ch == '\n') {
                Serial.printf("[mont] medindo %d ms, não toque na caixa...\n", CAL_WINDOW_MS);
                g_mode = CalMode::LevelMeasure;
                startCalWindow();
            } else if (ch == 'x') {
                Serial.println("[mont] calibração de montagem cancelada");
                g_mode = CalMode::None;
            }
            continue;
        case CalMode::LevelMeasure:
            if (ch == 'x') {
                g_cw.active = false;
                Serial.println("[mont] calibração de montagem cancelada");
                g_mode = CalMode::None;
            }
            continue;
        case CalMode::LevelReview:
            if (ch == 's') {
                LevelData d;
                d.valid = true;
                d.r = g_levelResult.r;
                d.mountRollDeg = g_levelResult.mountRollDeg;
                d.mountPitchDeg = g_levelResult.mountPitchDeg;
                d.tempC = g_levelTempSum / 2;
                if (levelStoreSave(d)) {
                    g_level = d;
                    fusionReset();   // a rotação de montagem mudou
                    Serial.println("[mont] calibração de montagem gravada na NVS");
                } else {
                    Serial.println("[mont] FALHA ao gravar na NVS; montagem anterior mantida");
                }
                g_mode = CalMode::None;
            } else if (ch == 'x') {
                Serial.println("[mont] resultado descartado; montagem anterior mantida");
                g_mode = CalMode::None;
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
        case 'z':
            if (g_mode != CalMode::None)
                Serial.println("[mont] espere a calibração em andamento terminar");
            else if (!g_cal.accelValid)
                Serial.println("[mont] acelerômetro sem calibração: faça antes as seis posições (6)");
            else if (!g_gyroDefinitive)
                Serial.println("[mont] bias do giroscópio ainda provisório: espere a calibração (ou use g)");
            else {
                g_levelIdx = 0;
                g_levelTempSum = 0;
                Serial.printf("[mont] calibração de montagem: duas medidas de %d ms, a segunda com a caixa girada "
                              "180°\n", CAL_WINDOW_MS);
                promptLevel();
            }
            break;
        case 'a': {
            const bool on = !g_as.on;
            g_as = AttStream();
            g_as.on = on;
            g_as.nextUs = micros();
            g_gyroOnly.reset();   // o diagnóstico só de giroscópio recomeça da atitude atual
            Serial.printf("[att] fluxo de atitude %s (%d Hz): filtro | só acelerômetro | só giroscópio desde o a\n",
                          on ? "ligado" : "desligado", ATT_PRINT_HZ);
            break;
        }
        case 'b':
            g_bsOn = !g_bsOn;
            g_bsNextUs = micros();
            Serial.printf("[baro] fluxo de altitude %s (%d Hz)\n", g_bsOn ? "ligado" : "desligado", BARO_PRINT_HZ);
            break;
        case '0':
            if (!g_baroReady) Serial.println("[baro] BME280 indisponível: sem altitude");
            else if (g_bzState == BaroZeroState::Settling) Serial.println("[baro] zero já em andamento");
            else baroZeroStart(false);
            break;
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
    // Buffer de transmissão em software (antes do begin): sem ele, cada impressão bloqueia o laço
    // até sobrar só a FIFO de 128 bytes da UART (~10 ms por linha longa a 115200) e descarta amostras
    Serial.setTxBufferSize(SERIAL_TX_BUFFER_BYTES);
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
    levelStoreLoad(g_level);
    Serial.printf("[A] NVS: acelerômetro %s | giroscópio %s | montagem %s\n",
                  g_cal.accelValid ? "calibrado" : "sem calibração", g_cal.gyroValid ? "com bias salvo" : "sem bias salvo",
                  g_level.valid ? "calibrada" : "sem calibração");

    startMpu();
    startBaro();
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
    baroService(readEndUs);

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
            if (g_cw.active) g_cw.startUs = s.tUs;   // a janela só começa a contar depois do descarte
            return;
        }
        g_settling = false;
    }
    calSample(s);
    fusionSample(s, latest, newerPulse);
}
