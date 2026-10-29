# ESP B: horizonte no TFT, OLED, encoder e alarmes

Semana 4 do [plano.md](plano.md). O B já recebe a telemetria e conta perdas e quedas do link ([radio.md](radio.md), "ESP B"). Nesta etapa, ele passa a mostrar o horizonte no TFT e os dados no OLED, a trocar de página pelo encoder e a avisar com o LED de alarme. Ele nunca mostra um dado velho ou inválido como atual.

Plano aprovado em 23/10/2026.

## Decisões

- **TFT só com o horizonte** (opção b): céu e terra girando com o roll, escala de pitch, marca de roll, proa e altitude em números e um indicador pequeno do link. Com a atitude inválida ou o dado velho, o horizonte fica riscado, com "SEM LINK" ou "ATT INVÁLIDA". Uma tela de números grandes no TFT pode entrar na folga da semana 7, sem retrabalho.
- **OLED com páginas, trocadas pelo giro do encoder:**
  1. dados: roll, pitch, proa (MAG ou VERD, com a declinação), altitude, pressão, temperatura e taxa de curva (`gz`); `---` com a flag `valid` desligada;
  2. link e saúde do A: estado, recebidos por segundo, perdidos (janela e % total), quedas, atrasados, intervalo entre chegadas, atraso estimado, sensores do A (`status`), calibração em curso e `loop_us`.
- **Botão do encoder:** a pressão curta reconhece o alarme (o LED para de piscar e fica aceso enquanto a condição durar). A pressão longa, desde a semana 5, zera a atitude na página 1 do OLED e a altitude na página 2 ([web.md](web.md)).
- **LEDs, o meio-termo:**
  - **link no LED azul da placa (GPIO 2)**, sem fiação;
  - **alarme no LED vermelho externo (GPIO 13, 330 Ω)**;
  - o LED verde externo (GPIO 27) saiu do projeto.
- **Buzzer adiado:** é ativo, e não há transistor. A `alarm_task` o deixa previsto no GPIO 26 ([plano.md](plano.md), semana 4). Se a corrente medida em 3,3 V der até ~12 mA, ele pode entrar direto no GPIO. **29/10/2026:** decidido ligar pelo BC337 ([caixa-b.md](caixa-b.md), "Buzzer").
- **Montagem na caixa (29/10/2026, a implementar):** segundo OLED no `Wire1` com uma página fixa em cada OLED, no lugar das páginas trocadas pelo encoder, e anel de LEDs na tampa. Decisões em [caixa-b.md](caixa-b.md).
- **Sprite de 8 bits (RGB332) para a tela inteira** (item 1 da revisão de 15/10):
  - ocupa 57,6 kB, contra 113 kB de um sprite de 16 bits, que não cabe no maior bloco livre do B (107 kB);
  - as cores ficam limitadas a 256 (escolher as que o RGB332 representa exatamente) e as fontes ficam as de bitmap (GLCD, 2 e 4), sem as suavizadas;
  - não há DMA: a TFT_eSPI só usa DMA com sprites de 16 bits. **Medido no passo 0:** o envio leva 28,3 ms (a conversão de 8 para 16 bits soma ~5 ms aos 23,4 ms de uma tela cheia), ~34,7 quadros/s no máximo. Sem DMA, a task que envia fica ocupada durante a transferência, então **a estimativa** é de ~71 % do núcleo 1 a 25 quadros/s e ~57 % a 20 (a duração do envio não é exatamente o tempo de processador; o passo 6 mede). A `link_task` tem prioridade maior, então **a expectativa** é que a recepção não seja afetada, a validar na bancada integrada (passo 6: fila cheia e intervalos no `r` do B). O passo 6 também decide entre 25 e 20 quadros/s;
  - alocar o sprite no início do boot, antes que o heap se fragmente (com o Wi-Fi, o maior bloco livre é de 107 kB);
  - se faltar folga: menos quadros/s, 4 bits com paleta (28,8 kB) ou faixas de 16 bits com DMA. A troca fica restrita à `display_task`.
