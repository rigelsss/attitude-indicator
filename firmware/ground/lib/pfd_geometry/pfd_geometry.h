// pfd_geometry.h — geometria do horizonte artificial no TFT do ESP B (docs/solo.md, passo 5). Lógica pura (sem
// Arduino.h), testável com pio test -e native.
//
// Coordenadas de tela em pixels, x para a direita e y para baixo. Sinais (fusao.md): roll positivo = asa
// direita para baixo; pitch positivo = nariz para cima.
//   - roll positivo gira o horizonte no sentido anti-horário na tela (lado esquerdo desce), como visto da
//     cabine: a direção da linha é (cos r, −sin r), e a normal que aponta para a terra é (sin r, cos r);
//   - nariz para cima desloca o horizonte para a terra (para baixo, nivelado): ponto = centro + terra·pitch·px/°;
//   - roll de 90° deixa a terra à direita; em dorso (180°), a terra fica em cima.
// O ponteiro de roll fica do lado do céu (estilo Boeing): com roll à direita, ele vai para a esquerda da escala.
#pragma once
#include <stdint.h>

struct Vec2 {
    float x, y;
};

struct PfdConfig {
    float cx, cy;      // centro do avião (fixo) na tela
    float width, height;
    float pxPerDeg;    // escala do pitch
};

struct Horizon {
    Vec2 point;   // ponto do horizonte (centro deslocado pelo pitch)
    Vec2 dir;     // direção unitária da linha do horizonte
    Vec2 down;    // normal unitária para o lado da terra
};

Horizon pfdHorizon(const PfdConfig &cfg, float rollDeg, float pitchDeg);

// Polígono da terra: o semiplano do lado de "down" recortado pelo retângulo da tela, em ordem, com até 5
// vértices (cabe em 6). Devolve o número de vértices: 0 = só céu; 4 com a tela inteira = só terra
int pfdGroundPolygon(const PfdConfig &cfg, const Horizon &h, Vec2 out[6]);

// Recorta o segmento a–b pelo retângulo da tela. false se ficar inteiro fora
bool pfdClipSegment(const PfdConfig &cfg, Vec2 &a, Vec2 &b);

// Traço da escala de pitch na marca markDeg (graus acima do horizonte): centro e pontas, paralelo ao horizonte
void pfdPitchMark(const PfdConfig &cfg, const Horizon &h, float markDeg, float halfLenPx, Vec2 &center, Vec2 &a,
                  Vec2 &b);

// Ponto da escala de roll fixa, no ângulo angleDeg (0 em cima, positivo à direita) e no raio radiusPx
Vec2 pfdRollScalePoint(const PfdConfig &cfg, float angleDeg, float radiusPx);

// Ponteiro de roll: do lado do céu, no raio radiusPx (gira com o horizonte)
Vec2 pfdRollPointer(const PfdConfig &cfg, const Horizon &h, float radiusPx);
