# Caixa do B: alimentação, tampa, segundo OLED, buzzer e anel de LEDs

Discutido em 29/10/2026, **sem código ainda**. Reúne as decisões sobre a montagem do ESP B na caixa e os extras que vieram com ela. Nada aqui foi medido: as correntes e a autonomia são estimativas, a conferir na bancada e registrar no [hardware.md](hardware.md).

**Prioridade:** o segundo OLED e o buzzer entram antes (não dependem da fita). O **anel de LEDs é o último item do projeto**, só se sobrar tempo ([plano.md](plano.md), "Opcionais"). A proa no anel é um extra dentro desse extra.

## Peças (29/10/2026)

| Peça | Situação |
|---|---|
| Power bank Espada BTE-28E (10 000 mAh, 32 Wh; saídas de 5 V e 3,1 A no rótulo) | comprado |
| Fita WS2812B, 1 m, 60 LEDs/m | comprada |
| 2 módulos conversores de nível bidirecionais I2C (BSS138, 3,3 ↔ 5 V) | comprados |
| Conectores macho e fêmea de 4 pinos para a tampa (provavelmente JST XH 2,54 mm; conferir) | comprados |
| BC337 (10 a 20) e 1N4148 (20 a 50) | a comprar |
| Capacitor de 1000 µF (10 ou 16 V), resistores de 330 Ω, 1 kΩ e 2,2 kΩ, capacitores de 100 nF | já disponíveis |
| Potenciômetro linear de 10 kΩ | conferir se há um |
| 74AHCT125 / 74HCT125 / 74HCT541 | **não encontrados em DIP** em tempo hábil; só reserva (ver "Sinal da fita") |

Atenção na compra de CIs: o **74HC** (sem o T) alimentado em 5 V só aceita como "1" uma entrada acima de 3,5 V e não converte os 3,3 V do ESP. O **HCT/AHCT** aceita a partir de 2,0 V.

## Alimentação pelo power bank

- **Consumo estimado do B sem a fita:** 200–350 mA (ESP32 com o AP e o ESP-NOW 120–180 mA, com picos de ~400 mA; TFT ~20–60 mA; cada OLED ~20 mA; LEDs de alarme). **Com o anel:** ~0,6–1 A.
- **Autonomia estimada:** ~5 Ah úteis em 5 V (32 Wh, eficiência de 80–85 %). Isso dá ~15–18 h sem a fita e ~5–8 h com ela.
- **A fita é alimentada direto pelos 5 V do power bank** (cabo separado ou um "Y"), nunca pelo VIN ou pelo 3V3 da placa. O GND é comum com o ESP, e um capacitor de 1000 µF fica entre o 5 V e o GND, na entrada da fita.
- **Bancada a fazer:**
  1. **Desligamento automático por carga baixa:** muitos power banks desligam abaixo de 50–100 mA. Deixar o B ligado no power bank por 30–60 min. Se ele desligar sozinho, o B reinicia, e isso pode ser confundido com o travamento do B ([plano.md](plano.md), semana 6).
  2. **Brownout:** um cabo micro-USB fino derruba a tensão nos picos do Wi-Fi. Procurar `Brownout detector was triggered` no monitor. Usar um cabo curto e bom.
  3. **Com a fita:** aquecimento e autonomia com o anel no teto de corrente.

## Layout da caixa

- **Quem olha de frente é o piloto.** O nariz do avião é a borda da tampa mais distante do observador, a asa direita fica à direita dele e a borda mais próxima é a cauda. O TFT e o anel se comportam como o horizonte visto da cabine.
- **A caixa fica com o lado longo (15,5 cm) de frente para o observador:** a linha de displays é larga.
- **Tampa (12,5 × 15,5 cm):** **OLED 1 | TFT | OLED 2** em linha, no centro, de pé para o observador. Com a fita na borda (~1 cm de largura), sobram ~10,5 × 13,5 cm por dentro.
- **Encoder e potenciômetro na parede da frente** da caixa, do lado do piloto. Os fios correm por dentro da caixa até o ESP e não passam pela tampa. Na tampa, a mão cobriria os LEDs.
- **A tampa abre: conectores de 4 pinos** entre a caixa e a tampa, um por grupo, para soltar a tampa sem dessoldar e testar um display de cada vez:

  | Conector | Pinos |
  |---|---|
  | 1. Fita | 5V, GND, DIN, (vazio) |
  | 2. OLED 1 | 3V3, GND, SDA (21), SCL (22) |
  | 3. OLED 2 | 3V3, GND, SDA2 (4), SCL2 (19) |
  | 4. TFT (a) | 3V3, GND, SCLK (18), MOSI (23) |
  | 5. TFT (b) | CS (5), DC (16), RST (17), (vazio) |

  - **Risco:** os conectores são iguais e podem ser trocados ao fechar a tampa. Os 5 V da fita numa linha de 3V3 ou de dados queimam o display. Marcar cada par com cor e número e separar a saída do conector da fita da dos displays.
  - O BLK do TFT continua preso ao 3V3 do lado da tampa ([solo.md](solo.md), passo 0).
  - Fio de 22 AWG no 5V e no GND da fita. O JST XH aguenta 3 A por pino.
  - **SPI do TFT pelo cabo da tampa (~20–30 cm):** se aparecerem pixels errados ou faixas, baixar a frequência do SPI no `platformio.ini`. Conferir na bancada da montagem.

