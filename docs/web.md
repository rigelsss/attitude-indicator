# Página web, WebSocket e comandos

Semana 5 do [plano.md](plano.md). Pelo AP "Horizonte-ESP" do ESP B, uma página mostra o horizonte e os dados ao vivo e envia comandos ao ESP A, com a confirmação (ACK) de cada um. Contrato JSON em [protocolo-websocket.md](protocolo-websocket.md); contrato binário em `firmware/shared/include/telemetry.h` (v4, sem mudança nesta etapa).

```
Navegador ──HTTP :80 (página, LittleFS)──▶ ESP B
Navegador ◀──WebSocket :81 (JSON, 30 Hz)── ESP B ◀──ESP-NOW (telemetria)── ESP A
Navegador ──{"cmd":…}──▶ ESP B ──CommandPacket──▶ ESP A ──AckPacket──▶ ESP B ──{"ack":…}──▶ Navegador
```

Plano aprovado em 23/10/2026.

## Decisões

- **Camada única de comandos no A** (item 8 da revisão de 15/10): o comando do rádio entra na mesma fila das teclas do console e é executado pela `sensor_task` pelo mesmo caminho e com as mesmas regras.
- **ACK:** `ACCEPTED` ao começar, `DONE` ao terminar nos comandos demorados, `REJECTED` com o motivo na serial do A. O mesmo `seq` recebido de novo (reenvio do B) reenvia o último ACK, sem executar de novo.
- **`SET_RATE_HZ`:** só 10, 20, 25 ou 50 Hz (período inteiro em ms); o resto é recusado (segunda parte do item 3).
- **Reenvio no B:** até 3 vezes, a cada 300 ms, com o mesmo `seq`, enquanto não chega `ACCEPTED` ou `REJECTED`.
- **Calibração do magnetômetro pelo rádio:** grava sozinha se passar nos critérios de aceitação do console (resíduo, raio, escalas) e responde `DONE`; senão, descarta e responde `REJECTED`.
- **Zero operacional da atitude** (`CMD_ZERO_ATTITUDE`): a posição atual vira roll = pitch = 0 **até o próximo boot**, sem gravar na NVS (um zero errado some num reinício). Não substitui a calibração de montagem (`z`).
- **Pressão longa do encoder, conforme a página do OLED** (revisão da decisão em 23/10/2026, para zerar a atitude sem o PC): na página 1 (dados), zera a **atitude**; na página 2 (link e saúde), zera a **altitude**. A página web também terá os botões.
- **Senha do AP** (item 15): fica `horizonte123` no `config.h`, visível no repositório, por ser um projeto de bancada.
- **Identificador de boot no contrato:** adiado; os comandos não exigem mudar o contrato.
- **`rssi` sai do JSON:** o callback do ESP-NOW desta versão do núcleo não informa a intensidade do sinal.
- **Página sem bibliotecas externas** (o AP não tem internet): HTML, CSS e JS próprios, horizonte num `<canvas>`.

## Passos

1. **A:** camada única de comandos e ACK.
2. **B:** envio do comando com reenvio, recepção e casamento do ACK (`lib/command_link`), pressão longa do encoder e teclas no console.
3. **B:** servidor HTTP (LittleFS) e WebSocket com a telemetria em JSON e os comandos (`web_task`).
4. **Página** em `ground-station/web`.
5. **Bancada.**

## Passo 1 (A): camada única de comandos

