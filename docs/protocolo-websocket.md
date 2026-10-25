# Protocolo WebSocket (ESP B ↔ navegador e cliente Python)

Endpoint: `ws://192.168.4.1:81/` (IP padrão do AP do ESP32). Página: `http://192.168.4.1/`. Montagem e conferência das mensagens no ESP B: `firmware/ground/lib/web_json` (testes `native`). Revisado em 23/10/2026, com a implementação ([web.md](web.md), passo 3).

Todas as mensagens são objetos JSON com o campo `type`. Um número não finito vai como `null`.

## B → clientes

### `hello` (na conexão)

```json
{ "type": "hello", "decl": -20.7, "pin": true, "rates": [10, 20, 25, 50] }
```

- `decl`: declinação magnética do local (`MAG_DECLINATION_DEG` no `config.h` do B, negativa para oeste). Proa verdadeira = `hdg + decl`, levada a 0..360.
- `pin`: os comandos exigem o PIN (`WEB_CMD_PIN`).
- `rates`: taxas aceitas pelo `set_rate`.
- Logo depois do `hello`, o B manda o **último estado dos até 8 comandos mais recentes** (mensagens `cmd`, do mais antigo ao mais novo), de qualquer origem. Assim, uma página recarregada ou reconectada mostra o resultado de um comando que terminou enquanto ela estava fora.

### `tel` (telemetria, `WS_RATE_HZ` = 30 Hz, só com clientes conectados)

```json
{
  "type": "tel", "cur": true,
  "t": 123456, "seq": 4321,
  "roll": -3.2, "pitch": 1.8, "hdg": 274.5,
  "gx": 0.1, "gy": -0.3, "gz": 0.0,
  "alt": 1.2, "hpa": 1013.2, "temp": 26.4,
  "status": 55, "valid": 13, "decl": -20.7,
  "link": { "state": "ok", "age_ms": 12, "delay_ms": 4, "rx_hz": 50, "lost": 3, "loss_pct": 0.06, "drops": 0 },
  "alarm": { "kind": "none", "acked": false }
}
```

- **`cur`: há dado atual.** É a regra única do B (`linkSnapshot`), a mesma do alarme, do OLED e do TFT: link vivo (último pacote com até 500 ms) e idade estimada do dado (atraso na chegada + tempo desde a chegada) até 250 ms. Com `cur` falso, o cliente mostra tudo como indisponível, mesmo com os números presentes.
- Os campos do pacote (`t` a `valid`) só vão depois do primeiro pacote do A. `hdg` é a proa **magnética**.
- **`link.state`:** `no_signal` (nenhum pacote desde o boot do B), `lost` (sem pacote há mais de 500 ms), `late` (dado atrasado) ou `ok`. `age_ms` e `delay_ms` vêm com o primeiro pacote; `rx_hz`, `lost` (desde o boot), `loss_pct` e `drops` (quedas), com o primeiro resumo de 1 s.
- **`alarm.kind`:** `none`, `bank_caution` (bank > 35°), `attitude` (atitude anormal), `att_invalid`, `data_late` ou `link_lost` ([solo.md](solo.md), passo 1). `acked`: reconhecido (botão do encoder ou tecla `k`).
- O `rssi` saiu do contrato: o callback do ESP-NOW desta versão do núcleo não informa a intensidade do sinal. O `loop_us` do pacote não entra no JSON.

#### `status` e `valid` (contrato v4, `TELEMETRY_VERSION` 4)

Os dois são números (bitmask), copiados do `TelemetryPacket` ([telemetry.h](../firmware/shared/include/telemetry.h)):

- **`status`** diz se cada sensor responde e se há calibração (`ST_MPU_OK` 1, `ST_MAG_OK` 2, `ST_BARO_OK` 4, `ST_CALIBRATING` 8, `ST_GYRO_CAL` 16, `ST_MAG_CAL` 32). Não diz se o dado vale agora.
- **`valid`** diz se cada dado pode ser mostrado:

