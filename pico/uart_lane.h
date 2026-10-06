// uart_lane.h - second input link: UART receive into a DMA ring buffer (no CPU work per byte, no overruns).
// Wire: host TX -> Pico RX pin, and GND. The Pico answers over USB only, so no TX wire is needed.
#ifndef USBDISPLAY_UART_LANE_H
#define USBDISPLAY_UART_LANE_H

#include <stddef.h>
#include <stdint.h>

#ifndef GFX_UART_RX_PIN
#define GFX_UART_RX_PIN 1           // GP1 = UART0 RX (GP0 = TX is unused)
#endif
#ifndef GFX_UART_BAUD
#define GFX_UART_BAUD 2000000
#endif
#ifndef GFX_UART_RING_BITS
#define GFX_UART_RING_BITS 14       // 16 KB ring; the host never has more than half of it unacknowledged
#endif
#define GFX_UART_RING_SIZE (1u << GFX_UART_RING_BITS)

// Starts the UART at GFX_UART_BAUD and a DMA channel that fills the ring endlessly. Returns the real baud rate.
uint32_t uart_lane_init(void);
// Bytes currently readable / pointer to the next unread byte (contiguous part only) / consume n bytes.
size_t uart_lane_available(void);
const uint8_t *uart_lane_peek(size_t *contiguous);
void uart_lane_consume(size_t n);
void uart_lane_flush(void);           // drop everything received so far
int  uart_lane_overruns(void);        // UART FIFO overrun / framing errors seen (0 = clean link)

#endif
