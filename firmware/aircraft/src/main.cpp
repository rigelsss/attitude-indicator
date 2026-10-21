// ESP A — avião. *** main.cpp PROVISÓRIO (etapa de bancada, antes do RTOS) ***
// Lê o MPU6050 a cada pulso de "data ready", cuida da calibração (docs/calibracao.md) e roda o
// filtro complementar de roll e pitch (docs/fusao.md). Lê também o BME280 e calcula a altitude
// relativa (docs/barometro.md), e o HMC5883L para a proa compensada (docs/magnetometro.md):
//   boot: carrega da NVS os coeficientes do acelerômetro, a montagem e a calibração do magnetômetro,
//         mede o bias do giroscópio e zera a altitude;
//   serial: c = estado | 6 = seis posições | g = calibrar giroscópio | z = calibrar montagem |
//           m = calibrar magnetômetro | a = fluxo de atitude | b = fluxo de altitude | p = fluxo de proa |
//           0 = zerar altitude | r = resumo por segundo.
// A lógica está em lib/imu_cal, lib/cal_flow, lib/attitude, lib/fusion, lib/acq, lib/baro, lib/mag e lib/sched
// (puras, testadas com pio test -e native).
// As calibrações do MPU (giroscópio, seis posições e montagem) e a carga da NVS ficam em lib/cal_service;
// o barômetro e o magnetômetro são atendidos por lib/baro_service e lib/mag_service. As teclas, o estado (c),
// o resumo (r), o fluxo de atitude (a) e a ajuda ficam em src/console.cpp. Este arquivo só inicializa e roda o
// laço de aquisição. A saída de texto passa por lib/logger. Refatoração: docs/refatoracao.md.
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
#include <driver/gpio.h>
#include <esp_timer.h>
#include "config.h"
#include "telemetry.h"
#include "mpu6050.h"
#include "imu_cal.h"
#include "attitude.h"
#include "fusion.h"
#include "acq.h"
#include "cal_service.h"
#include "baro_service.h"
#include "mag_service.h"
#include "telemetry_flags.h"
#include "logger.h"
#include "console.h"

extern const char *const BUILD_STAMP;   // build_info.cpp: data e hora da compilação

static Mpu6050 g_mpu;
static QueueHandle_t g_intQueue = nullptr;   // instantes (micros) dos pulsos de "data ready"
// Pulsos que não couberam na fila (cheia: o laço ficou parado mais de MPU_INT_QUEUE_LEN períodos).
// Só a ISR escreve; o laço só lê. Total desde o boot. O pulso perdido também aparece em "perdidas"
// (intervalo entre pulsos), e a leitura seguinte sai pela checagem de atraso, sem dt errado na fusão.
static volatile uint32_t g_intOverflow = 0;
static bool g_mpuReady = false;
static uint32_t g_mpuRetryMs = 0;            // MPU ausente: próxima tentativa de configuração

// ---------- Aquisição (lib/acq): janelas de 1 s do resumo (r) e do estado (c) ----------

static PulseTracker g_acq(MAIN_PRINT_MS * 1000UL);
// Após configurar o MPU, calibração e fusão descartam amostras por CAL_MPU_SETTLE_MS
static SettleGate g_settle;
static uint32_t g_lastRecoveries = 0;

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

// ---------- Fusão (lib/fusion, lib/attitude, docs/fusao.md) ----------

static const FusionParams FUSION_PARAMS = {COMP_FILTER_TAU_S, FUSION_ACC_FULL_DEV_G, FUSION_ACC_ZERO_DEV_G,
                                           FUSION_ROLL_MIN_G, FUSION_MAX_DT_MS / 1000.0f, FUSION_INIT_SAMPLES,
                                           FUSION_INIT_MAX_RATE_DPS};
static const HeadingParams HEADING_PARAMS = {MAG_HEADING_TAU_S, MAG_MAX_PITCH_DEG, MAG_DISTURB_PCT,
                                             MAG_MAX_DT_MS / 1000.0f};
