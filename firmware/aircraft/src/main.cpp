// ESP A — avião. *** main.cpp PROVISÓRIO (etapa de bancada, antes do RTOS) ***
// Lê o MPU6050 a cada pulso de "data ready", cuida da calibração (docs/calibracao.md) e roda o
// filtro complementar de roll e pitch (docs/fusao.md). Lê também o BME280 e calcula a altitude
// relativa (docs/barometro.md), e o HMC5883L para a proa compensada (docs/magnetometro.md):
//   boot: carrega da NVS os coeficientes do acelerômetro, a montagem e a calibração do magnetômetro,
//         mede o bias do giroscópio e zera a altitude;
//   serial: c = estado | 6 = seis posições | g = calibrar giroscópio | z = calibrar montagem |
//           m = calibrar magnetômetro | a = fluxo de atitude | b = fluxo de altitude | p = fluxo de proa |
//           0 = zerar altitude | r = resumo por segundo.
// A lógica está em lib/imu_cal, lib/attitude, lib/baro e lib/mag (puras, testadas com pio test -e native).
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
#include "hmc5883l.h"
#include "mag.h"
#include "mag_store.h"
#include "telemetry_flags.h"

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
    uint32_t ambiguous = 0;    // quadros fora da calibração e da fusão: pulso mais novo durante a leitura (ou atraso excessivo)
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

// ---------- Magnetômetro (lib/hmc5883l, lib/mag, lib/mag_store, docs/magnetometro.md) ----------

static Hmc5883l g_mag;
static bool g_magReady = false;           // begin() aceito; depois disso o driver se recupera sozinho
static Hmc5883lStatus g_magLastStatus = Hmc5883lStatus::Ok;
static uint32_t g_magRetryMs = 0;
static uint32_t g_magNextUs = 0;          // próxima leitura
static uint32_t g_magLastRecoveries = 0;
static MagCalData g_magCal;               // calibração em uso (offset zero e W identidade sem calibração)
static bool g_magHave = false;            // há uma leitura válida
static Vec3 g_magRaw{0, 0, 0};            // última leitura, eixos do modelo, sem calibração (µT)
static Vec3 g_magCorr{0, 0, 0};           // idem, calibrada
static bool g_magPrevValid = false;       // há uma leitura anterior (referência do dt da correção)
static uint32_t g_magPrevMs = 0;
static bool g_magHaveTilt = false;        // proa compensada calculada (atitude disponível)
static MagTilt g_magTilt = {0, 0};
static HeadingFilter::Mag g_magStep = HeadingFilter::Mag::Corrected;   // resultado da última correção
static bool g_magDisturbWarned = false;
static uint32_t g_magWinReads = 0;        // leituras válidas na janela do resumo
static uint32_t g_magLastWinReads = 0;
static const uint32_t MAG_PERIOD_US = 1000000UL / MAG_READ_HZ;
static const HeadingParams HEADING_PARAMS = {MAG_HEADING_TAU_S, MAG_MAX_PITCH_DEG, MAG_DISTURB_PCT,
                                             MAG_MAX_DT_MS / 1000.0f};
static HeadingFilter g_hdg(HEADING_PARAMS);

// Calibração (comando m): coleta girando a caixa, ajuste e revisão
enum class MagMode : uint8_t { Off, Collect, Review };
static MagMode g_magMode = MagMode::Off;
static const size_t MAG_CAL_MAX_POINTS = (size_t)MAG_CAL_MS / 1000 * MAG_READ_HZ;
static Vec3 *g_magPts = nullptr;          // alocado só durante a calibração (MAG_CAL_MAX_POINTS × 12 B, ~54 kB)
static size_t g_magN = 0;
static uint32_t g_magCalStartMs = 0;
static uint32_t g_magCalNextProgMs = 0;
static MagMinMax g_magMM;                 // centro provisório para a cobertura durante a coleta
static MagFitResult g_magFit;
static const MagFitLimits MAG_FIT_LIMITS = {MAG_CAL_MIN_RADIUS_UT, MAG_CAL_MAX_RADIUS_UT, MAG_CAL_MAX_RESID_PCT,
                                            MAG_CAL_MIN_SCALE,     MAG_CAL_MAX_SCALE,     MAG_CAL_MIN_SECTORS,
                                            MAG_CAL_MIN_PTS_SECTOR};

