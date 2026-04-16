#!/usr/bin/env bash
#
# deploy.sh — build and deploy airies-ups to the Pi
#
# Usage:
#   ./deploy.sh              # full deploy: sync, build, restart
#   ./deploy.sh build        # sync + build only (no service restart)
#   ./deploy.sh restart      # restart the service (no build)
#   ./deploy.sh frontend     # rebuild frontend locally, sync, restart
#
# Requires: ssh access to PI_HOST, gcc + libs on the Pi, bun locally for frontend

set -euo pipefail

PI_HOST="sysadmin@upspi.internal.airies.net"
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

sync_source() {
    info "Syncing c-utils to Pi..."
    rsync -az --delete "${RSYNC_EXCLUDE[@]}" \
        "$LOCAL_CUTILS_DIR/" "$PI_HOST:$PI_CUTILS_DIR/"

    info "Syncing airies-ups to Pi..."
    rsync -az --delete "${RSYNC_EXCLUDE[@]}" \
        ./ "$PI_HOST:$PI_APP_DIR/"
}

build_remote() {
    info "Building c-utils on Pi..."
    ssh "$PI_HOST" "cd $PI_CUTILS_DIR && make clean && make" || err "c-utils build failed"

    info "Building airies-ups on Pi..."
    ssh "$PI_HOST" "cd $PI_APP_DIR && make clean && make" || err "airies-ups build failed"

    info "Build complete"
    ssh "$PI_HOST" "ls -lh $PI_APP_DIR/build/airies-upsd $PI_APP_DIR/build/airies-ups"
}

restart_service() {
    info "Restarting $SERVICE..."
    ssh "$PI_HOST" "sudo systemctl restart $SERVICE"
    sleep 1
    ssh "$PI_HOST" "systemctl is-active $SERVICE" && info "Service is running" \
        || err "Service failed to start — check: ssh $PI_HOST journalctl -u $SERVICE -n 30"
}

install_service() {
    info "Installing service file..."
    ssh "$PI_HOST" "sudo cp $PI_APP_DIR/airies-ups.service /etc/systemd/system/$SERVICE && sudo systemctl daemon-reload"
}

build_frontend() {
    info "Building frontend locally..."
    (cd frontend && bun install && bun run build) || err "Frontend build failed"
}

case "${1:-full}" in
    full)
        build_frontend
        sync_source
        build_remote
        restart_service
        ;;
    build)
        build_frontend
        sync_source
        build_remote
        ;;
    restart)
        restart_service
        ;;
    frontend)
        build_frontend
        sync_source
        restart_service
        ;;
    install-service)
        install_service
        ;;
    *)
        echo "Usage: $0 [full|build|restart|frontend|install-service]"
        exit 1
        ;;
esac
