#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────
# vultureOS — Debug with GDB in QEMU
# Launches QEMU in debug mode and connects GDB
# ──────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

GREEN='\033[0;32m'
CYAN='\033[0;36m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

info()  { echo -e "${GREEN}[INFO]${NC} $1"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; }

# ── Build debug ──────────────────────────────────────────────
info "Building vultureOS (debug)..."
"$SCRIPT_DIR/build.sh" debug

KERNEL="$ROOT_DIR/target/x86_64-unknown-none/debug/vulture"

if [ ! -f "$KERNEL" ]; then
    error "Kernel binary not found: $KERNEL"
    exit 1
fi

# ── Check for debugger ───────────────────────────────────────
GDB_CMD=""
if command -v rust-gdb &>/dev/null; then
    GDB_CMD="rust-gdb"
elif command -v gdb &>/dev/null; then
    GDB_CMD="gdb"
elif command -v lldb &>/dev/null; then
    GDB_CMD="lldb"
else
    warn "No debugger found (gdb/lldb). Starting QEMU in debug mode only."
    warn "Connect manually: target remote :1234"
fi

# ── Launch QEMU in debug mode ────────────────────────────────
echo ""
echo -e "${CYAN}══════════════════════════════════════════════${NC}"
echo -e "${CYAN}  vultureOS — Debug Session${NC}"
echo -e "${CYAN}══════════════════════════════════════════════${NC}"
echo ""
info "QEMU will pause at startup, waiting for debugger on :1234"
info "Kernel: $KERNEL"
echo ""

# Start QEMU in the background, paused
qemu-system-x86_64 \
    -kernel "$KERNEL" \
    -m 256M \
    -smp 2 \
    -serial stdio \
    -s -S \
    -no-shutdown &

QEMU_PID=$!
info "QEMU started (PID: $QEMU_PID), waiting for debugger..."

# Connect with GDB/LLDB
if [ -n "$GDB_CMD" ]; then
    sleep 1
    if [ "$GDB_CMD" = "lldb" ]; then
        info "Connecting with LLDB..."
        "$GDB_CMD" -o "gdb-remote localhost:1234" "$KERNEL"
    else
        info "Connecting with GDB..."
        "$GDB_CMD" \
            -ex "target remote :1234" \
            -ex "set disassembly-flavor intel" \
            -ex "break _start" \
            -ex "continue" \
            "$KERNEL"
    fi
fi

# Cleanup
wait $QEMU_PID 2>/dev/null || true
