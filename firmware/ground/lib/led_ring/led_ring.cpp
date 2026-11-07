// led_ring.cpp — ver led_ring.h
#include "led_ring.h"
#include <math.h>

static const float DEG = 3.14159265358979f / 180.0f;

// ---------- Geometria ----------

uint16_t ledRingCount(const LedRingGeom &g) { return (uint16_t)(g.nNose + g.nRight + g.nTail + g.nLeft); }

// Cantos no sentido horário (vistos de cima, nariz para longe): nariz-esquerda, nariz-direita, cauda-direita,
// cauda-esquerda. O lado k vai do canto k ao canto k+1: nariz, direita, cauda, esquerda
static int cwIndex(LedCorner c) {
    switch (c) {
    case LedCorner::NoseLeft:  return 0;
    case LedCorner::NoseRight: return 1;
    case LedCorner::TailRight: return 2;
    case LedCorner::TailLeft:  return 3;
    }
    return 0;
}

void ledRingSides(const LedRingGeom &g, uint8_t sides[4], uint8_t counts[4]) {
    const uint8_t count[4] = {g.nNose, g.nRight, g.nTail, g.nLeft};
    const int s = cwIndex(g.start);
    for (int j = 0; j < 4; j++) {
        const int side = g.clockwise ? ((s + j) & 3) : ((s - j - 1) & 3);
        sides[j] = (uint8_t)side;
        counts[j] = count[side];
    }
}

const char *ledSideText(uint8_t side) {
    switch (side) {
    case 0: return "nariz (borda de longe)";
    case 1: return "lateral direita";
    case 2: return "cauda (borda de perto)";
    case 3: return "lateral esquerda";
    }
    return "?";
}

const char *ledCornerText(LedCorner c) {
    switch (c) {
    case LedCorner::TailLeft:  return "cauda-esquerda (perto, à esquerda)";
    case LedCorner::TailRight: return "cauda-direita (perto, à direita)";
    case LedCorner::NoseRight: return "nariz-direita (longe, à direita)";
    case LedCorner::NoseLeft:  return "nariz-esquerda (longe, à esquerda)";
    }
    return "?";
}

uint16_t ledRingLayout(const LedRingGeom &g, LedPoint *out, uint16_t max) {
    const float hx = g.depthMm / 2.0f, hy = g.widthMm / 2.0f;
    const LedPoint corner[4] = {{hx, -hy}, {hx, hy}, {-hx, hy}, {-hx, -hy}};
    const uint8_t count[4] = {g.nNose, g.nRight, g.nTail, g.nLeft};
    const int s = cwIndex(g.start);
    uint16_t k = 0;
    for (int j = 0; j < 4; j++) {
        int a, b, side;
        if (g.clockwise) {
            a = (s + j) & 3;
            b = (s + j + 1) & 3;
            side = a;
        } else {
            a = (s - j) & 3;
            b = (s - j - 1) & 3;
            side = b;   // o lado entre b e a, percorrido ao contrário
        }
        const uint8_t n = count[side];
        for (uint8_t i = 0; i < n && k < max; i++) {
            const float f = ((float)i + 0.5f) / (float)n;   // centro de cada trecho
            out[k].xMm = corner[a].xMm + (corner[b].xMm - corner[a].xMm) * f;
            out[k].yMm = corner[a].yMm + (corner[b].yMm - corner[a].yMm) * f;
            k++;
        }
    }
    return k;
}

// ---------- Cores ----------

static LedRgb mix(LedRgb a, LedRgb b, float t) {
    LedRgb c;
    c.r = (uint8_t)lroundf((float)a.r + ((float)b.r - (float)a.r) * t);
    c.g = (uint8_t)lroundf((float)a.g + ((float)b.g - (float)a.g) * t);
    c.b = (uint8_t)lroundf((float)a.b + ((float)b.b - (float)a.b) * t);
    return c;
}

static LedRgb scale(LedRgb c, float k) {
    if (k <= 0.0f) return {0, 0, 0};
    if (k >= 1.0f) return c;
    return {(uint8_t)lroundf((float)c.r * k), (uint8_t)lroundf((float)c.g * k), (uint8_t)lroundf((float)c.b * k)};
}

LedRgb ledHorizonColor(const LedLook &look, LedPoint p, float rollDeg, float pitchDeg) {
    const float r = sqrtf(p.xMm * p.xMm + p.yMm * p.yMm);
    if (r <= 0.0f || !isfinite(rollDeg) || !isfinite(pitchDeg)) return look.white;
    const float th = pitchDeg * DEG, ph = rollDeg * DEG;
    const float h = p.xMm * sinf(th) - p.yMm * sinf(ph) * cosf(th);
    float sr = h / r;   // o ponto gira mantendo a distância r ao centro: elevação = asin(h / r)
    if (sr > 1.0f) sr = 1.0f;
    if (sr < -1.0f) sr = -1.0f;
    const float tilt = asinf(sr) / DEG;   // inclinação da borda neste ponto
    const float mag = fabsf(tilt);
    if (mag <= look.deadbandDeg) return look.white;
    float t = 1.0f;
    if (look.saturationDeg > look.deadbandDeg) t = (mag - look.deadbandDeg) / (look.saturationDeg - look.deadbandDeg);
    if (t > 1.0f) t = 1.0f;
    return mix(look.white, tilt > 0.0f ? look.sky : look.ground, t);
}

