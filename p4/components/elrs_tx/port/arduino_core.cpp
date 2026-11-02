/* Arduino core functions for the ExpressLRS port (see Arduino.h). */
#include "Arduino.h"
#include "EEPROM.h"
#include "Wire.h"
#include "elrs_port.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"

static const char *TAG = "elrs_port";

HardwareSerial Serial(0);
TwoWire Wire;
EspClass ESP;

uint64_t EspClass::getEfuseMac()
{
    uint8_t mac[8] = {0};
    esp_efuse_mac_get_default(mac);
    uint64_t v = 0;
    memcpy(&v, mac, 6);
    return v;
}

uint32_t EspClass::getFreeHeap() { return (uint32_t)esp_get_free_heap_size(); }

void delay(uint32_t ms)
{
    if (ms == 0) taskYIELD();
    else vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1);
}

/* ---------------------------------------------------------------------- */
/* GPIO                                                                     */

static inline bool valid_pin(int pin) { return pin >= 0 && GPIO_IS_VALID_GPIO(pin); }

void pinMode(int pin, uint8_t mode)
{
    if (!valid_pin(pin)) return;
    gpio_config_t c = {};
    c.pin_bit_mask = 1ull << pin;
    c.mode = (mode & OUTPUT) == OUTPUT ? ((mode & OPEN_DRAIN) ? GPIO_MODE_INPUT_OUTPUT_OD : GPIO_MODE_INPUT_OUTPUT)
                                        : GPIO_MODE_INPUT;
    c.pull_up_en = (mode & PULLUP) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    c.pull_down_en = (mode & PULLDOWN) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
    c.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&c);
}

void IRAM_ATTR digitalWrite(int pin, uint8_t val)
{
    if (valid_pin(pin)) gpio_set_level((gpio_num_t)pin, val ? 1 : 0);
}

int IRAM_ATTR digitalRead(int pin)
{
    return valid_pin(pin) ? gpio_get_level((gpio_num_t)pin) : 0;
}

int analogRead(int pin) { (void)pin; return 0; }

/* ---------------------------------------------------------------------- */
/* Real-time task: hwTimer and GPIO "interrupts"                            */

#define RT_TIMER_BIT    (1u << 0)
#define RT_MAX_IRQ      8

static TaskHandle_t s_rt;
static SemaphoreHandle_t s_irq_lock;    /* recursive: noInterrupts() */
static void (*s_timer_cb)(void);
static struct { int pin; void (*isr)(void); } s_irq[RT_MAX_IRQ];
static uint32_t s_rt_runs;

static void IRAM_ATTR gpio_isr(void *arg)
{
    const uint32_t slot = (uint32_t)(uintptr_t)arg;
    BaseType_t woken = pdFALSE;
    if (s_rt) xTaskNotifyFromISR(s_rt, 1u << (slot + 1u), eSetBits, &woken);
    if (woken) portYIELD_FROM_ISR();
}

void IRAM_ATTR elrs_rt_notify_timer_from_isr(void)
{
    BaseType_t woken = pdFALSE;
    if (s_rt) xTaskNotifyFromISR(s_rt, RT_TIMER_BIT, eSetBits, &woken);
    if (woken) portYIELD_FROM_ISR();
}

void elrs_rt_set_timer_cb(void (*cb)(void)) { s_timer_cb = cb; }

static void rt_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY);
        xSemaphoreTakeRecursive(s_irq_lock, portMAX_DELAY);
        /* Radio events first (TX/RX done), then the packet timer. */
        for (int i = 0; i < RT_MAX_IRQ; ++i)
            if ((bits & (1u << (i + 1))) && s_irq[i].isr) s_irq[i].isr();
        if ((bits & RT_TIMER_BIT) && s_timer_cb) s_timer_cb();
        xSemaphoreGiveRecursive(s_irq_lock);
        s_rt_runs++;
    }
}

void elrs_rt_start(int core, int priority)
{
    if (s_rt) return;
    s_irq_lock = xSemaphoreCreateRecursiveMutex();
    for (int i = 0; i < RT_MAX_IRQ; ++i) s_irq[i].pin = -1;
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);   /* may already exist */
    xTaskCreatePinnedToCore(rt_task, "elrs_rt", 6144, NULL, priority, &s_rt, core);
}

void attachInterrupt(int pin, void (*isr)(void), int mode)
{
    if (!valid_pin(pin) || !isr) return;
    int slot = -1;
    for (int i = 0; i < RT_MAX_IRQ; ++i) {
        if (s_irq[i].pin == pin) { slot = i; break; }
        if (slot < 0 && s_irq[i].pin < 0) slot = i;
    }
    if (slot < 0) { ESP_LOGE(TAG, "no interrupt slot for GPIO%d", pin); return; }
    s_irq[slot].isr = isr;
    s_irq[slot].pin = pin;
    gpio_set_intr_type((gpio_num_t)pin, mode == RISING ? GPIO_INTR_POSEDGE
                                        : mode == FALLING ? GPIO_INTR_NEGEDGE : GPIO_INTR_ANYEDGE);
    gpio_isr_handler_add((gpio_num_t)pin, gpio_isr, (void *)(uintptr_t)slot);
    gpio_intr_enable((gpio_num_t)pin);
}

void detachInterrupt(int pin)
{
    if (!valid_pin(pin)) return;
    gpio_isr_handler_remove((gpio_num_t)pin);
    gpio_set_intr_type((gpio_num_t)pin, GPIO_INTR_DISABLE);
    for (int i = 0; i < RT_MAX_IRQ; ++i)
        if (s_irq[i].pin == pin) { s_irq[i].pin = -1; s_irq[i].isr = nullptr; }
}

void noInterrupts(void) { if (s_irq_lock) xSemaphoreTakeRecursive(s_irq_lock, portMAX_DELAY); }
void interrupts(void) { if (s_irq_lock) xSemaphoreGiveRecursive(s_irq_lock); }

/* ---------------------------------------------------------------------- */

static uint32_t s_rand_state = 0x12345678u;
void randomSeed(unsigned long seed) { if (seed) s_rand_state = (uint32_t)seed; }
static uint32_t next_rand(void)
{
    s_rand_state = s_rand_state * 1664525u + 1013904223u;
    return s_rand_state;
}
long random(long howbig) { return howbig > 0 ? (long)(next_rand() % (uint32_t)howbig) : 0; }
long random(long howsmall, long howbig) { return howbig > howsmall ? howsmall + random(howbig - howsmall) : howsmall; }
