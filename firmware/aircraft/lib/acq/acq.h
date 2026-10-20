// acq.h — contabilidade da aquisição do MPU6050 em lógica pura (sem Arduino.h), testável com
// pio test -e native: janelas de 1 s com limites contínuos (taxa, perdidas, descartadas, médias e
// ruído do resumo r e do estado c), validade temporal do quadro, estatística do fluxo a e descarte
// de partida. A fila do INT, a ISR e a leitura do chip ficam com quem chama (hoje, o loop()).
// Os instantes são de micros(), dados por quem chama; a virada dos 32 bits (~71 min) é tratada.
// Docs: docs/refatoracao.md (passo 7).
#pragma once
#include <stdint.h>
#include "imu_cal.h"
#include "attitude.h"

// Acúmulo de uma janela de impressão: [startUs, startUs + período)
struct SampleWindow {
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

    // Uma leitura válida do chip (g, °/s, °C)
    void add(const Vec3 &acc, const Vec3 &gyro, float tempC, bool accSat, bool gyroSat);
    // Atitude da fusão inicializada, na mesma amostra
    void addAttitude(float rollDeg, float pitchDeg);
    // Médias (válidas com samples > 0)
    Vec3 meanAcc() const;
    Vec3 meanGyro() const;
    float meanTempC() const { return tempSum / samples; }
};

// Números do resumo por segundo (r). Com samples == 0, só rateHz vale.
struct WindowSummary {
    float  rateHz;
    double mean[6];        // ax ay az gx gy gz
    double accNorm;        // |média do acelerômetro|
    double accMaxStdG;     // maior desvio entre os eixos do acelerômetro
    double gyroMaxStdDps;  // idem, giroscópio
    float  tempC;
};
WindowSummary summarize(const SampleWindow &w, uint32_t windowUs);

// Janelas com limites contínuos, medidos pelos instantes dos pulsos: o tempo gasto imprimindo não
// distorce a taxa. Quem chama fecha as janelas vencidas (due/rotate, imprimindo entre uma e outra)
// antes de contar um pulso.
class PulseTracker {
public:
    explicit PulseTracker(uint32_t windowUs) : windowUs_(windowUs) {}
    // MPU (re)configurado: janela nova a partir de agora e sem pulso anterior (a última janela fica)
    void begin(uint32_t nowUs);
    // A janela atual terminou antes de nowUs? Diferença com sinal: um instante anterior ao início dá
    // negativo (e não um intervalo enorme).
    bool due(uint32_t nowUs) const { return (int32_t)(nowUs - cur_.startUs) >= (int32_t)windowUs_; }
    // Guarda a janela atual como a última completa e abre a seguinte, sem lacuna
    void rotate();
    // Um pulso, com as janelas já fechadas até o instante dele. Um pulso anterior ao início da janela
    // (ela avançou pelo relógio enquanto ele esperava na fila) pertence a uma janela já impressa e não
    // entra na taxa da atual. Um intervalo maior que 1,5 período conta os pulsos que faltaram.
    void notePulse(uint32_t tInt, float periodUs);
    // O pulso anterior ficou sem leitura: conta na janela dele, antes do próximo notePulse()
    void noteStale() { cur_.stale++; }

    SampleWindow &cur() { return cur_; }
    const SampleWindow &cur() const { return cur_; }
    const SampleWindow &last() const { return last_; }   // última janela completa: base do comando c
    uint32_t windowUs() const { return windowUs_; }

private:
    uint32_t     windowUs_;
    SampleWindow cur_;
    SampleWindow last_;
    uint32_t     lastIntUs_ = 0;
    bool         havePrev_ = false;   // há pulso anterior (0 µs é um instante válido na virada do micros())
};

// Validade temporal do quadro, comum à calibração e à fusão. Sem FIFO, o quadro lido é o mais recente
// no instante da leitura. Se um pulso mais novo chegou antes do fim da leitura, o quadro pode ser
// dele (newerPulse), e o ciclo seguinte leria o mesmo quadro de novo. O teto de atraso é só uma rede
// de segurança (latência da ISR).
struct FrameCheck {
    bool     valid;
    uint32_t lagUs;   // pulso → início da leitura
};
FrameCheck checkFrame(uint32_t pulseUs, uint32_t sampleUs, bool newerPulse, uint32_t maxLagUs);

// Estatística da aquisição entre duas linhas do fluxo a
struct AcqStats {
    uint32_t lagMaxUs = 0;    // maior atraso pulso → início da leitura (quadros válidos)
    uint32_t ambiguous = 0;   // quadros fora da calibração e da fusão (checkFrame inválido)
    uint32_t dropped = 0;     // pulsos sem leitura (amostra sobrescrita)

    void noteFrame(const FrameCheck &f) {
        if (!f.valid) ambiguous++;
        else if (f.lagUs > lagMaxUs) lagMaxUs = f.lagUs;
    }
};

// Descarte de partida depois de configurar o MPU. O estado explícito evita comparar o prazo depois
// de vencido: a diferença em int32_t volta a ficar negativa ~36 min depois, e todas as amostras
// passariam a ser descartadas.
class SettleGate {
public:
    void begin(uint32_t nowUs, uint32_t durationUs) {
        active_ = true;
        untilUs_ = nowUs + durationUs;
    }
    // true enquanto a amostra de tUs deve ser descartada; a comparação só acontece durante o descarte
    bool hold(uint32_t tUs) {
        if (!active_) return false;
        if ((int32_t)(tUs - untilUs_) < 0) return true;
        active_ = false;
        return false;
    }
    bool active() const { return active_; }

private:
    bool     active_ = false;
    uint32_t untilUs_ = 0;
};
