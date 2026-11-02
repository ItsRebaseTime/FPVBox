/* I2C is not used by the FPVBox ELRS TX (the shield's bus belongs to the P4). */
#pragma once
#include <stdint.h>
class TwoWire
{
public:
    bool begin(int sda = -1, int scl = -1, uint32_t freq = 0) { (void)sda; (void)scl; (void)freq; return false; }
    void setClock(uint32_t) {}
};
extern TwoWire Wire;
