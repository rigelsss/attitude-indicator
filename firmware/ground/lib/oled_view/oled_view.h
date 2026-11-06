// oled_view.h — texto das telas dos dois OLEDs de 0,96" do ESP B (128x64; fonte 6x10: 6 linhas de 21
// caracteres, só ASCII), montado pela oled_task a partir das cópias publicadas (docs/solo.md, passo 3; uma tela
// fixa por OLED desde 02/11/2026, docs/caixa-b.md). Lógica pura (sem Arduino.h), testável com pio test -e native.
//   OLED 1 (dados): ROLL/PIT, PROA (MAG ou VERD), ALT, pressão e temperatura, CURVA (gz); um "<" na última
//                   coluna marca o alvo da pressão longa do encoder (ROLL/PIT = zerar a atitude, ALT = a altitude)
//   OLED 2 (link):  estado e taxa, perdas e quedas, intervalo entre chegadas, sensores do A, calibração e
//                   loop_us, alarme
// A primeira linha é o título; sem dado atual, vira uma faixa (vídeo inverso) com SEM SINAL, LINK PERDIDO ou
// DADO ATRASADO, e os valores dos dados vão para "---". Cada valor com a flag valid desligada também é "---":
// nunca o último valor congelado. Pressão e temperatura seguem a VAL_ALT (leitura recente), e não só o ST_BARO_OK.
// Atitude degradada (VAL_ATT_DEGRADED): o título dos dados vira "ATT DEGRADADA".
#pragma once
#include <stdint.h>
#include "command_link.h"
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

enum class OledScreen : uint8_t { Data, Link };

struct OledInputs {
    bool            everLinked;    // já chegou um pacote aceito
    bool            linkOk;        // o último pacote tem no máximo o timeout de idade
    bool            dataLate;      // a idade estimada do dado passa do limite (dataAgeMs): faixa DADO ATRASADO
    uint32_t        delayMs;       // atraso estimado do último pacote (página 2, "at")
    TelemetryPacket pkt;           // o último pacote aceito (vale com everLinked)
    OledLinkStats   link;
    const char     *alarmText;     // texto curto do alarme atual (alarmKindShort)
    OledScreen      screen;
    uint8_t         zeroTarget;    // alvo da pressão longa (CMD_ZERO_ATTITUDE ou CMD_ZERO_ALTITUDE), marcado com
                                   // "<" nos dados; 0 sem marca
    bool            headingTrue;   // false: proa magnética (MAG); true: verdadeira (VERD), com a declinação
    float           declinationDeg;
    const char     *footer;        // não nulo: substitui a última linha da tela (andamento do comando do
                                   // encoder; oledEncoderFooter)
};

struct OledPage {
    char lines[OLED_ROWS][OLED_COLS + 1];
    bool banner;   // linha 0 em vídeo inverso (sem link)
};

void oledBuildPage(const OledInputs &in, OledPage &out);

// Rodapé do comando da pressão longa (até 21 colunas, ASCII): com o botão apertado, o que vai acontecer
// ("segure 1s: ZERO ATT"); depois, o andamento ("ZERO ATT: ENVIADO", "ACEITO", "OK", "RECUSADO", "SEM RESP.",
// "SEM FIM", "OCUPADO"): em curso, sempre; os finais, por showMs. nullptr sem rodapé
const char *oledEncoderFooter(char *buf, int n, bool buttonHeld, uint8_t targetCmd, bool haveEvent, uint8_t evCmd,
                              CmdState state, bool localFull, uint32_t ageMs, uint32_t showMs);

// Alvo da pressão longa depois de um clique do encoder: com dois alvos, cada clique (nos dois sentidos) troca a
// atitude pela altitude e vice-versa. Um alvo desconhecido volta à atitude
uint8_t oledNextZeroTarget(uint8_t current);

// Proa exibida, 0..360 (exclusive): a magnética, ou a verdadeira = magnética + declinação
float oledHeading(float magneticDeg, bool headingTrue, float declinationDeg);
