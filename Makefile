CC      ?= cc
CSTD    ?= c17
CFLAGS  ?= -O2 -g
CFLAGS  += -std=$(CSTD) -Wall -Wextra -Werror -Wvla
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Iinclude -MMD -MP

BUILD := build
BIN   := bin

HUB_SRCS := $(wildcard src/hub/*.c)
SIM_SRCS := $(wildcard src/sim/*.c)

HUB_OBJS := $(HUB_SRCS:%.c=$(BUILD)/%.o)
SIM_OBJS := $(SIM_SRCS:%.c=$(BUILD)/%.o)

.PHONY: all hub sim clean

all: hub sim

hub: $(BIN)/dam-hub
sim: $(BIN)/dam-sim

$(BIN)/dam-hub: $(HUB_OBJS) | $(BIN)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BIN)/dam-sim: $(SIM_OBJS) | $(BIN)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BIN):
	mkdir -p $@

clean:
	rm -rf $(BUILD) $(BIN)

-include $(HUB_OBJS:.o=.d) $(SIM_OBJS:.o=.d)
