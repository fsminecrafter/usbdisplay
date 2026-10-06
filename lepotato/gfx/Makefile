# gfxstream: screen capture + tile encoder for the Pico USB display.
# X11 support is enabled automatically when libx11-dev and libxext-dev are installed.
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
CFLAGS  += -I../../common -I.
X11     := $(shell pkg-config --exists x11 xext 2>/dev/null && echo 1)
ifeq ($(X11),1)
CFLAGS  += -DHAVE_X11 $(shell pkg-config --cflags x11 xext)
LDLIBS  += $(shell pkg-config --libs x11 xext)
endif

gfxstream: gfxstream.c gfx_enc.c gfx_enc.h ../../common/gfxproto.h ../../common/lz4mini.h
	$(CC) $(CFLAGS) gfxstream.c gfx_enc.c -o $@ $(LDLIBS)
	@echo "built gfxstream (X11: $(if $(filter 1,$(X11)),yes,NO - install libx11-dev libxext-dev))"

clean:
	rm -f gfxstream

.PHONY: clean