- **Caminho:** o callback de recepção confere o `CommandPacket` (`decodeCommand`) e o coloca na fila da `sensor_task` (`SensorCmd` com a origem `rádio`, o comando, o argumento e o `seq`), a mesma das teclas. A `sensor_task` executa em `src/radio_commands.cpp`, com acesso direto aos serviços, como o console. Comando aceito que não cabe na fila é contado (`fila cheia`, no `c` do A) e o B o reenvia por falta de ACK.
- **`lib/cmd_tracker`** (testes `native`): o mesmo `seq` (o comando em curso ou um resultado ainda retido) devolve o ACK atual, sem executar de novo; um segundo comando com um demorado em curso é recusado; o demorado expira no tempo limite.
  - **Retenção por tempo** (revisão de 23/10/2026): cada resultado fica retido pelo tempo em que o B pode pedi-lo de novo. O de um comando **demorado**, por `CMD_RETENTION_LONG_MS` (210 s): o B o reconsulta enquanto espera o DONE, por até `CMD_COMPLETION_MS` (200 s), e a retenção cobre essa janela inteira, com margem (com 30 s, um link fora por mais de 30 s depois do DONE faria a consulta tardia executar a calibração de novo; revisão de 23/10/2026). Os dois valores ficam comentados nos dois `config.h`; o **imediato**, inclusive as recusas, por `CMD_RETENTION_SHORT_MS` (3 s), porque só os reenvios do B (~1 s) o pedem de novo. Com uma retenção única de 30 s, a tecla `g` repetida ~4 vezes por segundo lotou o histórico em ~5 s, e os comandos ficaram recusados por até 30 s (bancada de 23/10, [hardware.md](hardware.md)). Antes eram os 4 últimos: se o ACK de um comando se perdesse e outros quatro terminassem antes do reenvio, o primeiro voltaria a parecer novo e seria executado de novo (uma taxa antiga restaurada, por exemplo).
  - **Capacidade:** até 16 resultados retidos. Com todos ocupados por resultados ainda dentro da janela, o comando novo é recusado (`histórico de comandos cheio`) e não guardado, em vez de apagar um resultado que ainda pode ser consultado. Com um comando demorado em curso, um lugar fica reservado para o resultado dele. **A recusa por histórico cheio não é guardada** (`refuse`; revisão de 23/10/2026): antes, ela ocupava o último lugar livre, justamente o reservado, e o resultado do comando em curso se perdia se ele terminasse antes de alguma expiração. Um reenvio do mesmo `seq` é avaliado de novo.
- **Resultado de cada comando:**

| Comando | ACK | Termina com | Recusado quando |
|---|---|---|---|
| `CMD_CALIBRATE_GYRO` | ACCEPTED, depois DONE ou REJECTED | o bias aceito (DONE) ou recusado (caixa mexida); até `CMD_GYRO_TIMEOUT_MS` (30 s) | seis posições, montagem ou magnetômetro em curso |
| `CMD_CALIBRATE_MAG` | ACCEPTED, depois DONE ou REJECTED | na revisão, grava sozinho se o ajuste passar nos critérios (DONE), senão descarta (REJECTED); `x` no console cancela (REJECTED); até `MAG_CAL_MS` + 10 s | magnetômetro ausente ou outra calibração em curso |
| `CMD_ZERO_ALTITUDE` | ACCEPTED, depois DONE | o zero aceito (o recusado mede de novo sozinho); até `CMD_BARO_TIMEOUT_MS` (20 s) | barômetro ausente ou zero já em curso |
| `CMD_ZERO_ATTITUDE` | DONE ou REJECTED na hora | — | atitude inválida pelo critério da telemetria (sem `VAL_ATT`, que inclui a idade da última amostra, ou com `VAL_ATT_DEGRADED`), ou \|roll\| ou \|pitch\| acima de `ZERO_ATT_MAX_DEG` (10°) |
| `CMD_SET_RATE_HZ` | DONE ou REJECTED na hora | — | taxa fora de 10, 20, 25 e 50 Hz |

- **No tempo limite:** REJECTED. A coleta do magnetômetro é cancelada; a calibração do giroscópio e o zero continuam pela serial (o resultado aparece nas mensagens `[cal]` e `[baro]`, sem novo ACK).
- **Zero operacional:** o roll e o pitch da atitude no instante do comando são subtraídos **só da telemetria** (o `a` e o `c` do A continuam mostrando os ângulos sem o zero), até o próximo boot. A subtração dos ângulos é aproximada; por isso o limite de 10°, e a montagem continua sendo corrigida pelo `z`.
- **ACKs:** a `sensor_task` os enfileira (`radioSendAck`), e a `radio_task` os envia **no lugar da telemetria** do ciclo, sob a mesma regra de um envio em voo; a amostra daquele ciclo conta como perdida no B (o `seq` avança). O cabeçalho do ACK usa um contador próprio do A.
- **Revisão de 23/10/2026:**
  - **calibração do giroscópio recusada sem bias deste boot:** o fluxo volta ao tipo Boot antes de devolver o evento `RejectedKeptBootRestart`, e a recusa não era registrada (o B ficava em "aceito" até os 30 s). Agora a classificação é pelo evento; só o `Accepted` confere o tipo. Sem teste `native` (o `CalService` usa a NVS e o logger): conferir na bancada com a calibração do boot ainda não aceita;
  - **zero da atitude com o INT parado:** `initialized()` e `degraded()` continuavam como estavam, e o zero seria feito com ângulos parados. Agora vale o critério da telemetria (`VAL_ATT`);
  - as reconsultas respondidas com ACCEPTED não são impressas na serial do A (numa coleta de 3 min, seriam ~90 linhas).