// Fluxo de proa (comando p)
static bool g_psOn = false;
static uint32_t g_psNextUs = 0;
static const uint32_t MAG_PRINT_US = 1000000UL / MAG_PRINT_HZ;

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

    const BaroZeroResult r = evaluateZeroWindow(g_bzPress, g_bzTemp, BARO_ZERO_MIN_SAMPLES, BARO_ZERO_MAX_STD_PA,
                                                BARO_REF_MIN_HPA, BARO_REF_MAX_HPA);
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
    else if (r.status == BaroZeroCheck::Invalid)
        Serial.println("[baro] zero recusado: leitura inválida (não finita) na janela; medindo de novo");
    else if (r.status == BaroZeroCheck::OutOfRange)
        Serial.printf("[baro] zero recusado: p0 = %.2f hPa fora de %.0f–%.0f hPa (leitura absurda); medindo de novo\n",
                      r.refHpa, BARO_REF_MIN_HPA, BARO_REF_MAX_HPA);
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
            Serial.printf("[baro] BME280/BMP280: %s (chip_id=0x%02X); sem altitude, nova tentativa a cada %d s\n",
                          bme280StatusText(st), g_baro.chipId(), BARO_RETRY_MS / 1000);
        g_baroLastStatus = st;
        g_baroRetryMs = millis() + BARO_RETRY_MS;
        return;
    }
    g_baroLastStatus = st;
    g_baroReady = true;
    g_baroLastRecoveries = g_baro.recoveries();
    g_baroNextUs = micros();
    Serial.printf("[baro] %s configurado (chip_id=0x%02X): pressão x%d, temperatura x%d, IIR %d, modo normal\n",
                  g_baro.model(), g_baro.chipId(), BARO_OSRS_P, BARO_OSRS_T, BARO_IIR_COEF);
    baroZeroStart(true);
}

static void printBaroLine(const BaroSample &b) {
    const float alt = g_baroRefValid ? baroAltitudeM(b.pressHpa, g_baroRefHpa) : NAN;
    if (isfinite(alt))
        Serial.printf("[baro] %6.2f °C | %8.2f hPa | alt %+6.2f m | ΔT %+5.2f °C desde o zero\n", b.tempC, b.pressHpa,
                      alt, b.tempC - g_baroRefTempC);
    else if (g_baroRefValid)
        Serial.printf("[baro] %6.2f °C | %8.2f hPa | alt: inválida (pressão ou zero fora da faixa)\n", b.tempC,
                      b.pressHpa);
    else
        Serial.printf("[baro] %6.2f °C | %8.2f hPa | alt: sem zero\n", b.tempC, b.pressHpa);
}