- **25 quadros/s no TFT, decidido em 23/10/2026** pela bancada do passo 5 ([hardware.md](hardware.md), "Horizonte no TFT"): maior quadro de 30–32 ms (período de 40 ms) e recepção sem mudança (`fila cheia 0`, intervalos e atraso como antes). Os 20 quadros/s só fariam sentido se a recepção sofresse.
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

## Passo 2: encoder

`lib/encoder_decode` (testes `native`) e `src/tasks/input_task.cpp` (núcleo 0, prioridade 2).

- **Giro:**
  - uma **ISR em IRAM**, registrada pelo ESP-IDF (`gpio_isr_handler_add`, como a do INT no A), dispara nas duas bordas do CLK e do DT, lê os dois pinos numa só leitura do `GPIO_IN1` e **só enfileira** o estado (fila de `ENC_QUEUE_LEN` = 64; com 32, o repique de um giro rápido a encheu uma vez). Com a fila cheia, conta o descarte;
  - a task decodifica: cada transição válida soma ±1, e um clique sai quando a soma chega a ±2 num repouso (`11` ou `00`). O repique (ida e volta) se cancela. A transição que pula um estado é contada como inválida e **zera a soma**: a contagem só recomeça no próximo repouso, e um passo parcial não se junta a passos do outro lado dela (revisão de 23/10/2026). O sentido vem de `ENC_DIR` (horário positivo);
  - **lacuna na fila:** com a fila cheia, a ISR descarta o estado e, no primeiro item que couber depois, enfileira um **marcador de lacuna**. O marcador é copiado na pilha da ISR antes de entrar na fila: uma constante em `.flash.rodata` não pode ser lida com o cache desligado, como durante uma gravação na flash (conferido no binário: `movi` para a pilha, sem leitura da flash). A task ressincroniza exatamente ali: descarta o clique parcial e recomeça no próximo repouso. Sem isso, estados perdidos podiam inverter o sentido (um giro horário lido como 11 → 10 → 00 vira um clique anti-horário), e não só perder um clique;
  - **cada clique troca a página do OLED** (horário = próxima, anti-horário = anterior, dando a volta em `OLED_PAGES` = 2). A página fica numa caixa de correio, lida pela `oled_task` no passo 3.
- **Botão, por consulta periódica** (decisão de 23/10/2026, no lugar da interrupção prevista): a task o lê a cada 5 ms, com 20 ms de estabilidade. Uma interrupção receberia dezenas de bordas por aperto, por causa do repique.
  - **curto** (solto antes de 1 s): reconhece o alarme, o mesmo caminho da tecla `k`. Com a fila de reconhecimento cheia, a serial avisa `botão curto descartado`;
  - **longo** (`BTN_LONG_MS` = 1 s, avisado ainda com o botão apertado, sem curto ao soltar): reservado para os comandos da semana 5. Um segundo, e não os 600 ms do diagnóstico, para os comandos de calibração não dispararem sem querer.
- **Console:** `[enc] clique horário, página 2/2`, `[enc] botão curto: reconhecer o alarme`, `[enc] pressão longa (reservada …)`. O `c` mostra a página, os cliques de cada sentido, as transições inválidas, a fila da ISR cheia (com as lacunas tratadas) e os eventos descartados.

## Passo 3: OLED

`lib/oled_view` (testes `native`) e `src/tasks/oled_task.cpp` (núcleo 0, prioridade 1). A task é a **única dona do `Wire`**: o OLED é o único dispositivo no I2C do B.

- **Duas páginas**, trocadas pelo giro do encoder. Fonte 6×10: 6 linhas de 21 caracteres, só ASCII.

  ```
  DADOS             1/2      LINK OK 50/s      2/2
  ROLL +12.3 PIT  -4.5      perd 1 0.02% q0
  PROA 095 MAG              interv 12/20/28 ms
  ALT  +0.25 m              MPU ok BAR ok MAG ok
  1009.3hPa  25.9C          cal: - loop 180us
  CURVA   +3.2 g/s          ALARME nenhum
  ```

