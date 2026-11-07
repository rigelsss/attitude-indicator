// Diagnóstico do hardware do ESP B (ambiente ground_diag; passo 0 da semana 4, docs/solo.md). Não faz parte
// do firmware: sem tasks nem rádio, cada peça testada sozinha pela serial.
//   1  liga/desliga o LED do link (PIN_LED_LINK, o azul da placa)
//   2  liga/desliga o LED de alarme (PIN_LED_ALARM, vermelho externo)
//   i  scan dos dois I2C (Wire: PIN_I2C_SDA/SCL; Wire1: PIN_I2C1_SDA/SCL) e conferência dos OLEDs (ADDR_OLED_DATA)
//   o  os dois OLEDs: moldura, "OLED 1" ou "OLED 2" e um xadrez; mede o tempo de um quadro a DIAG_OLED_FREQ_HZ
//   e  liga/desliga o fluxo do encoder: passos de quadratura (com sinal; 2 por clique no KY-040 da bancada),
//      bordas e transições inválidas (repiques), e os
//      eventos do botão (curto/longo), sempre impressos
//   t  TFT: cores cheias (vermelho, verde, azul, branco, preto) com o tempo de cada uma, depois faixas
//      rotuladas e um "F" no canto superior esquerdo (cores trocadas, inversão e espelhamento)
//   v  inverte as cores do TFT (invertDisplay), para achar a configuração certa sem recompilar
//   s  sprite de 8 bits 240x240: cria, desenha um horizonte de teste, envia DIAG_SPRITE_FRAMES quadros e
//      mede o tempo; heap antes, com o sprite e depois
//   h  heap livre e maior bloco
//   w  fita de LEDs (docs/caixa-b.md, "Firmware"), um passo a cada toque:
//        1. o LED 0 de cada anel em verde e o 1 em azul (onde entra o DIN e o sentido);
//        2. o primeiro LED de cada lado, na ordem da fita: vermelho, verde, azul e branco (confere os LEDs por
//           lado, o canto e o sentido do config.h; a serial diz onde cada um deve estar);
//        3 a 6. vermelho, verde, azul e branco cheios, com o teto de corrente (cores e corrente);
//        7. apagada
//   p  liga/desliga a leitura do potenciômetro do brilho (ADC, média e brilho resultante)
//   r  teste da recuperação do driver da fita: envia o quadro atual com prazo zero (vence com a transmissão em
//      curso, e o driver para, desinstala e reinstala o RMT); depois envia de novo com o prazo normal, que deve
//      funcionar. Sem a fita ligada, o RMT transmite do mesmo jeito: o teste vale também sem ela
//   f  teste da recuperação malsucedida: o mesmo prazo vencido, com a reinstalação falhando de propósito (duas
//      vezes); mostra os envios voltando na hora com o driver fora, uma nova tentativa que falha e outra que funciona
#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <soc/gpio_reg.h>
#include "config.h"
#include "led_ring.h"
#include "ws2812_rmt.h"

extern const char *const BUILD_STAMP;   // build_info.cpp

static TFT_eSPI tft;
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled1(U8G2_R0, U8X8_PIN_NONE, PIN_I2C_SCL, PIN_I2C_SDA);
static U8G2_SSD1306_128X64_NONAME_F_2ND_HW_I2C oled2(U8G2_R0, U8X8_PIN_NONE);   // Wire1 (WIRE_INTERFACES_COUNT=2)
static bool s_tftReady = false, s_tftInverted = false;
static bool s_ledLink = false, s_ledAlarm = false;

// ---------- Encoder: ISR de quadratura (só no diagnóstico usa attachInterrupt) ----------
// Índice (anterior << 2) | atual, com cada estado = (CLK << 1) | DT. ±1 nas transições válidas; 0 nas que
// pulam um estado (repique ou leitura no meio da borda), contadas à parte
static const int8_t QDEC[16] = {0, -1, +1, 0, +1, 0, 0, -1, -1, 0, 0, +1, 0, +1, -1, 0};
static volatile int32_t s_encSteps = 0;
static volatile uint32_t s_encEdges = 0, s_encInvalid = 0;
static volatile uint8_t s_encPrev = 0;

