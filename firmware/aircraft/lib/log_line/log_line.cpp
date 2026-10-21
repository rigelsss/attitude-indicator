// log_line.cpp — ver log_line.h
#include "log_line.h"

bool LineAssembler::put(char c) {
    if (len_ >= cap_) len_ = 0;   // linha pronta não retirada: recomeça em vez de escrever fora do buffer
    buf_[len_++] = c;
    return c == '\n' || len_ == cap_;
}
