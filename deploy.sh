#!/usr/bin/env bash
#
# deploy.sh — build and deploy airies-ups to the Pis
#
# The frontend is embedded into the daemon binary at build time, so
# a single 'make all' on the Pi produces a self-contained binary.
# The dev machine needs bun (frontend build) and brotli (asset compression).
#
# Usage:
#   ./deploy.sh              # full deploy to all hosts: build, sync, build on Pi, restart
#   ./deploy.sh build        # build + sync + remote build (no service restart)
#   ./deploy.sh restart      # restart the service (no build)
#   ./deploy.sh install-service  # install systemd service file
#
# Target selection:
#   ./deploy.sh full upspi   # deploy to upspi only
#   ./deploy.sh full upspi2  # deploy to upspi2 only
#   ./deploy.sh full all     # deploy to all (default)
#
# Requires: ssh access to target hosts, gcc + libs on the Pi
#           bun + brotli + gzip + xxd locally for frontend embedding

set -euo pipefail

declare -A HOSTS=(
    [upspi]="sysadmin@upspi.internal.airies.net"
    [upspi2]="sysadmin@upspi2.internal.airies.net"
)

PI_APP_DIR="/home/sysadmin/airies-ups"
PI_CUTILS_DIR="/home/sysadmin/c-utils"
LOCAL_CUTILS_DIR="../c-utils"
SERVICE="airies-ups.service"

RSYNC_EXCLUDE=(
    --exclude='.git'
    --exclude='build'
    --exclude='node_modules'
    --exclude='.venv'
    --exclude='*.db'
    --exclude='*.db-shm'
    --exclude='*.db-wal'
    --exclude='config.yaml'
    --exclude='he_inhibit'
    --exclude='frontend/dist'
    --exclude='migrations'
)

info()  { echo -e "\033[1;34m==>\033[0m $*"; }
err()   { echo -e "\033[1;31m==>\033[0m $*" >&2; exit 1; }

# Resolve target list from second argument
resolve_targets() {
    local target="${1:-all}"
    if [[ "$target" == "all" ]]; then
        echo "${!HOSTS[@]}"
    elif [[ -v "HOSTS[$target]" ]]; then
        echo "$target"
    else
        err "unknown target: $target (available: ${!HOSTS[*]})"
    fi
}

build_local() {
    info "Building locally (frontend + tests + embed)..."
    make frontend || err "Frontend build failed"
    make frontend-test || err "Frontend tests failed"
    make embed-frontend || err "Frontend embedding failed"
    make embed-migrations || err "Migration embedding failed"
    make test || err "C tests failed"
    info "Local build passed"
}

sync_source() {
    local host="$1"
    info "[$host] Syncing c-utils..."
    rsync -az --delete "${RSYNC_EXCLUDE[@]}" \
        "$LOCAL_CUTILS_DIR/" "${HOSTS[$host]}:$PI_CUTILS_DIR/"

    info "[$host] Syncing airies-ups..."
    rsync -az --delete "${RSYNC_EXCLUDE[@]}" \
        ./ "${HOSTS[$host]}:$PI_APP_DIR/"

    # Sync generated C files (Pi doesn't have bun/brotli for frontend)
    info "[$host] Syncing generated build artifacts..."
    rsync -az build/embedded_assets.c build/migrations_compiled.c \
        "${HOSTS[$host]}:$PI_APP_DIR/build/"
}

build_remote() {
    local host="$1"
    info "[$host] Building c-utils..."
    ssh "${HOSTS[$host]}" "cd $PI_CUTILS_DIR && make clean && make" || err "[$host] c-utils build failed"

    info "[$host] Building airies-ups (release + embedded frontend)..."
    ssh "${HOSTS[$host]}" "cd $PI_APP_DIR && make clean && make BUILD_TYPE=release EMBED=1 _build" || err "[$host] airies-ups build failed"

    info "[$host] Build complete"
    ssh "${HOSTS[$host]}" "ls -lh $PI_APP_DIR/build/airies-upsd $PI_APP_DIR/build/airies-ups"
}

restart_service() {
    local host="$1"
    info "[$host] Restarting $SERVICE..."
    ssh "${HOSTS[$host]}" "sudo systemctl restart $SERVICE"
    sleep 1
    ssh "${HOSTS[$host]}" "systemctl is-active $SERVICE" && info "[$host] Service is running" \
        || err "[$host] Service failed to start — check: ssh ${HOSTS[$host]} journalctl -u $SERVICE -n 30"
}

install_service() {
    local host="$1"
    info "[$host] Installing service file..."
    ssh "${HOSTS[$host]}" "sudo cp $PI_APP_DIR/airies-ups.service /etc/systemd/system/$SERVICE && sudo systemctl daemon-reload"
}

ACTION="${1:-full}"
TARGET="${2:-all}"
TARGETS=$(resolve_targets "$TARGET")

case "$ACTION" in
    full)
        build_local
        for t in $TARGETS; do
            sync_source "$t"
            build_remote "$t"
            restart_service "$t"
        done
        ;;
    build)
        build_local
        for t in $TARGETS; do
            sync_source "$t"
            build_remote "$t"
        done
        ;;
    restart)
        for t in $TARGETS; do
            restart_service "$t"
        done
        ;;
    install-service)
        for t in $TARGETS; do
            install_service "$t"
        done
        ;;
    *)
        echo "Usage: $0 [full|build|restart|install-service] [upspi|upspi2|all]"
        exit 1
        ;;
esac
