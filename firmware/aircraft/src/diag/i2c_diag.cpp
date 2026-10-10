// Diagnóstico do barramento I2C do ESP A (ambiente i2cscan, não faz parte do firmware de voo).
// 1) Varre 0x08..0x77 e identifica os chips conhecidos pelos registradores de ID.
// 2) Compara com os endereços de config.h.
// 3) Se o MPU6050 responder, aceita comandos pela serial:
//      s  varre de novo (útil ao mexer em cabos e soldas)
//      l  liga/desliga o fluxo de leituras brutas a DIAG_PRINT_HZ (começa desligado)
//      m  média e desvio padrão de DIAG_AVG_SAMPLES leituras (placa parada)
//      f  alterna o I2C entre I2C_FREQ_DIAG_HZ e I2C_FREQ_HZ (a do firmware de voo)
//      t  teste de contato: lê o mais rápido possível por DIAG_STRESS_MS e conta falhas
//      i  teste do pino INT: conta pulsos de "data ready" por DIAG_INT_TEST_MS
// Acesso direto por registrador, sem bibliotecas, para isolar problemas de hardware.
#include <Arduino.h>
#include <Wire.h>
#include "config.h"

// Registradores usados apenas no diagnóstico
static const uint8_t MPU_REG_SMPLRT_DIV = 0x19;   // taxa = 1 kHz / (1 + div) com DLPF ligado
static const uint8_t MPU_REG_CONFIG     = 0x1A;   // DLPF_CFG nos bits 2..0
static const uint8_t MPU_REG_INT_ENABLE = 0x38;   // bit 0: DATA_RDY_EN
static const uint8_t MPU_REG_INT_STATUS = 0x3A;   // leitura limpa a interrupção
static const uint8_t MPU_REG_PWR_MGMT_1 = 0x6B;
static const uint8_t MPU_REG_ACCEL_XOUT = 0x3B;   // 14 bytes: accel, temp, gyro (big-endian)
static const uint8_t MPU_REG_WHO_AM_I   = 0x75;   // 0x68 no MPU6050
static const uint8_t BME_REG_CHIP_ID    = 0xD0;   // 0x60 BME280, 0x58 BMP280
static const uint8_t HMC_REG_ID_A       = 0x0A;   // 0x0A..0x0C = "H43"

static bool g_mpuOk = false;
static bool g_stream = false;
static uint32_t g_i2cFreq = I2C_FREQ_DIAG_HZ;
static volatile uint32_t g_intCount = 0;          // só a ISR escreve; leitura de 32 bits é atômica

static bool probe(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

static bool readRegs(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t n) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(addr, n) != n) return false;
    for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
    return true;
}

static bool writeReg(uint8_t addr, uint8_t reg, uint8_t val) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

// Descreve o dispositivo encontrado em addr, lendo o ID quando o chip tem um
static void identify(uint8_t addr) {
    uint8_t id[3] = {0};
    switch (addr) {
    case 0x68:
    case 0x69:
        if (readRegs(addr, MPU_REG_WHO_AM_I, id, 1))
            Serial.printf("MPU (WHO_AM_I=0x%02X%s)", id[0], id[0] == 0x68 ? ", MPU6050" : ", inesperado");
        else
            Serial.print("MPU? (falha ao ler WHO_AM_I)");
        break;
    case 0x76:
    case 0x77:
        if (readRegs(addr, BME_REG_CHIP_ID, id, 1))
            Serial.printf("barômetro (chip_id=0x%02X%s)", id[0],
                          id[0] == 0x60 ? ", BME280" : id[0] == 0x58 ? ", BMP280: sem umidade" : ", inesperado");
        else
            Serial.print("barômetro? (falha ao ler chip_id)");
        break;
    case 0x1E:
        if (readRegs(addr, HMC_REG_ID_A, id, 3))
            Serial.printf("magnetômetro (ID=\"%c%c%c\"%s)", id[0], id[1], id[2],
                          (id[0] == 'H' && id[1] == '4' && id[2] == '3') ? ", HMC5883L" : ", inesperado");
        else
            Serial.print("magnetômetro? (falha ao ler ID)");
        break;
    case 0x0D:
        Serial.print("QMC5883L? (clone do HMC5883L; config.h espera 0x1E)");
        break;
    case 0x3C:
    case 0x3D:
        Serial.print("OLED SSD1306 (sem registrador de ID)");
        break;
    default:
        Serial.print("desconhecido");
    }
}

static void checkExpected(const char *name, uint8_t addr) {
    Serial.printf("  %-12s 0x%02X  %s\n", name, addr, probe(addr) ? "OK" : "AUSENTE");
}

