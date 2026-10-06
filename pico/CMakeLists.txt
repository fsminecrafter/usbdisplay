cmake_minimum_required(VERSION 3.13)

# Pico 2 W. (Plain "pico2" also works; the Wi-Fi chip is not used.)
set(PICO_BOARD pico2_w CACHE STRING "Board type")

# Path to PicoDVI's software/ directory (the one containing libdvi/ and assets/).
set(PICODVI_DIR "$ENV{HOME}/pico/PicoDVI/software" CACHE PATH "PicoDVI software directory")

include(pico_sdk_import.cmake)

project(usbdisplay C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

add_compile_options(-Wall)

set(FONT "dejavu" CACHE STRING "Font: dejavu (default, 8x16 DejaVu Sans Mono), terminus (8x16) or legacy8x8 (PicoDVI 8x8)")
set_property(CACHE FONT PROPERTY STRINGS dejavu terminus legacy8x8)
set(FONT_SCALE_Y "" CACHE STRING "Vertical repeat per font row. Default: 1 for the 8x16 fonts, 2 for legacy8x8 (use 1 there for 80x60 chars)")
if (FONT STREQUAL "legacy8x8")
	set(FONT_CELL_H 8)
	set(FONT_SCALE_DEFAULT 2)
elseif (FONT STREQUAL "dejavu" OR FONT STREQUAL "terminus")
	set(FONT_CELL_H 16)
	set(FONT_SCALE_DEFAULT 1)
else()
	message(FATAL_ERROR "FONT must be dejavu, terminus or legacy8x8 (got '${FONT}')")
endif()
if (FONT_SCALE_Y STREQUAL "")
	set(FONT_SCALE_Y_VAL ${FONT_SCALE_DEFAULT})
else()
	set(FONT_SCALE_Y_VAL ${FONT_SCALE_Y})
endif()
option(INVERT_VIDEO "Black text on white" OFF)
option(GFX "Also build the streamed 640x480 RGB332 graphics mode (see README, section 4). Full-colour DVI output, untested on hardware." OFF)
set(GFX_CACHE_TILES 256 CACHE STRING "Graphics tile cache size in 16x16 tiles: 64 KB per 256 tiles of SRAM, or up to ~16000 tiles in PSRAM (USE_PSRAM)")
option(USE_PSRAM "Pico Plus 2: put the tile cache and bitmap memory in the 8 MB PSRAM (falls back to SRAM if it is missing or fails its self-test). Needs GFX." OFF)
set(GFX_SRAM_TILES 128 CACHE STRING "With USE_PSRAM: size of the SRAM fallback cache in tiles")
option(GFX_UART "Second input link on a UART (UART0 RX on GP1, plus GND) in addition to USB. Needs GFX. Untested on hardware." OFF)
set(GFX_UART_BAUD 2000000 CACHE STRING "Baud rate of the UART link (the host adopts the value the Pico reports)")
set(DVI_PIN_CONFIG "waveshare_pico_dvi_cfg" CACHE STRING "Pin layout: waveshare_pico_dvi_cfg (default, see dvi_pins.h) or a preset from PicoDVI's common_dvi_pin_configs.h")
set(DVI_INVERT "AUTO" CACHE STRING "Force TMDS pair polarity: AUTO (use preset), ON or OFF")

include_directories(${PICODVI_DIR}/assets ${PICODVI_DIR}/include)
add_subdirectory(${PICODVI_DIR}/libdvi libdvi)

math(EXPR TERM_ROWS_VAL "480 / (${FONT_CELL_H} * ${FONT_SCALE_Y_VAL})")

add_executable(usbdisplay main.c term.c led.c)
target_include_directories(usbdisplay PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_compile_definitions(usbdisplay PRIVATE
	DVI_VERTICAL_REPEAT=1
	DVI_N_TMDS_BUFFERS=3
	FONT_SCALE_Y=${FONT_SCALE_Y_VAL}
	TERM_COLS=80
	TERM_ROWS=${TERM_ROWS_VAL}
)
if (GFX)
	# Colour output needs all three TMDS channels, so DVI_MONOCHROME_TMDS must not be set.
	target_sources(usbdisplay PRIVATE gfx_dec.c)
	target_include_directories(usbdisplay PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../common)
	target_compile_definitions(usbdisplay PRIVATE GFX=1 GFX_CACHE_TILES=${GFX_CACHE_TILES})
	if (USE_PSRAM)
		target_sources(usbdisplay PRIVATE psram.c)
		target_compile_definitions(usbdisplay PRIVATE USE_PSRAM=1 GFX_SRAM_TILES=${GFX_SRAM_TILES})
	endif()
	if (GFX_UART)
		target_sources(usbdisplay PRIVATE uart_lane.c)
		target_compile_definitions(usbdisplay PRIVATE GFX_UART=1 GFX_UART_BAUD=${GFX_UART_BAUD})
		target_link_libraries(usbdisplay hardware_uart hardware_dma)
	endif()
else()
	target_compile_definitions(usbdisplay PRIVATE DVI_MONOCHROME_TMDS=1)
endif()
if (FONT STREQUAL "legacy8x8")
	target_compile_definitions(usbdisplay PRIVATE FONT_LEGACY_8X8=1)
elseif (FONT STREQUAL "terminus")
	target_compile_definitions(usbdisplay PRIVATE FONT_TERMINUS=1)
endif()
target_compile_definitions(usbdisplay PRIVATE DVI_PIN_CONFIG=${DVI_PIN_CONFIG})
if (DVI_INVERT STREQUAL "ON")
	target_compile_definitions(usbdisplay PRIVATE DVI_INVERT=1)
elseif (DVI_INVERT STREQUAL "OFF")
	target_compile_definitions(usbdisplay PRIVATE DVI_INVERT=0)
endif()
if (INVERT_VIDEO)
	target_compile_definitions(usbdisplay PRIVATE INVERT_VIDEO=1)
endif()

target_link_libraries(usbdisplay
	pico_stdlib
	pico_multicore
	pico_util
	libdvi
)

# Pico W / Pico 2 W: the LED is on the Wi-Fi chip, so pull in the CYW43 driver
# (Wi-Fi itself is never started).
if (PICO_CYW43_SUPPORTED)
	target_link_libraries(usbdisplay pico_cyw43_arch_none)
	target_compile_definitions(usbdisplay PRIVATE USE_CYW43_LED=1)
endif()

# Console over USB CDC only; the UART would otherwise grab GP0/GP1.
pico_enable_stdio_usb(usbdisplay 1)
pico_enable_stdio_uart(usbdisplay 0)

pico_add_extra_outputs(usbdisplay)
