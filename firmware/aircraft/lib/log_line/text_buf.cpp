// text_buf.cpp — ver text_buf.h
#include "text_buf.h"
#include <stdio.h>
#include <string.h>

void TextBuf::vprintf(const char *fmt, va_list arg) {
    const size_t room = cap_ - len_;   // inclui o '\0'
    const int n = vsnprintf(buf_ + len_, room, fmt, arg);
    if (n < 0) return;
    if ((size_t)n >= room) {
        truncated_ = true;
        len_ = cap_ - 1;   // o vsnprintf já terminou o texto em '\0' no último byte
        return;
    }
    len_ += (size_t)n;
}

void TextBuf::printf(const char *fmt, ...) {
    va_list arg;
    va_start(arg, fmt);
    vprintf(fmt, arg);
    va_end(arg);
}

void TextBuf::print(const char *s) { printf("%s", s); }

void TextBuf::println(const char *s) { printf("%s\r\n", s); }

void TextBuf::clear() {
    len_ = 0;
    truncated_ = false;
    buf_[0] = '\0';
}
