#!/usr/bin/env bash
#
# run-dispatch.sh — measure how cold-to-warm each SHA-256 codepath is
# on this host by peeling features off OPENSSL_ia32cap_P from the top
# of the dispatch priority list down to the scalar fallback.
#
# Usage:
#   ./run-dispatch.sh                        # defaults: 4.0.0, 3.5.6
#   ./run-dispatch.sh 3.6.2                  # one version
#   ITERS=2000000 ./run-dispatch.sh 4.0.0    # custom iterations

set -euo pipefail
cd "$(dirname "$0")"

VERSIONS="${*:-4.0.0 3.5.6}"
ITERS="${ITERS:-5000000}"

# OPENSSL_ia32cap masks (~<hex>: bits cleared from CPUID leaf)
# Hex in leaf-1 (CPUID[1].EDX:ECX) is:
#   bit  (32+28) = AVX
#   bit  (32+ 9) = SSSE3
# Hex in leaf-7 (CPUID[7].EBX:ECX) is:
#   bit  29 = SHA-NI
#   bit   8 = BMI2
#   bit   5 = AVX2
#   bit   3 = BMI1
declare -A MASKS=(
    [0_full]=""
    [1_no_shani]=":~0x20000000"
    [2_no_avx2]=":~0x20000128"
    [3_no_avx]="~0x1000000000000000:~0x20000128"
    [4_no_ssse3]="~0x1000020000000000:~0x20000128"
)

# Printable description of which path should win after each mask
declare -A LABELS=(
    [0_full]="(1) SHA-NI"
    [1_no_shani]="(2) AVX2 + BMI1 + BMI2"
    [2_no_avx2]="(3) AVX + SSSE3"
    [3_no_avx]="(4) SSSE3"
    [4_no_ssse3]="(5) scalar x86_64"
)

RESULTS_DIR=results-dispatch
mkdir -p "$RESULTS_DIR"

for v in $VERSIONS; do
    if ! docker image inspect "bench-sha256:$v" >/dev/null 2>&1; then
        echo "Missing image bench-sha256:$v — run ./run-bench.sh first."
        exit 1
    fi
    echo ""
    echo "============================================================"
    echo "  OpenSSL $v — dispatch matrix ($ITERS iters, (2) legacy column)"
    echo "============================================================"
    for key in $(printf '%s\n' "${!MASKS[@]}" | sort); do
        mask="${MASKS[$key]}"
        label="${LABELS[$key]}"
        echo ""
        echo "--- $v, mask='$mask' → expect $label ---"
        if [ -z "$mask" ]; then
            docker run --rm "bench-sha256:$v" "$ITERS" \
                > "$RESULTS_DIR/$v-$key.txt"
        else
            docker run --rm -e "OPENSSL_ia32cap=$mask" \
                "bench-sha256:$v" "$ITERS" \
                > "$RESULTS_DIR/$v-$key.txt"
        fi
        grep -E "^  \(2\)|^  \(3b\)" "$RESULTS_DIR/$v-$key.txt"
    done
done

# summary
{
    echo ""
    echo "============================================================"
    echo "  Dispatch matrix summary (ns/call, 'best' column)"
    echo "============================================================"
    printf "%-8s  %-22s  %10s  %10s\n" \
        "OpenSSL" "Path"                        "(2) legacy" "(3b) EVP reused"
    printf "%-8s  %-22s  %10s  %10s\n" \
        "--------" "----------------------"    "----------" "---------------"
    for v in $VERSIONS; do
        for key in $(printf '%s\n' "${!MASKS[@]}" | sort); do
            f="$RESULTS_DIR/$v-$key.txt"
            [ -f "$f" ] || continue
            label="${LABELS[$key]}"
            p2=$(awk '/^  \(2\)/   {for(i=1;i<=NF;i++) if($i ~ /^best=/) {split($i,a,"="); print a[2]}}' "$f")
            p3b=$(awk '/^  \(3b\)/ {for(i=1;i<=NF;i++) if($i ~ /^best=/) {split($i,a,"="); print a[2]}}' "$f")
            printf "%-8s  %-22s  %10s  %10s\n" "$v" "$label" "${p2:-?}" "${p3b:-?}"
        done
    done
} | tee "$RESULTS_DIR/summary.txt"
