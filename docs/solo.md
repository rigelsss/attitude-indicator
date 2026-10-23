# ESP B: horizonte no TFT, OLED, encoder e alarmes

Semana 4 do [plano.md](plano.md). O B já recebe a telemetria e conta perdas e quedas do link ([radio.md](radio.md), "ESP B"). Nesta etapa, ele passa a mostrar o horizonte no TFT e os dados no OLED, a trocar de página pelo encoder e a avisar com o LED de alarme. Ele nunca mostra um dado velho ou inválido como atual.

Plano aprovado em 23/10/2026.

## Decisões

- **TFT só com o horizonte** (opção b): céu e terra girando com o roll, escala de pitch, marca de roll, proa e altitude em números e um indicador pequeno do link. Com a atitude inválida ou o dado velho, o horizonte fica riscado, com "SEM LINK" ou "ATT INVÁLIDA". Uma tela de números grandes no TFT pode entrar na folga da semana 7, sem retrabalho.
- **OLED com páginas, trocadas pelo giro do encoder:**
  1. dados: roll, pitch, proa (MAG ou VERD, com a declinação), altitude, pressão, temperatura e taxa de curva (`gz`); `---` com a flag `valid` desligada;
  2. link e saúde do A: estado, recebidos por segundo, perdidos (janela e % total), quedas, atrasados, intervalo entre chegadas, atraso estimado, sensores do A (`status`), calibração em curso e `loop_us`.
- **Botão do encoder:** a pressão curta reconhece o alarme (o LED para de piscar e fica aceso enquanto a condição durar). A pressão longa fica reservada para os comandos da semana 5.
- **LEDs, o meio-termo:**
  - **link no LED azul da placa (GPIO 2)**, sem fiação;
  - **alarme no LED vermelho externo (GPIO 13, 330 Ω)**;
  - o LED verde externo (GPIO 27) saiu do projeto.
- **Buzzer adiado:** é ativo, e não há transistor. A `alarm_task` o deixa previsto no GPIO 26 ([plano.md](plano.md), semana 4). Se a corrente medida em 3,3 V der até ~12 mA, ele pode entrar direto no GPIO.
- **Sprite de 8 bits (RGB332) para a tela inteira** (item 1 da revisão de 15/10):
  - ocupa 57,6 kB, contra 113 kB de um sprite de 16 bits, que não cabe no maior bloco livre do B (107 kB);
  - as cores ficam limitadas a 256 (escolher as que o RGB332 representa exatamente) e as fontes ficam as de bitmap (GLCD, 2 e 4), sem as suavizadas;
  - não há DMA: a TFT_eSPI só usa DMA com sprites de 16 bits. **Medido no passo 0:** o envio leva 28,3 ms (a conversão de 8 para 16 bits soma ~5 ms aos 23,4 ms de uma tela cheia), ~34,7 quadros/s no máximo. Sem DMA, a task que envia fica ocupada durante a transferência, então **a estimativa** é de ~71 % do núcleo 1 a 25 quadros/s e ~57 % a 20 (a duração do envio não é exatamente o tempo de processador; o passo 6 mede). A `link_task` tem prioridade maior, então **a expectativa** é que a recepção não seja afetada, a validar na bancada integrada (passo 6: fila cheia e intervalos no `r` do B). O passo 6 também decide entre 25 e 20 quadros/s;
  - alocar o sprite no início do boot, antes que o heap se fragmente (com o Wi-Fi, o maior bloco livre é de 107 kB);
  - se faltar folga: menos quadros/s, 4 bits com paleta (28,8 kB) ou faixas de 16 bits com DMA. A troca fica restrita à `display_task`.
- **Meta de 25 quadros/s** no TFT.
- **O OLED numa task própria, de baixa prioridade:** um quadro de 128×64 leva **32,7 ms** a 400 kHz (medido no B; no A, 18 ms para 128×32) e derrubaria a taxa do horizonte. A 5 Hz, as chamadas de atualização somam ~164 ms por segundo. Isso não equivale necessariamente a 16 % de CPU, pois inclui a espera pela transferência I2C; o uso efetivo será verificado na bancada integrada.
- **O atraso dos pacotes vem antes do TFT** (pendência da semana 3, [radio.md](radio.md)). O TFT precisa saber se o dado está velho.

