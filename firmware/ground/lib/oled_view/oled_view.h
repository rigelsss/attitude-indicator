// oled_view.h — texto das duas páginas do OLED de 0,96" do ESP B (128x64; fonte 6x10: 6 linhas de 21
// caracteres, só ASCII), montado pela oled_task a partir das cópias publicadas (docs/solo.md, passo 3).
// Lógica pura (sem Arduino.h), testável com pio test -e native.
//   Página 1 (dados):   ROLL/PIT, PROA (MAG ou VERD), ALT, pressão e temperatura, CURVA (gz)
//   Página 2 (link):    estado e taxa, perdas e quedas, intervalo entre chegadas, sensores do A, calibração e
//                       loop_us, alarme
// A primeira linha é o título com a página ("1/2"); sem link, vira uma faixa (vídeo inverso) com SEM SINAL ou
// LINK PERDIDO, e os valores da página 1 vão para "---". Cada valor com a flag valid desligada também é "---":
// nunca o último valor congelado. Pressão e temperatura seguem a VAL_ALT (leitura recente), e não só o ST_BARO_OK.
// Atitude degradada (VAL_ATT_DEGRADED): o título da página 1 vira "ATT DEGRADADA".
#pragma once
#include <stdint.h>
#include "telemetry.h"

static const int OLED_ROWS = 6;
static const int OLED_COLS = 21;

// Resumo do link (da janela de 1 s da link_task)
struct OledLinkStats {
    bool     valid;            // já há um resumo
    uint32_t recPerS;          // pacotes por segundo na última janela
    uint32_t lostTotal;        // desde o boot
    float    lossPct;          // perdidos / (recebidos + perdidos), desde o boot
    uint32_t timeouts;         // quedas desde o boot
    bool     haveIntervals;
    float    intMinMs, intAvgMs, intMaxMs;
};

struct OledInputs {
    bool            everLinked;    // já chegou um pacote aceito
    bool            linkOk;        // o último pacote tem no máximo o timeout de idade
    TelemetryPacket pkt;           // o último pacote aceito (vale com everLinked)
    OledLinkStats   link;
    const char     *alarmText;     // texto curto do alarme atual (alarmKindShort)
    uint8_t         page;          // 0..pages−1
    uint8_t         pages;
    bool            headingTrue;   // false: proa magnética (MAG); true: verdadeira (VERD), com a declinação
    float           declinationDeg;
};

struct OledPage {
    char lines[OLED_ROWS][OLED_COLS + 1];
    bool banner;   // linha 0 em vídeo inverso (sem link)
};

void oledBuildPage(const OledInputs &in, OledPage &out);

// Proa exibida, 0..360 (exclusive): a magnética, ou a verdadeira = magnética + declinação
float oledHeading(float magneticDeg, bool headingTrue, float declinationDeg);
