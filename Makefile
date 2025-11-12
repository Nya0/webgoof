# comipiler
CC := gcc
CFLAGS := -g -Wall -Wextra -Iinclude
SANITIZERS := -fsanitize=address -fsanitize=undefined
LDFLAGS := $(SANITIZERS)

TARGET := bin/http-server
SRC := $(wildcard src/*.c)
OBJ := $(patsubst src/%.c, obj/%.o, $(SRC))
DEPS := $(wildcard include/*.h)

.PHONY: all run clean default

all: run

default: $(TARGET)

run: clean default
	./$(TARGET)

clean:
	rm -f obj/*.o
	rm -f bin/*

$(TARGET): $(OBJ) | bin
	$(CC) $(LDFLAGS) -o $@ $^

obj/%.o: src/%.c $(DEPS) | obj
	$(CC) $(CFLAGS) $(SANITIZERS) -c $< -o $@

bin obj:
	mkdir -p $@