// pfd_state.h — o que o TFT do ESP B mostra a cada quadro (docs/solo.md, passo 5): horizonte ou bandeira de
// falha, o motivo da bandeira, a marca de atitude degradada e a validade da proa. Lógica pura (sem Arduino.h),
// testável com pio test -e native; a display_task só desenha o que esta decisão manda.
//   - sem dado atual (sem sinal, link perdido ou dado atrasado), com VAL_ATT desligada ou com roll/pitch não
//     finito: bandeira, com o motivo nessa ordem de prioridade;
//   - a proa só vale com a atitude mostrada (nunca um número junto do X vermelho), VAL_HDG e proa finita; com
//     VAL_HDG_GYRO_ONLY, ela leva o "~";
//   - VAL_ATT_DEGRADED só marca a atitude quando ela é mostrada.
#pragma once
#include <stdint.h>

enum class PfdFlag : uint8_t { None, NoSignal, LinkLost, DataLate, AttInvalid };
// Texto do motivo, só ASCII (fontes de bitmap do TFT): "SEM SINAL", "LINK PERDIDO", "DADO ATRASADO",
// "ATT INVALIDA"; vazio sem bandeira
const char *pfdFlagText(PfdFlag f);

struct PfdInputs {
    bool     everLinked, linkOk, dataLate;   // da linkSnapshot (a regra única de "dado atual")
    uint16_t valid;                          // ValidFlags do último pacote
    float    roll, pitch, heading;
};

struct PfdState {
    PfdFlag flag;         // None: horizonte
    bool    degraded;     // marca ATT DEGRADADA (só com o horizonte)
    bool    headingOk;    // proa numérica; senão, "---"
    bool    gyroOnly;     // proa com "~" (só com headingOk)
};

PfdState pfdState(const PfdInputs &in);