- **Serviços:** três consultas novas, sem mudar o comportamento do console: `CalService::gyroCmdOutcomes()`/`gyroCmdAccepted()`, `BaroService::zeroAccepts()` e `MagService::decideReview()` (a mesma gravação do `s`, decidida pelos critérios).
- **Console do A:** mensagens `[cmd]`; o `c` mostra a taxa em uso, os comandos aceitos, recusados e sem lugar na fila, e os ACKs enviados.

## Passo 2 (B): comandos e ACK no rádio

- **`lib/command_link`** (testes `native`): seq (o primeiro aleatório a cada boot: um B reiniciado não repete os seq que o A lembra), reenvio a cada `CMD_RETRY_MS` (300 ms) até `CMD_MAX_TRIES` (3) envios, casamento do ACK pelo `cmd_seq` e pelo comando (repetido, atrasado ou desconhecido é ignorado), `sem resposta` depois do último envio e `sem conclusão` com ACCEPTED sem DONE em `CMD_COMPLETION_MS` (200 s). Até 4 comandos ao mesmo tempo; o quinto é recusado no próprio B.
  - **Reconsulta do comando aceito** (revisão de 23/10/2026): depois do ACCEPTED, o B reenvia o mesmo `seq` a cada `CMD_POLL_MS` (2 s), e o A devolve o resultado atual sem executar de novo. Um DONE perdido é recuperado na consulta seguinte; antes, o B esperava os 200 s e mostrava "sem conclusão" com o comando concluído no A. Cada consulta ocupa um ciclo de telemetria no A (1 em 100 pacotes a 50 Hz). As consultas não geram evento na serial do B; o `c` as conta.
- **Recepção:** o callback separa pelo tipo do cabeçalho: telemetria para a `link_task`, ACK (`decodeAck`) para a `command_task`.
- **`command_task`** (núcleo 0, prioridade 2): recebe os pedidos, envia (`esp_now_send` para o A), casa os ACKs e publica cada mudança de estado para a serial.
- **Pedidos:** as teclas do console do B (`g`, `m`, `z`, `0`, `f` para a próxima taxa em 50 → 25 → 20 → 10 Hz e `X` para uma taxa inválida, que testa a recusa) e a **pressão longa do encoder**: página 1 do OLED = zerar a atitude, página 2 = zerar a altitude (`encoderLongPressCommand`). Com o botão apertado, a última linha do OLED mostra o que vai acontecer (`segure 1s: ZERO ATT` ou `ZERO ALT`); depois, o andamento do comando (`ENVIADO`, `ACEITO`, `OK`, `RECUSADO`, `SEM RESP.`, `SEM FIM`, `OCUPADO`), com o resultado por `OLED_CMD_FOOTER_MS` (4 s). A página entra no passo 3.
- **Console do B:** `[cmd] … 'zerar altitude' (encoder, seq N) enviado`, `sem ACK: reenvio 2 de 3`, `aceito`, `concluído`, `recusado`, `sem resposta`; o `c` mostra pedidos, envios, ACKs casados e ignorados e recusas no próprio B.
- **Bancada** ([hardware.md](hardware.md), "Comandos do B para o A pelo console"): zerar atitude, taxa válida e inválida, calibração do giroscópio aceita e recusada e o "sem resposta" com o A desligado funcionaram. Pendentes: zerar altitude e pressão longa (próxima bancada) e a calibração do magnetômetro pelo rádio, num lugar aberto, quando o B tiver um power bank próprio.