static inline uint8_t IRAM_ATTR encState() {
    // GPIO 32 e 33 ficam no registrador GPIO_IN1 (bits 0 e 1): leitura direta, segura em IRAM
    const uint32_t in1 = REG_READ(GPIO_IN1_REG);
    const uint8_t clk = (in1 >> (PIN_ENC_CLK - 32)) & 1;
    const uint8_t dt = (in1 >> (PIN_ENC_DT - 32)) & 1;
    return (uint8_t)((clk << 1) | dt);
}

static void IRAM_ATTR onEncoder() {
    const uint8_t cur = encState();
    if (cur == s_encPrev) return;
    s_encEdges++;
    const int8_t d = QDEC[(s_encPrev << 2) | cur];
    if (d == 0) s_encInvalid++;
    else s_encSteps += d;
    s_encPrev = cur;
}

static_assert(PIN_ENC_CLK >= 32 && PIN_ENC_DT >= 32, "a leitura do encoder usa o GPIO_IN1 (pinos 32 a 39)");

// ---------- Botão do encoder: debounce e pressão curta/longa, por consulta ----------
static bool s_btnStable = false, s_btnLastRaw = false, s_btnLongReported = false;
static uint32_t s_btnChangeMs = 0, s_btnDownMs = 0;

static void pollButton() {
    const bool raw = digitalRead(PIN_ENC_SW) == LOW;   // pull-up interno: pressionado = LOW
    const uint32_t now = millis();
    if (raw != s_btnLastRaw) {
        s_btnLastRaw = raw;
        s_btnChangeMs = now;
    }
    if (raw != s_btnStable && now - s_btnChangeMs >= DIAG_BTN_DEBOUNCE_MS) {
        s_btnStable = raw;
        if (raw) {
            s_btnDownMs = now;
            s_btnLongReported = false;
        } else if (!s_btnLongReported) {
            Serial.printf("[enc] botão: CURTO (%lu ms)\n", (unsigned long)(now - s_btnDownMs));
        } else {
            Serial.printf("[enc] botão solto (%lu ms)\n", (unsigned long)(now - s_btnDownMs));
        }
    }
    if (s_btnStable && !s_btnLongReported && now - s_btnDownMs >= DIAG_BTN_LONG_MS) {
        s_btnLongReported = true;
        Serial.println("[enc] botão: LONGO");
    }
}

// ---------- Testes ----------

