/* MAVLink link mode is not part of the FPVBox ELRS TX build: CRSF only. */
#pragma once
#include <stdint.h>
#include <stddef.h>
static inline bool isThisAMavPacket(uint8_t *, uint16_t) { return false; }
static inline void convert_mavlink_to_crsf_telem(int, uint8_t *, uint8_t) {}
