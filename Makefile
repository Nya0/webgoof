CC := musl-gcc

CFLAGS  += -Wall -Wextra -D_GNU_SOURCE -idirafter /usr/include
LIBS    := -pthread

TARGET  := $(shell uname -s | tr '[A-Z]' '[a-z]' 2>/dev/null || echo unknown)

ifeq ($(TARGET), linux)
	CFLAGS  += -D_POSIX_C_SOURCE -D_DEFAULT_SOURCE
endif

SRC := $(wildcard src/*.c)

BIN  := http-server
VER  ?= $(shell git describe --tags --always --dirty)

ODIR := obj
OBJ  := $(patsubst src/%.c,$(ODIR)/%.o,$(SRC))
LIBS := -luring $(LIBS)

DEPS    := $(wildcard include/*.h)
CFLAGS  += -I$(ODIR)/include  -Iinclude
LDFLAGS += -L$(ODIR)/lib

CFLAGS += -DLOG_LEVEL=$(LOG_LEVEL)

all: $(BIN)

clean:
	$(RM) -rf $(BIN) obj/*

$(BIN): $(OBJ) $(ODIR)/lib/liburing.a
	@echo LINK $(BIN)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

$(OBJ): Makefile $(DEPS) | $(ODIR)

$(ODIR):
	@mkdir -p $@

$(ODIR)/bytecode.o: src/wrk.lua
	@echo LUAJIT $<
	@$(SHELL) -c 'PATH=obj/bin:$(PATH) luajit -b $(CURDIR)/$< $(CURDIR)/$@'

$(ODIR)/version.o:
	@echo 'const char *VERSION="$(VER)";' | $(CC) -xc -c -o $@ -

$(ODIR)/%.o : %.c
	@echo CC $<
	@$(CC) $(CFLAGS) -c -o $@ $<

# dependiencies

DEPS += $(ODIR)/lib/liburing.a

LIBURING := $(notdir $(patsubst %.tar.gz,%,$(wildcard libs/liburing*.tar.gz)))


$(ODIR)/$(LIBURING): libs/$(LIBURING).tar.gz | $(ODIR)
	@rm -rf $@
	@mkdir -p $@
	@tar -C $@ --strip-components=1 -xf $<

$(ODIR)/lib/liburing.a: $(ODIR)/$(LIBURING)
	@echo Building liburing...
	@mkdir -p $(ODIR)/lib
	@cd $< && ./configure --cc=musl-gcc --cxx=musl-gcc
	@$(MAKE) -C $</src \
		CC="musl-gcc -idirafter /usr/include" \
		ENABLE_SHARED=0
	@cp $</src/liburing.a $(ODIR)/lib/liburing.a

# ------------

.PHONY: all clean
.PHONY: $(ODIR)/version.o

.SUFFIXES:
.SUFFIXES: .c .o .lua

vpath %.c   src
vpath %.h   src
vpath %.lua scripts