// Atitude, diagnóstico só de giroscópio e proa (predição aqui, correção pelo serviço do magnetômetro)
static FusionCore g_fusion(FUSION_PARAMS, HEADING_PARAMS);
static const AttitudeFilter &g_att = g_fusion.att();

// Estatística da aquisição entre duas linhas do fluxo a (lida e zerada pelo console)
static AcqStats g_acqStats;

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
    MAG_KEY_HINT_MS,
};
static MagService g_magSvc(MAG_CFG, g_fusion.att(), g_fusion.heading());

static void fusionReset() { g_fusion.reset(); }

// ---------- Console (src/console.cpp): teclas, c, r, a e ajuda ----------

static ValidityInputs validityInputs();
static const ConsoleDeps CONSOLE_DEPS = {
    g_mpu, g_intOverflow, g_acq, g_acqStats, g_fusion, g_calSvc, g_baroSvc, g_magSvc, validityInputs, fusionReset,
};
static Console g_console(CONSOLE_DEPS);

// Relógio do custo do filtro no fluxo a (o micros() devolve unsigned long, não uint32_t)
static uint32_t clockUs() { return micros(); }

static void beginSettle() { g_settle.begin(micros(), CAL_MPU_SETTLE_MS * 1000UL); }

// ISR do INT, registrada pelo ESP-IDF (attachIntPin), sem o despachante do Arduino, que roda da flash.
// O carimbo vem do esp_timer_get_time(), que fica na IRAM: o micros() do núcleo está na flash (sem
// CONFIG_ARDUINO_ISR_IRAM) e devolve o mesmo relógio, truncado em 32 bits como aqui.
static void IRAM_ATTR onMpuInt(void *) {
    uint32_t t = (uint32_t)esp_timer_get_time();
    BaseType_t woke = pdFALSE;
    if (xQueueSendFromISR(g_intQueue, &t, &woke) != pdTRUE) g_intOverflow++;
    if (woke) portYIELD_FROM_ISR();
}

static_assert(MPU_INT_INTR_LEVEL >= 1 && MPU_INT_INTR_LEVEL <= 3, "MPU_INT_INTR_LEVEL fora de 1..3");

// Liga a ISR do INT uma vez só (o serviço de GPIO do IDF não aceita ser instalado de novo), em IRAM e
// no nível MPU_INT_INTR_LEVEL. Sem ela não há amostras: reinicia, como na falta da fila
static void attachIntPin() {
    static bool attached = false;
    if (attached) return;
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << PIN_MPU_INT;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_ENABLE;
    io.intr_type = GPIO_INTR_POSEDGE;
    esp_err_t err = gpio_config(&io);
    // ESP_INTR_FLAG_LEVELn vale (1 << n)
    if (err == ESP_OK) err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM | (1 << MPU_INT_INTR_LEVEL));
    if (err == ESP_OK) err = gpio_isr_handler_add((gpio_num_t)PIN_MPU_INT, onMpuInt, nullptr);
    if (err != ESP_OK) {
        logPrintf("[A] ERRO: ISR do INT não registrada (%s); reiniciando em 1 s\n", esp_err_to_name(err));
        logFlush();
        delay(1000);
        ESP.restart();
    }
    attached = true;
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
    attachIntPin();
    g_mpuReady = true;
    // O giroscópio tem um transitório logo após a configuração: calibração e fusão ignoram esse trecho
    beginSettle();
    fusionReset();
    g_lastRecoveries = g_mpu.recoveries();
    g_acq.begin(micros());
}

// Fecha (imprime) todas as janelas que terminaram antes de nowUs e abre a seguinte sem lacuna
static void closeWindowsUntil(uint32_t nowUs) {
    while (g_acq.due(nowUs)) {
        const SampleWindow &w = g_acq.cur();
        g_magSvc.closeWindow();
        g_console.windowClosed(w);   // resumo, se o r estiver ligado
        if (w.samples > 0) g_calSvc.checkTempDrift(w.meanTempC());
        digitalWrite(PIN_LED_STATUS, !digitalRead(PIN_LED_STATUS));
        g_acq.rotate();
    }
}

