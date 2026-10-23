// delay_estimator.h — atraso estimado de cada pacote no ESP B (docs/solo.md, passo 4). Lógica pura (sem
// Arduino.h), testável com pio test -e native.
//
// Os relógios do A e do B não estão sincronizados, então a diferença chegada no B − t_ms do A (o offset) não diz
// nada sozinha. O menor offset recente é o de um pacote que chegou "na hora", e o atraso de cada pacote é o
// offset dele menos esse mínimo. O mínimo é tomado numa janela de `buckets` faixas de `bucketMs` (10 x 1 s): ela
// acompanha a deriva entre os cristais (±20 ppm cada, até ~2,4 ms por minuto), com erro abaixo de 0,5 ms.
// Recomeça do zero com reset(): numa ressincronização (A reiniciado), o relógio do A volta a zero.
// Limitação: se todos os pacotes da janela chegarem atrasados, o mínimo sobe e o atraso fica subestimado. Com o A
// pulando o envio enquanto o anterior está em voo (radio_task), isso exige o link ruim por toda a janela.
#pragma once
#include <stdint.h>

struct DelayConfig {
    uint32_t bucketMs;   // duração de cada faixa
    uint8_t  buckets;    // faixas na janela (até DELAY_MAX_BUCKETS)
};

static const int DELAY_MAX_BUCKETS = 16;

// Idade estimada do dado mostrado (ms): o atraso com que o pacote chegou mais o tempo desde a chegada. É o que
// decide o "dado atrasado" (alarme, OLED e TFT): 240 ms de atraso na chegada seguidos de 490 ms sem pacote novo
// dão 730 ms, embora cada parte, sozinha, passe pelos limites
inline uint64_t dataAgeMs(uint32_t delayMs, uint64_t sinceArrivalMs) { return (uint64_t)delayMs + sinceArrivalMs; }

class DelayEstimator {
public:
    explicit DelayEstimator(const DelayConfig &cfg);
    void reset();
    // Pacote aceito: t_ms do A e instante da chegada no B (ms, relógio monotônico). Devolve o atraso estimado (ms)
    uint32_t feed(uint32_t tMsA, uint64_t arrivalMsB);

private:
    DelayConfig cfg_;
    int64_t     min_[DELAY_MAX_BUCKETS];   // menor offset de cada faixa
    uint64_t    id_[DELAY_MAX_BUCKETS];    // número da faixa (arrivalMsB / bucketMs) guardada em cada posição
    bool        used_[DELAY_MAX_BUCKETS];
};
