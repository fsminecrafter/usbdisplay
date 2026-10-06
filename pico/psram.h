// psram.h - QMI bring-up of the 8 MB PSRAM on the Pimoroni Pico Plus 2 (APS6404L, chip select GPIO 47).
#ifndef USBDISPLAY_PSRAM_H
#define USBDISPLAY_PSRAM_H

#include <stddef.h>
#include <stdint.h>

#ifndef PSRAM_CS_PIN
#define PSRAM_CS_PIN 47
#endif

// Detects, configures and memory-tests the PSRAM. Returns its size in bytes, or 0 when there is none or the
// self-test failed (callers then fall back to SRAM). MUST be called early in main(): after the system clock is
// set, before stdio_init_all() / interrupts / core 1, because it runs with the flash (XIP) unavailable.
size_t psram_init(void);

// Cached XIP window of the PSRAM (valid when psram_init() returned > 0).
static inline uint8_t *psram_base(void) { return (uint8_t *)0x11000000u; }

#endif
