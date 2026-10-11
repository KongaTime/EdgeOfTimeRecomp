# rexglue_probe: a payload that checks, on a console, what the ReXGlue SDK's PS5
# port relies on (rexglue_probe.c says what and why).
#
#   make PS5_PAYLOAD_SDK=<sdk>                  build rexglue_probe.elf
#   make test PS5_PAYLOAD_SDK=<sdk> PS5_HOST=<console address>
#                                               send it to the console's ELF loader
#
# <sdk> is the payload SDK fork installed by ps5/tools/build-rexglue.sh
# (.deps/native/ps5-payload-sdk): its context.h and corrected ucontext_t are
# what the fault probe reads registers through.

PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF := rexglue_probe.elf

CFLAGS := -std=c11 -O1 -g -Wall -Werror -fno-omit-frame-pointer

all: $(ELF)

$(ELF): rexglue_probe.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(ELF)

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^

.PHONY: all clean test
