#!/usr/bin/env bash
#
# run-bench.sh — iterate over pre-built bench-sha256:<version> images
# and capture output. Each run internally does 1 warmup pass + N measured
# passes (see bench-sha256.c).
#
# Env:
#   ITERS       iterations per pass (default 5000000)
#   VERSIONS    space-separated override

set -euo pipefail
cd "$(dirname "$0")"

if [ -z "${VERSIONS:-}" ]; then
    VERSIONS="3.0.20 3.1.8 3.2.6 3.3.7 3.4.5 3.5.6 3.6.2 4.0.0"
fi
ITERS="${ITERS:-5000000}"

RESULTS_DIR=results
mkdir -p "$RESULTS_DIR"

# build missing images via bake
missing=()
for v in $VERSIONS; do
    if ! docker image inspect "bench-sha256:$v" >/dev/null 2>&1; then
        missing+=("$v")
    fi
done
if [ ${#missing[@]} -gt 0 ]; then
    echo "Building ${#missing[@]} missing images via docker buildx bake..."
    docker buildx bake
fi

for v in $VERSIONS; do
    out="$RESULTS_DIR/$v.txt"
    echo "=== OpenSSL $v ==="
    docker run --rm "bench-sha256:$v" "$ITERS" | tee "$out"
    echo ""
done

# aggregate summary — each bench already reports min/med/max per API
{
    echo "== Host: $(uname -s) $(uname -m), $(nproc 2>/dev/null || sysctl -n hw.ncpu) cores =="
    echo "== $(date) =="
    echo ""
    for v in $VERSIONS; do
        out="$RESULTS_DIR/$v.txt"
        [ -f "$out" ] || continue
        echo "--- OpenSSL $v ---"
        grep -E "^\s*\([1-4]" "$out" || cat "$out"
        echo ""
    done
} | tee "$RESULTS_DIR/summary.txt"