- **Regras de exibição:**
  - sem link (antes do primeiro pacote, ou idade acima de `LINK_TIMEOUT_MS`): o título vira uma faixa em vídeo inverso, `SEM SINAL` ou `LINK PERDIDO`, e os valores da página 1 vão para `---`. A página 2 mantém as estatísticas e a saúde do A do último pacote (`?` antes do primeiro);
  - `VAL_ATT` desligada, ou ângulo não finito: roll, pitch e curva em `---`. `VAL_HDG`: proa. `VAL_ALT`: altitude **e também pressão e temperatura** (revisão de 23/10/2026). O `ST_BARO_OK` só diz que o sensor responde; a `VAL_ALT` inclui a idade da leitura, e uma pressão parada não fica na tela. Consequência aceita: pressão e temperatura só aparecem depois do zero da altitude, ~4 s depois do boot do A. Mostrá-las antes exigiria uma flag de leitura recente no contrato;
  - `VAL_ATT_DEGRADED` (giroscópio saturou, correção se recuperando): o título da página 1 vira `ATT DEGRADADA`, para a atitude não parecer a normal;
  - `VAL_HDG_GYRO_ONLY`: proa com `~` (`095~MAG`), porque segue só pelo giroscópio e pode derivar;
  - `ST_CALIBRATING`: `cal: SIM` na página 2.
- **Proa magnética (MAG)**, como nos instrumentos de cabine (decisão de 23/10/2026). `OLED_HEADING_TRUE 1` mostra a verdadeira (VERD = magnética + `MAG_DECLINATION_DEG`, −20,7° na bancada).
- **Linhas compactas para caber com os valores reais:** as perdas como `perd 1049 17.0% q11` (a rodada de distância de 23/10 deu 1 049 perdidos, 17 % e 11 quedas) e a calibração como `cal: SIM` (com `loop 65535us`, `EM CURSO` passaria de 21 colunas). Os testes conferem as linhas com os valores extremos.
- **Atualização:** a task dá uma volta a cada 50 ms e redesenha a 5 Hz (`OLED_RATE_HZ`) ou **logo que a página muda**. As chamadas de atualização somam ~164 ms por segundo.
- **Presença:** o endereço `0x3C` é conferido antes de iniciar e antes de cada quadro (o `sendBuffer` da U8g2 não informa erro). Ausente, a task tenta iniciar de novo a cada `OLED_RETRY_MS` (5 s). A serial avisa `[oled] AUSENTE` e `[oled] presente`; o `c` mostra a página, o último quadro e o total de quadros.
- **Tela de boot:** `Horizonte - ESP B` / `aguardando o A`, por 1 s.
- **No passo 4**, o atraso estimado do dado entra na página 2.

## Passo 4: atraso dos pacotes

Pendência da semana 3 ([radio.md](radio.md), "Passo 5"): com o link ruim, os pacotes ficavam presos no A e chegavam em rajadas, até ~1 s atrasados, e a idade do B (medida desde a chegada) não mostrava isso. Decisões de 23/10/2026.

- **A: um envio em voo por vez** (`radio_task`). Um indicador fica ligado do `esp_now_send` até o callback de envio (entregue ou falha MAC); com ele ligado, o ciclo é **pulado** e contado (`pulados` no `r` e no `c` do A). O pacote não entra mais na fila do ESP-NOW atrás do anterior: o dado velho é descartado em vez de atrasado.
  - **O `seq` avança no pulo:** o B conta a amostra não entregue como perdida.
  - **Nunca liberado só pelo tempo** (revisão de 23/10/2026): o callback do ESP-NOW não identifica o pacote, e um callback atrasado do envio antigo liberaria o novo, deixando dois em voo. Sem callback por `RADIO_INFLIGHT_TIMEOUT_MS` (100 ms), o episódio só é contado (`sem callback`, no `c` do A) e o A continua pulando. Preso por `RADIO_INFLIGHT_STUCK_MS` (1 s), o A **reinicia o ESP-NOW** (`esp_now_deinit` e `init`, com o peer): o envio pendente e o callback dele são descartados, e o próximo envio começa limpo (`ESP-NOW reiniciado`, no `c`). A regra fica em `lib/send_gate`, com o teste da sequência "tempo limite → novo envio → callback antigo". Nada disso deveria acontecer.
  - Sem o B, cada envio espera as retransmissões acabarem antes do próximo, e a taxa de envio cai.
