#!/usr/bin/env bash
# Builds audios.iso and audios.img inside WSL.
set -euo pipefail

cd "$(dirname "$0")/.."

# Git on Windows often checks out shell scripts with CRLF; WSL cannot execute those.
for script in kernel/get-deps; do
	if [[ -f "$script" ]]; then
		sed -i 's/\r$//' "$script"
		chmod +x "$script"
	fi
done

if ! command -v xorriso >/dev/null 2>&1; then
	echo "xorriso is not installed in WSL." >&2
	echo "Run once in Ubuntu: sudo apt-get update && sudo apt-get install -y xorriso" >&2
	exit 1
fi

make

failed=0
if [[ -f audios.iso ]]; then
	size_mb=$(du -m audios.iso | cut -f1)
	echo "Done: $(pwd)/audios.iso (${size_mb} MB)"
else
	echo "Build finished but audios.iso was not found." >&2
	failed=1
fi

if [[ -f audios.img ]]; then
	size_mb=$(du -m audios.img | cut -f1)
	echo "Done: $(pwd)/audios.img (${size_mb} MB)"
else
	echo "Build finished but audios.img was not found." >&2
	failed=1
fi

exit "$failed"
