// display_task.cpp — ver display_task.h
#include "display_task.h"
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <math.h>
#include <esp_timer.h>
#include "alarm_task.h"
#include "config.h"
#include "link_task.h"
#include "oled_view.h"   // oledHeading: a mesma proa exibida no OLED (MAG ou VERD)
#include "pfd_geometry.h"
#include "pfd_state.h"

static TFT_eSPI s_tft;
static TFT_eSprite s_spr(&s_tft);
static bool s_spriteOk = false;
static QueueHandle_t s_status = nullptr;

static const PfdConfig PFD = {TFT_WIDTH / 2.0f, TFT_HEIGHT / 2.0f, (float)TFT_WIDTH, (float)TFT_HEIGHT,
                              PFD_PX_PER_DEG};

// Cores escolhidas no RGB332 (r 3 bits, g 3 bits, b 2 bits) e convertidas para o 565 que o sprite recebe: assim
// cada cor é representada exatamente, sem degraus (docs/solo.md, sprite de 8 bits)
static uint16_t c332(uint8_t r3, uint8_t g3, uint8_t b2) {
    return s_tft.color8to16((uint8_t)((r3 << 5) | (g3 << 2) | b2));
}
static uint16_t SKY, GROUND, WHITE, BLACK, YELLOW, RED, AMBER, GREEN;

static void initColors() {
    SKY = c332(0, 3, 3);      // azul
    GROUND = c332(4, 2, 0);   // marrom
    WHITE = c332(7, 7, 3);
    BLACK = c332(0, 0, 0);
    YELLOW = c332(7, 7, 0);
    RED = c332(7, 0, 0);
    AMBER = c332(7, 5, 0);
    GREEN = c332(0, 6, 0);
}

static int ri(float v) { return (int)lroundf(v); }

// Linha suavizada (antialiasing do drawWideLine, misturando com o fundo lido do sprite), recortada pela tela.
// Sem suavização, uma linha quase horizontal vira degraus de 1 px: com a caixa a −1,4° de roll na mesa, os traços
// de 60 px da escala de pitch apareciam "quebrados" no meio (bancada de 23/10/2026). No RGB332 há poucos tons
// intermediários, então a suavização é discreta, mas o degrau seco some
static void line(Vec2 a, Vec2 b, float widthPx, uint16_t color) {
    if (pfdClipSegment(PFD, a, b)) s_spr.drawWideLine(a.x, a.y, b.x, b.y, widthPx, color);
}

// Texto com fundo (legível sobre o céu ou a terra)
static void label(const char *text, int x, int y, uint8_t font, uint16_t fg, uint16_t bg) {
    s_spr.setTextColor(fg, bg);
    s_spr.drawString(text, x, y, font);
}

// ---------- Partes do quadro ----------

static void drawSkyGround(const Horizon &h) {
    s_spr.fillSprite(SKY);
    Vec2 g[6];
    const int n = pfdGroundPolygon(PFD, h, g);
    for (int i = 1; i + 1 < n; i++)   // leque de triângulos (o polígono é convexo)
        s_spr.fillTriangle(ri(g[0].x), ri(g[0].y), ri(g[i].x), ri(g[i].y), ri(g[i + 1].x), ri(g[i + 1].y), GROUND);
    // Linha do horizonte: uma faixa cheia de 3 px (dois triângulos), com as bordas paralelas. Antes eram duas
    // linhas de 1 px deslocadas, cujos degraus não coincidiam. Sem suavização: o drawWideLine numa linha que
    // atravessa a tela levou o quadro a 40–42 ms com a caixa inclinada (período de 40 ms; bancada de 23/10/2026)
    const float L = 400.0f, w = 1.5f;
    const Vec2 a = {h.point.x - h.dir.x * L, h.point.y - h.dir.y * L};
    const Vec2 b = {h.point.x + h.dir.x * L, h.point.y + h.dir.y * L};
    const Vec2 up = {-h.down.x * w, -h.down.y * w}, dn = {h.down.x * w, h.down.y * w};
    const int x0 = ri(a.x + up.x), y0 = ri(a.y + up.y), x1 = ri(b.x + up.x), y1 = ri(b.y + up.y);
    const int x2 = ri(b.x + dn.x), y2 = ri(b.y + dn.y), x3 = ri(a.x + dn.x), y3 = ri(a.y + dn.y);
    s_spr.fillTriangle(x0, y0, x1, y1, x2, y2, WHITE);
    s_spr.fillTriangle(x0, y0, x2, y2, x3, y3, WHITE);
}