// Uma linha no resumo por segundo (r) e no estado (c)
static void printBaroSummary() {
    if (!g_baroReady) {
        Serial.printf("    baro: barômetro indisponível (%s)\n", bme280StatusText(g_baroLastStatus));
        return;
    }
    Serial.printf("    baro: %s | ", g_baro.ok() ? "ok" : "FALHANDO");
    const float alt = g_baroHave && g_baroRefValid ? baroAltitudeM(g_baroLast.pressHpa, g_baroRefHpa) : NAN;
    if (!g_baroHave) Serial.print("sem leitura");
    else if (isfinite(alt))
        Serial.printf("%.2f hPa | alt %+.2f m | %.1f °C (ΔT %+.1f desde o zero)", g_baroLast.pressHpa, alt,
                      g_baroLast.tempC, g_baroLast.tempC - g_baroRefTempC);
    else if (g_baroRefValid)
        Serial.printf("%.2f hPa | alt inválida | %.1f °C", g_baroLast.pressHpa, g_baroLast.tempC);
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
        Serial.printf("[baro] %s perdeu a configuração (queda de alimentação?): %s\n", g_baro.model(),
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
    g_hdg.reset();   // a proa depende da atitude
    g_fusPrevValid = false;
}

// ---------- Magnetômetro ----------

static void startMag() {
    const Hmc5883lConfig cfg = {&Wire, ADDR_HMC5883L, MAG_AVG_SAMPLES, MAG_OUTPUT_HZ, MAG_RANGE_MGA,
                                MAG_MAX_FAIL_STREAK};
    const Hmc5883lStatus st = g_mag.begin(cfg);
    if (st != Hmc5883lStatus::Ok) {
        if (st != g_magLastStatus)
            Serial.printf("[mag] HMC5883L: %s; sem proa, nova tentativa a cada %d s\n", hmc5883lStatusText(st),
                          MAG_RETRY_MS / 1000);
        g_magLastStatus = st;
        g_magRetryMs = millis() + MAG_RETRY_MS;
        return;
    }
    g_magLastStatus = st;
    g_magReady = true;
    g_magLastRecoveries = g_mag.recoveries();
    g_magNextUs = micros();
    g_magPrevValid = false;
    Serial.printf("[mag] HMC5883L configurado: ±%.1f Ga (%.0f LSB/Ga), %.0f Hz, média de %d, modo contínuo\n",
                  MAG_RANGE_MGA / 1000.0f, g_mag.lsbPerGa(), MAG_OUTPUT_HZ, MAG_AVG_SAMPLES);
}

static const char *magStateText() {
    if (g_magMode == MagMode::Collect) return "CALIBRANDO";
    if (!g_magHaveTilt) return "SEM_ATITUDE";
    switch (g_magStep) {
    case HeadingFilter::Mag::Disturbed: return "DISTURBIO";
    case HeadingFilter::Mag::Tilted:    return "INCL_ALTA";
    case HeadingFilter::Mag::AttitudeDegraded: return "ATITUDE_DEGRADADA";
    case HeadingFilter::Mag::BadData:   return "DADO_INVALIDO";
    default:                            return "ok";
    }
}

// Fluxo de proa (p): filtrada | magnetômetro compensada | deitada (sem compensação) | |m| corrigido |
// inclinação magnética medida | roll e pitch | estado
static void printMagLine() {
    char hdg[12] = "  --  ";
    if (g_hdg.initialized()) snprintf(hdg, sizeof(hdg), "%6.1f", g_hdg.headingDeg());
    char mag[12] = "  --  ", dip[12] = "  --  ";
    if (g_magHaveTilt) {
        snprintf(mag, sizeof(mag), "%6.1f", g_magTilt.headingDeg);
        snprintf(dip, sizeof(dip), "%+6.1f", g_magTilt.dipDeg);
    }
    Serial.printf("[mag] proa %s | mag %s | deitada %6.1f | |m| %5.1f µT | incl %s° | roll %+6.1f pitch %+6.1f | %s%s\n",
                  hdg, mag, magFlatHeadingDeg(g_magCorr), norm(g_magCorr), dip,
                  g_att.initialized() ? g_att.rollDeg() : 0.0f, g_att.initialized() ? g_att.pitchDeg() : 0.0f,
                  magStateText(), g_magCal.valid ? "" : " SEM_CAL");
}

// Uma linha no resumo por segundo (r) e no estado (c)
static void printMagSummary() {
    if (!g_magReady) {
        Serial.printf("    mag: HMC5883L indisponível (%s)\n", hmc5883lStatusText(g_magLastStatus));
        return;
    }
    Serial.printf("    mag: %s | %lu leituras no último 1 s | ", g_mag.ok() ? "ok" : "FALHANDO",
                  (unsigned long)g_magLastWinReads);
    if (!g_magHave) Serial.print("sem leitura");
    else {
        Serial.printf("|m| %.1f µT", norm(g_magCorr));
        if (g_hdg.initialized()) Serial.printf(" | proa %.1f°", g_hdg.headingDeg());
        Serial.printf(" | %s%s", magStateText(), g_magCal.valid ? "" : " SEM_CAL");
    }
    Serial.printf(" | falhas %lu (seguidas %lu, recuperações %lu) | saturadas %lu\n", (unsigned long)g_mag.failTotal(),
                  (unsigned long)g_mag.failStreak(), (unsigned long)g_mag.recoveries(),
                  (unsigned long)g_mag.saturations());
}

static void magCalFree() {
    free(g_magPts);
    g_magPts = nullptr;
    g_magN = 0;
    g_magMode = MagMode::Off;
    g_magPrevValid = false;   // a correção recomeça sem passo acumulado
}

static void magCalStart() {
    if (g_mode != CalMode::None) {
        Serial.println("[mag] espere a calibração em andamento terminar");
        return;
    }
    if (!g_magReady || !g_mag.ok()) {
        Serial.println("[mag] HMC5883L indisponível ou falhando: calibração não iniciada");
        return;
    }
    g_magPts = (Vec3 *)malloc(MAG_CAL_MAX_POINTS * sizeof(Vec3));
    if (g_magPts == nullptr) {
        Serial.println("[mag] ERRO: sem memória para os pontos da calibração");
        return;
    }
    g_magN = 0;
    g_magMM.reset();
    g_magCalStartMs = millis();
    g_magCalNextProgMs = g_magCalStartMs + MAG_CAL_PROGRESS_MS;
    g_magMode = MagMode::Collect;
    g_hdg.reset();
    Serial.printf("[mag] calibração: gire a caixa devagar em TODAS as orientações por até %d s (x cancela).\n",
                  MAG_CAL_MS / 1000);
    Serial.println("[mag] para cada face voltada para cima (topo, fundo, asas, nariz, cauda), um giro em torno da "
                   "vertical. Termina sozinha com os 24 setores cobertos");
}

static void magCalFinish() {
    const MagFitStatus st = magFitEllipsoid(g_magPts, g_magN, MAG_FIT_LIMITS, g_magFit);
    const MagFitResult &r = g_magFit;
    const MagCal &c = r.cal;
    Serial.printf("[mag] coleta encerrada: %lu pontos em %.0f s\n", (unsigned long)g_magN,
                  (millis() - g_magCalStartMs) / 1000.0f);
    if (st != MagFitStatus::TooFewPoints && st != MagFitStatus::Singular && st != MagFitStatus::NotEllipsoid) {
        Serial.printf("  offset %+.2f %+.2f %+.2f µT | semieixos %.3f %.3f %.3f\n", c.offset.x, c.offset.y,
                      c.offset.z, r.scale.x, r.scale.y, r.scale.z);
        for (int i = 0; i < 3; i++)
            Serial.printf("  W %s %+.4f %+.4f %+.4f\n", i == 0 ? "=" : " ", c.w.m[i][0], c.w.m[i][1], c.w.m[i][2]);
        Serial.printf("  raio %.2f µT (WMM na bancada: %.1f µT) | resíduo %.2f %% | cobertura %d/%d setores\n",
                      c.radiusUt, MAG_REF_FIELD_UT, r.residPct, r.sectors, MAG_SECTORS);
    }
    g_magMode = MagMode::Review;
    if (st == MagFitStatus::Ok) {
        Serial.println("[mag] calibração ok. s = gravar na NVS | x = descartar | d = pontos em CSV");
    } else {
        Serial.printf("[mag] calibração RECUSADA: %s (limites: raio %.0f–%.0f µT, resíduo ≤ %.1f %%, semieixos "
                      "%.2f–%.2f, ≥ %d setores). x = sair | d = pontos em CSV\n",
                      magFitStatusText(st), MAG_CAL_MIN_RADIUS_UT, MAG_CAL_MAX_RADIUS_UT, MAG_CAL_MAX_RESID_PCT,
                      MAG_CAL_MIN_SCALE, MAG_CAL_MAX_SCALE, MAG_CAL_MIN_SECTORS);
    }
}

// Ponto da coleta (eixos do modelo, sem calibração), com o progresso da cobertura a cada MAG_CAL_PROGRESS_MS
static void magCalSample(const Vec3 &raw, uint32_t tMs) {
    if (g_magN < MAG_CAL_MAX_POINTS) {
        g_magPts[g_magN++] = raw;
        g_magMM.add(raw);
    }
    const bool timeUp = tMs - g_magCalStartMs >= MAG_CAL_MS || g_magN >= MAG_CAL_MAX_POINTS;
    if ((int32_t)(tMs - g_magCalNextProgMs) < 0 && !timeUp) return;
    g_magCalNextProgMs += MAG_CAL_PROGRESS_MS;
    uint16_t counts[MAG_SECTORS];
    magCountSectors(g_magPts, g_magN, g_magMM.center(), counts);
    const int covered = magSectorsCovered(counts, MAG_CAL_MIN_PTS_SECTOR);
    uint8_t miss[MAG_FACES];
    magMissingByFace(counts, MAG_CAL_MIN_PTS_SECTOR, miss);
    Serial.printf("[mag] %2lu s | %4lu pontos | cobertura %2d/%d", (unsigned long)((tMs - g_magCalStartMs) / 1000),
                  (unsigned long)g_magN, covered, MAG_SECTORS);
    if (covered < MAG_SECTORS) {
        Serial.print(" | faltam:");
        for (int f = 0; f < MAG_FACES; f++)
            if (miss[f]) Serial.printf(" %s(%d)", magFaceName(f), miss[f]);
    }
    Serial.println();
    if (covered == MAG_SECTORS || timeUp) magCalFinish();
}

// Pontos da última coleta em CSV, para tools/mag_fit.py. Bloqueia o laço por alguns segundos
// (~25 bytes por ponto a 115200): a atitude e a proa recomeçam depois.
static void magCalDump() {
    const MagCal &c = g_magFit.cal;
    Serial.printf("# mag_cal inicio: %lu pontos (µT, eixos do modelo, sem calibração)\n", (unsigned long)g_magN);
    Serial.printf("# firmware status=%s b=%.4f,%.4f,%.4f w=%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f R=%.4f "
                  "resid=%.3f setores=%d\n",
                  magFitStatusText(g_magFit.status), c.offset.x, c.offset.y, c.offset.z, c.w.m[0][0], c.w.m[0][1],
                  c.w.m[0][2], c.w.m[1][0], c.w.m[1][1], c.w.m[1][2], c.w.m[2][0], c.w.m[2][1], c.w.m[2][2],
                  c.radiusUt, g_magFit.residPct, g_magFit.sectors);
    Serial.println("x,y,z");
    for (size_t i = 0; i < g_magN; i++)
        Serial.printf("%.3f,%.3f,%.3f\n", g_magPts[i].x, g_magPts[i].y, g_magPts[i].z);
    Serial.println("# mag_cal fim");
    Serial.flush();
    fusionReset();   // o laço ficou parado: a referência de tempo da fusão não vale mais
}

// Chamada logo depois de cada leitura do MPU, como o barômetro: a rajada de 9 bytes (~0,25 ms a 400 kHz)
// fica longe do próximo pulso do INT
static void magService(uint32_t nowUs) {
    if (!g_magReady) {
        if ((int32_t)(millis() - g_magRetryMs) >= 0) startMag();
        return;
    }
    if ((int32_t)(nowUs - g_magNextUs) < 0) return;

    MagSample ms;
    const bool ok = g_mag.read(ms);
    // Com o HMC5883L falhando, tenta só a cada MAG_FAIL_RETRY_MS (cada transação sem resposta atrasa o laço)
    const uint32_t period = g_mag.ok() ? MAG_PERIOD_US : MAG_FAIL_RETRY_MS * 1000UL;
    g_magNextUs += period;
    if ((int32_t)(nowUs - g_magNextUs) >= 0) g_magNextUs = nowUs + period;   // atrasou: ressincroniza
    if (g_mag.recoveries() != g_magLastRecoveries) {
        g_magLastRecoveries = g_mag.recoveries();
        Serial.printf("[mag] HMC5883L perdeu a configuração (queda de alimentação?): %s\n",
                      g_mag.configured() ? "reconfigurado" : "reconfiguração falhou, nova tentativa a cada leitura");
    }
    if (!ok) return;
    g_magWinReads++;
    const Vec3 raw = magChipToBody(ms.chipUt);
    g_magRaw = raw;
    g_magHave = true;
    const float dt = g_magPrevValid ? (ms.tMs - g_magPrevMs) / 1000.0f : 0.0f;
    g_magPrevMs = ms.tMs;
    g_magPrevValid = true;

    if (g_magMode == MagMode::Collect) {
        magCalSample(raw, ms.tMs);
        return;
    }
    if (g_magMode == MagMode::Review) return;   // proa suspensa até gravar ou descartar

    g_magCorr = magApplyCal(g_magCal.cal, raw);
    if (g_att.initialized()) {
        g_magTilt = magTiltCompensate(g_magCorr, g_att.rollDeg(), g_att.pitchDeg());
        g_magHaveTilt = true;
        g_magStep = g_hdg.correct(g_magTilt.headingDeg, norm(g_magCorr), g_magCal.cal.radiusUt, g_att.pitchDeg(), dt,
                                  g_att.degraded());
    } else {
        g_magHaveTilt = false;
    }
    // Aviso de distúrbio pelo próprio |m|, e não pelo resultado de correct(): com outra suspensão ativa
    // (atitude degradada, inclinação alta), correct() devolve outro estado mesmo com o campo fora do limite
    const bool disturbed = magDisturbed(norm(g_magCorr), g_magCal.cal.radiusUt, MAG_DISTURB_PCT);
    if (disturbed != g_magDisturbWarned) {
        g_magDisturbWarned = disturbed;
        if (disturbed)
            Serial.printf("[mag] DISTÚRBIO: |m| %.1f µT, raio da calibração %.1f µT (limite %.0f %%): correção "
                          "pelo magnetômetro suspensa\n", norm(g_magCorr), g_magCal.cal.radiusUt, MAG_DISTURB_PCT);
        else
            Serial.printf("[mag] distúrbio terminou: |m| %.1f µT de volta ao raio da calibração\n", norm(g_magCorr));
    }
    if (g_psOn && (int32_t)(nowUs - g_psNextUs) >= 0) {
        g_psNextUs += MAG_PRINT_US;
        if ((int32_t)(nowUs - g_psNextUs) >= 0) g_psNextUs = nowUs + MAG_PRINT_US;
        printMagLine();
    }
}

// Teclas durante a calibração do magnetômetro; true se a tecla foi consumida
static bool handleMagCalKey(char ch) {
    if (g_magMode == MagMode::Collect) {
        if (ch == 'x') {
            Serial.println("[mag] calibração cancelada; calibração anterior mantida");
            magCalFree();
        }
        return true;   // outras teclas são ignoradas durante a coleta
    }
    if (g_magMode != MagMode::Review) return false;
    if (ch == 'd') {
        magCalDump();
    } else if (ch == 'x') {
        Serial.println("[mag] resultado descartado; calibração anterior mantida");
        magCalFree();
    } else if (ch == 's') {
        if (g_magFit.status != MagFitStatus::Ok) {
            Serial.println("[mag] calibração recusada não pode ser gravada (x = sair | d = pontos em CSV)");
        } else if (magStoreSave(g_magFit.cal, g_magFit.residPct, (uint8_t)g_magFit.sectors, g_mag.crb())) {
            g_magCal.valid = true;
            g_magCal.cal = g_magFit.cal;
            g_magCal.residPct = g_magFit.residPct;
            g_magCal.sectors = (uint8_t)g_magFit.sectors;
            g_hdg.reset();
            Serial.println("[mag] calibração do magnetômetro gravada na NVS");
            magCalFree();
        } else {
            Serial.println("[mag] FALHA ao gravar na NVS; calibração anterior mantida");
            magCalFree();
        }
    }
    return true;
}

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
        Serial.printf("[A] MPU6050: %s (WHO_AM_I=0x%02X); nova tentativa em %d s\n", mpu6050StatusText(st),
                      g_mpu.whoAmI(), MPU_RETRY_MS / 1000);
        g_mpuRetryMs = millis() + MPU_RETRY_MS;
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
                  "descartadas %lu | falhas %lu (seguidas %lu, recuperações %lu) | fila cheia %lu\n",
                  sdA * 1000.0, sdG, g_win.peakGyro, (unsigned long)g_win.satAcc, (unsigned long)g_win.satGyro,
                  (unsigned long)g_win.lost, (unsigned long)g_win.stale, (unsigned long)g_mpu.failTotal(),
                  (unsigned long)g_mpu.failStreak(), (unsigned long)g_mpu.recoveries(),
                  (unsigned long)g_intOverflow);
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
        g_magLastWinReads = g_magWinReads;
        g_magWinReads = 0;
        if (g_summaryOn) {
            printWindow();
            printBaroSummary();
            printMagSummary();
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
    case RestCheck::Invalid:       return "leitura inválida (não finita) na janela";
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
    // Sem calibração do acelerômetro, |a| ≈ 1,10 g em repouso: vale uma faixa larga, que ainda recusa
    // um vetor nulo (sem direção da gravidade para comparar entre as janelas)
    const RestLimits lim = g_cal.accelValid
                               ? RestLimits{CAL_REST_GYRO_MAX_STD_DPS, CAL_REST_ACC_MIN_G, CAL_REST_ACC_MAX_G}
                               : RestLimits{CAL_REST_GYRO_MAX_STD_DPS, CAL_REST_ACC_RAW_MIN_G, CAL_REST_ACC_RAW_MAX_G};
    const GyroBiasResult r = evaluateGyroWindow(g_cw.gyro, g_cw.accCorr, CAL_MIN_SAMPLES, lim);
    const float tempC = g_cw.tempSum / fmaxf(1, g_cw.gyro.count());
    const BiasConfirmer::Result res = g_biasConfirm.feed(r, CAL_BIAS_MAX_DELTA_DPS, CAL_BIAS_MAX_TILT_DEG);

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
                      "janelas %.3f °/s e %.3f° na gravidade, desvio máx. %.3f °/s)%s\n",
                      b.x, b.y, b.z, t, g_biasConfirm.lastDelta(), g_biasConfirm.lastTilt(), r.gyroMaxStd,
                      saved ? "" : " | FALHA ao gravar na NVS");
        g_mode = CalMode::None;
        return;
    }
    if (res == BiasConfirmer::Result::Candidate || res == BiasConfirmer::Result::Inconsistent) {
        if (res == BiasConfirmer::Result::Inconsistent)
            Serial.printf("[cal] giroscópio: entre janelas, bias mudou %.3f °/s (limite %.3f) e gravidade %.3f° "
                          "(limite %.2f): caixa se acomodando ou girando; confirmando de novo\n",
                          g_biasConfirm.lastDelta(), CAL_BIAS_MAX_DELTA_DPS, g_biasConfirm.lastTilt(),
                          CAL_BIAS_MAX_TILT_DEG);
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
    case LevelRest::Invalid:       return "leitura inválida (não finita) na janela";
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
// rotação de montagem → filtro. intUs: instante do pulso de "data ready" desta amostra, que já
// passou pela validade temporal no loop().
static void fusionSample(const ImuSample &s, uint32_t intUs) {
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
    // Proa: predição pelo giroscópio a cada amostra; sem atitude (ou na coleta da calibração), recomeça
    if (g_att.initialized() && g_magMode == MagMode::Off) g_hdg.predict(gyro, g_att.rollDeg(), g_att.pitchDeg(), dt);
    else g_hdg.reset();

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

// ---------- Validade dos dados (campo valid do TelemetryPacket, lib/telemetry_flags) ----------

static const ValidityLimits VALIDITY_LIMITS = {FUSION_MAX_DT_MS, BARO_STALE_MS, MAG_STALE_MS};

static uint32_t ageMs(bool have, uint32_t tMs) {
    return have ? millis() - tMs : AGE_NEVER;
}

static ValidityInputs validityInputs() {
    ValidityInputs in;
    in.attInitialized = g_att.initialized();
    in.attDegraded = g_att.degraded();
    in.attAgeMs = g_fusPrevValid ? (micros() - g_fusPrevIntUs) / 1000UL : AGE_NEVER;
    in.baroOk = g_baroReady && g_baro.ok();
    in.baroZeroValid = g_baroRefValid;
    in.altFinite = g_baroHave && g_baroRefValid && isfinite(baroAltitudeM(g_baroLast.pressHpa, g_baroRefHpa));
    in.baroAgeMs = ageMs(g_baroHave, g_baroLast.tMs);
    in.hdgInitialized = g_hdg.initialized();
    in.hdgMagSuspended = g_magStep == HeadingFilter::Mag::Tilted || g_magStep == HeadingFilter::Mag::Disturbed ||
                         g_magStep == HeadingFilter::Mag::AttitudeDegraded || g_magStep == HeadingFilter::Mag::BadData;
    in.magAgeMs = ageMs(g_magPrevValid, g_magPrevMs);
    return in;
}

// Uma linha no estado (c): cada flag com o motivo quando está desligada. in é capturado no início do c:
// as linhas anteriores levam ~100 ms na serial, e as idades medidas depois delas refletiriam a impressão
static void printValidity(const ValidityInputs &in) {
    const uint16_t v = telemetryValidFlags(in, VALIDITY_LIMITS);
    Serial.print("  validade (telemetria): ATT ");
    if (v & VAL_ATT) Serial.print("ok");
    else if (!in.attInitialized) Serial.print("não (filtro não inicializado)");
    else Serial.printf("não (MPU sem amostra há %lu ms)", (unsigned long)in.attAgeMs);
    Serial.printf(" | ATT_DEGRADED %s | ALT ", (v & VAL_ATT_DEGRADED) ? "sim" : "não");
    if (v & VAL_ALT) Serial.print("ok");
    else if (!in.baroOk) Serial.print("não (sem barômetro)");
    else if (!in.baroZeroValid) Serial.print("não (sem zero)");
    else if (!in.altFinite) Serial.print("não (valor inválido)");
    else Serial.printf("não (leitura de %lu ms atrás)", (unsigned long)in.baroAgeMs);
    Serial.print(" | HDG ");
    if (v & VAL_HDG) Serial.print("ok");
    else if (!(v & VAL_ATT)) Serial.print("não (sem atitude)");
    else Serial.print("não (filtro da proa não inicializado)");
    Serial.print(" | HDG_GYRO_ONLY ");
    if (!(v & VAL_HDG_GYRO_ONLY)) Serial.print("não");
    else if (in.attDegraded) Serial.print("sim (ATITUDE_DEGRADADA)");
    else if (in.magAgeMs > VALIDITY_LIMITS.magMaxAgeMs) Serial.print("sim (magnetômetro sem leitura recente)");
    else Serial.printf("sim (%s)", magStateText());
    Serial.printf(" | valid = 0x%04X\n", v);
}

static void printStatus() {
    const ValidityInputs validity = validityInputs();   // antes de imprimir (ver printValidity)
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

    if (g_magCal.valid) {
        const MagCal &mc = g_magCal.cal;
        Serial.printf("  magnetômetro: calibrado | offset %+.1f %+.1f %+.1f µT | raio %.1f µT | resíduo %.1f %% | "
                      "%d setores\n",
                      mc.offset.x, mc.offset.y, mc.offset.z, mc.radiusUt, g_magCal.residPct, g_magCal.sectors);
        Serial.printf("  magnetômetro: W = [%+.3f %+.3f %+.3f; %+.3f %+.3f %+.3f; %+.3f %+.3f %+.3f]\n", mc.w.m[0][0],
                      mc.w.m[0][1], mc.w.m[0][2], mc.w.m[1][0], mc.w.m[1][1], mc.w.m[1][2], mc.w.m[2][0],
                      mc.w.m[2][1], mc.w.m[2][2]);
    } else {
        Serial.println("  magnetômetro: NÃO calibrado (offset zero, escala 1, sem detecção de distúrbio): use m");
    }
    Serial.printf("  magnetômetro: ±%.1f Ga, %.0f Hz, média de %d, leitura a %d Hz\n", MAG_RANGE_MGA / 1000.0f,
                  MAG_OUTPUT_HZ, MAG_AVG_SAMPLES, MAG_READ_HZ);
    printMagSummary();
    printValidity(validity);

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
    Serial.printf("  pulsos do INT: no último 1 s, perdidos %lu e descartados %lu | fila cheia %lu desde o boot\n",
                  (unsigned long)g_lastWin.lost, (unsigned long)g_lastWin.stale, (unsigned long)g_intOverflow);
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
                   "m = calibrar magnetômetro | a = fluxo de atitude | b = fluxo de altitude | p = fluxo de proa | "
                   "0 = zerar altitude | r = resumo por segundo");
}

static void handleSerial() {
    while (Serial.available()) {
        const char ch = (char)Serial.read();
        if (handleMagCalKey(ch)) continue;
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
                    // A montagem foi medida com o acelerômetro corrigido pela calibração anterior. O registro
                    // fica na NVS, mas não bate mais com a calibração e é ignorado também nos próximos boots.
                    if (g_level.valid) {
                        g_level = LevelData();
                        Serial.println("[mont] montagem descartada: foi medida com a calibração anterior do "
                                       "acelerômetro. Refaça com z");
                    }
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
                d.accelCalValid = g_cal.accelValid;   // as medidas usaram o acelerômetro com esta calibração
                d.accelCal = g_cal.accel;
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
        case 'm': magCalStart(); break;
        case 'p':
            g_psOn = !g_psOn;
            g_psNextUs = micros();
            Serial.printf("[mag] fluxo de proa %s (%d Hz): filtrada | magnetômetro compensada | deitada | |m| | "
                          "inclinação | roll pitch | estado\n", g_psOn ? "ligado" : "desligado", MAG_PRINT_HZ);
            break;
        case '0':
            if (!g_baroReady) Serial.println("[baro] barômetro indisponível: sem altitude");
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
    if (g_intQueue == nullptr) {   // a ISR do INT usa a fila: sem ela não há como seguir
        Serial.println("[A] ERRO: sem memória para a fila do INT; reiniciando em 1 s");
        Serial.flush();
        delay(1000);
        ESP.restart();
    }

    imuStoreLoad(g_cal, MPU_ACCEL_RANGE_G, CAL_STORE_ACC_OFFSET_MAX_G, CAL_STORE_GYRO_BIAS_MAX_DPS);
    if (g_cal.accelRejected)
        Serial.println("[A] NVS: calibração do acelerômetro com valores impossíveis, ignorada. Refaça com 6");
    if (g_cal.gyroRejected) Serial.println("[A] NVS: bias do giroscópio com valores impossíveis, ignorado");
    if (g_cal.gyroValid) {
        g_gyroBias = g_cal.gyroBias;   // provisório até a janela deste boot ser aceita
        g_gyroBiasTempC = g_cal.gyroTempC;
    }
    levelStoreLoad(g_level, LEVEL_MAX_TILT_DEG);
    if (g_level.rejected) Serial.println("[A] NVS: calibração de montagem com valores impossíveis, ignorada. Refaça com z");
    if (g_level.valid && !sameAccelCal(g_level.accelCalValid, g_level.accelCal, g_cal.accelValid, g_cal.accel)) {
        g_level = LevelData();
        Serial.println("[A] NVS: montagem medida com outra calibração do acelerômetro, ignorada. Refaça com z");
    }
    uint8_t magCrb;
    float magLsb;
    if (hmcCrbCode(MAG_RANGE_MGA, magCrb, magLsb)) magStoreLoad(g_magCal, magCrb);
    if (g_magCal.rejected) Serial.println("[A] NVS: calibração do magnetômetro com valores impossíveis, ignorada");
    if (g_magCal.otherAxisMap)
        Serial.println("[A] NVS: calibração do magnetômetro feita com outro mapeamento de eixos, ignorada. Refaça com m");
    if (g_magCal.otherGain)
        Serial.println("[A] NVS: calibração do magnetômetro feita com outro ganho, ignorada. Refaça com m");
    Serial.printf("[A] NVS: acelerômetro %s | giroscópio %s | montagem %s | magnetômetro %s\n",
                  g_cal.accelValid ? "calibrado" : "sem calibração", g_cal.gyroValid ? "com bias salvo" : "sem bias salvo",
                  g_level.valid ? "calibrada" : "sem calibração",
                  g_magCal.valid ? "calibrado" : "sem calibração (proa com offset zero; use m)");

    startMpu();
    startBaro();
    startMag();
    printHelp();
    startGyroCal(CalMode::GyroBoot);
}

// Espera o próximo pulso do INT por até MPU_INT_TIMEOUT_MS, em fatias de MPU_INT_WAIT_SLICE_MS.
// Com o MPU funcionando, o pulso chega (a cada 5 ms) dentro da primeira fatia, e o barômetro segue
// atendido logo depois da leitura do MPU. Se os pulsos pararem, ele é atendido entre as fatias.
static bool waitPulse(uint32_t &tInt) {
    const uint32_t t0 = millis();
    for (;;) {
        if (xQueueReceive(g_intQueue, &tInt, pdMS_TO_TICKS(MPU_INT_WAIT_SLICE_MS)) == pdTRUE) return true;
        if (millis() - t0 >= MPU_INT_TIMEOUT_MS) return false;
        baroService(micros());
        magService(micros());
    }
}

void loop() {
    handleSerial();
    if (!g_mpuReady) {
        // Sem MPU, o barômetro continua sendo atendido; a configuração é tentada a cada MPU_RETRY_MS
        if ((int32_t)(millis() - g_mpuRetryMs) >= 0) startMpu();
        baroService(micros());
        magService(micros());
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
    baroService(readEndUs);
    magService(micros());

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
    calSample(s);
    fusionSample(s, latest);
}
