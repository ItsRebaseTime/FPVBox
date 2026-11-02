/* Minimal Arduino core for ExpressLRS on ESP-IDF (ESP32-P4, FPVBox).
 * Only what the ExpressLRS TX sources use; see port/arduino_core.cpp. */
#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <algorithm>
#include <cstddef>

#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "nvs.h"

typedef uint8_t byte;
typedef bool boolean;
typedef unsigned int word;

#define HIGH            0x1
#define LOW             0x0
#define INPUT           0x01
#define OUTPUT          0x03
#define PULLUP          0x04
#define INPUT_PULLUP    0x05
#define PULLDOWN        0x08
#define INPUT_PULLDOWN  0x09
#define OPEN_DRAIN      0x10
#define OUTPUT_OPEN_DRAIN 0x13

#define RISING          0x01
#define FALLING         0x02
#define CHANGE          0x03

#define LSBFIRST        0
#define MSBFIRST        1

#define DEC 10
#define HEX 16
#define OCT 8
#define BIN 2

#define PROGMEM
#define PGM_P const char *
#define pgm_read_byte(addr) (*(const unsigned char *)(addr))
#define pgm_read_word(addr) (*(const unsigned short *)(addr))
#define pgm_read_dword(addr) (*(const unsigned long *)(addr))
#define F(s) (s)

#define bit(b)                  (1UL << (b))
#define bitRead(value, bit)     (((value) >> (bit)) & 0x01)
#define bitSet(value, bit)      ((value) |= (1UL << (bit)))
#define bitClear(value, bit)    ((value) &= ~(1UL << (bit)))
#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))
#define digitalPinToInterrupt(p) (p)
#define NOT_A_PIN               (-1)
#ifndef ICACHE_RAM_ATTR
#define ICACHE_RAM_ATTR IRAM_ATTR
#endif
#ifndef APB_CLK_FREQ
#define APB_CLK_FREQ            80000000
#endif

using std::min;
using std::max;

/* Time */
static inline unsigned long IRAM_ATTR micros(void) { return (unsigned long)esp_timer_get_time(); }
static inline unsigned long IRAM_ATTR millis(void) { return (unsigned long)(esp_timer_get_time() / 1000); }
void delay(uint32_t ms);
static inline void IRAM_ATTR delayMicroseconds(uint32_t us) { esp_rom_delay_us(us); }
static inline void yield(void) { taskYIELD(); }

/* GPIO */
void pinMode(int pin, uint8_t mode);
void digitalWrite(int pin, uint8_t val);
int digitalRead(int pin);
int analogRead(int pin);
/* No DAC-driven PA on the FPVBox (power_control = 0). */
static inline void dacWrite(int pin, int value) { (void)pin; (void)value; }
void attachInterrupt(int pin, void (*isr)(void), int mode);
void detachInterrupt(int pin);

/* "Interrupts": the ExpressLRS ISRs run in a high-priority task on the loop's
 * core, so masking interrupts maps to masking that task. */
void noInterrupts(void);
void interrupts(void);

static inline long map(long x, long in_min, long in_max, long out_min, long out_max)
{
    return in_max == in_min ? out_min : (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

/* Random */
long random(long howbig);
long random(long howsmall, long howbig);
void randomSeed(unsigned long seed);


#include "WString.h"

class Print
{
public:
    virtual ~Print() {}
    virtual size_t write(uint8_t c) = 0;
    virtual size_t write(const uint8_t *buf, size_t len)
    {
        size_t n = 0;
        while (len--) n += write(*buf++);
        return n;
    }
    size_t write(const char *s) { return s ? write((const uint8_t *)s, strlen(s)) : 0; }
    size_t write(const char *s, size_t len) { return write((const uint8_t *)s, len); }
    virtual int availableForWrite() { return 0; }
    virtual void flush() {}

    size_t print(const char *s) { return write(s); }
    size_t print(const String &s) { return write(s.c_str()); }
    size_t print(char c) { return write((uint8_t)c); }
    size_t print(unsigned char v, int base = DEC) { return print((unsigned long)v, base); }
    size_t print(int v, int base = DEC) { return print((long)v, base); }
    size_t print(unsigned int v, int base = DEC) { return print((unsigned long)v, base); }
    size_t print(long v, int base = DEC)
    {
        char b[24];
        if (base == 16) snprintf(b, sizeof(b), "%lx", v); else snprintf(b, sizeof(b), "%ld", v);
        return write(b);
    }
    size_t print(unsigned long v, int base = DEC)
    {
        char b[24];
        if (base == 16) snprintf(b, sizeof(b), "%lx", v); else snprintf(b, sizeof(b), "%lu", v);
        return write(b);
    }
    size_t print(double v, int digits = 2)
    {
        char b[32];
        snprintf(b, sizeof(b), "%.*f", digits, v);
        return write(b);
    }
    size_t println() { return write("\r\n"); }
    template <typename T> size_t println(T v) { size_t n = print(v); return n + println(); }
    template <typename T> size_t println(T v, int base) { size_t n = print(v, base); return n + println(); }
    size_t printf(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
    {
        char b[256];
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(b, sizeof(b), fmt, ap);
        va_end(ap);
        if (n < 0) return 0;
        return write((const uint8_t *)b, (size_t)n < sizeof(b) ? (size_t)n : sizeof(b) - 1);
    }
};

class Stream : public Print
{
public:
    virtual int available() = 0;
    virtual int read() = 0;
    virtual int peek() = 0;
    virtual size_t readBytes(uint8_t *buffer, size_t length)
    {
        size_t count = 0;
        while (count < length) {
            const int c = read();
            if (c < 0) break;
            buffer[count++] = (uint8_t)c;
        }
        return count;
    }
    size_t readBytes(char *buffer, size_t length) { return readBytes((uint8_t *)buffer, length); }
    void setTimeout(unsigned long) {}
};

#include "HardwareSerial.h"

/* ESP object (restart, chip id) */
class EspClass
{
public:
    void restart() { esp_restart(); }
    uint64_t getEfuseMac();
    uint32_t getFreeHeap();
};
extern EspClass ESP;

/* Watchdog helpers referenced by the device framework. */
static inline void disableCore0WDT(void) {}
static inline void disableCore1WDT(void) {}
static inline void enableCore0WDT(void) {}
