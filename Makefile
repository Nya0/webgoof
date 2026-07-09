# Compiler
CC := gcc

# flags
CFLAGS := -Wall -Wextra -Iinclude -D_GNU_SOURCE
DEBUG_FLAGS := -g -O2 -fno-omit-frame-pointer
PROD_FLAGS := -Os -DNDEBUG -march=native -flto \
  -ffunction-sections -fdata-sections -Wl,--gc-sections -s -static
  
LDFLAGS := -pthread

# Default to debug
BUILD_TYPE ?= debug

ifeq ($(BUILD_TYPE), debug)
	CFLAGS += $(DEBUG_FLAGS) -DLOG_LEVEL=4
	LDFLAGS += -fsanitize=address -fsanitize=undefined
else ifeq ($(BUILD_TYPE), prod)
	CC = musl-gcc
	CFLAGS += $(PROD_FLAGS) -DLOG_LEVEL=0
else ifeq ($(BUILD_TYPE), dev)
	CFLAGS += $(DEBUG_FLAGS)
endif

TARGET := bin/http-server
SRC := $(wildcard src/*.c)
OBJ := $(patsubst src/%.c, obj/%.o, $(SRC))
DEPS := $(wildcard include/*.h)

.PHONY: default debug prod dev clean run

default: debug

debug:
	@make clean
	@BUILD_TYPE=debug make $(TARGET)
	@echo "Debug build complete"

prod: 
	@make clean
	@BUILD_TYPE=prod make $(TARGET)
	@echo "Production build complete"

run: default
	./$(TARGET)

clean:
	rm -f obj/*.o bin/*

dev:
	@find src include -type f | entr -r sh -c 'BUILD_TYPE=debug make run'

$(TARGET): $(OBJ) | bin
	$(CC) -o $@ $^ $(LDFLAGS)

obj/%.o: src/%.c $(DEPS) | obj
	$(CC) $(CFLAGS) -c $< -o $@

bin obj:
	mkdir -p $@
