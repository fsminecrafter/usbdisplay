/* Dumps to_ascii() of lepotato/usbdisplay-tty.c for every code point, to compare with the Python reference. */
#define main usbdisplay_tty_main
#include "../lepotato/usbdisplay-tty.c"
#undef main
int main(void)
{
    static uint8_t buf[0x110000];
    for (uint32_t cp = 0; cp < 0x110000; cp++)
        buf[cp] = to_ascii(cp);
    fwrite(buf, 1, sizeof buf, stdout);
    return 0;
}
