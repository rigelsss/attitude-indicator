// status_view.h — tela do OLED de status do ESP A (128x32, 4 linhas de 8 px) e padrão do LED, montados pela
// status_task a partir das cópias do estado (docs/tasks.md, passo 6). Lógica pura (sem Arduino.h), testável
// com pio test -e native.
//   L1 sensores:  "MPU OK  BARO OK  MAG OK"  (sensor falhando: "--" em vídeo inverso)
//   L2 link:      "TX 50/s  ENTREGUES 50/s"  ("RÁDIO LIGANDO..." / "RÁDIO DESLIGADO")
//   L3 tempo:     "LIGADO 1:23:45"
//   L4 avisos:    em vídeo inverso, um por vez (revezamento); sem aviso, "SEM AVISOS"
// A fonte do OLED (NokiaSmallPlain) é proporcional: o vídeo inverso é dado em caracteres, e a status_task o
// converte em pixels. Os textos mais largos (taxas com 2 dígitos, pulsos falsos com 4) cabem nos 128 px
// (medidos nos dados da fonte em 22/10/2026; a status_task avisa na serial se uma linha passar)
#pragma once
#include <stddef.h>
#include <stdint.h>

static const int STATUS_LINES = 4;
static const int STATUS_COLS = 25;                         // limite em caracteres de uma linha
static const size_t STATUS_LINE_BYTES = STATUS_COLS * 2 + 1;   // UTF-8: acentos ocupam 2 bytes

// Motivo do reinício anterior, já classificado (a status_task traduz o esp_reset_reason())
enum class ResetKind : uint8_t { Normal, Brownout, Panic, Watchdog };

enum class RadioState : uint8_t {
    Off,        // rádio desligado neste firmware ou falhou ao iniciar
    Starting,   // task criada, Wi-Fi ainda não ativo (RADIO_START_DELAY_MS)
    Active,     // ESP-NOW enviando
};

enum class LedMode : uint8_t {
    Off,           // rádio inativo
    NoDelivery,    // enviando, sem entregas no último segundo (sem o B)
    Delivering,    // enviando, com o B confirmando
};

// Fatos do instante, lidos pela status_task das cópias da sensor_task e dos contadores do rádio
struct StatusInputs {
    bool       haveState;         // a sensor_task já publicou o estado
    bool       mpuOk, baroOk, magOk;
    bool       calibrating;
    uint32_t   intSpurious;       // pulsos falsos no INT desde o boot
    RadioState radio;
    uint32_t   sent, delivered;   // contadores do rádio desde o boot (esp_now_send aceito, ACK do B)
    uint32_t   uptimeS;           // tempo ligado
    ResetKind  lastReset;
};

struct StatusViewParams {
    uint32_t warnRotateMs;     // OLED_WARN_ROTATE_MS: tempo de cada aviso no revezamento
    uint32_t spuriousShowMs;   // OLED_SPURIOUS_SHOW_MS: aviso dos pulsos falsos depois do último aumento
    uint32_t ledOnMs, ledPeriodMs;           // Delivering: pisca (LED_LINK_ON_MS / LED_LINK_PERIOD_MS)
    uint32_t ledIdleOnMs, ledIdlePeriodMs;   // NoDelivery: piscada curta (LED_IDLE_ON_MS / LED_IDLE_PERIOD_MS)
};

struct StatusScreen {
    char     line[STATUS_LINES][STATUS_LINE_BYTES];
    uint32_t inverse[STATUS_LINES];   // bit c: coluna c em vídeo inverso
};

// Caracteres (pontos de código UTF-8) de um texto: a largura na fonte monoespaçada
size_t utf8Columns(const char *s);
// "h:mm:ss", com as horas sem limite
void formatUptime(uint32_t s, char *out, size_t n);

class StatusView {
public:
    explicit StatusView(const StatusViewParams &p) : p_(p) {}

    // Atualiza as taxas por segundo do rádio e a janela dos pulsos falsos. Chamar a cada volta da task
    void update(const StatusInputs &in, uint32_t nowMs);
    // Tela do último update(); o aviso da vez é escolhido por nowMs
    void render(uint32_t nowMs, StatusScreen &out) const;

    LedMode ledMode() const;
    bool ledOn(uint32_t nowMs) const;

    // Taxas do último segundo completo; false no primeiro segundo com o rádio ativo
    bool haveRates() const { return haveRates_; }
    uint32_t sentPerS() const { return sentPerS_; }
    uint32_t deliveredPerS() const { return deliveredPerS_; }
    bool spuriousRecent() const { return spuriousRecent_; }

private:
    // Avisos ativos, em ordem de prioridade; devolve a quantidade
    int warnings(const char *out[3], char *spuriousText, size_t n) const;

    StatusViewParams p_;
    StatusInputs in_ = {};
    // Taxas: referência no início do segundo corrente
    bool     haveRef_ = false, haveRates_ = false;
    uint32_t refMs_ = 0, refSent_ = 0, refDelivered_ = 0;
    uint32_t sentPerS_ = 0, deliveredPerS_ = 0;
    // Pulsos falsos: último total visto e instante do último aumento
    bool     spuriousSeen_ = false, spuriousRecent_ = false;
    uint32_t lastSpurious_ = 0, spuriousMs_ = 0;
};
