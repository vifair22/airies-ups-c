PI_HOST    = sysadmin@upspi.internal.airies.net
PI_DIR     = /home/sysadmin/airies-ups
CUTILS_DIR = ../c-utils
BUILD_DIR  = build

CC       = gcc
CFLAGS   = -Wall -Wextra -Wpedantic -Wshadow -Wunused -Wunused-function \
           -Wunused-variable -Wunused-parameter -Wunused-result \
           -Wdouble-promotion -Wformat=2 -Wformat-truncation \
           -Wmissing-prototypes -Wstrict-prototypes -Wmissing-declarations \
           -Wcast-align -Wcast-qual -Wnull-dereference \
           -Wconversion -Wsign-conversion \
           -fstack-protector-strong -fstack-clash-protection \
           -O2 -std=c11 -D_POSIX_C_SOURCE=200809L \
           -Isrc -I$(CUTILS_DIR)/include -I$(CUTILS_DIR)/lib/cJSON
LIBS     = -L$(CUTILS_DIR) -lc-utils -lmodbus -lsqlite3 -lcurl -lcrypto -lpthread

# --- Source files ---

# UPS driver layer (shared between daemon and CLI via the daemon)
UPS_SRCS   = src/ups/ups.c src/ups/ups_srt.c src/ups/ups_smt.c

# Daemon sources
DAEMON_SRCS = src/daemon/main.c \
              $(UPS_SRCS)

# CLI sources
CLI_SRCS    = src/cli/main.c

# All sources (for syntax checking)
ALL_SRCS    = $(DAEMON_SRCS) $(CLI_SRCS)

# --- Targets ---

all: $(BUILD_DIR)/airies-upsd $(BUILD_DIR)/airies-ups

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/airies-upsd: $(DAEMON_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(DAEMON_SRCS) $(LIBS)

$(BUILD_DIR)/airies-ups: $(CLI_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(CLI_SRCS) $(LIBS)

# Syntax check (local, no linking)
check:
	$(CC) $(CFLAGS) -fsyntax-only $(ALL_SRCS)

# Static analysis
analyze: check
	$(CC) $(CFLAGS) -fanalyzer $(ALL_SRCS) -fsyntax-only
	cppcheck --enable=warning,performance,portability \
	         --suppress=missingIncludeSystem \
	         -Isrc -I$(CUTILS_DIR)/include -I$(CUTILS_DIR)/lib/cJSON src/

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all check analyze clean
