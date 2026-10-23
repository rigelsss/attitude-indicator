// delay_estimator.cpp — ver delay_estimator.h
#include "delay_estimator.h"

DelayEstimator::DelayEstimator(const DelayConfig &cfg) : cfg_(cfg) {
    if (cfg_.buckets == 0) cfg_.buckets = 1;
    if (cfg_.buckets > DELAY_MAX_BUCKETS) cfg_.buckets = DELAY_MAX_BUCKETS;
    if (cfg_.bucketMs == 0) cfg_.bucketMs = 1;
    reset();
}

void DelayEstimator::reset() {
    for (int i = 0; i < DELAY_MAX_BUCKETS; i++) {
        used_[i] = false;
        min_[i] = 0;
        id_[i] = 0;
    }
}

uint32_t DelayEstimator::feed(uint32_t tMsA, uint64_t arrivalMsB) {
    const int64_t offset = (int64_t)arrivalMsB - (int64_t)tMsA;
    const uint64_t id = arrivalMsB / cfg_.bucketMs;
    const int slot = (int)(id % cfg_.buckets);
    if (!used_[slot] || id_[slot] != id) {   // faixa nova: substitui a que saiu da janela
        used_[slot] = true;
        id_[slot] = id;
        min_[slot] = offset;
    } else if (offset < min_[slot]) {
        min_[slot] = offset;
    }
    // Mínimo das faixas ainda dentro da janela (inclui a deste pacote)
    int64_t base = offset;
    for (int i = 0; i < cfg_.buckets; i++)
        if (used_[i] && id_[i] + cfg_.buckets > id && id_[i] <= id && min_[i] < base) base = min_[i];
    const int64_t d = offset - base;
    return d > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)d;
}
