// level_store.h — persistência da calibração de montagem na NVS (Preferences, namespace "att_level").
// A rotação leva os eixos nominais do modelo (chipToBody) para os eixos da caixa (docs/fusao.md).
#pragma once
#include "attitude.h"

struct LevelData {
    bool  valid = false;          // há uma calibração gravada e a matriz é uma rotação
    bool  rejected = false;       // registro lido, mas com valores impossíveis (ignorado)
    Mat3  r = mat3Identity();     // identidade quando não há calibração
    float mountRollDeg = 0;       // desalinhamento do módulo, para exibição
    float mountPitchDeg = 0;
    float tempC = 0;
    // Calibração do acelerômetro com que a montagem foi medida: a rotação vem do acelerômetro já
    // corrigido, então só vale com a mesma calibração (sameAccelCal)
    bool     accelCalValid = false;
    AccelCal accelCal;
};

// Carrega o que houver. Registro ausente ou de outra versão (o formato 1 não guardava a calibração do
// acelerômetro) volta como valid = false, com a identidade. Também volta assim, com rejected = true, um
// registro com valores impossíveis: matriz que não é rotação, rotação maior que maxTiltDeg (o mesmo
// limite da calibração z) ou ângulos e temperatura não finitos. A gravação substitui o registro
// inteiro de uma vez (atômica).
void levelStoreLoad(LevelData &out, float maxTiltDeg);
bool levelStoreSave(const LevelData &data);
