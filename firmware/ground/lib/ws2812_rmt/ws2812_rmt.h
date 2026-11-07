// ws2812_rmt.h — driver da fita WS2812 pelo RMT do ESP32 (ESP-IDF 4.4), para o anel de LEDs do ESP B
// (docs/caixa-b.md, "Driver próprio do RMT"). O RMT gera o sinal sozinho; as interrupções do Wi-Fi não estragam a
// temporização.
//
// Por que não a Adafruit NeoPixel: na versão 1.12 com o ESP-IDF 4.4, ela instala e desinstala o driver do RMT a
// cada show(). A 30 quadros/s, seriam 30 alocações e liberações por segundo no heap do B, que já é apertado
// (trava de memória e CORRUPT HEAP, docs/cliente-python.md). Aqui o driver é instalado uma vez, e o buffer dos
// bytes da fita é reservado uma vez, em ws2812Begin().
//
// A sequência de envio e de recuperação está em lib/ws2812_core (testada no native com o RMT simulado); este
// arquivo só liga as funções reais do ESP-IDF, conferindo cada retorno.
// Driver: não conhece rede, display nem a lógica do anel; recebe as cores prontas.
#pragma once
#include <stdint.h>
#include "led_ring.h"
#include "ws2812_core.h"

static const uint32_t WS2812_TIMEOUT_MS = 20;   // um quadro de 65 LEDs leva ~2 ms

// Instala o RMT no canal dado (com 2 blocos de memória: o canal seguinte fica reservado) e reserva o buffer de
// count LEDs. grb: ordem das cores da fita (a WS2812 usa GRB). false sem memória ou com erro do driver
bool ws2812Begin(int pin, int rmtChannel, uint16_t count, bool grb);
// Envia o quadro e espera o fim da transmissão (~30 us por LED) por até timeoutMs. false se o driver não está
// pronto (volta na hora) ou se o prazo venceu; no prazo vencido, o canal é recuperado antes de voltar
// (ws2812_core.h). timeoutMs 0: só confere se já terminou (a tecla r do ground_diag força a recuperação assim)
bool ws2812Show(const LedRgb *colors, uint16_t count, uint32_t timeoutMs = WS2812_TIMEOUT_MS);
bool ws2812Ready();
Ws2812State ws2812State();
uint32_t ws2812Failures();     // envios com prazo vencido ou erro, desde o boot
uint32_t ws2812Recoveries();   // recuperações executadas
uint32_t ws2812RecoveryFailures();
// Nova instalação depois de uma reinstalação que falhou. false nos outros estados ou se falhar de novo
bool ws2812Retry();
// Só para o diagnóstico (tecla f do ground_diag): as próximas n instalações falham de propósito, antes de tocar
// no driver (simula a reinstalação malsucedida)
void ws2812InjectInstallFailures(uint8_t n);
