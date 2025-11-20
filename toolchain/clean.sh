#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────
# vultureOS — Clean Build Artifacts
# ──────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m'

info() { echo -e "${GREEN}[INFO]${NC} $1"; }

echo -e "${CYAN}Cleaning vultureOS build artifacts...${NC}"

cd "$ROOT_DIR"

export RUSTUP_TOOLCHAIN=nightly

cargo clean 2>&1

# Clean ISO artifacts
if [ -d "$ROOT_DIR/target/iso" ]; then
    rm -rf "$ROOT_DIR/target/iso"
    info "Removed ISO build directory"
fi

if [ -f "$ROOT_DIR/target/vultureOS.iso" ]; then
    rm -f "$ROOT_DIR/target/vultureOS.iso"
    info "Removed ISO image"
fi

info "Clean complete!"
