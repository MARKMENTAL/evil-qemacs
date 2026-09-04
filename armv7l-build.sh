#!/usr/bin/env bash
# -------------------------------------------------------------------------
# Cross‑compile QEmacs (tqe / qe) for arm‑v7l (arm-linux‑gnueabihf)
# -------------------------------------------------------------------------
set -euo pipefail

# ----- configuration -------------------------------------------------------
# Adjust these if your toolchain lives elsewhere
CROSS_PREFIX="arm-linux-gnueabihf"
CC="${CROSS_PREFIX}-gcc"
EXE=""                     # empty → no suffix; set to e.g. ".bin" if needed

# Target selection
TARGET="${1:-qe}"        # "qe" or "tqe" (passed as first arg)
BUILD_TINY=0

# ----- helper functions ----------------------------------------------------
die() { echo "ERROR: $*" >&2; exit 1; }

check_tool() {
    if ! command -v "$1" >/dev/null 2>&1; then
        die "required tool '$1' not found in PATH"
    fi
}

# ----- validation ----------------------------------------------------------
check_tool "$CC"
[ "$TARGET" = "qe" ] || [ "$TARGET" = "tqe" ] || die "unknown target '$TARGET', use 'qe' or 'tqe'"

# ----- prepare Makefile config --------------------------------------------
# Build the make command line.
# -B forces rebuilding all targets regardless of timestamps.
# Pass CC on the command line to override config.mak's CC=clang.
# For tqe also set TARGET_TINY=1.
# Make sure the build is static for devices that may not have newest glibc ala miyoo mini plus
MAKE_FLAGS="-B LDFLAGS=-static -Wl,--no-as-needed -lc"
case "$TARGET" in
    qe)
        MAKE_FLAGS="$MAKE_FLAGS TARGET=qe CC=${CC}"
        ;;
    tqe)
        MAKE_FLAGS="$MAKE_FLAGS TARGET=tqe TARGET_TINY=1 CC=${CC}"
        ;;
    *)
        die "unsupported target '$TARGET'"
        ;;
esac

# ----- build ---------------------------------------------------------------
if ! make $MAKE_FLAGS 2>&1; then
    die "make failed"
fi

# ----- post‑build ---------------------------------------------------------
# The Makefile creates the final binary and may leave an intermediate *_g file.
# Check for the target-specific binary first, then fall back to the *_g version.
if [ "$TARGET" = "tqe" ]; then
    BIN="tqe"
else
    BIN="qe"
fi

# Also check for the intermediate *_g version if the target-specific one is missing.
if [ ! -f "$BIN" ]; then
    BIN="${TARGET}_g"
fi

if [ -n "$EXE" ]; then
    BIN="${BIN}${EXE}"
fi

if [ -f "$BIN" ]; then
    # strip may fail on already‑stripped or foreign-format binaries – ignore errors
    strip -s -R .comment -R .note "$BIN" 2>/dev/null || true
    echo "Built ${BIN} successfully."
else
    die "expected binary '$BIN' not found after make"
fi