static void printHeap(const char *when) {
    Serial.printf("[heap] %s: livre %u B, maior bloco %u B\n", when,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static void toggleLed(int pin, bool &state, const char *name) {
    state = !state;
    digitalWrite(pin, state ? HIGH : LOW);
    Serial.printf("[led] %s (GPIO %d): %s\n", name, pin, state ? "ACESO" : "apagado");
}

// Os dois barramentos dos OLEDs (docs/caixa-b.md): OLED 1 no Wire, OLED 2 no Wire1
struct DiagBus {
    TwoWire    &wire;
    U8G2       &oled;
    int         sda, scl;
    const char *name;
    bool        started, oledReady;
};

static DiagBus s_buses[2] = {
    {Wire, oled1, PIN_I2C_SDA, PIN_I2C_SCL, "OLED 1 (Wire)", false, false},
    {Wire1, oled2, PIN_I2C1_SDA, PIN_I2C1_SCL, "OLED 2 (Wire1)", false, false},
};

// Inicia cada barramento só uma vez: um segundo begin() não muda a frequência e imprime "Bus already started".
// A frequência é acertada a cada uso com setClock (o OLED deixa o barramento a DIAG_OLED_FREQ_HZ)
static void wireStart(DiagBus &b, uint32_t freqHz) {
    if (!b.started) b.started = b.wire.begin(b.sda, b.scl, freqHz);
    b.wire.setClock(freqHz);
}

static void scanI2C() {
    for (DiagBus &b : s_buses) {
        wireStart(b, DIAG_I2C_FREQ_HZ);
        Serial.printf("[i2c] %s: scan em SDA %d / SCL %d a %lu Hz:\n", b.name, b.sda, b.scl,
                      (unsigned long)DIAG_I2C_FREQ_HZ);
        int found = 0;
        bool oled = false;
        for (uint8_t a = 0x08; a <= 0x77; a++) {
            b.wire.beginTransmission(a);
            if (b.wire.endTransmission() == 0) {
                Serial.printf("[i2c]   0x%02X%s\n", a, a == ADDR_OLED_DATA ? " (OLED esperado)" : "");
                found++;
                if (a == ADDR_OLED_DATA) oled = true;
            }
        }
        Serial.printf("[i2c] %s: %d dispositivo(s); OLED 0x%02X: %s\n", b.name, found, ADDR_OLED_DATA,
                      oled ? "OK" : "AUSENTE");
    }
}

static void testOled(DiagBus &b, int number) {
    // O begin() da U8g2 sempre devolve sucesso, sem conferir o ACK: a presença é conferida antes, a cada teste
    wireStart(b, DIAG_I2C_FREQ_HZ);
    b.wire.beginTransmission(ADDR_OLED_DATA);
    if (b.wire.endTransmission() != 0) {
        Serial.printf("[oled] %s: ERRO: sem resposta em 0x%02X (fiação ou endereço; ver o i)\n", b.name,
                      ADDR_OLED_DATA);
        b.oledReady = false;   // na volta, inicia de novo
        return;
    }
    if (!b.oledReady) {
        b.oled.setI2CAddress(ADDR_OLED_DATA << 1);   // o U8g2 usa o endereço de 8 bits
        b.oled.setBusClock(DIAG_OLED_FREQ_HZ);
        // O begin() da U8g2 inicia o barramento: ele é liberado antes, para não haver o aviso "Bus already
        // started". O Wire1 é iniciado pela U8g2 sem pinos: eles vão antes, por setPins
        b.wire.end();
        b.wire.setPins(b.sda, b.scl);
        b.oled.begin();
        b.started = true;
        b.oledReady = true;
    }
    char title[24];
    snprintf(title, sizeof(title), "ESP B - OLED %d", number);
    b.oled.clearBuffer();
    b.oled.drawFrame(0, 0, 128, 64);
    b.oled.setFont(u8g2_font_6x10_tf);
    b.oled.drawStr(4, 12, title);
    b.oled.drawStr(4, 24, "128x64 SSD1306");
    for (int y = 34; y < 60; y += 4)
        for (int x = 4 + ((y / 4) % 2) * 4; x < 124; x += 8) b.oled.drawBox(x, y, 4, 4);
    const uint32_t t0 = micros();
    b.oled.sendBuffer();
    const uint32_t dt = micros() - t0;
    Serial.printf("[oled] %s: desenhado: moldura, \"%s\" e um xadrez. Quadro em %lu us a %lu Hz\n", b.name, title,
                  (unsigned long)dt, (unsigned long)DIAG_OLED_FREQ_HZ);
}

static void testOleds() {
    for (int i = 0; i < 2; i++) testOled(s_buses[i], i + 1);
    Serial.println("[oled] confira em cada um: moldura inteira nas bordas (sem faixa de lixo nas laterais), texto "
                   "legível e o número certo (OLED 1 à esquerda, OLED 2 à direita)");
}

static void ensureTft() {
    if (s_tftReady) return;
    tft.init();
    tft.setRotation(0);
    // Estado efetivo da inversão depois do init(): o ST7789_Init.h da TFT_eSPI manda INVON, e o init() só o
    // troca com TFT_INVERSION_ON/OFF. Sem isso, o primeiro v mandaria INVON de novo e não mudaria nada
#if defined(TFT_INVERSION_OFF)
    s_tftInverted = false;
#else
    s_tftInverted = true;
#endif
    s_tftReady = true;
    Serial.printf("[tft] iniciado: %dx%d, SPI a %lu Hz\n", tft.width(), tft.height(), (unsigned long)SPI_FREQUENCY);
}

static void testTft() {
    ensureTft();
    struct { uint16_t c; const char *name; } colors[] = {
        {TFT_RED, "VERMELHO"}, {TFT_GREEN, "VERDE"}, {TFT_BLUE, "AZUL"}, {TFT_WHITE, "BRANCO"}, {TFT_BLACK, "PRETO"}};
    for (auto &col : colors) {
        const uint32_t t0 = micros();
        tft.fillScreen(col.c);
        const uint32_t dt = micros() - t0;
        Serial.printf("[tft] tela %s em %lu us\n", col.name, (unsigned long)dt);
        delay(DIAG_TFT_COLOR_MS);
    }
    // Faixas rotuladas: confere se cada cor aparece com o nome certo
    tft.fillScreen(TFT_BLACK);
    tft.fillRect(0, 60, 80, 120, TFT_RED);
    tft.fillRect(80, 60, 80, 120, TFT_GREEN);
    tft.fillRect(160, 60, 80, 120, TFT_BLUE);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextFont(2);
    tft.drawString("VERM", 20, 190);
    tft.drawString("VERDE", 98, 190);
    tft.drawString("AZUL", 182, 190);
    // "F" no canto superior esquerdo: aparece normal se a tela não estiver espelhada nem girada
    tft.setTextFont(4);
    tft.drawString("F", 4, 4);
    tft.setTextFont(2);
    tft.drawString("ESP B - TFT", 70, 10);
    tft.drawRect(0, 0, 240, 240, TFT_YELLOW);
    Serial.println("[tft] faixas: VERMELHO, VERDE, AZUL da esquerda para a direita; fundo PRETO; moldura amarela");
    Serial.println("[tft] confira: cores com o nome certo, fundo preto (se branco: v), \"F\" normal no canto "
                   "superior esquerdo e a moldura inteira");
}

static void toggleInversion() {
    ensureTft();
    s_tftInverted = !s_tftInverted;
    tft.invertDisplay(s_tftInverted);
    Serial.printf("[tft] invertDisplay(%s)%s\n", s_tftInverted ? "true" : "false",
                  s_tftInverted ? " (o padrão do ST7789 da TFT_eSPI)" : " (contrário ao padrão: se corrigir as cores, "
                                                                         "definir TFT_INVERSION_OFF)");
}

static void testSprite() {
    ensureTft();
    printHeap("antes do sprite");
    TFT_eSprite spr(&tft);
    spr.setColorDepth(8);
    if (spr.createSprite(240, 240) == nullptr) {
        Serial.println("[sprite] ERRO: sem memória para o sprite de 8 bits 240x240");
        return;
    }
    printHeap("com o sprite");
    const uint16_t sky = tft.color565(0, 96, 192), ground = tft.color565(128, 64, 0);
    uint32_t drawUs = 0, pushUs = 0;
    for (int f = 0; f < DIAG_SPRITE_FRAMES; f++) {
        const uint32_t t0 = micros();
        // Horizonte de teste: linha inclinada que gira um pouco a cada quadro
        const int dy = (f - DIAG_SPRITE_FRAMES / 2) * 4;
        spr.fillSprite(sky);
        spr.fillTriangle(0, 120 + dy, 239, 120 - dy, 239, 239, ground);
        spr.fillTriangle(0, 120 + dy, 0, 239, 239, 239, ground);
        spr.drawLine(0, 120 + dy, 239, 120 - dy, TFT_WHITE);
        spr.setTextColor(TFT_WHITE);
        spr.drawString("SPRITE 8 BITS", 70, 10, 2);
        const uint32_t t1 = micros();
        spr.pushSprite(0, 0);
        const uint32_t t2 = micros();
        drawUs += t1 - t0;
        pushUs += t2 - t1;
    }
    Serial.printf("[sprite] %d quadros: desenho %lu us, envio %lu us por quadro (média) -> ~%.1f quadros/s\n",
                  DIAG_SPRITE_FRAMES, (unsigned long)(drawUs / DIAG_SPRITE_FRAMES),
                  (unsigned long)(pushUs / DIAG_SPRITE_FRAMES), 1e6f * DIAG_SPRITE_FRAMES / (drawUs + pushUs));
    spr.deleteSprite();
    printHeap("depois do sprite");
}

// ---------- Fita de LEDs e potenciômetro ----------

static LedRgb s_leds[LED_COUNT];
static int s_ledStep = 0;
static const LedRingGeom TOP_GEOM = {LED_TOP_DEPTH_MM, LED_TOP_WIDTH_MM, LED_TOP_N_NOSE, LED_TOP_N_RIGHT,
                                     LED_TOP_N_TAIL,   LED_TOP_N_LEFT,   LED_TOP_START,  LED_TOP_CLOCKWISE};
static const LedRingGeom SIDE_GEOM = {LED_SIDE_DEPTH_MM, LED_SIDE_WIDTH_MM, LED_SIDE_N_NOSE, LED_SIDE_N_RIGHT,
                                      LED_SIDE_N_TAIL,   LED_SIDE_N_LEFT,   LED_SIDE_START,  LED_SIDE_CLOCKWISE};

static void stripShow(float brightness) {
    const LedPowerConfig power = {LED_MA_PER_CHANNEL, LED_MA_IDLE, LED_MA_MAX};
    const LedPowerResult r = ledApplyBrightness(s_leds, LED_COUNT, brightness, power);
    const uint32_t t0 = micros();
    const bool ok = ws2812Show(s_leds, LED_COUNT);
    Serial.printf("[fita] corrente estimada %.0f mA%s | envio %lu us%s\n", r.estimatedMa,
                  r.capped ? " (limitada pelo teto)" : "", (unsigned long)(micros() - t0),
                  ok ? "" : " | ERRO: driver do RMT não iniciado ou envio fora do prazo");
}

static void fill(LedRgb c) {
    for (uint16_t i = 0; i < LED_COUNT; i++) s_leds[i] = c;
}

static void describeRing(const char *name, const LedRingGeom &g, uint16_t first) {
    static const char *const COLOR[4] = {"vermelho", "verde", "azul", "branco"};
    static const LedRgb RGB[4] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
    uint8_t sides[4], counts[4];
    ledRingSides(g, sides, counts);
    uint16_t k = first;
    for (int j = 0; j < 4; j++) {
        Serial.printf("[fita]   %s, LED %u (%s): primeiro da %s, %u LEDs nesse lado\n", name, (unsigned)k, COLOR[j],
                      ledSideText(sides[j]), (unsigned)counts[j]);
        s_leds[k] = RGB[j];
        k = (uint16_t)(k + counts[j]);
    }
}

static void stripStep() {
    s_ledStep = s_ledStep % 7 + 1;
    fill({0, 0, 0});
    switch (s_ledStep) {
    case 1:
        s_leds[0] = {0, 255, 0};
        s_leds[1] = {0, 0, 255};
        s_leds[LED_TOP_COUNT] = {0, 255, 0};
        s_leds[LED_TOP_COUNT + 1] = {0, 0, 255};
        Serial.printf("[fita] passo 1/7: LED 0 verde e LED 1 azul em cada anel (cima: 0 e 1; lateral: %d e %d). O verde "
                      "é onde entra o DIN; o azul mostra o sentido. Esperado pelo config.h: cima em %s, %s; lateral em %s, %s\n",
                      LED_TOP_COUNT, LED_TOP_COUNT + 1, ledCornerText(LED_TOP_START), LED_TOP_CLOCKWISE ? "horário" : "anti-horário",
                      ledCornerText(LED_SIDE_START), LED_SIDE_CLOCKWISE ? "horário" : "anti-horário");
        stripShow(DIAG_LED_BRIGHT);
        break;
    case 2:
        Serial.println("[fita] passo 2/7: o primeiro LED de cada lado, na ordem da fita (pelo config.h):");
        describeRing("cima", TOP_GEOM, 0);
        describeRing("lateral", SIDE_GEOM, LED_TOP_COUNT);
        stripShow(DIAG_LED_BRIGHT);
        break;
    case 3: Serial.println("[fita] passo 3/7: vermelho cheio"); fill({255, 0, 0}); stripShow(1.0f); break;
    case 4: Serial.println("[fita] passo 4/7: verde cheio"); fill({0, 255, 0}); stripShow(1.0f); break;
    case 5: Serial.println("[fita] passo 5/7: azul cheio"); fill({0, 0, 255}); stripShow(1.0f); break;
    case 6:
        Serial.printf("[fita] passo 6/7: branco cheio, com o teto de %.0f mA (medir a corrente aqui)\n", LED_MA_MAX);
        fill({255, 255, 255});
        stripShow(1.0f);
        break;
    default: Serial.println("[fita] passo 7/7: apagada"); stripShow(1.0f); break;
    }
}

// O quadro atual (s_leds, já com o brilho do último passo) com prazo zero: a transmissão de ~2 ms ainda está em
// curso na conferência, o prazo vence e o driver recupera o canal. Em seguida, o mesmo quadro com o prazo normal
static void printDriver(const char *tag) {
    Serial.printf("[fita] %s: driver %s | falhas %lu | recuperações %lu (sem sucesso %lu)\n", tag,
                  ws2812StateText(ws2812State()), (unsigned long)ws2812Failures(), (unsigned long)ws2812Recoveries(),
                  (unsigned long)ws2812RecoveryFailures());
}

// Envio com prazo zero: a transmissão de ~2 ms ainda está em curso na conferência, e o prazo vence
static bool forcedTimeout(const char *tag) {
    const uint32_t t0 = micros();
    const bool ok = ws2812Show(s_leds, LED_COUNT, 0);
    Serial.printf("[fita] %s: envio com prazo zero %s em %lu us\n", tag,
                  ok ? "terminou (inesperado: o prazo não venceu)" : "venceu, como esperado", (unsigned long)(micros() - t0));
    return !ok;
}

static void normalSend(const char *tag) {
    const uint32_t t0 = micros();
    const bool ok = ws2812Show(s_leds, LED_COUNT);
    Serial.printf("[fita] %s: envio com prazo de %lu ms %s em %lu us\n", tag, (unsigned long)WS2812_TIMEOUT_MS,
                  ok ? "OK" : "voltou false", (unsigned long)(micros() - t0));
}

// Tecla r: prazo vencido com recuperação bem-sucedida. Esperado: falhas +1, recuperações +1, driver pronto e o
// envio seguinte OK (a fita continua mostrando o quadro)
static void testStripRecovery() {
    if (!ws2812Ready()) {
        printDriver("recuperação: driver não está pronto; nada a testar");
        return;
    }
    printDriver("recuperação, antes");
    forcedTimeout("recuperação");
    printDriver("recuperação, depois");
    normalSend("recuperação, quadro seguinte");
}

// Tecla f: prazo vencido com a reinstalação falhando de propósito. Esperado: driver "reinstalação falhou", os
// envios seguintes voltando false na hora (poucos us, sem transmitir nem tocar no buffer), uma nova tentativa que
// falha (a falha simulada vale para 2 instalações) e outra que funciona, com o envio seguinte OK
static void testStripFailedRecovery() {
    if (!ws2812Ready()) {
        printDriver("recuperação malsucedida: driver não está pronto; nada a testar");
        return;
    }
    ws2812InjectInstallFailures(2);
    forcedTimeout("recuperação malsucedida");
    printDriver("recuperação malsucedida, depois");
    normalSend("recuperação malsucedida, envio com o driver fora");
    normalSend("recuperação malsucedida, outro envio");
    Serial.printf("[fita] recuperação malsucedida: nova tentativa (ainda com falha simulada) %s\n",
                  ws2812Retry() ? "funcionou (inesperado)" : "falhou, como esperado");
    Serial.printf("[fita] recuperação malsucedida: nova tentativa (sem falha simulada) %s\n",
                  ws2812Retry() ? "funcionou" : "FALHOU");
    printDriver("recuperação malsucedida, fim");
    normalSend("recuperação malsucedida, quadro seguinte");
    ws2812InjectInstallFailures(0);
}

static bool s_potStream = false;

static void pollPot() {
    static LedPot pot({4095, LED_POT_HYST, LED_POT_GAMMA, LED_BRIGHT_MIN});
    static uint32_t lastPrint = 0;
    const uint16_t raw = (uint16_t)analogRead(PIN_LED_POT);
    pot.update(raw);
    if (s_potStream && millis() - lastPrint >= DIAG_POT_PRINT_MS) {
        Serial.printf("[pot] ADC %u | média %u | brilho %.0f %%\n", (unsigned)raw, (unsigned)pot.value(),
                      pot.brightness() * 100.0f);
        lastPrint = millis();
    }
}

static void printHelp() {
    Serial.println("[diag] teclas: 1 = LED do link | 2 = LED de alarme | i = scan I2C (os dois) | o = os dois OLEDs | e = encoder | "
                   "t = TFT | v = inverter cores do TFT | s = sprite 8 bits | h = heap | w = fita de LEDs (passo a passo) | "
                   "p = potenciômetro | r = recuperação do driver da fita | f = recuperação malsucedida");
}

// ---------- Setup e laço ----------

static bool s_encStream = false;

void setup() {
    Serial.begin(115200);
    delay(500);   // tempo para o monitor reabrir depois do reset
    Serial.printf("\n[diag] ESP B — diagnóstico do hardware — compilado em %s\n", BUILD_STAMP);
    Serial.printf("[diag] LEDs: link GPIO %d, alarme GPIO %d | I2C: OLED 1 SDA %d, SCL %d; OLED 2 SDA %d, SCL %d | "
                  "encoder: CLK %d, DT %d, SW %d\n",
                  PIN_LED_LINK, PIN_LED_ALARM, PIN_I2C_SDA, PIN_I2C_SCL, PIN_I2C1_SDA, PIN_I2C1_SCL, PIN_ENC_CLK,
                  PIN_ENC_DT, PIN_ENC_SW);
    Serial.printf("[diag] TFT: MOSI %d, SCLK %d, CS %d, DC %d, RST %d\n", TFT_MOSI, TFT_SCLK, TFT_CS, TFT_DC, TFT_RST);
    pinMode(PIN_LED_LINK, OUTPUT);
    pinMode(PIN_LED_ALARM, OUTPUT);
    digitalWrite(PIN_LED_LINK, LOW);
    digitalWrite(PIN_LED_ALARM, LOW);
    // O KY-040 tem pull-ups no CLK e no DT (alimentado em 3V3); o botão usa o pull-up interno
    pinMode(PIN_ENC_CLK, INPUT);
    pinMode(PIN_ENC_DT, INPUT);
    pinMode(PIN_ENC_SW, INPUT_PULLUP);
    s_encPrev = encState();
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_CLK), onEncoder, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_DT), onEncoder, CHANGE);
    Serial.printf("[diag] encoder: estado inicial CLK %d, DT %d (parado num clique: 1 e 1 ou 0 e 0; o KY-040 da "
                  "bancada descansa nos dois, com 2 passos por clique)\n",
                  (s_encPrev >> 1) & 1, s_encPrev & 1);
    Serial.printf("[diag] fita: DIN GPIO %d, %d LEDs (cima %d, lateral %d) | potenciômetro: GPIO %d\n", PIN_LED_STRIP,
                  LED_COUNT, LED_TOP_COUNT, LED_SIDE_COUNT, PIN_LED_POT);
    if (ws2812Begin(PIN_LED_STRIP, LED_RMT_CHANNEL, LED_COUNT, LED_COLOR_GRB)) {
        fill({0, 0, 0});
        ws2812Show(s_leds, LED_COUNT);
    } else {
        Serial.println("[diag] ERRO: driver do RMT da fita não iniciado");
    }
    analogReadResolution(12);
    analogSetPinAttenuation(PIN_LED_POT, ADC_11db);
    printHeap("boot");
    printHelp();
}

