// ws2812_rmt.cpp — ver ws2812_rmt.h. O tradutor é adaptado do exemplo "led_strip" do ESP-IDF 4.4 (o mesmo esquema
// da Adafruit NeoPixel): converte cada bit em um pulso do RMT enquanto a transmissão anda.
#include "ws2812_rmt.h"
#include <driver/rmt.h>
#include <esp_attr.h>
#include <stdlib.h>

// Temporização da WS2812 (ns), as mesmas da Adafruit NeoPixel
static const uint32_t T0H_NS = 400, T0L_NS = 850, T1H_NS = 800, T1L_NS = 450;

static rmt_channel_t s_channel = RMT_CHANNEL_0;
static int s_pin = -1;
static uint8_t s_injectInstallFailures = 0;
static Ws2812Driver s_driver;
// Pulsos de um bit 0 e de um bit 1, montados a cada instalação pelo clock do canal (lidos no tradutor, que roda
// na interrupção)
static DRAM_ATTR rmt_item32_t s_bit0, s_bit1;

// Tradutor: bytes → pulsos do RMT. Chamado pelo driver, inclusive na interrupção, para encher a memória do canal
static void IRAM_ATTR translate(const void *src, rmt_item32_t *dest, size_t srcSize, size_t wanted, size_t *translated,
                                size_t *items) {
    if (src == nullptr || dest == nullptr) {
        *translated = 0;
        *items = 0;
        return;
    }
    const uint8_t *p = (const uint8_t *)src;
    size_t size = 0, num = 0;
    while (size < srcSize && num + 8 <= wanted) {
        for (int i = 7; i >= 0; i--) {
            dest->val = (*p & (1u << i)) ? s_bit1.val : s_bit0.val;
            dest++;
        }
        num += 8;
        size++;
        p++;
    }
    *translated = size;
    *items = num;
}

// ---------- Operações do RMT para o ws2812_core, cada retorno conferido ----------

// Configura, instala, obtém o clock e registra o tradutor. Se uma etapa depois da instalação falhar, desinstala:
// "falhou" sempre quer dizer "não instalado"
static bool opInstall(void *) {
    if (s_injectInstallFailures > 0) {   // diagnóstico: falha simulada, sem tocar no driver
        s_injectInstallFailures--;
        return false;
    }
    rmt_config_t cfg = RMT_DEFAULT_CONFIG_TX((gpio_num_t)s_pin, s_channel);
    cfg.clk_div = 2;          // 80 MHz / 2 = 40 MHz: 25 ns por tique
    cfg.mem_block_num = 2;    // menos interrupções de recarga por quadro
    if (rmt_config(&cfg) != ESP_OK) return false;
    if (rmt_driver_install(s_channel, 0, 0) != ESP_OK) return false;
    uint32_t clkHz = 0;
    if (rmt_get_counter_clock(s_channel, &clkHz) != ESP_OK || clkHz == 0) {
        rmt_driver_uninstall(s_channel);
        return false;
    }
    const float ticksPerNs = (float)clkHz / 1e9f;
    s_bit0.level0 = 1;
    s_bit0.duration0 = (uint32_t)(ticksPerNs * T0H_NS);
    s_bit0.level1 = 0;
    s_bit0.duration1 = (uint32_t)(ticksPerNs * T0L_NS);
    s_bit1.level0 = 1;
    s_bit1.duration0 = (uint32_t)(ticksPerNs * T1H_NS);
    s_bit1.level1 = 0;
    s_bit1.duration1 = (uint32_t)(ticksPerNs * T1L_NS);
    if (rmt_translator_init(s_channel, translate) != ESP_OK) {
        rmt_driver_uninstall(s_channel);
        return false;
    }
    return true;
}

// Sem espera: com wait_tx_done, o rmt_write_sample() espera o fim sem prazo (portMAX_DELAY). A espera com prazo
// é a do opWaitDone
static bool opWrite(void *, const uint8_t *buf, size_t n) {
    return rmt_write_sample(s_channel, buf, n, false) == ESP_OK;
}

static bool opWaitDone(void *, uint32_t timeoutMs) {
    return rmt_wait_tx_done(s_channel, pdMS_TO_TICKS(timeoutMs)) == ESP_OK;
}

static bool opStop(void *) { return rmt_tx_stop(s_channel) == ESP_OK; }

// Sai a interrupção e o tradutor: depois disso, ninguém lê o buffer. O semáforo da transmissão presa só voltaria
// na interrupção de fim, por isso não basta parar o hardware (o próximo envio travaria esperando por ele). O
// rmt_driver_uninstall() só espera esse semáforo quando o envio foi feito com wait_tx_done, e aqui nunca é
static bool opUninstall(void *) { return rmt_driver_uninstall(s_channel) == ESP_OK; }

// ---------- API ----------

bool ws2812Begin(int pin, int rmtChannel, uint16_t count, bool grb) {
    if (s_driver.state() != Ws2812State::Off) return s_driver.ready();
    static uint8_t *buf = nullptr;   // reservado uma vez; nunca liberado (o driver pode precisar dele até o fim)
    if (buf == nullptr) buf = (uint8_t *)malloc((size_t)count * 3);
    if (buf == nullptr) return false;
    s_pin = pin;
    s_channel = (rmt_channel_t)rmtChannel;
    const Ws2812Ops ops = {nullptr, opInstall, opWrite, opWaitDone, opStop, opUninstall};
    return s_driver.begin(ops, buf, count, grb);
}

bool ws2812Show(const LedRgb *colors, uint16_t count, uint32_t timeoutMs) {
    return s_driver.show(colors, count, timeoutMs);
}

bool ws2812Ready() { return s_driver.ready(); }
Ws2812State ws2812State() { return s_driver.state(); }
uint32_t ws2812Failures() { return s_driver.failures(); }
uint32_t ws2812Recoveries() { return s_driver.recoveries(); }
uint32_t ws2812RecoveryFailures() { return s_driver.recoveryFailures(); }
bool ws2812Retry() { return s_driver.retry(); }
void ws2812InjectInstallFailures(uint8_t n) { s_injectInstallFailures = n; }
