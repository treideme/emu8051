#####################################################################
# Config
#####################################################################
BIN := emu

CFLAGS += -O2
CFLAGS += -pipe
CFLAGS += -g -Wall -Wextra -Wno-unused-parameter -Wshadow

# Uncomment to activate LTO
#CFLAGS += -flto

LDLIBS += -lcurses

#####################################################################
# Rules
#####################################################################
HEADERS := $(wildcard *.h)
SRC := $(wildcard *.c)
OBJ := $(SRC:.c=.o)

%.o: %.c $(HEADERS)
	 $(CC) $(CFLAGS) $(LDFLAGS) -c -o $@ $<

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

clean:
	-rm -f $(BIN) $(OBJ)

#####################################################################
# The peripheral simulation layer (sim/), without meson: the library
# the Python bindings load, and the HC6800-ES board as a plugin of its
# own, the same source that the library links in.
#
#     make sim                      # build/libem8051sim.so, build/hc6800_es.so
#     make sim SIM_BUILD=elsewhere  # next to a meson build/ of the same tree
#####################################################################
SIM_BUILD ?= build
SIM_CFLAGS := $(CFLAGS) -std=gnu99 -fPIC -I.
CORE_SRC := core.c disasm.c opcodes.c
SIM_BOARD_SRC := sim/bus.c $(wildcard sim/devices/*.c) sim/boards/hc6800_es.c sim/boards/hc6800_es_plugin.c
SIM_SRC := sim/capi.c sim/plugin.c $(SIM_BOARD_SRC)
SIM_HEADERS := emu8051.h $(wildcard sim/*.h sim/devices/*.h sim/boards/*.h)

sim: $(SIM_BUILD)/libem8051sim.so $(SIM_BUILD)/hc6800_es.so

$(SIM_BUILD)/libem8051sim.so: $(CORE_SRC) $(SIM_SRC) $(SIM_HEADERS)
	mkdir -p $(SIM_BUILD)
	$(CC) $(SIM_CFLAGS) -shared $(LDFLAGS) -o $@ $(CORE_SRC) $(SIM_SRC) -ldl

# A plugin: complete on its own (-z defs), only its entry point exported.
$(SIM_BUILD)/hc6800_es.so: sim/boards/hc6800_es_entry.c $(SIM_BOARD_SRC) $(SIM_HEADERS)
	mkdir -p $(SIM_BUILD)
	$(CC) $(SIM_CFLAGS) -fvisibility=hidden -shared -Wl,-z,defs $(LDFLAGS) -o $@ \
		sim/boards/hc6800_es_entry.c $(SIM_BOARD_SRC)

.PHONY: clean all sim

all: $(BIN)
