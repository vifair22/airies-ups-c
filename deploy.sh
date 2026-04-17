#!/usr/bin/env bash
#
# deploy.sh — build and deploy airies-ups to the Pis
#
# Usage:
#   ./deploy.sh              # full deploy to all hosts: sync, build, restart
#   ./deploy.sh build        # sync + build only (no service restart)
#   ./deploy.sh restart      # restart the service (no build)
#   ./deploy.sh frontend     # rebuild frontend locally, sync, restart
#   ./deploy.sh install-service  # install systemd service file
#
# Target selection:
#   ./deploy.sh full upspi   # deploy to upspi only
#   ./deploy.sh full upspi2  # deploy to upspi2 only
#   ./deploy.sh full all     # deploy to all (default)
#
# Requires: ssh access to target hosts, gcc + libs on the Pi, bun locally for frontend

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

sync_source() {
    local host="$1"
    info "[$host] Syncing c-utils..."
    rsync -az --delete "${RSYNC_EXCLUDE[@]}" \
        "$LOCAL_CUTILS_DIR/" "${HOSTS[$host]}:$PI_CUTILS_DIR/"

    info "[$host] Syncing airies-ups..."
    rsync -az --delete "${RSYNC_EXCLUDE[@]}" \
        ./ "${HOSTS[$host]}:$PI_APP_DIR/"
}

build_remote() {
    local host="$1"
    info "[$host] Building c-utils..."
    ssh "${HOSTS[$host]}" "cd $PI_CUTILS_DIR && make clean && make" || err "[$host] c-utils build failed"

    info "[$host] Building airies-ups..."
    ssh "${HOSTS[$host]}" "cd $PI_APP_DIR && make clean && make" || err "[$host] airies-ups build failed"

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

build_frontend() {
    info "Building frontend locally..."
    (cd frontend && bun install && bun run build) || err "Frontend build failed"
}

ACTION="${1:-full}"
TARGET="${2:-all}"
TARGETS=$(resolve_targets "$TARGET")

case "$ACTION" in
    full)
        build_frontend
        for t in $TARGETS; do
            sync_source "$t"
            build_remote "$t"
            restart_service "$t"
        done
        ;;
    build)
        build_frontend
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
    frontend)
        build_frontend
        for t in $TARGETS; do
            sync_source "$t"
            restart_service "$t"
        done
        ;;
    install-service)
        for t in $TARGETS; do
            install_service "$t"
        done
        ;;
    *)
        echo "Usage: $0 [full|build|restart|frontend|install-service] [upspi|upspi2|all]"
        exit 1
        ;;
esac
