// Diagnóstico do barramento I2C do ESP A (ambiente i2cscan, não faz parte do firmware de voo).
// 1) Varre 0x08..0x77 e identifica os chips conhecidos pelos registradores de ID.
// 2) Compara com os endereços de config.h.
// 3) Se o MPU6050 responder, imprime leituras brutas a DIAG_PRINT_HZ.
// Envie 's' pela serial para varrer de novo (útil ao mexer em cabos e soldas).
// Acesso direto por registrador, sem bibliotecas, para isolar problemas de hardware.
#include <Arduino.h>
#include <Wire.h>
#include "config.h"

// Registradores usados apenas na identificação
static const uint8_t MPU_REG_PWR_MGMT_1 = 0x6B;
static const uint8_t MPU_REG_ACCEL_XOUT = 0x3B;   // 14 bytes: accel, temp, gyro (big-endian)
static const uint8_t MPU_REG_WHO_AM_I   = 0x75;   // 0x68 no MPU6050
static const uint8_t BME_REG_CHIP_ID    = 0xD0;   // 0x60 BME280, 0x58 BMP280
static const uint8_t HMC_REG_ID_A       = 0x0A;   // 0x0A..0x0C = "H43"

static bool g_mpuOk = false;

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
                  (unsigned long)I2C_FREQ_DIAG_HZ);
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
        Serial.println("[diag] MPU6050 brutos: ax ay az (LSB) | gx gy gz (LSB) | ax ay az (g) | gx gy gz (°/s) | T (°C)");
    } else {
        Serial.println("[diag] MPU6050 indisponível: sem leituras brutas");
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_DIAG_HZ);
    scan();
}

void loop() {
    if (Serial.available()) {
        if (Serial.read() == 's') scan();
    }
    if (!g_mpuOk) return;

    uint8_t b[14];
    if (!readRegs(ADDR_MPU6050, MPU_REG_ACCEL_XOUT, b, sizeof(b))) {
        Serial.println("[diag] falha de leitura do MPU6050");
        delay(1000 / DIAG_PRINT_HZ);
        return;
    }
    int16_t v[7];
    for (int i = 0; i < 7; i++) v[i] = (int16_t)((b[2 * i] << 8) | b[2 * i + 1]);
    // v: ax ay az temp gx gy gz
    Serial.printf("%6d %6d %6d | %6d %6d %6d | %5.2f %5.2f %5.2f | %7.1f %7.1f %7.1f | %5.1f\n",
                  v[0], v[1], v[2], v[4], v[5], v[6],
                  v[0] / 16384.0f, v[1] / 16384.0f, v[2] / 16384.0f,
                  v[4] / 131.0f, v[5] / 131.0f, v[6] / 131.0f,
                  v[3] / 340.0f + 36.53f);
    delay(1000 / DIAG_PRINT_HZ);
}
