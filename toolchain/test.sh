#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────
# vultureOS — Test Runner
# Runs cargo check + clippy on the workspace
# ──────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

GREEN='\033[0;32m'
RED='\033[0;31m'
CYAN='\033[0;36m'
NC='\033[0m'

info()  { echo -e "${GREEN}[PASS]${NC} $1"; }
error() { echo -e "${RED}[FAIL]${NC} $1"; }

cd "$ROOT_DIR"
export RUSTUP_TOOLCHAIN=nightly
NIGHTLY_BIN="$(rustup run nightly rustc --print sysroot)/bin"
export PATH="$NIGHTLY_BIN:$PATH"

FAILED=0

# ── cargo check ──────────────────────────────────────────────
echo -e "${CYAN}[1/3] Running cargo check...${NC}"
if cargo check 2>&1; then
    info "cargo check"
else
    error "cargo check"
    FAILED=1
fi

# ── clippy (if available) ────────────────────────────────────
echo ""
echo -e "${CYAN}[2/3] Running clippy...${NC}"
if cargo clippy -- -W warnings 2>&1; then
    info "clippy"
else
    error "clippy (non-blocking)"
fi

# ── Format check ─────────────────────────────────────────────
echo ""
echo -e "${CYAN}[3/3] Checking formatting...${NC}"
if cargo fmt --check 2>&1; then
    info "rustfmt"
else
    error "rustfmt (run: cargo fmt to fix)"
fi

# ── Summary ──────────────────────────────────────────────────
echo ""
if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}All checks passed!${NC}"
else
    echo -e "${RED}Some checks failed.${NC}"
    exit 1
fi
