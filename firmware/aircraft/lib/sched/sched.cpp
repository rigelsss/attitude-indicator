// sched.cpp — ver sched.h
#include "sched.h"

void Periodic::advance(uint32_t nowUs, uint32_t periodUs) {
    next_ += periodUs;
    if (due(nowUs)) next_ = nowUs + periodUs;   // atrasou: ressincroniza
}