void loop() {
    while (Serial.available()) {
        const char k = (char)Serial.read();
        switch (k) {
        case '1': toggleLed(PIN_LED_LINK, s_ledLink, "LED do link"); break;
        case '2': toggleLed(PIN_LED_ALARM, s_ledAlarm, "LED de alarme"); break;
        case 'i': scanI2C(); break;
        case 'o': testOleds(); break;
        case 'e':
            s_encStream = !s_encStream;
            Serial.printf("[enc] fluxo %s\n", s_encStream ? "ligado (gire devagar, um clique por vez)" : "desligado");
            break;
        case 't': testTft(); break;
        case 'v': toggleInversion(); break;
        case 's': testSprite(); break;
        case 'h': printHeap("agora"); break;
        case 'w': stripStep(); break;
        case 'r': testStripRecovery(); break;
        case 'f': testStripFailedRecovery(); break;
        case 'p':
            s_potStream = !s_potStream;
            Serial.printf("[pot] leitura %s\n", s_potStream ? "ligada (gire de ponta a ponta)" : "desligada");
            break;
        case '\r':
        case '\n':
        case ' ': break;
        default: printHelp(); break;
        }
    }
    pollButton();   // eventos do botão sempre impressos
    pollPot();
    static uint32_t lastPrint = 0;
    static int32_t lastSteps = 0;
    static uint32_t lastInvalid = 0;
    if (s_encStream && millis() - lastPrint >= DIAG_ENC_PRINT_MS) {
        noInterrupts();
        const int32_t steps = s_encSteps;
        const uint32_t edges = s_encEdges, invalid = s_encInvalid;
        interrupts();
        if (steps != lastSteps || invalid != lastInvalid) {
            Serial.printf("[enc] passos %+ld (variação %+ld) | bordas %lu | inválidas %lu | estado CLK %d DT %d\n",
                          (long)steps, (long)(steps - lastSteps), (unsigned long)edges, (unsigned long)invalid,
                          (encState() >> 1) & 1, encState() & 1);
            lastSteps = steps;
            lastInvalid = invalid;
        }
        lastPrint = millis();
    }
    delay(1);
}