static void drawPitchLadder(const Horizon &h, float pitchDeg) {
    s_spr.setTextDatum(MC_DATUM);
    const int lo = (int)ceilf((pitchDeg - PFD_LADDER_SPAN_DEG) / 5.0f) * 5;
    for (int mark = lo; mark <= pitchDeg + PFD_LADDER_SPAN_DEG; mark += 5) {
        if (mark == 0 || mark < -90 || mark > 90) continue;
        const bool major = mark % 10 == 0;
        const float half = major ? 30.0f : 12.0f;
        Vec2 c, a, b;
        pfdPitchMark(PFD, h, (float)mark, half, c, a, b);
        line(a, b, 2.0f, WHITE);
        if (!major) continue;
        char num[4];
        snprintf(num, sizeof(num), "%d", abs(mark));
        for (int side = -1; side <= 1; side += 2) {
            const float off = half + 12.0f;
            const int x = ri(c.x + h.dir.x * off * side), y = ri(c.y + h.dir.y * off * side);
            if (x < 8 || x > TFT_WIDTH - 8 || y < 8 || y > TFT_HEIGHT - 8) continue;
            s_spr.setTextColor(WHITE);   // sem fundo: o número fica sobre o céu ou a terra
            s_spr.drawString(num, x, y, 2);
        }
    }
}

static void drawRollScale(const Horizon &h) {
    const float R = PFD_ROLL_RADIUS_PX;
    const int ticks[] = {-60, -45, -30, -20, -10, 10, 20, 30, 45, 60};
    for (int t : ticks) {
        const float len = (t % 30 == 0) ? 12.0f : 7.0f;   // ±30 e ±60 longos
        line(pfdRollScalePoint(PFD, (float)t, R), pfdRollScalePoint(PFD, (float)t, R + len), 2.0f, WHITE);
    }
    // Índice do zero (fixo, apontando para baixo)
    const Vec2 z = pfdRollScalePoint(PFD, 0, R);
    s_spr.fillTriangle(ri(z.x), ri(z.y), ri(z.x) - 7, ri(z.y) - 11, ri(z.x) + 7, ri(z.y) - 11, WHITE);
    // Ponteiro (do lado do céu, gira com o horizonte), apontando para a escala
    const Vec2 tip = pfdRollPointer(PFD, h, R - 2);
    const Vec2 base = pfdRollPointer(PFD, h, R - 14);
    s_spr.fillTriangle(ri(tip.x), ri(tip.y), ri(base.x - h.dir.x * 7), ri(base.y - h.dir.y * 7),
                       ri(base.x + h.dir.x * 7), ri(base.y + h.dir.y * 7), YELLOW);
}

static void drawAircraft() {
    const int cx = ri(PFD.cx), cy = ri(PFD.cy);
    // Contorno preto e asas amarelas, fixos no centro
    s_spr.fillRect(cx - 71, cy - 3, 47, 7, BLACK);
    s_spr.fillRect(cx + 24, cy - 3, 47, 7, BLACK);
    s_spr.fillRect(cx - 30, cy - 3, 7, 14, BLACK);
    s_spr.fillRect(cx + 23, cy - 3, 7, 14, BLACK);
    s_spr.fillRect(cx - 4, cy - 4, 9, 9, BLACK);
    s_spr.fillRect(cx - 70, cy - 2, 45, 5, YELLOW);
    s_spr.fillRect(cx + 25, cy - 2, 45, 5, YELLOW);
    s_spr.fillRect(cx - 29, cy - 2, 5, 12, YELLOW);
    s_spr.fillRect(cx + 24, cy - 2, 5, 12, YELLOW);
    s_spr.fillRect(cx - 3, cy - 3, 7, 7, YELLOW);
}

