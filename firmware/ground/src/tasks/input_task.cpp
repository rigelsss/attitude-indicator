// input_task.cpp — ver input_task.h
#include "input_task.h"
#include <Arduino.h>
#include <atomic>
#include <driver/gpio.h>
#include <soc/gpio_reg.h>
#include "alarm_task.h"
#include "command_task.h"
#include "config.h"
#include "encoder_decode.h"

static_assert(PIN_ENC_CLK >= 32 && PIN_ENC_DT >= 32, "a ISR lê o encoder pelo GPIO_IN1 (pinos 32 a 39)");

static QueueHandle_t s_encQueue = nullptr, s_events = nullptr, s_status = nullptr;
static volatile uint32_t s_isrDropped = 0;   // só a ISR escreve
static volatile bool s_gap = false;          // só a ISR usa: houve descarte e o marcador ainda não entrou

// Marcador de lacuna na fila (fora dos 2 bits de estado): os estados seguintes não continuam os anteriores
static const uint8_t ENC_GAP_MARK = 0x80;
static std::atomic<uint32_t> s_eventsDropped{0};
static volatile bool s_startOk = false, s_startDone = false;

// Os dois pinos numa leitura só do registrador: CLK e DT do mesmo instante, seguro em IRAM
static inline uint8_t IRAM_ATTR encState() {
    const uint32_t in1 = REG_READ(GPIO_IN1_REG);
    return (uint8_t)((((in1 >> (PIN_ENC_CLK - 32)) & 1) << 1) | ((in1 >> (PIN_ENC_DT - 32)) & 1));
}

// Com a fila cheia, o estado é descartado e o primeiro item que couber depois é o marcador de lacuna, na posição
// exata da perda: a task ressincroniza ali, e não lê como transição dois estados que não foram seguidos
static void IRAM_ATTR onEncoder(void *) {
    const uint8_t st = encState();
    BaseType_t woke = pdFALSE;
    if (s_gap) {
        // Cópia na pilha: a constante fica em .flash.rodata, que não pode ser lida com o cache desligado (a ISR
        // é ESP_INTR_FLAG_IRAM e roda durante gravações na flash). A fila copia o byte na hora
        const uint8_t gapMark = ENC_GAP_MARK;
        if (xQueueSendFromISR(s_encQueue, &gapMark, &woke) == pdTRUE) s_gap = false;
    }
    if (s_gap || xQueueSendFromISR(s_encQueue, &st, &woke) != pdTRUE) {
        s_isrDropped = s_isrDropped + 1;
        s_gap = true;
    }
    if (woke) portYIELD_FROM_ISR();
}

static void publish(InputEventMsg::Type type, int8_t dir, uint8_t page) {
    const InputEventMsg e = {type, dir, page, millis()};
    if (xQueueSend(s_events, &e, 0) != pdTRUE) s_eventsDropped.fetch_add(1, std::memory_order_relaxed);
}

// Pinos e ISR, no núcleo da task (a interrupção é alocada no núcleo de quem a registra)
static bool setupPins() {
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << PIN_ENC_CLK) | (1ULL << PIN_ENC_DT);
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;   // o KY-040 tem pull-ups no CLK e no DT (alimentado em 3V3)
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_ANYEDGE;
    esp_err_t err = gpio_config(&io);
    // O serviço de GPIO é único na placa: já instalado conta como sucesso
    if (err == ESP_OK) {
        err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
        if (err == ESP_ERR_INVALID_STATE) err = ESP_OK;
    }
    if (err == ESP_OK) err = gpio_isr_handler_add((gpio_num_t)PIN_ENC_CLK, onEncoder, nullptr);
    if (err == ESP_OK) err = gpio_isr_handler_add((gpio_num_t)PIN_ENC_DT, onEncoder, nullptr);
    pinMode(PIN_ENC_SW, INPUT_PULLUP);   // o botão do KY-040 não tem pull-up
    return err == ESP_OK;
}

static void inputTask(void *) {
    s_startOk = setupPins();
    s_startDone = true;
    if (!s_startOk) {
        vTaskDelete(nullptr);
        return;
    }
    const EncoderConfig cfg = {ENC_STEPS_PER_CLICK, ENC_DIR};
    EncoderDecoder dec(cfg, encState());
    ButtonDebounce btn(BTN_DEBOUNCE_MS, BTN_LONG_MS);
    InputStatus st = {};
    for (;;) {
        uint8_t s;
        if (xQueueReceive(s_encQueue, &s, pdMS_TO_TICKS(INPUT_TASK_WAIT_MS)) == pdTRUE) {
            do {
                if (s == ENC_GAP_MARK) {   // estados perdidos aqui: descarta o clique parcial
                    dec.resync();
                    st.gaps++;
                    continue;
                }
                const int8_t click = dec.feed(s);
                if (click != 0) {
                    st.page = (uint8_t)((st.page + OLED_PAGES + click) % OLED_PAGES);
                    (click > 0 ? st.clicksCw : st.clicksCcw)++;
                    publish(InputEventMsg::Click, click, st.page);
                }
            } while (xQueueReceive(s_encQueue, &s, 0) == pdTRUE);
        }
        switch (btn.update(digitalRead(PIN_ENC_SW) == LOW, millis())) {
        case ButtonEvent::Short:
            publish(InputEventMsg::ShortPress, 0, st.page);
            // A alarm_task avisa "reconhecido" ou "nada a reconhecer"; com a fila dela cheia, o pedido não chega
            if (!alarmAcknowledge()) publish(InputEventMsg::AckDropped, 0, st.page);
            break;
        case ButtonEvent::Long: {
            // Pressão longa conforme a página do OLED: dados = zerar a atitude; link = zerar a altitude
            // (docs/web.md). A serial e o OLED mostram o andamento do comando
            const uint8_t cmd = encoderLongPressCommand(st.page);
            publish(InputEventMsg::LongPress, (int8_t)cmd, st.page);
            if (!commandSubmit(cmd, 0, CMD_FROM_ENCODER)) publish(InputEventMsg::CmdDropped, (int8_t)cmd, st.page);
            break;
        }
        case ButtonEvent::None: break;
        }
        st.invalid = dec.invalid();
        st.isrDropped = s_isrDropped;
        st.buttonHeld = btn.pressed();
        xQueueOverwrite(s_status, &st);
    }
}

bool inputTaskStart() {
    s_encQueue = xQueueCreate(ENC_QUEUE_LEN, sizeof(uint8_t));
    s_events = xQueueCreate(INPUT_EVENT_QUEUE_LEN, sizeof(InputEventMsg));
    s_status = xQueueCreate(1, sizeof(InputStatus));
    if (s_encQueue == nullptr || s_events == nullptr || s_status == nullptr) return false;
    if (xTaskCreatePinnedToCore(inputTask, "input", INPUT_TASK_STACK, nullptr, INPUT_TASK_PRIO, nullptr,
                                INPUT_TASK_CORE) != pdPASS)
        return false;
    while (!s_startDone) vTaskDelay(1);   // a task configura os pinos e a ISR no próprio núcleo
    return s_startOk;
}

uint8_t encoderLongPressCommand(uint8_t page) { return page == 0 ? CMD_ZERO_ATTITUDE : CMD_ZERO_ALTITUDE; }

bool inputEventReceive(InputEventMsg &out) { return s_events != nullptr && xQueueReceive(s_events, &out, 0) == pdTRUE; }

uint32_t inputEventsDropped() { return s_eventsDropped.load(std::memory_order_relaxed); }

bool inputCurrent(InputStatus &out) { return s_status != nullptr && xQueuePeek(s_status, &out, 0) == pdTRUE; }
