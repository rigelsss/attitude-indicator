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
#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <soc/gpio_reg.h>
#include "config.h"

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

static void printHelp() {
    Serial.println("[diag] teclas: 1 = LED do link | 2 = LED de alarme | i = scan I2C (os dois) | o = os dois OLEDs | e = encoder | "
                   "t = TFT | v = inverter cores do TFT | s = sprite 8 bits | h = heap");
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
        case '\r':
        case '\n':
        case ' ': break;
        default: printHelp(); break;
        }
    }
    pollButton();   // eventos do botão sempre impressos
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
