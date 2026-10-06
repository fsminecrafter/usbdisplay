#include "pico/stdlib.h"
#include "led.h"

#if USE_CYW43_LED
#include "pico/cyw43_arch.h"

static bool led_ok;

void led_init(void) {
	led_ok = (cyw43_arch_init() == 0);
}

void led_set(bool on) {
	if (led_ok) cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
}

#elif defined(PICO_DEFAULT_LED_PIN)

void led_init(void) {
	gpio_init(PICO_DEFAULT_LED_PIN);
	gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
}

void led_set(bool on) {
	gpio_put(PICO_DEFAULT_LED_PIN, on);
}

#else

void led_init(void) {}
void led_set(bool on) { (void)on; }

#endif
