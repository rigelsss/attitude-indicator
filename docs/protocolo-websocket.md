# Protocolo WebSocket (ESP B ↔ PC)

Endpoint: `ws://192.168.4.1:81/` (IP padrão do AP do ESP32). Página: `http://192.168.4.1/`.

## B → clientes (telemetria, ~30 Hz)

```json
{
  "t": 123456,
  "roll": -3.2, "pitch": 1.8, "hdg": 274.5, "decl": -20.7,
  "gx": 0.1, "gy": -0.3, "gz": 0.0,
  "alt": 1.2, "hpa": 1013.2, "temp": 26.4,
  "status": 55, "valid": 13,
  "link": { "ok": true, "rssi": -48, "lost": 3, "rx_hz": 49.8, "age_ms": 12 },
  "alarm": { "bank": false, "pitch": false, "link": false }
}
```

`hdg` é a proa **magnética**, como no `TelemetryPacket`. `decl` é a declinação magnética do local (`MAG_DECLINATION_DEG` no `config.h` do B, negativa para oeste): a página mostra a proa verdadeira como `hdg + decl`, levada a 0..360, com o rótulo "VERD", ou a magnética com "MAG".

Campos de `TelemetryPacket` com nomes curtos, mais o bloco `link`, que é calculado pelo B (pacotes perdidos via `seq`, taxa real, idade do último pacote).

### `status` e `valid` (contrato v3, `TELEMETRY_VERSION` 3)

Os dois são números (bitmask), copiados do `TelemetryPacket` ([telemetry.h](../firmware/shared/include/telemetry.h)):

- **`status`** diz se cada sensor responde e se há calibração (`ST_MPU_OK`, `ST_MAG_OK`, `ST_BARO_OK`, `ST_CALIBRATING`, `ST_GYRO_CAL`, `ST_MAG_CAL`). Não diz se o dado vale agora.
- **`valid`** diz se cada dado pode ser mostrado. Com a flag desligada, a página mostra o dado como indisponível, em vez de congelar o último valor:

| Bit | Valor | Flag | Liga quando | Na página |
|---|---|---|---|---|
| 0 | 1 | `VAL_ATT` | Filtro de atitude inicializado e última amostra do MPU com até 50 ms | Desligada: horizonte riscado |
| 1 | 2 | `VAL_ATT_DEGRADED` | Com `VAL_ATT`: giroscópio saturou e a correção pelo acelerômetro está se recuperando | Atitude em amarelo |
| 2 | 4 | `VAL_ALT` | Barômetro ok, zero válido, última leitura com até 200 ms e altitude finita | Desligada: altitude "---" |
| 3 | 8 | `VAL_HDG` | Filtro da proa inicializado, `VAL_ATT` e magnetômetro calibrado (sem calibração a proa pode errar dezenas de graus) | Desligada: proa "---" |
| 4 | 16 | `VAL_HDG_GYRO_ONLY` | Com `VAL_HDG`: correção pelo magnetômetro suspensa (atitude degradada, ligada já no mesmo pacote de `VAL_ATT_DEGRADED`; distúrbio; inclinação acima de 70°) ou magnetômetro sem leitura há mais de 200 ms. A proa segue só pelo giroscópio e pode derivar | Proa em amarelo, com "GIRO" |

No exemplo acima, `valid` = 13 = `VAL_ATT` + `VAL_ALT` + `VAL_HDG`: tudo válido, com a proa corrigida pelo magnetômetro.

O enlace caído (o A parou de transmitir) não tem flag: o B o detecta pela idade do último pacote (`link.age_ms`).

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
{ "ack": "calibrate_gyro", "result": "done", "ok": true }
```

`result` vem do `AckPacket.result` (contrato v3): `"rejected"` (não executado), `"accepted"` (recebido e iniciado; nos comandos demorados, um `"done"` vem depois) ou `"done"` (concluído). `ok` continua por compatibilidade e vale `true` só com `"done"`. O B casa cada ACK com o comando pelo `cmd_seq` (o `seq` do `CommandPacket` confirmado) e descarta ACK duplicado ou atrasado. A lógica dos estados entra com os comandos (semana 5); a v3 só reserva os campos.
