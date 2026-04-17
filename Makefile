CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
LDFLAGS ?=
LDLIBS  ?= -lssl -lcrypto

ifeq ($(shell uname),Darwin)
    BREW_OPENSSL := $(shell brew --prefix openssl@3 2>/dev/null || brew --prefix openssl 2>/dev/null)
    ifneq ($(BREW_OPENSSL),)
        CFLAGS  += -I$(BREW_OPENSSL)/include
        LDFLAGS += -L$(BREW_OPENSSL)/lib
    endif
endif

bench-sha256: bench-sha256.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

run: bench-sha256
	./bench-sha256

clean:
	rm -f bench-sha256

.PHONY: run clean
