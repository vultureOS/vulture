#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────
# vultureOS — Setup Development Environment
# Installs all required tools for building and running vultureOS
# ──────────────────────────────────────────────────────────────
set -euo pipefail

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
CYAN='\033[0;36m'
NC='\033[0m'

info()  { echo -e "${GREEN}[✓]${NC} $1"; }
warn()  { echo -e "${YELLOW}[!]${NC} $1"; }
error() { echo -e "${RED}[✗]${NC} $1"; }
step()  { echo -e "\n${CYAN}→ $1${NC}"; }

echo -e "${CYAN}"
echo "  ╔═══════════════════════════════════════════╗"
echo "  ║     vultureOS Development Environment       ║"
echo "  ║            Setup Script                    ║"
echo "  ╚═══════════════════════════════════════════╝"
echo -e "${NC}"

# ── 1. Rustup ────────────────────────────────────────────────
step "Checking rustup..."
if command -v rustup &>/dev/null; then
    info "rustup installed: $(rustup --version 2>&1 | head -1)"
else
    error "rustup not found!"
    echo "  Install: curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh"
    exit 1
fi

# ── 2. Nightly Toolchain ────────────────────────────────────
step "Installing nightly Rust toolchain..."
rustup toolchain install nightly 2>&1 | tail -1
info "Nightly: $(rustup run nightly rustc --version 2>&1)"

# ── 3. Required Components ──────────────────────────────────
step "Installing required components..."
rustup component add rust-src --toolchain nightly 2>&1 | tail -1
rustup component add rustfmt --toolchain nightly 2>&1 | tail -1
rustup component add clippy --toolchain nightly 2>&1 | tail -1
info "Components: rust-src, rustfmt, clippy"

# ── 4. Target ───────────────────────────────────────────────
step "Adding x86_64-unknown-none target..."
rustup target add x86_64-unknown-none --toolchain nightly 2>&1 | tail -1
info "Target: x86_64-unknown-none"

# ── 5. QEMU ─────────────────────────────────────────────────
step "Checking QEMU..."
if command -v qemu-system-x86_64 &>/dev/null; then
    info "QEMU: $(qemu-system-x86_64 --version | head -1)"
else
    warn "QEMU not found. Install it to run the OS:"
    if [[ "$OSTYPE" == "darwin"* ]]; then
        echo "    brew install qemu"
    elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
        echo "    sudo apt install qemu-system-x86  (Debian/Ubuntu)"
        echo "    sudo pacman -S qemu-full           (Arch)"
        echo "    sudo dnf install qemu-system-x86   (Fedora)"
    fi
fi

# ── 6. GRUB (optional) ──────────────────────────────────────
step "Checking GRUB (optional, for ISO creation)..."
if command -v grub-mkrescue &>/dev/null || command -v grub2-mkrescue &>/dev/null; then
    info "grub-mkrescue: found"
else
    warn "grub-mkrescue not found (optional — needed for ISO boot only)"
    if [[ "$OSTYPE" == "linux-gnu"* ]]; then
        echo "    sudo apt install grub-pc-bin xorriso  (Debian/Ubuntu)"
    fi
fi

# ── 7. Summary ──────────────────────────────────────────────
echo ""
echo -e "${CYAN}══════════════════════════════════════════════${NC}"
echo -e "${GREEN}  Setup complete! Available commands:${NC}"
echo -e "${CYAN}══════════════════════════════════════════════${NC}"
echo ""
echo "  ./toolchain/build.sh          Build (debug)"
echo "  ./toolchain/build.sh release  Build (release)"
echo "  ./toolchain/build.sh check    Type-check only"
echo "  ./toolchain/run.sh            Build & run in QEMU"
echo "  ./toolchain/debug.sh          Build & debug with GDB"
echo "  ./toolchain/clean.sh          Clean build artifacts"
echo "  ./toolchain/test.sh           Run tests"
echo ""
