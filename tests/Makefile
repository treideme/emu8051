#####################################################################
# Standalone regression tests for the emulator core.
#
# Each test_*.c is a self-contained program that links core.c,
# opcodes.c and disasm.c and exits non-zero if a check fails.  No
# curses, no framework, nothing to install.
#
#     make -C tests check
#####################################################################
CFLAGS += -O2 -pipe -g -Wall -Wextra -Wno-unused-parameter -Wshadow -I..

CORE := ../core.c ../opcodes.c ../disasm.c
HEADERS := ../emu8051.h testrig.h
TESTS := $(patsubst %.c,%,$(wildcard test_*.c))

all: $(TESTS)

test_%: test_%.c $(CORE) $(HEADERS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(CORE)

check: $(TESTS)
	@fail=0; for t in $(TESTS); do ./$$t || fail=1; done; exit $$fail

clean:
	-rm -f $(TESTS)

.PHONY: all check clean
