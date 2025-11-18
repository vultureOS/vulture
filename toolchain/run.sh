#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────
# vultureOS — Run in QEMU
# Builds a bootable image and launches it in QEMU x86_64
# ──────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

info()  { echo -e "${GREEN}[INFO]${NC} $1"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; }

# ── Configuration ────────────────────────────────────────────
MODE="${1:-debug}"
MEMORY="${QEMU_MEMORY:-256M}"
CPUS="${QEMU_CPUS:-2}"
SERIAL="${QEMU_SERIAL:-stdio}"
EXTRA_ARGS="${QEMU_EXTRA:-}"

# ── Check QEMU ───────────────────────────────────────────────
if ! command -v qemu-system-x86_64 &>/dev/null; then
    error "qemu-system-x86_64 not found!"
    echo ""
    echo "Install QEMU:"
    echo "  macOS:  brew install qemu"
    echo "  Ubuntu: sudo apt install qemu-system-x86"
    echo "  Arch:   sudo pacman -S qemu-full"
    exit 1
fi
info "QEMU: $(qemu-system-x86_64 --version | head -1)"

# ── Build bootable image ────────────────────────────────────
info "Building vultureOS ($MODE)..."
"$SCRIPT_DIR/build.sh" "$MODE"

# ── Locate the boot image ───────────────────────────────────
BOOT_IMAGE="$ROOT_DIR/target/x86_64-unknown-none/$MODE/bootimage-vulture.bin"

if [ ! -f "$BOOT_IMAGE" ]; then
    error "Boot image not found at: $BOOT_IMAGE"
    error "Make sure 'cargo bootimage' completed successfully."
    exit 1
fi

# ── Run in QEMU ──────────────────────────────────────────────
echo ""
echo -e "${CYAN}══════════════════════════════════════════════${NC}"
echo -e "${CYAN}  Launching vultureOS in QEMU${NC}"
echo -e "${CYAN}══════════════════════════════════════════════${NC}"
echo ""
info "Memory:  $MEMORY"
info "CPUs:    $CPUS"
info "Serial:  $SERIAL"
info "Image:   $BOOT_IMAGE"
echo ""

qemu-system-x86_64 \
    -drive format=raw,file="$BOOT_IMAGE" \
    -m "$MEMORY" \
    -smp "$CPUS" \
    -serial "$SERIAL" \
    -no-shutdown \
    $EXTRA_ARGS