- **B: atraso estimado** (`lib/delay_estimator`, testes `native`). Para cada pacote aceito, offset = chegada no B − `t_ms` do A; o atraso é o offset menos o menor offset numa janela de 10 faixas de 1 s. A janela acompanha a deriva entre os cristais (erro abaixo de 0,5 ms) e recomeça numa ressincronização (A reiniciado). **Limitação:** se todos os pacotes da janela chegarem atrasados, a base sobe e o atraso fica subestimado.
  - A `link_task` calcula o atraso e o publica junto com o pacote (`RxItem.delayMs`).
  - **Idade estimada do dado acima de `LINK_STALE_DELAY_MS` (250 ms, metade do timeout): dado atrasado**, tratado como sem dado atual. A idade é o atraso na chegada **mais** o tempo desde a chegada (`dataAgeMs`; revisão de 23/10/2026): 240 ms de atraso seguidos de 490 ms sem pacote novo dão 730 ms, embora cada parte, sozinha, passasse pelos limites. **Consequência:** numa queda do link, o `DADO ATRASADO` aparece aos 250 ms sem pacote, e o `LINK PERDIDO` aos 500 ms;
    - alarme **`DADO ATRASADO`**, no ritmo do link perdido (1 Hz), logo abaixo dele na prioridade. O LED do link continua aceso, porque o link está vivo;
    - OLED com a faixa `DADO ATRASADO` e `---` na página 1;
    - o TFT, no passo 5, segue a mesma regra.
  - **Console do B:** o `r` mostra `atraso N (máx N) ms`; o `a`, o atraso de cada pacote; o `c`, o limite e a janela. Para não confundir, o pacote que chega depois de um mais novo passou a aparecer como `fora de ordem` (antes, `atrasados`).
  - **OLED, página 2:** a linha do intervalo virou `int 12/20/28 at 3` (mín/médio/máx do intervalo; "at" = atraso do último pacote, em ms). O atraso sempre aparece inteiro: se a linha não couber, o intervalo vira mín-máx (`int 100-400 at 412`).
- **Regravar os dois:** o A (pela USB, com o B desconectado) e o B.
- **Bancada** ([hardware.md](hardware.md), "Atraso dos pacotes"): sem o B, o A envia ~29/s e pula ~21/s, com 0 erros de envio (antes, ~10/s). No trajeto até a varanda, o **atraso máximo foi de 76 ms** (antes, ~1 s), sem rajadas longas e sem `DADO ATRASADO`.

## Passo 5: horizonte no TFT

`lib/pfd_geometry` (testes `native`) e `src/tasks/display_task.cpp` (núcleo 1, prioridade 2, abaixo da `link_task`). Decisões de 23/10/2026.

