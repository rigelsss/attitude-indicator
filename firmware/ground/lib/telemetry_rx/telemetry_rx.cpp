// telemetry_rx.cpp — ver telemetry_rx.h
#include "telemetry_rx.h"
#include <string.h>

RxDecode decodeTelemetry(const uint8_t *data, size_t len, TelemetryPacket &out) {
    if (data == nullptr || len < sizeof(MsgHeader)) return RxDecode::BadSize;
    MsgHeader hdr;
    memcpy(&hdr, data, sizeof(hdr));
    if (hdr.magic != TELEMETRY_MAGIC) return RxDecode::BadMagic;
    if (hdr.version != TELEMETRY_VERSION) return RxDecode::BadVersion;
    if (hdr.type != MSG_TELEMETRY) return RxDecode::BadType;
    if (len != sizeof(TelemetryPacket)) return RxDecode::BadSize;
    memcpy(&out, data, sizeof(out));
    return RxDecode::Ok;
}

RxDecode decodeAck(const uint8_t *data, size_t len, AckPacket &out) {
    if (data == nullptr || len < sizeof(MsgHeader)) return RxDecode::BadSize;
    MsgHeader hdr;
    memcpy(&hdr, data, sizeof(hdr));
    if (hdr.magic != TELEMETRY_MAGIC) return RxDecode::BadMagic;
    if (hdr.version != TELEMETRY_VERSION) return RxDecode::BadVersion;
    if (hdr.type != MSG_ACK) return RxDecode::BadType;
    if (len != sizeof(AckPacket)) return RxDecode::BadSize;
    memcpy(&out, data, sizeof(out));
    return RxDecode::Ok;
}

uint8_t rxMessageType(const uint8_t *data, size_t len) {
    if (data == nullptr || len < sizeof(MsgHeader)) return 0;
    MsgHeader hdr;
    memcpy(&hdr, data, sizeof(hdr));
    return hdr.type;
}

const char *rxDecodeText(RxDecode r) {
    switch (r) {
    case RxDecode::Ok:         return "ok";
    case RxDecode::BadSize:    return "tamanho";
    case RxDecode::BadMagic:   return "magic";
    case RxDecode::BadVersion: return "versão";
    case RxDecode::BadType:    return "tipo";
    }
    return "?";
}
