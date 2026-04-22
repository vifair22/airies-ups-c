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
LIBS     := -L$(CUTILS_DIR)/build -lc-utils -lmodbus -lsqlite3 -lcurl -lcrypto -lmicrohttpd -lpthread -lm

# ---- Cross-compilation (aarch64 Pi target) --------------------------------

CROSS_PREFIX  := aarch64-unknown-linux-gnu-
SYSROOT       := $(HOME)/.sysroot/aarch64
CROSS_CC      := $(CROSS_PREFIX)gcc
CROSS_AR      := $(CROSS_PREFIX)ar
CROSS_INCLUDES = $(INCLUDES) -I$(SYSROOT)/usr/include
CROSS_LIBS     = -L$(CUTILS_DIR)/build -lc-utils -L$(SYSROOT)/usr/lib \
                 -Wl,--allow-shlib-undefined -Wl,-rpath-link,$(SYSROOT)/usr/lib \
                 -lmodbus -lsqlite3 -lcurl -lcrypto -lmicrohttpd -lpthread -lm

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

UPS_SRCS   = src/ups/ups.c src/ups/ups_format.c src/ups/ups_srt.c src/ups/ups_smt.c \
             src/ups/ups_backups_hid.c src/ups/hid_parser.c
API_SRCS   = src/api/server.c src/api/auth.c \
             src/api/routes/routes.c src/api/routes/auth.c \
             src/api/routes/shutdown.c src/api/routes/config.c \
             src/api/routes/weather.c
MON_SRCS   = src/monitor/monitor.c \
             src/monitor/retention.c \
             src/alerts/alerts.c \
             src/shutdown/shutdown.c \
             src/weather/weather.c

DAEMON_SRCS = src/daemon/main.c $(UPS_SRCS) $(API_SRCS) $(MON_SRCS)
CLI_SRCS    = src/cli/main.c src/cli/cli.c src/cli/commands.c
ALL_SRCS    = $(DAEMON_SRCS) $(CLI_SRCS)

# ---- Object files ---------------------------------------------------------

OBJ_DIR     = $(BUILD_DIR)/obj
DAEMON_OBJS = $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(DAEMON_SRCS))
CLI_OBJS    = $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(CLI_SRCS))