- **Layout** (aprovado "por enquanto"): céu e terra com o horizonte branco, uma faixa cheia de 3 px; escala de pitch a **4 px/°** (±30° visíveis), traço a cada 5° e número a cada 10°, desenhada até ±25° do pitch atual; **escala de roll fixa** em cima (0, ±10, ±20, ±30, ±45, ±60) com o **ponteiro do lado do céu** (estilo Boeing: com roll à direita, ele vai para a esquerda); avião fixo amarelo no centro; **proa** embaixo (`095`, `095~` só pelo giroscópio, `---`; rótulo MAG); **sem altitude** (retirada na bancada de 23/10/2026: fica só no OLED); ponto do link no canto (verde com dado atual, vermelho sem).
- **Sinais** (testes de geometria): roll à direita gira o horizonte no sentido anti-horário (lado esquerdo desce); nariz para cima desce o horizonte; roll de 90° deixa a terra à direita; em dorso, a terra fica em cima.
- **Estado visual em `lib/pfd_state`** (testes `native`, revisão de 23/10/2026): a escolha entre horizonte e bandeira, o motivo, a marca de degradada e a validade da proa saíram da `display_task` para uma função pura. **A proa só vale com a atitude mostrada:** um pacote incoerente (roll NaN com `VAL_ATT` e `VAL_HDG` ligadas) mostrava o X vermelho com a proa numérica; agora mostra `---`.
- **Bandeira de falha:** sem dado atual (a mesma regra do alarme e do OLED, `linkSnapshot`) ou com `VAL_ATT` desligada, a atitude some: fundo preto, **X vermelho** de canto a canto, `ATT` em vermelho e o motivo (`SEM SINAL`, `LINK PERDIDO`, `DADO ATRASADO`, `ATT INVALIDA`). A caixa de proa continua, com `---`.
- **Alarmes no TFT, com os termos da aviação:** `BANK ANGLE` em âmbar (aviso, > 35°) e `ATTITUDE` em vermelho (alarme de atitude anormal). `ATT DEGRADADA` em âmbar com a `VAL_ATT_DEGRADED`.
- **Linhas suavizadas** (bancada de 23/10/2026): com a caixa a −1,4° de roll na mesa, os traços de 60 px da escala de pitch apareciam quebrados por um degrau de 1 px no meio, e o horizonte de 2 px (duas linhas deslocadas) tinha degraus desencontrados. Agora os traços das escalas têm 2 px, com o `drawWideLine` da TFT_eSPI, que mistura a borda com o fundo lido do sprite (no RGB332 há poucos tons intermediários, então a suavização é discreta). O horizonte é uma faixa cheia de 3 px (dois triângulos), com as bordas paralelas e **sem** suavização: suavizado, ele levou o maior quadro a 40–42 ms com a caixa inclinada (24 quadros/s), contra 38 ms parada e 30–32 ms antes da suavização.
- **Cores** escolhidas no RGB332 e convertidas com o `color8to16` da TFT_eSPI, para cada cor ser representada exatamente no sprite de 8 bits. Fontes de bitmap (1, 2 e 4).
- **O sprite é alocado no `setup()`, antes do Wi-Fi** (`displayInit`), com o heap ainda sem fragmentar; sem memória, o TFT mostra `SEM MEMORIA PARA O SPRITE`, e o resto do firmware segue.
- **Ritmo:** `DISPLAY_FPS` = 25 pelo próprio relógio (`vTaskDelayUntil`). O `c` mostra quadros/s, desenho, envio, maior quadro e quadros acima do período; o `r` mostra os quadros/s e o maior quadro, para o passo 6 comparar a recepção com o TFT ligado.
- **Consulta única do estado do link** (`linkSnapshot`, na `link_task`): o alarme, o OLED e o TFT usam a mesma função para decidir se há dado atual (timeout de 500 ms e idade total acima de 250 ms), em vez de três cópias da conta.
- **`TFT_BL` saiu do `platformio.ini`:** o BLK está no 3V3, e a biblioteca ligava o GPIO 4 à toa.

## Passo 6: bancada integrada

Coberto pela rodada de desempenho do passo 5 ([hardware.md](hardware.md), "Horizonte no TFT"): quadros/s, tempos de quadro, recepção com o TFT ligado e alarmes com a caixa inclinada. **Decisão:** ficar com 25 quadros/s (item 1 da revisão de 15/10, sprite e heap, fechado). Opcional: uma rodada de distância com o TFT ligado, para ver `DADO ATRASADO` e `LINK PERDIDO` na bandeira em uso real.
