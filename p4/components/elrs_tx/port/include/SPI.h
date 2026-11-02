/* Arduino SPIClass surface used by ExpressLRS' SPIEx (see SPIEx.h). */
#pragma once
#include <stdint.h>

#define SPI_MODE0 0
#define HSPI 2
#define FSPI 1

typedef void *spi_t;

class SPIClass
{
public:
    explicit SPIClass(uint8_t bus = HSPI) : bus_num_(bus) {}
    void begin(int8_t sck = -1, int8_t miso = -1, int8_t mosi = -1, int8_t ss = -1);
    void end();
    void setFrequency(uint32_t hz) { freq_ = hz; }
    void setHwCs(bool) {}
    void setBitOrder(uint8_t) {}
    void setDataMode(uint8_t) {}
    spi_t *bus() { return &bus_; }
protected:
    uint8_t bus_num_;
    uint32_t freq_ = 10000000;
    spi_t bus_ = nullptr;
};

static inline void spiAttachSS(spi_t *, uint8_t, int8_t) {}
static inline void spiEnableSSPins(spi_t *, uint8_t) {}
static inline void spiDisableSSPins(spi_t *, uint8_t) {}