const char *ledModeText(LedMode m) {
    switch (m) {
    case LedMode::NoData:      return "sem dado atual (LED girando)";
    case LedMode::Calibrating: return "A calibrando (âmbar pulsando)";
    case LedMode::AttInvalid:  return "atitude inválida (âmbar fixo)";
    case LedMode::Horizon:     return "horizonte";
    }
    return "?";
}

// ---------- Flash do alarme ----------

bool LedAlarmFlash::update(bool active, uint8_t kind, uint32_t nowMs, uint32_t onMs, uint32_t periodMs) {
    if (!active) {
        active_ = false;
        return false;
    }
    if (!active_ || kind != kind_) {   // alarme novo: a fase recomeça, aceso
        active_ = true;
        kind_ = kind;
        since_ = nowMs;
    }
    if (periodMs == 0) return true;
    return (nowMs - since_) % periodMs < onMs;
}

// ---------- Quadro ----------

void ledRenderFrame(const LedLook &look, const LedPoint *pts, const LedRingSpan *rings, uint8_t nRings,
                    const LedFrameInput &in, uint32_t nowMs, LedRgb *out) {
    for (uint8_t ri = 0; ri < nRings; ri++) {
        const LedRingSpan &rs = rings[ri];
        uint16_t spin = 0;
        if (in.mode == LedMode::NoData && rs.count > 0 && look.spinPeriodMs > 0)
            spin = (uint16_t)((uint64_t)(nowMs % look.spinPeriodMs) * rs.count / look.spinPeriodMs);
        float pulse = 1.0f;
        if (in.mode == LedMode::Calibrating && look.pulsePeriodMs > 0) {
            const float ph = (float)(nowMs % look.pulsePeriodMs) / (float)look.pulsePeriodMs;
            const float tri = ph < 0.5f ? ph * 2.0f : 2.0f - ph * 2.0f;   // 0 → 1 → 0
            pulse = look.pulseMin + (1.0f - look.pulseMin) * tri;
        }
        for (uint16_t i = 0; i < rs.count; i++) {
            const uint16_t k = (uint16_t)(rs.first + i);
            LedRgb c;
            switch (in.mode) {
            case LedMode::NoData:      c = i == spin ? look.alarm : LedRgb{0, 0, 0}; break;
            case LedMode::Calibrating: c = scale(look.amber, pulse); break;
            case LedMode::AttInvalid:  c = look.amber; break;
            case LedMode::Horizon:
            default:
                c = in.flash ? look.alarm : ledHorizonColor(look, pts[k], in.rollDeg, in.pitchDeg);
                break;
            }
            out[k] = scale(c, rs.gain);
        }
    }
}

// ---------- Potenciômetro ----------

float LedPot::curve(uint16_t v) const {
    float u = cfg_.adcMax > 0 ? (float)v / (float)cfg_.adcMax : 0.0f;
    if (u < 0.0f) u = 0.0f;
    if (u > 1.0f) u = 1.0f;
    return cfg_.minBrightness + (1.0f - cfg_.minBrightness) * powf(u, cfg_.gamma);
}

float LedPot::update(uint16_t raw) {
    if (raw > cfg_.adcMax) raw = cfg_.adcMax;
    buf_[i_] = raw;
    i_ = (uint8_t)((i_ + 1) % N);
    if (n_ < N) n_++;
    uint32_t sum = 0;
    for (uint8_t k = 0; k < n_; k++) sum += buf_[k];
    uint16_t avg = (uint16_t)((sum + n_ / 2) / n_);
    // Nas pontas, encosta no fim do curso: a histerese não pode impedir o brilho mínimo nem o máximo
    if (avg + cfg_.hysteresis >= cfg_.adcMax) avg = cfg_.adcMax;
    else if (avg <= cfg_.hysteresis) avg = 0;
    const int diff = (int)avg - (int)held_;
    if (!have_ || diff >= (int)cfg_.hysteresis || -diff >= (int)cfg_.hysteresis || avg == 0 || avg == cfg_.adcMax) {
        held_ = avg;
        have_ = true;
    }
    brightness_ = curve(held_);
    return brightness_;
}

// ---------- Brilho e teto de corrente ----------

LedPowerResult ledApplyBrightness(LedRgb *buf, uint16_t n, float brightness, const LedPowerConfig &cfg) {
    if (brightness < 0.0f) brightness = 0.0f;
    if (brightness > 1.0f) brightness = 1.0f;
    uint32_t sum = 0;
    for (uint16_t i = 0; i < n; i++) {
        buf[i] = scale(buf[i], brightness);
        sum += (uint32_t)buf[i].r + buf[i].g + buf[i].b;
    }
    const float idle = cfg.maIdle * (float)n;
    float active = (float)sum / 255.0f * cfg.maPerChannel;
    LedPowerResult res = {idle + active, false};
    if (res.estimatedMa > cfg.maMax && active > 0.0f) {
        float k = (cfg.maMax - idle) / active;
        if (k < 0.0f) k = 0.0f;
        sum = 0;
        for (uint16_t i = 0; i < n; i++) {
            const LedRgb c = buf[i];
            // Arredonda para baixo: o quadro reduzido nunca passa do teto
            buf[i] = {(uint8_t)((float)c.r * k), (uint8_t)((float)c.g * k), (uint8_t)((float)c.b * k)};
            sum += (uint32_t)buf[i].r + buf[i].g + buf[i].b;
        }
        active = (float)sum / 255.0f * cfg.maPerChannel;
        res.estimatedMa = idle + active;
        res.capped = true;
    }
    return res;
}
