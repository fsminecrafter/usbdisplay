// psram.c - PSRAM bring-up for RP2350 boards that wire a QSPI PSRAM to QMI chip-select 1 (Pimoroni Pico Plus 2).
//
// NOT TESTED ON HARDWARE. The register sequence follows the RP2350 datasheet (QMI chapter) and the common
// PSRAM examples for this board; it is deliberately conservative (PSRAM clock <= ~63 MHz, so reads are slower than
// the part could do) and ends with a read-back test, so a mistake shows up as "no PSRAM" and the firmware falls back
// to SRAM instead of corrupting data.
#include "psram.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip.h"
#include "pico/platform.h"
#include <stdbool.h>

#define PSRAM_MAX_CLK_HZ 63000000u      // APS6404L-3SQR could do 133 MHz; stay far below until measured

static size_t __no_inline_not_in_flash_func(psram_detect)(void) {
	size_t size = 0;
	qmi_hw->direct_csr = 30u << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;
	while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) ;

	// Leave quad mode in case a previous run left the chip in it (0xF5), then read the ID (0x9F) in SPI mode.
	qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
	qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS | (QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB) | 0xF5;
	while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) ;
	(void)qmi_hw->direct_rx;
	qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;

	qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
	uint8_t kgd = 0, eid = 0;
	for (int i = 0; i < 7; ++i) {
		qmi_hw->direct_tx = i == 0 ? 0x9F : 0xFF;
		while (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS)) ;
		while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) ;
		uint8_t rx = (uint8_t)qmi_hw->direct_rx;
		if (i == 5) kgd = rx; else if (i == 6) eid = rx;
	}
	qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);

	if (kgd == 0x5D) {                                   // "known good die" marker of the AP Memory parts
		size = 1024u * 1024u;
		unsigned size_id = eid >> 5;
		if (eid == 0x26 || size_id == 2) size *= 8;
		else if (size_id == 0) size *= 2;
		else if (size_id == 1) size *= 4;
	}
	return size;
}

static void __no_inline_not_in_flash_func(psram_configure)(uint32_t sys_hz) {
	// Clock divider so that the PSRAM clock stays at or below PSRAM_MAX_CLK_HZ.
	uint32_t div = (sys_hz + PSRAM_MAX_CLK_HZ - 1) / PSRAM_MAX_CLK_HZ;
	if (div < 1) div = 1;
	if (div > 255) div = 255;
	uint32_t rxdelay = div > 1 ? 1 : 0;                  // low clock: a small RX delay is enough
	// Max chip-select low time must stay under 8 us (units of 64 sys clocks); min deselect >= 18 ns (sys clocks).
	uint32_t max_sel = (uint32_t)(((uint64_t)sys_hz * 8 / 1000000u / 64u) * 9u / 10u);
	if (max_sel > 31) max_sel = 31;
	uint32_t min_desel = (uint32_t)(((uint64_t)sys_hz * 18 + 999999999u) / 1000000000u) + 1u;
	if (min_desel > 31) min_desel = 31;

	// Direct mode: reset (0x66, 0x99) then enter quad mode (0x35), all as plain SPI commands.
	qmi_hw->direct_csr = 10u << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_AUTO_CS1N_BITS;
	while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) ;
	static const uint8_t cmds[3] = { 0x66, 0x99, 0x35 };
	for (int i = 0; i < 3; ++i) {
		qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
		qmi_hw->direct_tx = cmds[i];
		while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) ;
		qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
		(void)qmi_hw->direct_rx;
		for (volatile int k = 0; k < 200; ++k) ;         // tRST / tCPH margin
	}

	qmi_hw->m[1].timing = (1u << QMI_M1_TIMING_COOLDOWN_LSB) |
	                      (QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB) |
	                      (max_sel << QMI_M1_TIMING_MAX_SELECT_LSB) |
	                      (min_desel << QMI_M1_TIMING_MIN_DESELECT_LSB) |
	                      (rxdelay << QMI_M1_TIMING_RXDELAY_LSB) |
	                      (div << QMI_M1_TIMING_CLKDIV_LSB);

	// Quad read: 0xEB, 24 bit address, 6 dummy cycles (24 bits at quad width), quad data.
	qmi_hw->m[1].rfmt = (QMI_M1_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_PREFIX_WIDTH_LSB) |
	                    (QMI_M1_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M1_RFMT_ADDR_WIDTH_LSB) |
	                    (QMI_M1_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_SUFFIX_WIDTH_LSB) |
	                    (QMI_M1_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M1_RFMT_DUMMY_WIDTH_LSB) |
	                    (QMI_M1_RFMT_DATA_WIDTH_VALUE_Q << QMI_M1_RFMT_DATA_WIDTH_LSB) |
	                    (QMI_M1_RFMT_PREFIX_LEN_VALUE_8 << QMI_M1_RFMT_PREFIX_LEN_LSB) |
	                    (6u << QMI_M1_RFMT_DUMMY_LEN_LSB);
	qmi_hw->m[1].rcmd = 0xEB;

	// Quad write: 0x38, 24 bit address, no dummy cycles.
	qmi_hw->m[1].wfmt = (QMI_M1_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_PREFIX_WIDTH_LSB) |
	                    (QMI_M1_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M1_WFMT_ADDR_WIDTH_LSB) |
	                    (QMI_M1_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_SUFFIX_WIDTH_LSB) |
	                    (QMI_M1_WFMT_DUMMY_WIDTH_VALUE_Q << QMI_M1_WFMT_DUMMY_WIDTH_LSB) |
	                    (QMI_M1_WFMT_DATA_WIDTH_VALUE_Q << QMI_M1_WFMT_DATA_WIDTH_LSB) |
	                    (QMI_M1_WFMT_PREFIX_LEN_VALUE_8 << QMI_M1_WFMT_PREFIX_LEN_LSB);
	qmi_hw->m[1].wcmd = 0x38;

	qmi_hw->direct_csr = 0;                              // leave direct mode: M1 is now memory mapped
	xip_ctrl_hw->ctrl |= XIP_CTRL_WRITABLE_M1_BITS;      // allow stores to the PSRAM window
}

// Write/read-back test over the start, the middle and the end of the PSRAM (address-in-data pattern, then its inverse).
static bool __no_inline_not_in_flash_func(psram_selftest)(size_t size) {
	volatile uint32_t *p = (volatile uint32_t *)psram_base();
	const size_t words = size / 4;
	const size_t spots[3] = { 0, words / 2 - 4096, words - 8192 };
	for (int pass = 0; pass < 2; ++pass)
		for (int s = 0; s < 3; ++s) {
			for (size_t i = 0; i < 4096; ++i) {
				uint32_t v = (uint32_t)(spots[s] + i) * 2654435761u;
				p[spots[s] + i] = pass ? ~v : v;
			}
			for (size_t i = 0; i < 4096; ++i) {
				uint32_t v = (uint32_t)(spots[s] + i) * 2654435761u;
				if (p[spots[s] + i] != (pass ? ~v : v)) return false;
			}
		}
	return true;
}

size_t __no_inline_not_in_flash_func(psram_init)(void) {
	gpio_set_function(PSRAM_CS_PIN, GPIO_FUNC_XIP_CS1);
	uint32_t status = save_and_disable_interrupts();
	size_t size = psram_detect();
	if (size) psram_configure(clock_get_hz(clk_sys));
	restore_interrupts(status);
	if (!size) return 0;
	if (!psram_selftest(size)) return 0;
	return size;
}
