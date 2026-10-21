// text_buf.h — acumulador de texto num buffer fixo. A serial_task monta nele um bloco inteiro (o c, o r, uma
// linha do a) e o entrega ao logger numa escrita só, que é atômica: linhas de outras tasks não caem no meio.
// Lógica pura (sem Arduino.h), testável com pio test -e native.
#pragma once
#include <stdarg.h>
#include <stddef.h>

class TextBuf {
public:
    // buf: memória do texto, com cap bytes (cap > 0); o texto termina sempre em '\0'
    TextBuf(char *buf, size_t cap) : buf_(buf), cap_(cap) { buf_[0] = '\0'; }
    void printf(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
    void vprintf(const char *fmt, va_list arg);
    void print(const char *s);
    void println(const char *s = "");   // termina com "\r\n", como o logPrintln()
    const char *c_str() const { return buf_; }
    size_t size() const { return len_; }
    // Algum acréscimo não coube: o texto guardado vai até cap − 1 bytes (o fim foi cortado)
    bool truncated() const { return truncated_; }
    void clear();

private:
    char  *buf_;
    size_t cap_;
    size_t len_ = 0;
    bool   truncated_ = false;
};
