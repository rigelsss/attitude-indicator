// ESP A — avião. *** main.cpp PROVISÓRIO (etapa de bancada, antes do RTOS) ***
// Valida o driver do MPU6050: lê uma amostra a cada pulso de "data ready" e imprime,
// a cada MAIN_PRINT_MS, médias, ruído, picos, taxa medida e contadores de falha.
// Valores nos eixos do chip e SEM calibração.
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

static Mpu6050 g_mpu;
static QueueHandle_t g_intQueue = nullptr;   // instantes (micros) dos pulsos de "data ready"
static bool g_mpuReady = false;

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
static uint32_t g_lastIntUs = 0;

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
    g_win = Window();
    g_win.startUs = micros();
}

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

// Fecha (imprime) todas as janelas que terminaram antes de nowUs e abre a seguinte sem lacuna.
// Diferença com sinal: um instante anterior ao início da janela dá negativo (e não um intervalo
// enorme), e a virada do micros() a cada ~71 min continua tratada.
static void closeWindowsUntil(uint32_t nowUs) {
    while ((int32_t)(nowUs - g_win.startUs) >= (int32_t)WINDOW_US) {
        printWindow();
        digitalWrite(PIN_LED_STATUS, !digitalRead(PIN_LED_STATUS));
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

void setup() {
    Serial.begin(115200);
    delay(500);
    pinMode(PIN_LED_STATUS, OUTPUT);
    Serial.printf("[A] boot — TelemetryPacket = %u bytes (main provisório: validação do driver MPU6050)\n",
                  (unsigned)sizeof(TelemetryPacket));
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
    Wire.setTimeOut(I2C_TIMEOUT_MS);   // vale para o barramento inteiro e sobrevive a Wire.end()/begin()
    g_intQueue = xQueueCreate(MPU_INT_QUEUE_LEN, sizeof(uint32_t));
    startMpu();
}

void loop() {
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
    if (g_mpu.read(s)) accumulate(s);
}