// Proa (embaixo, ao centro), também na bandeira de falha, com "---" (st.headingOk exige a atitude mostrada).
// A altitude fica só no OLED (decisão de 23/10/2026)
static void drawHeading(const TelemetryPacket &p, const PfdState &st) {
    char txt[16];
    s_spr.setTextDatum(MC_DATUM);

    const int hx = ri(PFD.cx), hy = TFT_HEIGHT - 17;
    s_spr.fillRect(hx - 33, hy - 15, 66, 30, BLACK);
    s_spr.drawRect(hx - 33, hy - 15, 66, 30, WHITE);
    if (st.headingOk) {
        const int hdg = (int)lroundf(oledHeading(p.heading, OLED_HEADING_TRUE != 0, MAG_DECLINATION_DEG)) % 360;
        snprintf(txt, sizeof(txt), "%03d%s", hdg, st.gyroOnly ? "~" : "");
    } else {
        snprintf(txt, sizeof(txt), "---");
    }
    label(txt, hx, hy + 1, 4, WHITE, BLACK);
    s_spr.setTextDatum(TL_DATUM);
    label(OLED_HEADING_TRUE ? "VERD" : "MAG", hx + 37, hy - 12, 1, WHITE, BLACK);
}

static void drawLinkDot(bool current) { s_spr.fillCircle(8, 8, 4, current ? GREEN : RED); }

// Bandeira de falha: a atitude some, e um X vermelho com o motivo ocupa a tela
static void drawFlag(const char *reason) {
    s_spr.fillSprite(BLACK);
    for (int k = -1; k <= 1; k++) {
        s_spr.drawLine(0 + k, 0, TFT_WIDTH - 1 + k, TFT_HEIGHT - 1, RED);
        s_spr.drawLine(TFT_WIDTH - 1 + k, 0, 0 + k, TFT_HEIGHT - 1, RED);
    }
    s_spr.setTextDatum(MC_DATUM);
    label(" ATT ", ri(PFD.cx), ri(PFD.cy) - 16, 4, RED, BLACK);
    label(reason, ri(PFD.cx), ri(PFD.cy) + 12, 2, WHITE, BLACK);
}

static void drawFrame() {
    const LinkSnapshot s = linkSnapshot((uint64_t)esp_timer_get_time());
    const TelemetryPacket &p = s.item.pkt;
    // O que mostrar (lib/pfd_state, testado no native): horizonte ou bandeira, degradada, validade da proa
    const PfdState st = pfdState({s.everLinked, s.linkOk, s.dataLate, p.valid, p.roll, p.pitch, p.heading});
    if (st.flag != PfdFlag::None) {
        drawFlag(pfdFlagText(st.flag));
    } else {
        const Horizon h = pfdHorizon(PFD, p.roll, p.pitch);
        drawSkyGround(h);
        drawPitchLadder(h, p.pitch);
        drawRollScale(h);
        drawAircraft();
        s_spr.setTextDatum(MC_DATUM);
        if (st.degraded) label(" ATT DEGRADADA ", ri(PFD.cx), 52, 2, AMBER, BLACK);
        AlarmStatus al;
        if (alarmCurrent(al)) {
            if (al.kind == AlarmKind::AttitudeWarning) label(" ATTITUDE ", ri(PFD.cx), 180, 4, RED, BLACK);
            else if (al.kind == AlarmKind::BankCaution) label(" BANK ANGLE ", ri(PFD.cx), 180, 4, AMBER, BLACK);
        }
    }
    drawHeading(p, st);
    drawLinkDot(s.current());
}

