# CupidChat build system 
# Targets:   cupid-chatd  (server binary)
#            cupid-chat   (client TUI binary)
#            test-*       (unit-test binaries)
#            all / clean / check
#
# Requires:  gcc (C11), ncurses-dev, libreadline (optional)
# TLS:       pass TLS=1 to enable OpenSSL (skeleton is wired in already)
# 

CC      := gcc
STD     := -std=c11
WARN    := -Wall -Wextra -Wpedantic -Wno-unused-parameter -Werror
SAN     := -fsanitize=address,undefined -fno-omit-frame-pointer
OPT     := -O2 -g $(SAN)
DEFS    := -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L

CFLAGS  := $(STD) $(WARN) $(OPT) $(DEFS) -Iinclude -MMD -MP

# Keep 'all' as the default goal even though generated-file rules appear
# before it in this file.
.DEFAULT_GOAL := all

# Optional TLS (OpenSSL) 
ifeq ($(TLS),1)
CFLAGS  += -DCUPID_TLS
LDTLS   := -lssl -lcrypto
else
LDTLS   :=
endif

# Directories 
BUILDDIR := build
SRVDIR   := src/server
CLIDIR   := src/client
SHDIR    := src/shared
TESTDIR  := tests

$(shell mkdir -p $(BUILDDIR)/server/core  \
                 $(BUILDDIR)/server/util  \
                 $(BUILDDIR)/server/db    \
                 $(BUILDDIR)/server       \
                 $(BUILDDIR)/client/net   \
                 $(BUILDDIR)/client/state \
                 $(BUILDDIR)/client/ui    \
                 $(BUILDDIR)/shared/proto \
                 $(BUILDDIR)/shared/net   \
                 $(BUILDDIR)/tests/proto  \
                 $(BUILDDIR)/tests/server)

# Embedded sound data — generated from sounds/*.wav at build time
SOUNDS_GEN_C := src/client/sounds_data.c
SOUNDS_GEN_H := include/client/sounds_data.h
# N.B. "You've Got Mail.wav" is omitted — make cannot handle apostrophes in
# prerequisite names.  Modify gen_sounds.py or touch any listed WAV to force
# regeneration if that file changes.
SOUNDS_WAVS  := sounds/Welcome.wav sounds/Goodbye.wav \
                sounds/IM.wav sounds/BuddyIn.wav sounds/BuddyOut.wav

$(SOUNDS_GEN_C) $(SOUNDS_GEN_H): $(SOUNDS_WAVS) scripts/gen_sounds.py
	@echo "  GEN  sounds_data"
	@python3 scripts/gen_sounds.py sounds $(SOUNDS_GEN_C) $(SOUNDS_GEN_H)

# Shared sources 
SHARED_SRC := \
    $(SHDIR)/proto/frame.c \
    $(SHDIR)/proto/tlv.c   \
    $(SHDIR)/net/transport_posix.c

ifeq ($(TLS),1)
SHARED_SRC += $(SHDIR)/net/transport_tls.c
else
SHARED_SRC += $(SHDIR)/net/transport_tls_stub.c
endif

SHARED_OBJ := $(patsubst src/%.c, $(BUILDDIR)/%.o, $(SHARED_SRC))

# Server sources 
SERVER_SRC := \
    $(SRVDIR)/main.c          \
    $(SRVDIR)/core/loop.c     \
    $(SRVDIR)/core/conn.c     \
    $(SRVDIR)/core/dispatch.c \
    $(SRVDIR)/core/room.c     \
    $(SRVDIR)/core/state.c    \
    $(SRVDIR)/core/keepalive.c \
    $(SRVDIR)/core/rate_limit.c \
    $(SRVDIR)/core/backpressure.c \
    $(SRVDIR)/util/log.c      \
    $(SRVDIR)/util/config.c   \
    $(SRVDIR)/db/db.c

SERVER_OBJ := $(patsubst src/%.c, $(BUILDDIR)/%.o, $(SERVER_SRC))
SERVER_BIN := cupid-chatd

# Client sources 
CLIENT_SRC := \
    $(CLIDIR)/main.c               \
    $(CLIDIR)/net/client_conn.c    \
    $(CLIDIR)/state/model.c        \
    $(CLIDIR)/state/history.c      \
    $(CLIDIR)/ui/layout.c          \
    $(CLIDIR)/ui/input.c           \
    $(CLIDIR)/ui/render.c          \
    $(CLIDIR)/sound.c        \
    $(SOUNDS_GEN_C)

CLIENT_OBJ := $(patsubst src/%.c, $(BUILDDIR)/%.o, $(CLIENT_SRC))

# Ensure the generated header exists before any client source is compiled.
# The .d files track it for rebuilds; this covers the initial build.
$(CLIENT_OBJ): | $(SOUNDS_GEN_H)
CLIENT_BIN := cupid-chat

# Test sources 
TEST_BINS := \
    $(BUILDDIR)/tests/proto/test_frame \
    $(BUILDDIR)/tests/proto/test_tlv   \
    $(BUILDDIR)/tests/server/test_rate_limit

# Default target 
.PHONY: all clean check install

all: $(SERVER_BIN) $(CLIENT_BIN)

# Link binaries 
$(SERVER_BIN): $(SERVER_OBJ) $(SHARED_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDTLS) -lsqlite3 -lcrypt

$(CLIENT_BIN): $(CLIENT_OBJ) $(SHARED_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ -lncurses $(LDTLS)

# Compile rule 
$(BUILDDIR)/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Tests 
$(BUILDDIR)/tests/proto/test_frame: $(TESTDIR)/proto/test_frame.c $(SHARED_OBJ)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILDDIR)/tests/proto/test_tlv: $(TESTDIR)/proto/test_tlv.c $(SHARED_OBJ)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILDDIR)/tests/server/test_rate_limit: \
    $(TESTDIR)/server/test_rate_limit.c \
    $(BUILDDIR)/server/core/rate_limit.o \
    $(SHARED_OBJ)
	$(CC) $(CFLAGS) $^ -o $@

check: $(TEST_BINS)
	@echo "=== Running unit tests ==="
	@for t in $(TEST_BINS); do echo "--- $$t ---"; $$t; done
	@echo "=== All tests done ==="

clean:
	rm -rf $(BUILDDIR) $(SERVER_BIN) $(CLIENT_BIN) \
	        $(SOUNDS_GEN_C) $(SOUNDS_GEN_H)

# Auto-generated header dependencies
-include $(shell find $(BUILDDIR) -name '*.d' 2>/dev/null)

install: all
	install -Dm755 $(SERVER_BIN) /usr/local/bin/$(SERVER_BIN)
	install -Dm755 $(CLIENT_BIN) /usr/local/bin/$(CLIENT_BIN)
