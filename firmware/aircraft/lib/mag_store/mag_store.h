// mag_store.h — persistência da calibração do magnetômetro na NVS (Preferences, namespace "mag_cal").
// Guarda offset (µT) e matriz W nos eixos do modelo, com o ganho do chip e o mapeamento de eixos
// (MAG_AXIS_MAP_ID) usados na calibração.
#pragma once
#include "mag.h"

struct MagCalData {
    bool     valid = false;          // há uma calibração gravada, plausível, com o ganho e o mapeamento atuais
    bool     rejected = false;       // registro lido, mas com valores impossíveis (ignorado)
    bool     otherAxisMap = false;   // registro feito com outro mapeamento de eixos (ignorado)
    bool     otherGain = false;      // registro feito com outro ganho (ignorado)
    MagCal   cal;                    // offset zero, W identidade e raio 0 quando inválida
    float    residPct = 0;
    uint8_t  sectors = 0;
};

// Carrega o que houver. Registro ausente, de outra versão, de outro ganho (CRB) ou de outro mapeamento de
// eixos volta como valid = false. A gravação substitui o registro inteiro de uma vez (atômica na NVS).
void magStoreLoad(MagCalData &out, uint8_t currentCrb);
bool magStoreSave(const MagCal &cal, float residPct, uint8_t sectors, uint8_t crb);