## Passos

| Passo | O quê | Task (núcleo, prioridade) |
|---|---|---|
| 0. Montagem e diagnóstico | Uma peça por vez, com o ambiente `ground_diag` (abaixo) | — |
| 1. Alarmes | `lib/alarm_logic`: condições, histerese e padrões do LED de alarme e do LED do link (seção "Passo 1"). Testes `native` | `alarm_task` (0, 2) |
| 2. Encoder | ISR em IRAM, curta: lê CLK e DT e enfileira (`xQueueSendFromISR`). `lib/encoder_decode` decodifica a sequência e filtra os repiques (testes `native`). Botão curto e longo | `input_task` (0, 2) |
| 3. OLED | Páginas de dados e de link, a 5 Hz (quadro de 32,7 ms) | `oled_task` (0, 1) |
| 4. Atraso dos pacotes | B: atraso estimado (chegada − `t_ms`, contra o menor valor visto); acima de um limite, o dado conta como velho. A: não envia com o anterior pendente (pula e conta). **Exige regravar o A** | `link_task` (B) e `radio_task` (A) |
| 5. TFT | Horizonte com o sprite de 8 bits. `lib/pfd_geometry` (roll e pitch → coordenadas, testes `native`) | `display_task` (1, 2) |
| 6. Bancada | Quadros/s, tempo de desenho, heap, efeito do desenho na recepção (`r` do B), alarmes com a caixa inclinada, queda do link | — |

## Passo 0: montagem e diagnóstico

### Ligação

Na NodeMCU-32S, o LED de alarme e o encoder ficam no lado esquerdo, e o TFT e o OLED no direito. Conferir sempre pela serigrafia da placa.

| Peça | Pino do módulo | ESP32 | Observação |
|---|---|---|---|
| LED vermelho (alarme) | ânodo (perna longa) | GPIO 13, via 330 Ω | cátodo no GND |
| LED do link | — | GPIO 2 | o LED azul da placa, sem fiação |
| OLED 0,96" | VCC / GND / SCL / SDA | 3V3 / GND / 22 / 21 | endereço esperado `0x3C` |
| Encoder KY-040 | + / GND / CLK / DT / SW | **3V3** / GND / 32 / 33 / 25 | nunca 5 V: os pull-ups do módulo levariam os 5 V ao GPIO. O botão usa o pull-up interno |
| TFT ST7789 240×240 | VCC / GND | 3V3 / GND | |
| | SCL / SDA | 18 / 23 | clock e dado do **SPI**, e não I2C |
| | RES / DC / CS | 17 / 16 / 5 | o GPIO 5 é pino de strapping; o CS do TFT é só entrada e não deve atrapalhar o boot (item 13 da revisão, conferir na bancada) |
| | BLK | 3V3 | luz de fundo sempre acesa; dispensa medir a corrente |
| *(buzzer, adiado)* | | *GPIO 26* | |

- **Protoboards:** duas lado a lado, com o ESP por cima da divisão entre elas e uma fileira de pinos em cada uma. O 3V3 e o GND do ESP vão para as trilhas laterais das duas.
- **Cuidado:** desconectar a USB antes de ligar ou mexer em fios. Depois de mexer com a placa ligada, um módulo pode ficar mudo até a USB sair por ~5 s.

### Ambiente `ground_diag`

Separado do firmware, em `src/diag/`, sem tasks, como o `i2cscan` do A. Também passa pelo `check_board_mac.py`.

| Tecla | Teste |
|---|---|
| `1` / `2` | liga e desliga o LED do link (GPIO 2) e o de alarme (GPIO 13) |
| `i` | scan do I2C (21/22) e conferência do `0x3C` |
| `o` | OLED: texto e um padrão de pixels |
| `e` | fluxo do encoder: contagem, sentido, botão (curto/longo) e repiques descartados |
| `t` | TFT: cores cheias, texto e tempo para mandar uma tela inteira |
| `s` | sprite de 8 bits 240×240: cria, desenha, envia, mede o tempo e mostra o heap antes e depois |
| `h` | heap livre e maior bloco |