static void scan() {
    Serial.printf("\n[diag] scan I2C  SDA=%d SCL=%d  %lu Hz\n", PIN_I2C_SDA, PIN_I2C_SCL,
                  (unsigned long)g_i2cFreq);
    unsigned found = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (!probe(addr)) continue;
        found++;
        Serial.printf("  0x%02X  ", addr);
        identify(addr);
        Serial.println();
    }
    Serial.printf("[diag] %u dispositivo(s)\n", found);

    Serial.println("[diag] esperados em config.h:");
    checkExpected("MPU6050", ADDR_MPU6050);
    checkExpected("BME280", ADDR_BME280);
    checkExpected("HMC5883L", ADDR_HMC5883L);
    checkExpected("OLED", ADDR_OLED_STATUS);

    // Tira o MPU do modo sleep; faixas padrão: ±2 g e ±250 °/s
    g_mpuOk = probe(ADDR_MPU6050) && writeReg(ADDR_MPU6050, MPU_REG_PWR_MGMT_1, 0x00);
    if (g_mpuOk) {
        delay(100);
        Serial.println("[diag] comandos: s = scan | l = leituras | m = média parada | f = 100/400 kHz | t = contato | i = INT");
    } else {
        Serial.println("[diag] MPU6050 indisponível: sem leituras brutas (s = novo scan)");
    }
}

// Lê accel, temp e gyro; v: ax ay az temp gx gy gz
static bool readMpu(int16_t v[7]) {
    uint8_t b[14];
    if (!readRegs(ADDR_MPU6050, MPU_REG_ACCEL_XOUT, b, sizeof(b))) return false;
    for (int i = 0; i < 7; i++) v[i] = (int16_t)((b[2 * i] << 8) | b[2 * i + 1]);
    return true;
}

static void printHeader() {
    Serial.println("[diag] ax ay az (LSB) | gx gy gz (LSB) | ax ay az (g) | gx gy gz (°/s) | T (°C)");
}

// Média e desvio padrão com a placa parada: offsets do giroscópio e eixo que marca 1 g
static void average() {
    double sum[7] = {0}, sq[7] = {0};
    unsigned n = 0;
    Serial.printf("[diag] média de %d leituras, mantenha a placa parada...\n", DIAG_AVG_SAMPLES);
    for (int k = 0; k < DIAG_AVG_SAMPLES; k++) {
        int16_t v[7];
        if (readMpu(v)) {
            for (int i = 0; i < 7; i++) { sum[i] += v[i]; sq[i] += (double)v[i] * v[i]; }
            n++;
        }
        delay(1000 / SAMPLE_RATE_HZ);
    }
    if (n == 0) { Serial.println("[diag] falha de leitura do MPU6050"); return; }
    const char *name[7] = {"ax", "ay", "az", "T", "gx", "gy", "gz"};
    const float scale[7] = {16384.0f, 16384.0f, 16384.0f, 1.0f, 131.0f, 131.0f, 131.0f};
    for (int i = 0; i < 7; i++) {
        if (i == 3) continue;
        double mean = sum[i] / n;
        double sd = sqrt(sq[i] / n - mean * mean);
        Serial.printf("  %s  média %8.1f LSB = %7.3f %s   desvio %6.1f LSB\n", name[i], mean,
                      mean / scale[i], i < 3 ? "g" : "°/s", sd);
    }
    Serial.printf("  T   média %5.1f °C   (%u de %d leituras, I2C %lu Hz)\n", sum[3] / n / 340.0 + 36.53, n,
                  DIAG_AVG_SAMPLES, (unsigned long)g_i2cFreq);
}

static void toggleFreq() {
    g_i2cFreq = (g_i2cFreq == I2C_FREQ_DIAG_HZ) ? I2C_FREQ_HZ : I2C_FREQ_DIAG_HZ;
    Wire.setClock(g_i2cFreq);
    Serial.printf("[diag] I2C agora em %lu Hz\n", (unsigned long)g_i2cFreq);
}