// ---------- Task ----------

static void displayTask(void *) {
    const uint32_t periodUs = 1000000UL / DISPLAY_FPS;
    DisplayStatus st = {};
    st.spriteOk = s_spriteOk;
    uint32_t winFrames = 0, winDrawUs = 0, winPushUs = 0, winMaxUs = 0;
    uint64_t winStartUs = (uint64_t)esp_timer_get_time();
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(1000 / DISPLAY_FPS));
        if (s_spriteOk) {
            const uint64_t t0 = (uint64_t)esp_timer_get_time();
            drawFrame();
            const uint64_t t1 = (uint64_t)esp_timer_get_time();
            s_spr.pushSprite(0, 0);
            const uint64_t t2 = (uint64_t)esp_timer_get_time();
            const uint32_t frameUs = (uint32_t)(t2 - t0);
            winFrames++;
            winDrawUs += (uint32_t)(t1 - t0);
            winPushUs += (uint32_t)(t2 - t1);
            if (frameUs > winMaxUs) winMaxUs = frameUs;
            if (frameUs > periodUs) st.overruns++;
            st.frames++;
        }
        const uint64_t now = (uint64_t)esp_timer_get_time();
        if (now - winStartUs >= 1000000ULL) {
            const float secs = (now - winStartUs) / 1e6f;
            st.fps = winFrames / secs;
            st.drawUs = winFrames ? winDrawUs / winFrames : 0;
            st.pushUs = winFrames ? winPushUs / winFrames : 0;
            st.maxFrameUs = winMaxUs;
            xQueueOverwrite(s_status, &st);
            winFrames = winDrawUs = winPushUs = winMaxUs = 0;
            winStartUs = now;
        }
    }
}

bool displayInit() {
    s_tft.init();
    s_tft.setRotation(0);
    initColors();
    s_tft.fillScreen(TFT_BLACK);
    s_spr.setColorDepth(8);
    const size_t before = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    s_spriteOk = s_spr.createSprite(TFT_WIDTH, TFT_HEIGHT) != nullptr;
    if (!s_spriteOk) {
        s_tft.setTextColor(TFT_RED, TFT_BLACK);
        s_tft.setTextDatum(MC_DATUM);
        s_tft.drawString("SEM MEMORIA", TFT_WIDTH / 2, TFT_HEIGHT / 2 - 10, 4);
        s_tft.drawString("PARA O SPRITE", TFT_WIDTH / 2, TFT_HEIGHT / 2 + 16, 2);
        Serial.printf("[tft] ERRO: sem memória para o sprite de 8 bits %dx%d (maior bloco %u B); TFT só com a "
                      "mensagem de erro\n",
                      TFT_WIDTH, TFT_HEIGHT, (unsigned)before);
        return false;
    }
    s_tft.setTextColor(TFT_WHITE, TFT_BLACK);
    s_tft.setTextDatum(MC_DATUM);
    s_tft.drawString("Horizonte - ESP B", TFT_WIDTH / 2, TFT_HEIGHT / 2, 2);
    Serial.printf("[tft] sprite de 8 bits %dx%d alocado (maior bloco antes: %u B, depois: %u B)\n", TFT_WIDTH,
                  TFT_HEIGHT, (unsigned)before, (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return true;
}

bool displayTaskStart() {
    s_status = xQueueCreate(1, sizeof(DisplayStatus));
    if (s_status == nullptr) return false;
    return xTaskCreatePinnedToCore(displayTask, "display", DISPLAY_TASK_STACK, nullptr, DISPLAY_TASK_PRIO, nullptr,
                                   DISPLAY_TASK_CORE) == pdPASS;
}

bool displayCurrent(DisplayStatus &out) { return s_status != nullptr && xQueuePeek(s_status, &out, 0) == pdTRUE; }
