#####################################################################
# Standalone regression tests.
#
# Each test_*.c is a self-contained program that exits non-zero if a
# check fails.  No curses, no framework, nothing to install.  The core's
# tests link core.c, opcodes.c and disasm.c; test_hc6800_plugin also links
# the sim layer, and runs three times: against the HC6800-ES board linked
# in and built as a shared object (hc6800_es.so, built here), and in a
# build without a dynamic loader (SIM_NO_DLOPEN), which must not refer to
# dlopen at all.
#
#     make -C tests check
#####################################################################
CFLAGS += -O2 -pipe -g -Wall -Wextra -Wno-unused-parameter -Wshadow -I..

CORE := ../core.c ../opcodes.c ../disasm.c
HEADERS := ../emu8051.h testrig.h
TESTS := $(patsubst %.c,%,$(wildcard test_*.c))

SIM_BOARD := ../sim/bus.c $(wildcard ../sim/devices/*.c) ../sim/boards/hc6800_es.c ../sim/boards/hc6800_es_plugin.c
SIM := ../sim/capi.c ../sim/plugin.c $(SIM_BOARD)
SIM_HEADERS := $(wildcard ../sim/*.h ../sim/devices/*.h ../sim/boards/*.h)

all: $(TESTS) test_hc6800_plugin_nodl hc6800_es.so

test_%: test_%.c $(CORE) $(HEADERS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(CORE)

test_hc6800_plugin: test_hc6800_plugin.c $(CORE) $(SIM) $(HEADERS) $(SIM_HEADERS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(CORE) $(SIM) -ldl

test_hc6800_plugin_nodl: test_hc6800_plugin.c $(CORE) $(SIM) $(HEADERS) $(SIM_HEADERS)
	$(CC) $(CFLAGS) -DSIM_NO_DLOPEN $(LDFLAGS) -o $@ $< $(CORE) $(SIM)

# The board as a plugin: complete on its own, only the entry point exported.
hc6800_es.so: ../sim/boards/hc6800_es_entry.c $(SIM_BOARD) $(SIM_HEADERS)
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -shared -Wl,-z,defs $(LDFLAGS) -o $@ \
		../sim/boards/hc6800_es_entry.c $(SIM_BOARD)

check: all
	@fail=0; for t in $(filter-out test_hc6800_plugin,$(TESTS)); do ./$$t || fail=1; done; \
	./test_hc6800_plugin ./hc6800_es.so || fail=1; \
	./test_hc6800_plugin_nodl || fail=1; \
	if nm test_hc6800_plugin_nodl | grep -q dlopen; then \
		echo "test_hc6800_plugin_nodl: refers to dlopen"; fail=1; \
	else echo "test_hc6800_plugin_nodl: no reference to dlopen"; fi; \
	exit $$fail

clean:
	-rm -f $(TESTS) test_hc6800_plugin_nodl hc6800_es.so

.PHONY: all check clean
