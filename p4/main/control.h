/* Control CDC port: box commands ("!..." lines) and C5VRX console passthrough. */
#pragma once
#include <stddef.h>
#include <stdint.h>

void control_start(void);
/* C5 console output to the control port (c5_link callback). */
void control_c5_output(const uint8_t *data, size_t len, void *ctx);
/* Text for the status log / control port. */
void control_print_status(void (*out)(const char *line, void *ctx), void *ctx);
