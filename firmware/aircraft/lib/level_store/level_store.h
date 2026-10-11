// level_store.h — persistência da calibração de montagem na NVS (Preferences, namespace "att_level").
// A rotação leva os eixos nominais do modelo (chipToBody) para os eixos da caixa (docs/fusao.md).
#pragma once
#include "attitude.h"

struct LevelData {
    bool  valid = false;          // há uma calibração gravada e a matriz é uma rotação
    Mat3  r = mat3Identity();     // identidade quando não há calibração
    float mountRollDeg = 0;       // desalinhamento do módulo, para exibição
    float mountPitchDeg = 0;
    float tempC = 0;
};

// Carrega o que houver. Registro ausente, de outra versão ou com matriz inválida volta como
// valid = false, com a identidade. A gravação substitui o registro inteiro de uma vez (atômica).
void levelStoreLoad(LevelData &out);
bool levelStoreSave(const LevelData &data);