| Bit | Valor | Flag | Liga quando | Na página |
|---|---|---|---|---|
| 0 | 1 | `VAL_ATT` | Filtro de atitude inicializado e última amostra do MPU com até 50 ms | Desligada: bandeira de falha no horizonte; roll, pitch e curva "---" |
| 1 | 2 | `VAL_ATT_DEGRADED` | Com `VAL_ATT`: giroscópio saturou e a correção pelo acelerômetro está se recuperando | `ATT DEGRADADA` em âmbar |
| 2 | 4 | `VAL_ALT` | Barômetro ok, zero válido, última leitura com até 200 ms e altitude finita | Desligada: altitude, pressão e temperatura "---" |
| 3 | 8 | `VAL_HDG` | Filtro da proa inicializado, `VAL_ATT` e magnetômetro calibrado | Desligada: proa "---" |
| 4 | 16 | `VAL_HDG_GYRO_ONLY` | Com `VAL_HDG`: correção pelo magnetômetro suspensa (atitude degradada, distúrbio, inclinação acima de 70°) ou magnetômetro sem leitura há mais de 200 ms | Proa em âmbar, com "GIRO" (`~` no horizonte) |

No exemplo, `valid` = 13 = `VAL_ATT` + `VAL_ALT` + `VAL_HDG`.

### `cmd` (andamento de um comando, para todos os clientes)

```json
{ "type": "cmd", "cmd": "set_rate", "arg": 25, "seq": 41850, "state": "accepted", "origin": "página" }
```

- `state`: `sent` (enviado ao A; repetido a cada reenvio), `accepted` (em curso no A), `done`, `rejected` (recusado pelo A; o motivo fica na serial do A), `no_response` (nenhum ACK depois de 3 envios), `no_completion` (aceito sem conclusão em 200 s) ou `busy` (o B já tem 4 comandos em curso).
- `origin`: `página`, `console` ou `encoder`. Todos os clientes veem os comandos de todas as origens.
- `seq`: o `seq` do `CommandPacket`; as mensagens de um mesmo comando têm o mesmo.
- **Entrega:** as mensagens `hello` e `cmd` não se perdem por um congestionamento curto (o B as guarda numa fila por cliente e as manda antes da telemetria); a `tel` pode faltar, porque a próxima a substitui. Um cliente que fica 2 s sem receber nada (o B escreve no socket sem esperar; ver [web.md](web.md)) é desconectado e, ao reconectar, recebe os comandos recentes.

**Recusa no próprio B** (só para quem pediu, sem `seq`):

```json
{ "type": "cmd", "cmd": "zero_attitude", "state": "refused", "reason": "PIN incorreto" }
```

Motivos: `PIN incorreto`, `mensagem inválida`, `comando desconhecido`, `argumento inválido`, `fila de comandos cheia`.

## Clientes → B (comandos)

```json
{ "cmd": "calibrate_gyro", "pin": "2468" }
{ "cmd": "calibrate_mag", "pin": "2468" }
{ "cmd": "zero_attitude", "pin": "2468" }
{ "cmd": "zero_altitude", "pin": "2468" }
{ "cmd": "set_rate", "arg": 25, "pin": "2468" }
```

- **Todos veem; só quem envia o PIN certo comanda** (decisão de 23/10/2026). O PIN fica no `config.h` do B (`WEB_CMD_PIN`); vazio desliga a exigência. O WebSocket não é criptografado: o PIN protege contra o uso acidental, não contra quem capture o tráfego.
- O B confere o JSON, o nome do comando, o PIN e, no `set_rate`, que o `arg` é inteiro. A faixa da taxa (10, 20, 25 ou 50 Hz) e as outras condições (calibração em curso, inclinação do zero, sensores) são conferidas pelo A, que responde `rejected`.
- O B traduz para `CommandPacket` (ESP-NOW), reenvia sem ACK, reconsulta o aceito e casa os ACKs pelo `cmd_seq` ([web.md](web.md), passo 2).
