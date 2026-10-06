// uart_lane.c - UART0 RX -> DMA -> ring buffer. NOT TESTED ON HARDWARE (written against the Pico SDK 2.x API).
#include "uart_lane.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "hardware/structs/uart.h"

#define LANE_UART uart0

static uint8_t ring[GFX_UART_RING_SIZE] __attribute__((aligned(GFX_UART_RING_SIZE)));
static int dma_ch = -1;
static uint32_t tail;                 // read offset into the ring
static int errors;

uint32_t uart_lane_init(void) {
	uint32_t baud = uart_init(LANE_UART, GFX_UART_BAUD);
	gpio_set_function(GFX_UART_RX_PIN, GPIO_FUNC_UART);
	uart_set_hw_flow(LANE_UART, false, false);
	uart_set_format(LANE_UART, 8, 1, UART_PARITY_NONE);
	uart_set_fifo_enabled(LANE_UART, true);
	uart_get_hw(LANE_UART)->dmacr = UART_UARTDMACR_RXDMAE_BITS;       // RX DMA request enable

	dma_ch = dma_claim_unused_channel(true);
	dma_channel_config c = dma_channel_get_default_config((uint)dma_ch);
	channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
	channel_config_set_read_increment(&c, false);
	channel_config_set_write_increment(&c, true);
	channel_config_set_ring(&c, true, GFX_UART_RING_BITS);            // wrap the write address inside the ring
	channel_config_set_dreq(&c, uart_get_dreq(LANE_UART, false));
	dma_channel_configure((uint)dma_ch, &c, ring, &uart_get_hw(LANE_UART)->dr, dma_encode_endless_transfer_count(), true);
	tail = 0;
	return baud;
}

static inline uint32_t head_offset(void) {
	return (uint32_t)(dma_channel_hw_addr((uint)dma_ch)->write_addr - (uint32_t)(uintptr_t)ring) & (GFX_UART_RING_SIZE - 1);
}

size_t uart_lane_available(void) {
	if (dma_ch < 0) return 0;
	return (head_offset() - tail) & (GFX_UART_RING_SIZE - 1);
}

const uint8_t *uart_lane_peek(size_t *contiguous) {
	size_t avail = uart_lane_available();
	size_t to_end = GFX_UART_RING_SIZE - tail;
	*contiguous = avail < to_end ? avail : to_end;
	return ring + tail;
}

void uart_lane_consume(size_t n) { tail = (tail + (uint32_t)n) & (GFX_UART_RING_SIZE - 1); }

void uart_lane_flush(void) { if (dma_ch >= 0) tail = head_offset(); }

int uart_lane_overruns(void) {
	uint32_t rsr = uart_get_hw(LANE_UART)->rsr;                        // framing / parity / break / overrun
	if (rsr & 0xF) { errors++; uart_get_hw(LANE_UART)->rsr = 0; }
	return errors;
}
