/* ExpressLRS SPIEx on ESP-IDF's SPI master (polling, no DMA). Transfers run
 * in task context: the port runs ExpressLRS' radio ISR work in a task. */
#pragma once
#include "targets.h"
#include <SPI.h>

class SPIExClass : public SPIClass
{
public:
    explicit SPIExClass(uint8_t spi_bus = HSPI) : SPIClass(spi_bus) {}
    void read(uint8_t cs_mask, uint8_t *data, uint32_t size) { _transfer(cs_mask, data, size, true); }
    void write(uint8_t cs_mask, uint8_t *data, uint32_t size) { _transfer(cs_mask, data, size, false); }
private:
    void _transfer(uint8_t cs_mask, uint8_t *data, uint32_t size, bool reading);
};

extern SPIExClass SPIEx;
