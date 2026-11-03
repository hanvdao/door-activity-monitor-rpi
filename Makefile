CC      ?= cc
CSTD    ?= c17
CFLAGS  ?= -O2 -g
CFLAGS  += -std=$(CSTD) -Wall -Wextra -Werror -Wvla
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Iinclude -MMD -MP

BUILD ?= build
BIN   ?= bin

HUB_SRCS := $(wildcard src/hub/*.c)
SIM_SRCS := $(wildcard src/sim/*.c)
TEST_SRCS := tests/recv_check.c

HUB_OBJS := $(HUB_SRCS:%.c=$(BUILD)/%.o)
SIM_OBJS := $(SIM_SRCS:%.c=$(BUILD)/%.o)
TEST_OBJS := $(TEST_SRCS:%.c=$(BUILD)/%.o)

DOCKER_IMAGE := dam-dev
DOCKER_RUN := docker run --rm -v "$(CURDIR)":/src -w /src $(DOCKER_IMAGE)
DOCKER_MAKE := make BUILD=build-linux BIN=bin-linux

.PHONY: all hub sim test clean docker-image docker docker-test docker-shell

all: hub sim

hub: $(BIN)/dam-hub
sim: $(BIN)/dam-sim

$(BIN)/dam-hub: $(HUB_OBJS) | $(BIN)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BIN)/dam-sim: $(SIM_OBJS) | $(BIN)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) -lm

$(BIN)/recv_check: $(TEST_OBJS) | $(BIN)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test: $(BIN)/dam-sim $(BIN)/recv_check
	BIN=$(BIN) tests/sim_test.sh

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BIN):
	mkdir -p $@

clean:
	rm -rf $(BUILD) $(BIN) build-linux bin-linux

# Linux container for development on macOS (no SOCK_SEQPACKET there).
docker-image:
	docker build -t $(DOCKER_IMAGE) .

docker: docker-image
	$(DOCKER_RUN) $(DOCKER_MAKE) all

docker-test: docker-image
	$(DOCKER_RUN) $(DOCKER_MAKE) test

docker-shell: docker-image
	docker run --rm -it -v "$(CURDIR)":/src -w /src $(DOCKER_IMAGE) bash

-include $(HUB_OBJS:.o=.d) $(SIM_OBJS:.o=.d) $(TEST_OBJS:.o=.d)
