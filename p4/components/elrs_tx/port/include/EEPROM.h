/* Arduino EEPROM emulation in an NVS blob (ExpressLRS elrs_eeprom). */
#pragma once
#include <stdint.h>
#include <stddef.h>

class EEPROMClass
{
public:
    bool begin(size_t size);
    uint8_t read(int address);
    void write(int address, uint8_t val);
    bool commit();
    void end() {}
    template <typename T> T &get(int address, T &t)
    {
        for (size_t i = 0; i < sizeof(T); ++i) ((uint8_t *)&t)[i] = read(address + (int)i);
        return t;
    }
    template <typename T> const T &put(int address, const T &t)
    {
        for (size_t i = 0; i < sizeof(T); ++i) write(address + (int)i, ((const uint8_t *)&t)[i]);
        return t;
    }
private:
    uint8_t *data_ = nullptr;
    size_t size_ = 0;
    bool dirty_ = false;
};

extern EEPROMClass EEPROM;
