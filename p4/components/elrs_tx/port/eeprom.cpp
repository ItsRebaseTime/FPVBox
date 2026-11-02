/* Arduino EEPROM emulation on an NVS blob (namespace "elrs_eeprom"). */
#include "EEPROM.h"
#include <stdlib.h>
#include <string.h>
#include "nvs.h"

EEPROMClass EEPROM;

static const char *NS = "elrs_eeprom";
static const char *KEY = "data";

bool EEPROMClass::begin(size_t size)
{
    if (data_ && size_ == size) return true;
    free(data_);
    data_ = (uint8_t *)calloc(1, size);
    if (!data_) return false;
    size_ = size;
    memset(data_, 0xff, size);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = size;
        nvs_get_blob(h, KEY, data_, &len);
        nvs_close(h);
    }
    return true;
}

uint8_t EEPROMClass::read(int address)
{
    return (data_ && address >= 0 && (size_t)address < size_) ? data_[address] : 0xff;
}

void EEPROMClass::write(int address, uint8_t val)
{
    if (!data_ || address < 0 || (size_t)address >= size_) return;
    if (data_[address] != val) { data_[address] = val; dirty_ = true; }
}

bool EEPROMClass::commit()
{
    if (!data_ || !dirty_) return true;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    const bool ok = nvs_set_blob(h, KEY, data_, size_) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) dirty_ = false;
    return ok;
}
