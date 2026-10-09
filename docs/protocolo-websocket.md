# Protocolo WebSocket (ESP B ↔ PC)

Endpoint: `ws://192.168.4.1:81/` (IP padrão do AP do ESP32). Página: `http://192.168.4.1/`.

## B → clientes (telemetria, ~30 Hz)

```json
{
  "t": 123456,
  "roll": -3.2, "pitch": 1.8, "hdg": 274.5,
  "gx": 0.1, "gy": -0.3, "gz": 0.0,
  "alt": 1.2, "hpa": 1013.2, "temp": 26.4,
  "status": 55,
  "link": { "ok": true, "rssi": -48, "lost": 3, "rx_hz": 49.8, "age_ms": 12 },
  "alarm": { "bank": false, "pitch": false, "link": false }
}
```

Campos de `TelemetryPacket` com nomes curtos, mais o bloco `link`, que é calculado pelo B (pacotes perdidos via `seq`, taxa real, idade do último pacote).

## Clientes → B (comandos)

```json
{ "cmd": "calibrate_gyro" }
{ "cmd": "calibrate_mag" }
{ "cmd": "zero_attitude" }
{ "cmd": "zero_altitude" }
{ "cmd": "set_rate", "arg": 25 }
```

O B traduz para `CommandPacket` (ESP-NOW) e repassa o `AckPacket` do A:

```json
{ "ack": "calibrate_gyro", "ok": true }
```