## Segundo OLED

- **O segundo OLED 0,96" não tem o resistor de seleção de endereço** e fica em `0x3C`, como o primeiro. Por isso vai no **segundo controlador I2C (`Wire1`), SDA no GPIO 4 e SCL no GPIO 19**. Os dois pinos estão livres e não são de strapping crítico. Os pull-ups vêm no módulo.
  - O GPIO 19 é o MISO padrão do SPI. O TFT não usa MISO, mas conferir na compilação que a TFT_eSPI não reserva o pino.
  - Barramentos independentes: cada OLED leva os mesmos 32,7 ms por quadro a 400 kHz ([solo.md](solo.md), passo 3), e um não atrasa o outro.
- **Uma página fixa por OLED**, em vez das duas páginas trocadas pelo giro do encoder (`OLED_PAGES`): **à esquerda os dados** (a página 1 de hoje), **à direita o link e a saúde do A** (a página 2).
- **A decidir na implementação:**
  - a pressão longa do botão hoje depende da página (zera a atitude na 1 e a altitude na 2, [web.md](web.md)). Sem páginas, ela precisa de outra regra;
  - o giro do encoder fica sem função;
  - como a `oled_task` passa a ser dona dos dois barramentos;
  - estender o `ground_diag` ao `Wire1`.

## Buzzer

- **Fica o buzzer ativo.** Os alarmes se diferenciam pelo ritmo dos bipes, que a `alarm_task` já prevê ([solo.md](solo.md), "Decisões").
- **Ligação com o BC337:** GPIO 26 → 1 kΩ → base; emissor no GND; coletor no (−) do buzzer; (+) do buzzer na alimentação dele (5 V ou 3V3, conforme o buzzer); 1N4148 em paralelo com o buzzer, com o cátodo (faixa) no (+).
  - O diodo protege o transistor do pico da bobina de um buzzer magnético. Num piezo, não faz falta, mas não atrapalha.
  - **Pinagem do BC337:** com a face plana virada para você e os pinos para baixo, **C – B – E** (diferente do 2N2222). Conferir no datasheet antes de soldar.

## Anel de LEDs (última prioridade)

### Montagem

- **Fita colada no topo da tampa, rente às bordas.** O perímetro é de 56 cm. Cortando nas marcas (1,67 cm por LED): **9 LEDs em cada lado de 15,5 cm e 7 em cada lado de 12,5 cm, 32 no total**. A sobra (~28 LEDs) fica de reserva.
- **Cantos cortados e emendados com fio:** 3 fios por canto (5V, dado, GND), 22–24 AWG, pontes curtas e termorretrátil. A dobra em 45° sem solda força as trilhas e pode abrir com a tampa sendo manuseada.
- **Difusor** (perfil com difusor leitoso, acrílico fosco ou papel vegetal): sem ele, a fita fica pontilhada e ofusca.
- **Posição de cada LED em mm no `config.h`**, calculada a partir das medidas. Assim os cantos irregulares não importam. O LED 0 é onde entra o DIN; o canto de início e o sentido (horário ou anti-horário) ficam para a montagem.

### Visual (opção A, "nível de bolha")

- Com o A nivelado, o anel inteiro está no plano do horizonte, e não há "metade céu, metade terra". Por isso, o anel mostra **a inclinação de cada ponto da borda**. A altura de um LED na posição (x para o nariz, y para a asa direita) em relação ao horizonte é aproximadamente:

  ```
  h ≈ −x·sin(pitch) + y·sin(roll)·cos(pitch)
  ```

  Os sinais dependem da convenção de eixos do A e serão conferidos na bancada contra o TFT.
