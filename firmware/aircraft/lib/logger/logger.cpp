// logger.cpp — ver logger.h
#include "logger.h"
#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

// Mesmo esquema do Print::printf do núcleo: buffer de 64 bytes na pilha e, para linhas
// maiores, um buffer alocado do tamanho exato
void logPrintf(const char *fmt, ...) {
    char loc[64];
    char *buf = loc;
    va_list arg, copy;
    va_start(arg, fmt);
    va_copy(copy, arg);
    int len = vsnprintf(loc, sizeof(loc), fmt, copy);
    va_end(copy);
    if (len < 0) {
        va_end(arg);
        return;
    }
    if (len >= (int)sizeof(loc)) {
        buf = (char *)malloc(len + 1);
        if (buf == nullptr) {
            va_end(arg);
            return;
        }
        len = vsnprintf(buf, len + 1, fmt, arg);
    }
    va_end(arg);
    Serial.write((const uint8_t *)buf, len);
    if (buf != loc) free(buf);
}

void logPrint(const char *s) { Serial.print(s); }

void logPrintln(const char *s) { Serial.println(s); }

void logFlush() { Serial.flush(); }
