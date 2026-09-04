#!/usr/bin/env bash
# One-time WSL host setup for audiOS builds (matches .github/workflows/build.yml).
set -euo pipefail

echo "Installing audiOS build dependencies in WSL..."
sudo apt-get update
sudo apt-get install -y gcc make xorriso qemu-system-x86 mtools python3 curl git

echo ""
echo "WSL toolchain ready:"
command -v make gcc xorriso python3 git curl qemu-system-x86_64
