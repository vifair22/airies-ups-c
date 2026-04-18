#!/usr/bin/env bash
#
# deploy.sh — cross-compile and deploy airies-ups to the Pis
#
# Everything is built locally (frontend, tests, cross-compile for aarch64).
# Only the binaries, migrations dir, and service file are synced to the Pi.
# The Pi needs no build tools — just the runtime libraries.
#
# Usage:
#   ./deploy.sh              # full deploy to all hosts: cross-compile, sync, restart
#   ./deploy.sh build        # cross-compile + sync (no service restart)
#   ./deploy.sh restart      # restart the service (no build)
#   ./deploy.sh install-service  # install systemd service file
#
# Target selection:
#   ./deploy.sh full upspi   # deploy to upspi only
#   ./deploy.sh full upspi2  # deploy to upspi2 only
#   ./deploy.sh full all     # deploy to all (default)
#
# Requires: aarch64-unknown-linux-gnu-gcc (crossdev), bun, brotli, gzip, xxd

set -euo pipefail

declare -A HOSTS=(
    [upspi]="sysadmin@upspi.internal.airies.net"
    [upspi2]="sysadmin@upspi2.internal.airies.net"
)

PI_APP_DIR="/home/sysadmin/airies-ups"
SERVICE="airies-ups.service"

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

build_cross() {
    info "Cross-compiling for aarch64 (frontend + tests + embed + cross-compile)..."
    make cross || err "Cross-compile failed"
    info "Cross-compile complete"
    file build/airies-upsd | grep -q "ARM aarch64" || err "Binary is not aarch64"
}

deploy_binaries() {
    local host="$1"
    info "[$host] Deploying binaries..."
    ssh "${HOSTS[$host]}" "mkdir -p $PI_APP_DIR/build"
    rsync -az build/airies-upsd build/airies-ups \
        "${HOSTS[$host]}:$PI_APP_DIR/build/"
    info "[$host] Deploy complete"
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
    rsync -az airies-ups.service "${HOSTS[$host]}:$PI_APP_DIR/"
    ssh "${HOSTS[$host]}" "sudo cp $PI_APP_DIR/airies-ups.service /etc/systemd/system/$SERVICE && sudo systemctl daemon-reload"
}

ACTION="${1:-full}"
TARGET="${2:-all}"
TARGETS=$(resolve_targets "$TARGET")

case "$ACTION" in
    full)
        build_cross
        for t in $TARGETS; do
            deploy_binaries "$t"
            restart_service "$t"
        done
        ;;
    build)
        build_cross
        for t in $TARGETS; do
            deploy_binaries "$t"
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
