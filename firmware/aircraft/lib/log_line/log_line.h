// log_line.h — junta caracteres soltos em linhas. Usado pelo lib/logger para a saída do putc do núcleo
// (ets_printf, que entrega um caractere por vez), para cada linha entrar inteira no buffer do logger.
// Lógica pura (sem Arduino.h), testável com pio test -e native.
#pragma once
#include <stddef.h>

class LineAssembler {
public:
    // buf: memória da linha, com cap bytes (cap > 0)
    LineAssembler(char *buf, size_t cap) : buf_(buf), cap_(cap) {}
    // Acrescenta um caractere. Devolve true quando a linha ficou pronta: terminou em '\n' ou encheu o
    // buffer (uma linha longa demais sai em pedaços de cap bytes). Depois de usar data()/size(), chame clear().
    bool put(char c);
    const char *data() const { return buf_; }
    size_t size() const { return len_; }
    void clear() { len_ = 0; }

private:
    char *buf_;
    size_t cap_;
    size_t len_ = 0;
};
