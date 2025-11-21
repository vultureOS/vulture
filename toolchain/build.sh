#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────
# vultureOS — Build Script
# Compiles the entire OS kernel for x86_64
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

header() { echo -e "\n${CYAN}══════════════════════════════════════════════${NC}"; echo -e "${CYAN}  $1${NC}"; echo -e "${CYAN}══════════════════════════════════════════════${NC}\n"; }
info()   { echo -e "${GREEN}[INFO]${NC} $1"; }
warn()   { echo -e "${YELLOW}[WARN]${NC} $1"; }
error()  { echo -e "${RED}[ERROR]${NC} $1"; }

# ── Ensure nightly toolchain ────────────────────────────────
setup_toolchain() {
    export RUSTUP_TOOLCHAIN=nightly
    NIGHTLY_BIN="$(rustup run nightly rustc --print sysroot)/bin"
    export PATH="$NIGHTLY_BIN:$PATH"
}

# ── Check prerequisites ─────────────────────────────────────
check_prereqs() {
    header "Checking Prerequisites"

    if ! command -v rustup &>/dev/null; then
        error "rustup not found. Install from https://rustup.rs"
        exit 1
    fi
    info "rustup: $(rustup --version 2>&1 | head -1)"

    if ! rustup toolchain list | grep -q nightly; then
        warn "Nightly toolchain not found. Installing..."
        rustup toolchain install nightly
    fi
    info "Nightly: $(rustup run nightly rustc --version 2>&1)"

    if ! rustup component list --toolchain nightly | grep -q "rust-src (installed)"; then
        warn "rust-src not found. Installing..."
        rustup component add rust-src --toolchain nightly
    fi
    info "rust-src: installed"

    if ! rustup target list --toolchain nightly | grep -q "x86_64-unknown-none (installed)"; then
        warn "x86_64-unknown-none target not found. Installing..."
        rustup target add x86_64-unknown-none --toolchain nightly
    fi
    info "Target: x86_64-unknown-none"

    # Check bootimage
    if ! command -v bootimage &>/dev/null; then
        warn "bootimage not found. Installing..."
        cargo install bootimage
    fi
    info "bootimage: installed"
}

# ── Build ────────────────────────────────────────────────────
build() {
    local mode="${1:-debug}"
    header "Building vultureOS ($mode)"

    cd "$ROOT_DIR"
    setup_toolchain

    if [ "$mode" = "release" ]; then
        info "Building bootable image (release)..."
        cargo bootimage --release 2>&1
    else
        info "Building bootable image (debug)..."
        cargo bootimage 2>&1
    fi

    BOOT_IMAGE="$ROOT_DIR/target/x86_64-unknown-none/$mode/bootimage-vulture.bin"

    if [ -f "$BOOT_IMAGE" ]; then
        info "Build succeeded!"
        info "Boot image: $BOOT_IMAGE"
        echo ""
        ls -lh "$BOOT_IMAGE"
    else
        # Fallback: check for the ELF binary
        BINARY="$ROOT_DIR/target/x86_64-unknown-none/$mode/vulture"
        if [ -f "$BINARY" ]; then
            info "Build succeeded (ELF only, no boot image)"
            info "Binary: $BINARY"
            ls -lh "$BINARY"
        else
            error "Build failed!"
            exit 1
        fi
    fi
}

# ── Main ─────────────────────────────────────────────────────
MODE="${1:-debug}"

case "$MODE" in
    --release|release)
        check_prereqs
        build release
        ;;
    --check|check)
        check_prereqs
        header "Checking vultureOS (no codegen)"
        cd "$ROOT_DIR"
        setup_toolchain
        cargo check 2>&1
        info "Check passed!"
        ;;
    --help|-h|help)
        echo "Usage: $0 [debug|release|check|help]"
        echo ""
        echo "  debug    Build bootable image (default)"
        echo "  release  Build optimized bootable image"
        echo "  check    Type-check only (no binary output)"
        echo "  help     Show this help"
        ;;
    *)
        check_prereqs
        build debug
        ;;
esac