# Pattern rule: src/**/*.c → build/obj/**/*.o (mirrors directory structure)
$(OBJ_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- Build targets --------------------------------------------------------

.PHONY: all debug asan cross clean clean-all check analyze lint frontend frontend-test embed-frontend embed-migrations test coverage

all: frontend frontend-test embed-frontend embed-migrations test
	$(MAKE) BUILD_TYPE=release EMBED=1 _build

cross: frontend frontend-test embed-frontend embed-migrations
	@echo "=== Building c-utils natively for tests ==="
	$(MAKE) -C $(CUTILS_DIR) clean
	$(MAKE) -C $(CUTILS_DIR)
	$(MAKE) test
	@echo "=== Cross-compiling c-utils for aarch64 ==="
	$(MAKE) -C $(CUTILS_DIR) clean
	$(MAKE) -C $(CUTILS_DIR) \
		CC="$(CROSS_CC)" \
		AR="$(CROSS_AR)" \
		"COMMON_CFLAGS=-std=c11 -D_POSIX_C_SOURCE=200809L $(WARN_FLAGS) -fstack-protector-strong -fstack-clash-protection -Iinclude -Ilib/cJSON -I$(SYSROOT)/usr/include"
	@echo "=== Cross-compiling airies-ups for aarch64 ==="
	rm -rf $(BUILD_DIR)/obj
	$(MAKE) EMBED=1 CC="$(CROSS_CC)" \
		CFLAGS='-std=c11 -D_POSIX_C_SOURCE=200809L $(WARN_FLAGS) -fstack-protector-strong -fstack-clash-protection $(CROSS_INCLUDES) -DVERSION_STRING="\"$(VERSION)\"" -O2 -DEMBED_FRONTEND' \
		"LDFLAGS=$(CROSS_LIBS)" \
		_build

debug: embed-migrations
	$(MAKE) BUILD_TYPE=debug _build

asan: embed-migrations
	$(MAKE) BUILD_TYPE=asan _build

.PHONY: _build
_build: $(BUILD_DIR)/airies-upsd $(BUILD_DIR)/airies-ups

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# ---- Embedded frontend assets ---------------------------------------------

EMBED_SRC = $(BUILD_DIR)/embedded_assets.c
EMBED_OBJ = $(OBJ_DIR)/embedded_assets.o

embed-frontend: $(EMBED_SRC)

$(EMBED_SRC): $(wildcard frontend/dist/**) | $(BUILD_DIR)
	scripts/embed_frontend.sh frontend/dist > $@

ifeq ($(EMBED),1)
  CFLAGS  += -DEMBED_FRONTEND
  DAEMON_OBJS += $(EMBED_OBJ)

$(EMBED_OBJ): $(EMBED_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wno-unused-const-variable -c $< -o $@
endif

# ---- Embedded SQL migrations ----------------------------------------------

MIGRATE_SRC = $(BUILD_DIR)/migrations_compiled.c
MIGRATE_OBJ = $(OBJ_DIR)/migrations_compiled.o

embed-migrations: $(MIGRATE_SRC)

$(MIGRATE_SRC): $(wildcard migrations/*.sql) | $(BUILD_DIR)
	$(CUTILS_DIR)/tools/embed_sql.sh migrations/ app_migrations > $@

$(MIGRATE_OBJ): $(MIGRATE_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

DAEMON_OBJS += $(MIGRATE_OBJ)

# ---- Link targets ---------------------------------------------------------

$(BUILD_DIR)/airies-upsd: $(DAEMON_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR)/airies-ups: $(CLI_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

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

# ---- Testing --------------------------------------------------------------
#
# Test binaries are defined once via the build_test macro.
# 'make test' compiles with debug flags; 'make coverage' adds --coverage.
# Each test declares its source files and link libraries.

TEST_CFLAGS := $(COMMON_CFLAGS) -Og -g

# Link library groups
TEST_COMMON_LIBS := -lcmocka -lpthread
TEST_FULL_LIBS   := $(TEST_COMMON_LIBS) -L$(CUTILS_DIR)/build -lc-utils -lsqlite3 -lcurl -lcrypto
TEST_MATH_LIBS   := $(TEST_COMMON_LIBS) -lm

# Test definitions: name, sources, libraries
#   $(1) = test name (e.g., test_ups_strings)
#   $(2) = source files
#   $(3) = link libraries
#   $(4) = output directory
#   $(5) = extra CFLAGS
define build_test
$(4)/$(1): $(2) | $(4)
	$$(CC) $(TEST_CFLAGS) $(5) -o $$@ $$^ $(3)
endef

TEST_DIR = $(BUILD_DIR)/tests
COV_DIR  = $(BUILD_DIR)/coverage
COV_TEST = $(COV_DIR)/tests

$(TEST_DIR) $(COV_TEST):
	mkdir -p $@

# --- Test definitions (single source of truth) ---

$(eval $(call build_test,test_ups_strings,\
  tests/test_ups_strings.c src/ups/ups_format.c,\
  $(TEST_COMMON_LIBS),$(TEST_DIR),))

$(eval $(call build_test,test_hid_parser,\
  tests/test_hid_parser.c src/ups/hid_parser.c,\
  $(TEST_MATH_LIBS),$(TEST_DIR),))

$(eval $(call build_test,test_alerts,\
  tests/test_alerts.c src/alerts/alerts.c src/ups/ups_format.c,\
  $(TEST_FULL_LIBS),$(TEST_DIR),))

$(eval $(call build_test,test_cli,\
  tests/test_cli.c src/cli/cli.c,\
  $(TEST_COMMON_LIBS),$(TEST_DIR),))

$(eval $(call build_test,test_auth,\
  tests/test_auth.c src/api/auth.c,\
  $(TEST_FULL_LIBS),$(TEST_DIR),))

$(eval $(call build_test,test_shutdown,\
  tests/test_shutdown.c src/shutdown/shutdown.c src/ups/ups.c src/ups/ups_format.c tests/test_stubs.c,\
  $(TEST_FULL_LIBS) -lmicrohttpd,$(TEST_DIR),))

$(eval $(call build_test,test_config_validation,\
  tests/test_config_validation.c src/ups/ups.c src/ups/ups_format.c tests/test_stubs.c,\
  $(TEST_FULL_LIBS),$(TEST_DIR),))

# --- Coverage builds (same definitions, --coverage flag) ---

$(eval $(call build_test,test_ups_strings,\
  tests/test_ups_strings.c src/ups/ups_format.c,\
  $(TEST_COMMON_LIBS),$(COV_TEST),--coverage))

$(eval $(call build_test,test_hid_parser,\
  tests/test_hid_parser.c src/ups/hid_parser.c,\
  $(TEST_MATH_LIBS),$(COV_TEST),--coverage))

$(eval $(call build_test,test_alerts,\
  tests/test_alerts.c src/alerts/alerts.c src/ups/ups_format.c,\
  $(TEST_FULL_LIBS),$(COV_TEST),--coverage))

$(eval $(call build_test,test_cli,\
  tests/test_cli.c src/cli/cli.c,\
  $(TEST_COMMON_LIBS),$(COV_TEST),--coverage))

$(eval $(call build_test,test_auth,\
  tests/test_auth.c src/api/auth.c,\
  $(TEST_FULL_LIBS),$(COV_TEST),--coverage))

$(eval $(call build_test,test_shutdown,\
  tests/test_shutdown.c src/shutdown/shutdown.c src/ups/ups.c src/ups/ups_format.c tests/test_stubs.c,\
  $(TEST_FULL_LIBS) -lmicrohttpd,$(COV_TEST),--coverage))

$(eval $(call build_test,test_config_validation,\
  tests/test_config_validation.c src/ups/ups.c src/ups/ups_format.c tests/test_stubs.c,\
  $(TEST_FULL_LIBS),$(COV_TEST),--coverage))

# --- Test names (used by both targets) ---

TEST_NAMES := test_ups_strings test_hid_parser test_alerts test_cli test_auth test_shutdown test_config_validation

# --- Run targets ---

define run_tests
@pass=0; fail=0; \
for name in $(TEST_NAMES); do \
    echo "=== $$name ==="; \
    if $(1)/$$name; then pass=$$((pass + 1)); \
    else fail=$$((fail + 1)); fi; \
done; \
echo ""; \
echo "$$pass passed, $$fail failed"; \
[ $$fail -eq 0 ]
endef

test: $(addprefix $(TEST_DIR)/,$(TEST_NAMES))
	$(call run_tests,$(TEST_DIR))

coverage: $(addprefix $(COV_TEST)/,$(TEST_NAMES))
	@for name in $(TEST_NAMES); do \
	    echo "=== $$name ==="; \
	    $(COV_TEST)/$$name || exit 1; \
	done
	gcovr --root . \
	    --filter 'src/' \
	    --exclude 'src/daemon/' \
	    --html-details $(COV_DIR)/index.html \
	    --print-summary
	@echo ""
	@echo "Coverage report: $(COV_DIR)/index.html"

# ---- Frontend -------------------------------------------------------------

frontend:
	cd frontend && bun install && bun run build

frontend-test:
	cd frontend && bun run test

# ---- Clean ----------------------------------------------------------------

clean:
	rm -rf $(BUILD_DIR)

clean-all: clean
	rm -rf frontend/dist frontend/node_modules
