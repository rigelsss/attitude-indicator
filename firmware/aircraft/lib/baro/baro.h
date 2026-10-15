// baro.h — barômetro em lógica pura (sem Arduino.h), testável com pio test -e native.
// Compensação do BME280 pelas fórmulas inteiras do datasheet (seção 4.2.3), altitude relativa e
// janela de zero. Estratégia: docs/barometro.md. Evidência de bancada: docs/hardware.md.
#pragma once
#include <stdint.h>

static const uint8_t BME280_CALIB_TP_LEN = 24;   // 0x88..0x9F: dig_T1..dig_P9 (little-endian)
static const int32_t BME280_ADC_SKIPPED = 0x80000;   // valor de medição desligada (ou ainda não feita)

// Coeficientes de fábrica de temperatura e pressão, gravados em cada chip
struct Bme280Calib {
    uint16_t T1;
    int16_t  T2, T3;
    uint16_t P1;
    int16_t  P2, P3, P4, P5, P6, P7, P8, P9;
};

// Decodifica os 24 bytes lidos a partir de 0x88. Devolve false se T1 ou P1 forem zero
// (coeficientes ausentes: a compensação dividiria por zero ou daria valores sem sentido).
bool bme280ParseCalib(const uint8_t raw[BME280_CALIB_TP_LEN], Bme280Calib &out);

// Decodifica os 6 bytes de 0xF7..0xFC em leituras brutas de 20 bits (pressão, temperatura)
void bme280ParseAdc(const uint8_t raw[6], int32_t &adcP, int32_t &adcT);

enum class BaroComp : uint8_t {
    Ok,
    Skipped,   // temperatura ou pressão com o valor de "não medido" (ex.: logo após configurar)
    Invalid,   // divisão por zero na compensação (coeficientes inválidos)
};

// Compensa uma leitura. Saída em unidades físicas: °C e hPa.
BaroComp bme280Compensate(const Bme280Calib &cal, int32_t adcT, int32_t adcP, float &tempC, float &pressHpa);

// Altitude em metros de p em relação a pRef, pela fórmula barométrica padrão:
// h = 44330 · (1 − (p/pRef)^0,1903). Positiva quando p < pRef (subiu).
// NaN se p ou pRef não forem finitas e positivas: quem chama trata como "sem altitude".
float baroAltitudeM(float pressHpa, float refHpa);

// Média e desvio padrão (populacional) de uma grandeza, de forma incremental (Welford)
class ScalarStats {
public:
    void     reset();
    void     add(double v);
    uint32_t count() const { return n_; }
    double   mean() const { return mean_; }
    double   stddev() const;

private:
    uint32_t n_ = 0;
    double   mean_ = 0;
    double   m2_ = 0;
};

// ---------- Zero da altitude ----------

enum class BaroZeroCheck : uint8_t {
    Ok,
    TooFewSamples,   // janela incompleta (falhas de leitura ou chip reconfigurado)
    Unstable,        // desvio da pressão acima do limite: placa mexida ou ar agitado
    Invalid,         // média ou desvio não finitos (leitura inválida na janela)
};

struct BaroZeroResult {
    BaroZeroCheck status;
    float refHpa;    // média da pressão na janela (válida mesmo se recusada, para diagnóstico)
    float stdPa;     // desvio padrão da pressão, Pa
    float tempC;     // temperatura média do chip na janela
};

// Avalia a janela de zero: pressão em Pa (estatística em Pa para o limite ficar em Pa) e temperatura em °C
BaroZeroResult evaluateZeroWindow(const ScalarStats &pressPa, const ScalarStats &tempC, uint32_t minSamples,
                                  float maxStdPa);