- **Até 2° de inclinação (faixa morta): o anel todo branco.** Acima disso, o lado alto vai para o **azul** e o lado baixo para o **marrom**, cada vez mais saturados com o ângulo. O ângulo de saturação total fica para a implementação.
- **Coerência com o TFT:** com o nariz para cima, a borda de longe fica azul e a de perto marrom (no TFT, o horizonte desce). Com rolagem para a direita, a borda direita fica marrom e a esquerda azul (no TFT, o horizonte sobe do lado direito).
- **Estados:**
  - **perda de link** (o B sem dado atual do A, a mesma regra do `linkSnapshot` usada pelo alarme, pelo OLED e pelo TFT): anel apagado, com um LED vermelho girando devagar;
  - **inclinação excessiva:** o anel inteiro pisca em vermelho, junto com o LED de alarme;
  - **`ST_CALIBRATING`** (o A calibrando, precisa ficar parado): âmbar pulsando;
  - **`VAL_ATT` desligada com o link ok** (a atitude não vale, por exemplo logo no boot do A): âmbar fixo;
  - **`VAL_ATT_DEGRADED`:** o anel continua normal (é passageiro).
- **Extra do extra, a proa:** um LED branco aponta o norte magnético em relação ao nariz, só com a `VAL_HDG`. Implementar só se sobrar tempo.

### Brilho e corrente

- **Teto de corrente da fita no firmware** (por exemplo, `setMaxPowerInVoltsAndMilliamps(5, 1000)` da FastLED), no `config.h`. Sem ele, 32 LEDs em branco cheio pedem ~1,9 A. Em branco a ~25 %, a estimativa é de ~0,5 A. Cada LED gasta ~1 mA mesmo apagado.
- **Potenciômetro linear de 10 kΩ para o brilho:** extremos no **3V3** (nunca 5 V) e no GND, cursor no **GPIO 34** e 100 nF do GPIO 34 ao GND.
  - Precisa ser um pino do **ADC1** (GPIO 32 a 39): o ADC2 não funciona com o Wi-Fi ligado, e o B mantém o AP. O 32 e o 33 são do encoder; o 34 é só de entrada.
  - No firmware: média e histerese (o ADC do ESP32 é ruidoso e pouco linear), curva de gama no curso, brilho mínimo (o anel nunca some) e máximo abaixo do teto de corrente.

### Sinal da fita

- **GPIO 27** (livre desde 23/10/2026) → **330 Ω** → DIN, com o ESP perto do primeiro LED.
- **Biblioteca com o RMT do ESP32** (FastLED ou Adafruit NeoPixel recentes). Sem o RMT, as interrupções do Wi-Fi do AP corrompem a temporização.
- **A WS2812B em 5 V espera ≥ 3,5 V no "1"**, e o ESP entrega 3,3 V. Ordem de teste (parar no primeiro que funcionar sem cor errada por alguns minutos):
  1. **direto**, com o 330 Ω;
  2. **pixel sacrificado:** um LED da sobra, perto do ESP, alimentado pelos 5 V através de um 1N4148 (~4,3 V). Ele aceita 3,3 V como "1" e repassa o sinal com ~4,3 V, que a fita aceita. No firmware, o anel ganha um pixel a mais (índice 0, apagado);
  3. **módulo BSS138:** LV no 3V3, HV no 5 V, GND comum, um canal. A subida do lado de 5 V é lenta (pull-up de 10 kΩ); se falhar, um pull-up extra de 1–2,2 kΩ do pino HV ao 5 V;
  4. **74HC541 alimentado pelos 5 V através de um 1N4148** (o mesmo princípio do item 2), ou um HCT/AHCT, se aparecer. No 541: OE1 e OE2 (pinos 1 e 19) no GND, entradas sem uso no GND e 100 nF entre o VCC (20) e o GND (10).

### Firmware (a planejar na implementação)

- Task própria, de baixa prioridade, sem disputar com a `link_task` e a `display_task`; a taxa de atualização fica para o plano.
- O cálculo das cores (posição dos LEDs, faixa morta, saturação, estados) fica numa lógica pura em `lib/`, com testes `native`.
- Pinos, dimensões, número de LEDs, teto de corrente e limites no `config.h`.
- Um passo de bancada no `ground_diag` para testar a fita sozinha.

## Pinos previstos no B

| GPIO | Uso |
|---|---|
| 4 / 19 | SDA / SCL do `Wire1` (OLED 2) |
| 26 | buzzer, pelo BC337 (já previsto) |
| 27 | DIN da fita |
| 34 | cursor do potenciômetro do brilho |
