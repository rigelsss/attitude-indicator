// pfd_geometry.cpp — ver pfd_geometry.h
#include "pfd_geometry.h"
#include <math.h>

static const float DEG = 3.14159265f / 180.0f;

Horizon pfdHorizon(const PfdConfig &cfg, float rollDeg, float pitchDeg) {
    const float r = rollDeg * DEG;
    const float s = sinf(r), c = cosf(r);
    Horizon h;
    h.dir = {c, -s};
    h.down = {s, c};
    const float off = pitchDeg * cfg.pxPerDeg;
    h.point = {cfg.cx + h.down.x * off, cfg.cy + h.down.y * off};
    return h;
}

// Distância com sinal até o horizonte, positiva do lado da terra
static float side(const Horizon &h, Vec2 p) { return (p.x - h.point.x) * h.down.x + (p.y - h.point.y) * h.down.y; }

int pfdGroundPolygon(const PfdConfig &cfg, const Horizon &h, Vec2 out[6]) {
    const Vec2 rect[4] = {{0, 0}, {cfg.width, 0}, {cfg.width, cfg.height}, {0, cfg.height}};
    // Sutherland–Hodgman com uma aresta (o horizonte): um quadrilátero convexo recortado por um semiplano dá até 5
    int n = 0;
    for (int i = 0; i < 4; i++) {
        const Vec2 s = rect[i], e = rect[(i + 1) % 4];
        const float ds = side(h, s), de = side(h, e);
        const bool sin = ds >= 0, ein = de >= 0;
        if (sin) out[n++] = s;
        if (sin != ein) {
            const float t = ds / (ds - de);
            out[n++] = {s.x + (e.x - s.x) * t, s.y + (e.y - s.y) * t};
        }
    }
    return n;
}

bool pfdClipSegment(const PfdConfig &cfg, Vec2 &a, Vec2 &b) {
    // Liang–Barsky
    const float dx = b.x - a.x, dy = b.y - a.y;
    float t0 = 0.0f, t1 = 1.0f;
    const float p[4] = {-dx, dx, -dy, dy};
    const float q[4] = {a.x, cfg.width - a.x, a.y, cfg.height - a.y};
    for (int i = 0; i < 4; i++) {
        if (p[i] == 0.0f) {
            if (q[i] < 0.0f) return false;   // paralelo e fora
            continue;
        }
        const float t = q[i] / p[i];
        if (p[i] < 0.0f) {
            if (t > t1) return false;
            if (t > t0) t0 = t;
        } else {
            if (t < t0) return false;
            if (t < t1) t1 = t;
        }
    }
    const Vec2 a0 = a;
    a = {a0.x + dx * t0, a0.y + dy * t0};
    b = {a0.x + dx * t1, a0.y + dy * t1};
    return true;
}

void pfdPitchMark(const PfdConfig &cfg, const Horizon &h, float markDeg, float halfLenPx, Vec2 &center, Vec2 &a,
                  Vec2 &b) {
    const float off = markDeg * cfg.pxPerDeg;   // acima do horizonte = para o lado do céu
    center = {h.point.x - h.down.x * off, h.point.y - h.down.y * off};
    a = {center.x - h.dir.x * halfLenPx, center.y - h.dir.y * halfLenPx};
    b = {center.x + h.dir.x * halfLenPx, center.y + h.dir.y * halfLenPx};
}

Vec2 pfdRollScalePoint(const PfdConfig &cfg, float angleDeg, float radiusPx) {
    const float t = angleDeg * DEG;
    return {cfg.cx + radiusPx * sinf(t), cfg.cy - radiusPx * cosf(t)};
}

Vec2 pfdRollPointer(const PfdConfig &cfg, const Horizon &h, float radiusPx) {
    return {cfg.cx - h.down.x * radiusPx, cfg.cy - h.down.y * radiusPx};
}