// Conta um pulso na janela do seu instante e detecta pulsos que faltaram
static void notePulse(uint32_t tInt) {
    closeWindowsUntil(tInt);
    g_acq.notePulse(tInt, 1e6f / g_mpu.actualSampleRateHz());
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

// Uma amostra válida, fora do descarte: calibração nos eixos do chip → eixos do modelo →
// rotação de montagem → filtro. intUs: instante do pulso de "data ready" desta amostra, que já
// passou pela validade temporal no loop().
static void fusionSample(const ImuSample &s, uint32_t intUs) {
    const ImuCalData &cal = g_calSvc.imuCal();
    const BodyImu b = imuToBody({s.ax, s.ay, s.az}, {s.gx, s.gy, s.gz}, cal.accelValid, cal.accel, g_calSvc.gyroBias(),
                                g_calSvc.level().r);
    const ImuFlags flags = {s.accelSaturated, s.gyroSaturated};
    g_fusion.update(b.acc, b.gyro, flags, intUs, g_magSvc.calibrating());
    if (g_att.initialized()) g_acq.cur().addAttitude(g_att.rollDeg(), g_att.pitchDeg());
    g_console.attitudeSample(b.acc, s.tempC, intUs);   // linha do a, se estiver ligado e vencida
}

// ---------- Validade dos dados (campo valid do TelemetryPacket, lib/telemetry_flags) ----------
// Entradas no instante da chamada; hoje usadas pelo c, na semana 3 também pela radio_task

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

void setup() {
    // Buffer de transmissão em software (antes do begin): sem ele, cada impressão bloqueia o laço
    // até sobrar só a FIFO de 128 bytes da UART (~10 ms por linha longa a 115200) e descarta amostras
    Serial.setTxBufferSize(SERIAL_TX_BUFFER_BYTES);
    Serial.begin(115200);
    const bool logBuffered = logBegin(LOG_BUFFER_BYTES);   // também redireciona os logs do núcleo e do IDF
    delay(500);
    pinMode(PIN_LED_STATUS, OUTPUT);
    logPrintf("[A] boot — compilado em %s — TelemetryPacket = %u bytes (main provisório: MPU6050 e "
              "calibração)\n", BUILD_STAMP, (unsigned)sizeof(TelemetryPacket));
    if (!logBuffered) logPrintln("[A] AVISO: sem memória para o buffer do logger; impressões direto no Serial");
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
    g_console.printHelp();
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
        logPump();
        g_baroSvc.service(micros());
        g_magSvc.service(micros());
    }
}

void loop() {
    logPump();   // mensagens do ciclo anterior, no espaço livre do Serial, sem esperar
    g_console.poll();
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
        g_acq.noteStale();   // o pulso anterior fica sem leitura; conta na janela dele, antes de avançar
        notePulse(pulses[i]);
    }
    g_acqStats.dropped += n - 1;

    checkRecovery();   // read() pode ter reconfigurado o chip (e então devolveu false)
    if (!ok) return;
    g_acq.cur().add({s.ax, s.ay, s.az}, {s.gx, s.gy, s.gz}, s.tempC, s.accelSaturated, s.gyroSaturated);
    // Descarte de partida
    if (g_settle.hold(s.tUs)) {
        g_calSvc.holdWindowStart(s.tUs);   // a janela só começa a contar depois do descarte
        return;
    }

    // Validade temporal, comum à calibração e à fusão (lib/acq, checkFrame). Um quadro ambíguo fica
    // fora das duas; na fusão, o dt seguinte cobre a lacuna (a referência de tempo não avança). Um
    // atraso de alguns ms, depois de uma impressão, é normal e o quadro é válido.
    const FrameCheck frame = checkFrame(latest, s.tUs, newerPulse, FUSION_MAX_INT_LAG_US);
    g_acqStats.noteFrame(frame);
    if (!frame.valid) return;
    g_calSvc.sample(s);
    fusionSample(s, latest);
}
