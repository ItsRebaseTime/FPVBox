#pragma once
#include "Arduino.h"
class StreamString : public Stream, public String
{
public:
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    size_t write(uint8_t c) override { concat((char)c); return 1; }
    size_t write(const uint8_t *b, size_t n) override { concat((const char *)b, (unsigned)n); return n; }
};
