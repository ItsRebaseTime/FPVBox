/* Arduino serial classes for the ExpressLRS port. ExpressLRS talks to the
 * host through the FPVBox USB CRSF handset, so the serial ports here are
 * only the debug log (ESP-IDF console) and inert stand-ins. */
#pragma once
#include <stdint.h>
#include <stdio.h>

#define SERIAL_8N1 0x800001c

class HardwareSerial : public Stream
{
public:
    explicit HardwareSerial(int uart_nr = 0) : uart_(uart_nr) {}
    void begin(unsigned long baud, uint32_t config = SERIAL_8N1, int8_t rxPin = -1, int8_t txPin = -1,
               bool invert = false, unsigned long timeout_ms = 20000UL, uint8_t rxfifo_full_thrhd = 112)
    {
        (void)baud; (void)config; (void)rxPin; (void)txPin; (void)invert; (void)timeout_ms; (void)rxfifo_full_thrhd;
    }
    void end() {}
    void updateBaudRate(unsigned long) {}
    void setRxBufferSize(size_t) {}
    void setTxBufferSize(size_t) {}
    void setTimeout(unsigned long) {}
    void enableHalfDuplexRx() {}
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    int availableForWrite() override { return 256; }
    void flush() override { fflush(stdout); }
    size_t write(uint8_t c) override { return uart_ == 0 ? (fputc(c, stdout) == EOF ? 0 : 1) : 1; }
    size_t write(const uint8_t *b, size_t n) override { return uart_ == 0 ? fwrite(b, 1, n, stdout) : n; }
    operator bool() const { return true; }
private:
    int uart_;
};

extern HardwareSerial Serial;
