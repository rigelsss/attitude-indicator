// led_ring.h — anel de LEDs da caixa do ESP B (docs/caixa-b.md, "Anel de LEDs"): posição de cada LED, cor do
// "nível de bolha", estados, flash do alarme, brilho pelo potenciômetro e teto de corrente.
// Lógica pura (sem Arduino.h), testável com pio test -e native.
//
// Eixos do modelo (os do ESP A, docs/fusao.md): x para o nariz, y para a asa direita. Na caixa do B, quem olha de
// frente é o piloto: o nariz é a borda de longe, e a asa direita fica à direita dele. Roll positivo = asa direita
// descendo; pitch positivo = nariz subindo. Um ponto (x, y) da borda fica à altura
//     h = x·sin(pitch) − y·sin(roll)·cos(pitch)
// e a inclinação da borda naquele ponto é asin(h / r), com r a distância do ponto ao centro (o ponto gira
// mantendo essa distância). Positiva: lado alto
// (azul, o céu); negativa: lado baixo (marrom, a terra). Coerente com o TFT: nariz para cima deixa a borda de
// longe azul; roll à direita deixa a borda direita marrom.
#pragma once
#include <stdint.h>

struct LedRgb {
    uint8_t r, g, b;
};

struct LedPoint {
    float xMm, yMm;   // x para o nariz, y para a asa direita, a partir do centro da caixa
};

// Cantos do retângulo, vistos de cima com o nariz para longe
enum class LedCorner : uint8_t { TailLeft, TailRight, NoseRight, NoseLeft };

// Um anel: retângulo depthMm (cauda → nariz) × widthMm (asa esquerda → direita), com os LEDs espalhados por igual
// em cada lado, no centro de cada trecho. O LED 0 é o primeiro depois do canto onde entra o DIN; clockwise conta
// no sentido horário visto de cima
struct LedRingGeom {
    float     depthMm, widthMm;
    uint8_t   nNose, nRight, nTail, nLeft;   // LEDs em cada lado
    LedCorner start;
    bool      clockwise;
};

uint16_t ledRingCount(const LedRingGeom &g);
// Preenche out com a posição de cada LED, na ordem da fita. Devolve quantos (no máximo max)
uint16_t ledRingLayout(const LedRingGeom &g, LedPoint *out, uint16_t max);
// Lados na ordem da fita (0 nariz, 1 direita, 2 cauda, 3 esquerda) e quantos LEDs em cada: para o diagnóstico
// mostrar onde cada trecho deve ficar
void ledRingSides(const LedRingGeom &g, uint8_t sides[4], uint8_t counts[4]);
const char *ledSideText(uint8_t side);
const char *ledCornerText(LedCorner c);

struct LedLook {
    float    deadbandDeg;     // até aqui, branco
    float    saturationDeg;   // daqui em diante, a cor pura
    LedRgb   white, sky, ground, amber, alarm;
    uint32_t flashOnMs, flashPeriodMs;   // alarme sem reconhecimento: flash na cor do alarme
    uint32_t spinPeriodMs;    // sem dado atual: uma volta do LED vermelho
    uint32_t pulsePeriodMs;   // calibrando: um ciclo do âmbar pulsando
    float    pulseMin;        // brilho mínimo do pulso (0..1)
};

// Cor do horizonte num ponto, pela inclinação da borda ali
LedRgb ledHorizonColor(const LedLook &look, LedPoint p, float rollDeg, float pitchDeg);

// Em ordem de prioridade (docs/caixa-b.md, tabela de estados)
enum class LedMode : uint8_t {
    NoData,        // sem dado atual (link perdido, dado atrasado, nenhum pacote): apagado, com um LED girando
    Calibrating,   // ST_CALIBRATING: âmbar pulsando
    AttInvalid,    // sem VAL_ATT: âmbar fixo
    Horizon,       // o horizonte (com o flash, se houver alarme de atitude sem reconhecimento)
};
const char *ledModeText(LedMode m);

// Flash do alarme de atitude sem reconhecimento: começa aceso no instante em que o alarme aparece (ou muda de
// tipo, ou volta depois de reconhecido e terminado), e acende flashOnMs a cada flashPeriodMs
class LedAlarmFlash {
public:
    // active: alarme de atitude (aviso de bank ou atitude anormal) sem reconhecimento; kind: o tipo, para recomeçar
    // a fase quando ele muda. Devolve se o flash está aceso agora
    bool update(bool active, uint8_t kind, uint32_t nowMs, uint32_t onMs, uint32_t periodMs);

private:
    bool     active_ = false;
    uint8_t  kind_ = 0;
    uint32_t since_ = 0;
};

// Trecho da fita que forma um anel, com o fator de brilho dele (o de cima, sem difusor, mais fraco)
struct LedRingSpan {
    uint16_t first, count;
    float    gain;
};

struct LedFrameInput {
    LedMode mode;
    float   rollDeg, pitchDeg;   // só no Horizon
    bool    flash;               // flash do alarme aceso agora (só no Horizon)
};

// Monta o quadro (sem o brilho do potenciômetro nem o teto), já com o fator de cada anel
void ledRenderFrame(const LedLook &look, const LedPoint *pts, const LedRingSpan *rings, uint8_t nRings,
                    const LedFrameInput &in, uint32_t nowMs, LedRgb *out);

// Potenciômetro do brilho: média das últimas leituras e histerese (o ADC do ESP32 é ruidoso), curva de gama no
// curso e brilho mínimo
struct LedPotConfig {
    uint16_t adcMax;        // leitura no fim do curso (4095 no ESP32)
    uint16_t hysteresis;    // em contagens: a média precisa andar isto para mudar o brilho
    float    gamma;         // brilho = min + (1 − min)·u^gamma, com u = posição de 0 a 1
    float    minBrightness;
};

class LedPot {
public:
    static const uint8_t N = 8;   // leituras na média
    explicit LedPot(const LedPotConfig &cfg) : cfg_(cfg) {}
    // Uma leitura nova. Devolve o brilho (0..1)
    float update(uint16_t raw);
    float brightness() const { return brightness_; }
    uint16_t value() const { return held_; }   // média com a histerese, em contagens

private:
    LedPotConfig cfg_;
    uint16_t     buf_[N] = {};
    uint8_t      n_ = 0, i_ = 0;
    uint16_t     held_ = 0;
    bool         have_ = false;
    float        brightness_ = 0.0f;
    float        curve(uint16_t v) const;
};

// Estimativa de corrente da fita: mA por canal em 255 e por LED (mesmo apagado)
struct LedPowerConfig {
    float maPerChannel;   // ~20 mA por canal cheio na WS2812
    float maIdle;         // ~1 mA por LED, mesmo apagado
    float maMax;          // teto da fita
};

struct LedPowerResult {
    float estimatedMa;    // depois do teto
    bool  capped;         // o teto reduziu o quadro
};

// Aplica o brilho (0..1) ao quadro e, se a corrente estimada passar do teto, reduz o quadro inteiro na mesma
// proporção
LedPowerResult ledApplyBrightness(LedRgb *buf, uint16_t n, float brightness, const LedPowerConfig &cfg);