// Lê sem pausa e conta falhas de transação e quadros suspeitos (todos os bytes 0x00 ou 0xFF,
// típicos de linha solta). Mexa nos fios e na placa durante o teste.
static void stressTest() {
    Serial.printf("[diag] teste de contato por %d ms a %lu Hz: mexa nos fios agora...\n", DIAG_STRESS_MS,
                  (unsigned long)g_i2cFreq);
    uint32_t ok = 0, fail = 0, suspect = 0;
    uint32_t t0 = millis();
    while (millis() - t0 < DIAG_STRESS_MS) {
        uint8_t b[14];
        if (!readRegs(ADDR_MPU6050, MPU_REG_ACCEL_XOUT, b, sizeof(b))) { fail++; continue; }
        bool all00 = true, allFF = true;
        for (uint8_t i = 0; i < sizeof(b); i++) {
            if (b[i] != 0x00) all00 = false;
            if (b[i] != 0xFF) allFF = false;
        }
        if (all00 || allFF) suspect++;
        else ok++;
    }
    Serial.printf("[diag] %lu leituras: %lu ok, %lu falhas, %lu suspeitas (%.0f leituras/s)\n",
                  (unsigned long)(ok + fail + suspect), (unsigned long)ok, (unsigned long)fail,
                  (unsigned long)suspect, (ok + fail + suspect) * 1000.0f / DIAG_STRESS_MS);
    // Uma falha pode deixar o MPU sem resposta; confirma que ele continua lá
    Serial.printf("[diag] MPU após o teste: %s\n", probe(ADDR_MPU6050) ? "OK" : "SEM RESPOSTA");
}

static void IRAM_ATTR onMpuInt() { g_intCount++; }

// Configura o MPU para gerar "data ready" a SAMPLE_RATE_HZ e conta as bordas no PIN_MPU_INT.
// Ao final, volta à configuração padrão usada pelos outros comandos.
static void intTest() {
    const uint8_t div = 1000 / SAMPLE_RATE_HZ - 1;
    bool cfg = writeReg(ADDR_MPU6050, MPU_REG_CONFIG, 0x01) &&           // DLPF 188 Hz: base de 1 kHz
               writeReg(ADDR_MPU6050, MPU_REG_SMPLRT_DIV, div) &&
               writeReg(ADDR_MPU6050, MPU_REG_INT_ENABLE, 0x01);
    if (!cfg) { Serial.println("[diag] falha ao configurar o MPU6050"); return; }

    pinMode(PIN_MPU_INT, INPUT_PULLDOWN);   // fio solto lê 0 em vez de flutuar e gerar pulsos falsos
    g_intCount = 0;
    attachInterrupt(digitalPinToInterrupt(PIN_MPU_INT), onMpuInt, RISING);
    uint32_t t0 = millis();
    while (millis() - t0 < DIAG_INT_TEST_MS) {
        uint8_t st;
        readRegs(ADDR_MPU6050, MPU_REG_INT_STATUS, &st, 1);   // limpa o pedido, como fará o firmware
        delay(1);
    }
    detachInterrupt(digitalPinToInterrupt(PIN_MPU_INT));
    uint32_t n = g_intCount;

    writeReg(ADDR_MPU6050, MPU_REG_INT_ENABLE, 0x00);
    writeReg(ADDR_MPU6050, MPU_REG_SMPLRT_DIV, 0x00);
    writeReg(ADDR_MPU6050, MPU_REG_CONFIG, 0x00);

    float hz = n * 1000.0f / DIAG_INT_TEST_MS;
    Serial.printf("[diag] INT no GPIO %d: %lu pulsos em %d ms = %.1f Hz (esperado %d Hz)  %s\n", PIN_MPU_INT,
                  (unsigned long)n, DIAG_INT_TEST_MS, hz, SAMPLE_RATE_HZ,
                  n == 0 ? "SEM PULSOS: conferir o fio INT"
                         : fabsf(hz - SAMPLE_RATE_HZ) <= SAMPLE_RATE_HZ * 0.05f ? "OK" : "TAXA FORA DO ESPERADO");
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_DIAG_HZ);
    scan();
}

void loop() {
    if (Serial.available()) {
        switch (Serial.read()) {
        case 's': g_stream = false; scan(); break;
        case 'l':
            if (!g_mpuOk) break;
            g_stream = !g_stream;
            if (g_stream) printHeader();
            else Serial.println("[diag] leituras pausadas");
            break;
        case 'm': if (g_mpuOk) { g_stream = false; average(); } break;
        case 'f': toggleFreq(); break;
        case 't': if (g_mpuOk) { g_stream = false; stressTest(); } break;
        case 'i': if (g_mpuOk) { g_stream = false; intTest(); } break;
        }
    }
    if (!g_stream) return;

    int16_t v[7];
    if (!readMpu(v)) {
        Serial.println("[diag] falha de leitura do MPU6050");
        delay(1000 / DIAG_PRINT_HZ);
        return;
    }
    Serial.printf("%6d %6d %6d | %6d %6d %6d | %5.2f %5.2f %5.2f | %7.1f %7.1f %7.1f | %5.1f\n",
                  v[0], v[1], v[2], v[4], v[5], v[6],
                  v[0] / 16384.0f, v[1] / 16384.0f, v[2] / 16384.0f,
                  v[4] / 131.0f, v[5] / 131.0f, v[6] / 131.0f,
                  v[3] / 340.0f + 36.53f);
    delay(1000 / DIAG_PRINT_HZ);
}
