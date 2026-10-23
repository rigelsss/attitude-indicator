// pfd_state.cpp — ver pfd_state.h
#include "pfd_state.h"
#include <math.h>
#include "telemetry.h"

const char *pfdFlagText(PfdFlag f) {
    switch (f) {
    case PfdFlag::None:       return "";
    case PfdFlag::NoSignal:   return "SEM SINAL";
    case PfdFlag::LinkLost:   return "LINK PERDIDO";
    case PfdFlag::DataLate:   return "DADO ATRASADO";
    case PfdFlag::AttInvalid: return "ATT INVALIDA";
    }
    return "?";
}

PfdState pfdState(const PfdInputs &in) {
    PfdState s = {};
    if (!in.everLinked) s.flag = PfdFlag::NoSignal;
    else if (!in.linkOk) s.flag = PfdFlag::LinkLost;
    else if (in.dataLate) s.flag = PfdFlag::DataLate;
    else if (!(in.valid & VAL_ATT) || !isfinite(in.roll) || !isfinite(in.pitch)) s.flag = PfdFlag::AttInvalid;
    else s.flag = PfdFlag::None;
    const bool att = s.flag == PfdFlag::None;
    s.degraded = att && (in.valid & VAL_ATT_DEGRADED);
    s.headingOk = att && (in.valid & VAL_HDG) && isfinite(in.heading);
    s.gyroOnly = s.headingOk && (in.valid & VAL_HDG_GYRO_ONLY);
    return s;
}
