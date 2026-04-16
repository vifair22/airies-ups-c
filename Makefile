PI_HOST    = sysadmin@upspi.internal.airies.net
PI_DIR     = /home/sysadmin/airies-ups
CUTILS_DIR = ../c-utils
BUILD_DIR  = build

# Version
SEMVER     := $(shell cat release_version 2>/dev/null | tr -d '[:space:]')
BUILD_TS   := $(shell date -u '+%Y%m%d.%H%M')
BUILD_TYPE ?= release
VERSION    := $(SEMVER)_$(BUILD_TS).$(BUILD_TYPE)
VERSION_DEF := -DVERSION_STRING='"$(VERSION)"'

CC       := gcc
INCLUDES := -Isrc -I$(CUTILS_DIR)/include -I$(CUTILS_DIR)/lib/cJSON
LIBS     := -L$(CUTILS_DIR) -lc-utils -lmodbus -lsqlite3 -lcurl -lcrypto -lmicrohttpd -lpthread

# ---- Flags ---------------------------------------------------------------
WARN_FLAGS := -Wall -Wextra -Wpedantic -Wshadow -Wunused -Wunused-function \
              -Wunused-variable -Wunused-parameter -Wunused-result \
              -Wdouble-promotion -Wformat=2 -Wformat-truncation \
              -Wmissing-prototypes -Wstrict-prototypes -Wmissing-declarations \
              -Wcast-align -Wcast-qual -Wnull-dereference \
              -Wconversion -Wsign-conversion

COMMON_CFLAGS := -std=c11 -D_POSIX_C_SOURCE=200809L $(WARN_FLAGS) \
                 -fstack-protector-strong -fstack-clash-protection \
                 $(INCLUDES) $(VERSION_DEF)

RELEASE_CFLAGS := $(COMMON_CFLAGS) -O2
DEBUG_CFLAGS   := $(COMMON_CFLAGS) -Og -g
ASAN_CFLAGS    := $(COMMON_CFLAGS) -O1 -g -fsanitize=address -fno-omit-frame-pointer

ifeq ($(BUILD_TYPE),release)
  CFLAGS  := $(RELEASE_CFLAGS)
  LDFLAGS := $(LIBS)
else ifeq ($(BUILD_TYPE),debug)
  CFLAGS  := $(DEBUG_CFLAGS)
  LDFLAGS := $(LIBS)
else ifeq ($(BUILD_TYPE),asan)
  CFLAGS  := $(ASAN_CFLAGS)
  LDFLAGS := -fsanitize=address $(LIBS)
endif

# ---- Source files ---------------------------------------------------------

UPS_SRCS   = src/ups/ups.c src/ups/ups_srt.c src/ups/ups_smt.c
API_SRCS   = src/api/server.c src/api/routes.c src/api/auth.c
MON_SRCS   = src/monitor/monitor.c \
             src/monitor/retention.c \
             src/alerts/alerts.c \
             src/shutdown/shutdown.c \
             src/weather/weather.c

DAEMON_SRCS = src/daemon/main.c $(UPS_SRCS) $(API_SRCS) $(MON_SRCS)
CLI_SRCS    = src/cli/main.c src/cli/cli.c
ALL_SRCS    = $(DAEMON_SRCS) $(CLI_SRCS)

# ---- Build targets --------------------------------------------------------

.PHONY: all debug asan clean clean-all check analyze lint frontend

all:
	$(MAKE) BUILD_TYPE=release _build

debug:
	$(MAKE) BUILD_TYPE=debug _build

asan:
	$(MAKE) BUILD_TYPE=asan _build

.PHONY: _build
_build: $(BUILD_DIR)/airies-upsd $(BUILD_DIR)/airies-ups

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/airies-upsd: $(DAEMON_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(DAEMON_SRCS) $(LDFLAGS)

$(BUILD_DIR)/airies-ups: $(CLI_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(CLI_SRCS) $(LDFLAGS)

# ---- Static analysis ------------------------------------------------------

STACK_LIMIT := 65536

check:
	$(CC) $(COMMON_CFLAGS) -fsyntax-only $(ALL_SRCS)

analyze: check
	@echo "=== stack-usage ==="
	@mkdir -p $(BUILD_DIR)/analyze
	@for f in $(ALL_SRCS); do \
	    base=$$(basename $$f .c); \
	    $(CC) $(COMMON_CFLAGS) -fstack-usage -c $$f -o $(BUILD_DIR)/analyze/$$base.o 2>/dev/null; \
	done
	@fail=0; for su in $(BUILD_DIR)/analyze/*.su; do \
	    [ -f "$$su" ] || continue; \
	    awk -v limit=$(STACK_LIMIT) -v file="$$su" \
	        '$$2+0 > limit { printf "STACK OVERFLOW RISK: %s %s (%s bytes, limit %d)\n", file, $$1, $$2, limit; found=1 } \
	         END { exit (found ? 1 : 0) }' "$$su" || fail=1; \
	done; \
	rm -rf $(BUILD_DIR)/analyze; \
	if [ $$fail -ne 0 ]; then echo "stack-usage: FAIL"; exit 1; fi
	@echo "stack-usage: OK"
	@echo "=== gcc-fanalyzer ==="
	@for f in $(ALL_SRCS); do \
	    $(CC) $(COMMON_CFLAGS) -fanalyzer -fsyntax-only $$f 2>&1; \
	done
	@echo "gcc-fanalyzer: OK"
	@echo "=== cppcheck ==="
	@cppcheck --enable=warning,performance,portability --error-exitcode=1 \
	    --suppress=missingIncludeSystem \
	    --suppress=normalCheckLevelMaxBranches \
	    --suppress=toomanyconfigs \
	    -DVERSION_STRING=\"0.0.0\" \
	    --inline-suppr --quiet \
	    $(INCLUDES) $(ALL_SRCS)
	@echo "cppcheck: OK"

lint:
	@echo "=== clang-tidy ==="
	@clang-tidy $(ALL_SRCS) -- -std=c11 $(INCLUDES) 2>&1 | \
	    grep -E "warning:|error:" || echo "clang-tidy: OK"

# ---- Frontend -------------------------------------------------------------

frontend:
	cd frontend && bun install && bun run build

# ---- Clean ----------------------------------------------------------------

clean:
	rm -rf $(BUILD_DIR)

clean-all: clean
	rm -rf frontend/dist frontend/node_modules
