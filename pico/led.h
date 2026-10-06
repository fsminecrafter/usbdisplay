// led.h - onboard LED. On a Pico 2 W the LED hangs off the CYW43 Wi-Fi chip
// (needs cyw43_arch); on a plain Pico 2 it is a normal GPIO.
#ifndef USBDISPLAY_LED_H
#define USBDISPLAY_LED_H

#include <stdbool.h>

void led_init(void);
void led_set(bool on);

#endif
