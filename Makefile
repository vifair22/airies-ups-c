PI_HOST  = sysadmin@upspi.internal.airies.net
PI_DIR   = /home/sysadmin/airies-ups-c
BIN      = airies-ups

SRCS     = src/main.c src/ups.c src/ups_srt.c src/ups_smt.c src/shutdown.c src/config.c src/ipc.c src/alerts.c
HDRS     = src/ups.h src/ups_driver.h src/shutdown.h src/config.h src/ipc.h src/alerts.h
LIBS     = -lmodbus -lcurl
CFLAGS   = -Wall -Wextra -O2 -std=c11 -D_POSIX_C_SOURCE=200809L
CC       = gcc

# Local build (for syntax checking — won't link without arm libs)
check:
	$(CC) $(CFLAGS) -fsyntax-only $(SRCS)

# Deploy source to Pi, compile there, run
deploy:
	ssh $(PI_HOST) "mkdir -p $(PI_DIR)/src"
	scp $(SRCS) $(PI_HOST):$(PI_DIR)/src/
	scp $(HDRS) $(PI_HOST):$(PI_DIR)/src/
	scp lib/log.h lib/pushover.h $(PI_HOST):$(PI_DIR)/src/
	ssh $(PI_HOST) "cd $(PI_DIR) && $(CC) $(CFLAGS) -o $(BIN) $(SRCS) $(LIBS)"

# Deploy and run
run: deploy
	ssh $(PI_HOST) "cd $(PI_DIR) && ./$(BIN)"

# Deploy and run with no-shutdown flags (safe testing)
run-safe: deploy
	ssh $(PI_HOST) "cd $(PI_DIR) && ./$(BIN) --no-ssh-shutdown --no-ups-shutdown --no-pi-shutdown"

# One-shot status
status: deploy
	ssh $(PI_HOST) "cd $(PI_DIR) && ./$(BIN) --status"

# Test SSH connectivity to shutdown targets
test-ssh: deploy
	ssh $(PI_HOST) "cd $(PI_DIR) && ./$(BIN) --test-ssh"

clean:
	ssh $(PI_HOST) "rm -f $(PI_DIR)/$(BIN)"

.PHONY: check deploy run run-safe status test-ssh clean
