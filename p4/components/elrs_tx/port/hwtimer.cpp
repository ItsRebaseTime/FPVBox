/* ExpressLRS hwTimer on a GPTimer (1 MHz). The alarm ISR wakes the port's
 * real-time task, which runs the tock callback (SPI to the radio). The TX
 * only uses tock: one call per packet interval. */
#include "hwTimer.h"
#include "driver/gptimer.h"
#include "elrs_port.h"
#include "esp_log.h"

void (*hwTimer::callbackTick)() = nullptr;
void (*hwTimer::callbackTock)() = nullptr;
volatile bool hwTimer::running = false;
volatile bool hwTimer::isTick = false;
volatile uint32_t hwTimer::HWtimerInterval = TimerIntervalUSDefault;
volatile int32_t hwTimer::PhaseShift = 0;
volatile int32_t hwTimer::FreqOffset = 0;

static gptimer_handle_t s_timer;

static bool IRAM_ATTR on_alarm(gptimer_handle_t t, const gptimer_alarm_event_data_t *e, void *arg)
{
    (void)t; (void)e; (void)arg;
    elrs_rt_notify_timer_from_isr();
    return false;   /* the notify yields if needed */
}

static void set_alarm(uint32_t interval_us)
{
    if (!s_timer) return;
    gptimer_alarm_config_t a = {};
    a.alarm_count = interval_us ? interval_us : 1;
    a.reload_count = 0;
    a.flags.auto_reload_on_alarm = true;
    gptimer_set_alarm_action(s_timer, &a);
}

void hwTimer::init(void (*tick)(), void (*tock)())
{
    if (s_timer) return;
    callbackTick = tick;
    callbackTock = tock;
    elrs_rt_set_timer_cb(&hwTimer::callback);
    gptimer_config_t cfg = {};
    cfg.clk_src = GPTIMER_CLK_SRC_DEFAULT;
    cfg.direction = GPTIMER_COUNT_UP;
    cfg.resolution_hz = 1000000;
    if (gptimer_new_timer(&cfg, &s_timer) != ESP_OK) { s_timer = nullptr; return; }
    gptimer_event_callbacks_t cbs = {};
    cbs.on_alarm = on_alarm;
    gptimer_register_event_callbacks(s_timer, &cbs, nullptr);
    gptimer_enable(s_timer);
}

void hwTimer::stop()
{
    if (s_timer && running) {
        running = false;
        gptimer_stop(s_timer);
    }
}

void hwTimer::resume()
{
    if (s_timer && !running) {
        gptimer_set_raw_count(s_timer, 0);
        set_alarm(HWtimerInterval);
        running = true;
        gptimer_start(s_timer);
    }
}

void hwTimer::updateInterval(uint32_t time)
{
    HWtimerInterval = time;
    set_alarm(time);
}

void hwTimer::phaseShift(int32_t newPhaseShift)
{
    const int32_t lim = (int32_t)(HWtimerInterval >> 2);
    PhaseShift = constrain(newPhaseShift, -lim, lim);
}

void hwTimer::callback()
{
    if (!running) return;
    if (callbackTock) callbackTock();
}
