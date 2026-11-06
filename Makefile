#####################################################################
# Config
#####################################################################
BIN := emu
TESTBIN := testprog

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
# testcase.c has its own main(), so it is linked into $(TESTBIN), not $(BIN)
SRC := $(filter-out testcase.c,$(wildcard *.c))
OBJ := $(SRC:.c=.o)
CORE_OBJ := core.o disasm.o opcodes.o

%.o: %.c $(HEADERS)
	 $(CC) $(CFLAGS) $(LDFLAGS) -c -o $@ $<

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TESTBIN): testcase.o $(CORE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

clean:
	-rm -f $(BIN) $(TESTBIN) $(OBJ) testcase.o

.PHONY: clean all

all: $(BIN) $(TESTBIN)
