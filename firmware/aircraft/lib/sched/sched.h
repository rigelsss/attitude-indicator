// sched.h — agenda periódica sobre micros(), em lógica pura (sem Arduino.h), testável com
// pio test -e native. As comparações usam a diferença com sinal, então a virada dos 32 bits
// (~71 min) continua tratada, desde que o período e o atraso fiquem abaixo de ~35 min.
#pragma once
#include <stdint.h>

class Periodic {
public:
    // Próxima execução já vencida
    void start(uint32_t nowUs) { next_ = nowUs; }
    bool due(uint32_t nowUs) const { return (int32_t)(nowUs - next_) >= 0; }
    // Fase contínua: avança um período a partir da execução prevista. Se ainda estiver vencida
    // (atraso maior que um período), ressincroniza: a próxima fica um período depois de agora.
    // O período pode mudar de uma chamada para outra (ex.: mais longo com o sensor falhando).
    void advance(uint32_t nowUs, uint32_t periodUs);
    // Recomeça a contagem a partir de agora
    void restart(uint32_t nowUs, uint32_t periodUs) { next_ = nowUs + periodUs; }
    uint32_t next() const { return next_; }

private:
    uint32_t next_ = 0;
};