### Rodadas (uma peça por vez, um log cada)

1. **LEDs:** `1` e `2` acendem e apagam cada um.
2. **OLED:** o `i` mostra `0x3C`, e o `o` desenha na tela.
3. **Encoder:** no `e`, os passos de quadratura mudam sempre no mesmo sentido para cada lado (horário positivo), e o botão separa curto e longo. Com muitos repiques, testar um capacitor de 100 nF. **Na bancada:** 2 passos por clique, com o repouso alternando entre `11` e `00` ([hardware.md](hardware.md)).
4. **TFT:** no `t`, as cores certas (sem vermelho trocado com azul e sem tela espelhada) e o tempo de uma tela (previsto ~23 ms). O `s` informa o heap e o tempo do sprite.

Os resultados vão para o [hardware.md](hardware.md).

## Passo 1: alarmes

`lib/alarm_logic` (testes `native`) e `src/tasks/alarm_task.cpp` (núcleo 0, prioridade 2, volta de 20 ms). A task lê o último pacote aceito da `link_task` e a hora de chegada; a idade acima de `LINK_TIMEOUT_MS` é link perdido, a mesma regra do timeout da `link_task`.

**Limites, pelos critérios de aviação:**

- **Alarme (warning), atitude anormal:** bank > 45°, pitch > +25° ou pitch < −10°. É a definição de "upset" do *Airplane Upset Recovery Training Aid*, também usada pela FAA (AC 120-111). O pitch é assimétrico: nariz para baixo é mais perigoso.
- **Aviso (caution) de bank:** > 35°, o início do "BANK ANGLE" do EGPWS em aeronaves como o 737.
- **Histerese de 3°** em todos os limites (o aviso sai abaixo de 32°, o alarme abaixo de 42° de bank, +22° e acima de −7° de pitch). Os limites entram na hora: o filtro já é suave, e o aviso deve ser imediato.

| Prioridade | Condição | LED de alarme (GPIO 13) | LED do link (GPIO 2) |
|---|---|---|---|
| — | Normal | apagado | aceso |
| — | Sem sinal (antes do primeiro pacote) | apagado, sem alarme | apagado |
| 1 | Link perdido (depois de ter funcionado) | 1 Hz | apagado |
| 2 | Atitude inválida por mais de 1 s, fora das calibrações (`ST_CALIBRATING`) | duas piscadas curtas a cada 2 s | aceso |
| 3 | Atitude anormal | 5 Hz | aceso |
| 4 | Aviso de bank | 2 Hz | aceso |
| — | Reconhecido (tecla `k`; botão do encoder no passo 2) | aceso fixo enquanto a mesma condição durar | — |

- Os limites só são avaliados com o link OK e a atitude válida. Sem isso, a histerese recomeça do zero quando a atitude voltar. A flag `VAL_ATT` ligada com roll ou pitch não finito (pacote incoerente) conta como atitude inválida.
- **As piscadas começam acesas** e contam a fase a partir da mudança de condição: um alarme novo aparece na hora. Com a fase contada desde o boot, as duas piscadas da atitude inválida podiam atrasar até ~1,7 s (revisão de 23/10/2026).
- **Mudanças de alarme descartadas** com a fila cheia são contadas e avisadas na serial (`[alarme] N mudanças descartadas`), e o `c` mostra o total, como os eventos do link.
- Uma condição diferente (inclusive o alarme caindo para o aviso), ou a mesma voltando depois de terminar, alarma de novo.
- **Buzzer:** os mesmos ritmos do LED, desligado por `ALARM_BUZZER_ENABLED 0`.
- **Console do B:** `[alarme] … ATITUDE ANORMAL (roll …, pitch …, valid …, status …)`, `terminou`, `reconhecido`; o `c` mostra o alarme atual e os limites.
- No passo 4, o dado velho (atraso estimado) entra na condição 1.
