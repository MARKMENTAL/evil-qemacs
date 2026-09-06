#!/usr/bin/env bash
# -------------------------------------------------------------------------
# Cross-compile Evil QEmacs (teqe / eqe) for arm-v7l (arm-linux-gnueabihf)
#
#   ./armv7l-build.sh teqe   statically linked tiny build  (default: eqe)
#   ./armv7l-build.sh eqe    statically linked full build
#
# The binaries are statically linked so they run on devices with older
# glibc versions (e.g. the Miyoo Mini Plus ships glibc 2.28).
# -------------------------------------------------------------------------
set -euo pipefail

# ----- configuration -------------------------------------------------------
# Adjust these if your toolchain lives elsewhere
CROSS_PREFIX="arm-linux-gnueabihf"
CC="${CROSS_PREFIX}-gcc"
AR="${CROSS_PREFIX}-ar"
EXE=""                     # empty → no suffix; set to e.g. ".bin" if needed

# 32-bit armv7l target notes:
# The "${CROSS_PREFIX}-gcc" toolchain defaults to ARMv7-A hard-float
# (armv7-a+fp, EABI5), so no extra -march/-mfpu CFLAGS are needed and
# none are passed: overriding CFLAGS on the make command line would
# clobber the Makefiles' own CFLAGS+= include paths (-I., -I..,
# -I./libqhtml, -I$(OBJS_DIR)) and break the build.

# Target selection: teqe (tiny) or eqe (full)
TARGET="${1:-eqe}"

# ----- helper functions ----------------------------------------------------
die() { echo "ERROR: $*" >&2; exit 1; }

check_tool() {
    if ! command -v "$1" >/dev/null 2>&1; then
        die "required tool '$1' not found in PATH"
    fi
}

# ----- validation ----------------------------------------------------------
check_tool "$CC"
check_tool "$AR"
[ "$TARGET" = "eqe" ] || [ "$TARGET" = "teqe" ] || die "unknown target '$TARGET', use 'eqe' or 'teqe'"

# ----- prepare Makefile config --------------------------------------------
# Build the make command line (bash array so values containing spaces
# such as LDFLAGS/CFLAGS stay a single make argument).
# -B forces rebuilding all targets regardless of timestamps.
# CC/AR are passed on the command line to override config.mak's natives;
# they propagate to the libqhtml sub-make via MAKEFLAGS.
# TARGET_ARCH=arm keeps the .objs dir named for the target, not the host.
# For teqe also set TARGET_TINY=1.
# Static linking keeps the binary self-contained for devices that may
# not have a recent glibc (e.g. the Miyoo Mini Plus).
# HOST_CFLAGS=-I. is needed because the host tools (fbftoqe & co) are
# built with HOST_CC while CC is the cross compiler; without it the
# tools cannot find cutils.h.
# The goals are the binary itself plus its cross-built static dep,
# not "all": the full target also builds host-side extras (html2png,
# manuals) that have no business in a cross-compiled device binary.
# NOTE: goal `eqe` alone is not enough: with TARGET=qe set, `libqhtml.a`
# is a prerequisite of eqe_g but the `libqhtml` sub-make is only wired
# into `all`, so it must be requested explicitly (teqe needs no libqhtml).
MAKE_ARGS=(-B "LDFLAGS=-static -Wl,--no-as-needed -lc" "HOST_CFLAGS=-I." "TARGET_ARCH=arm" "CC=${CC}" "AR=${AR}")
case "$TARGET" in
    eqe)
        MAKE_ARGS+=("TARGET=qe" "libqhtml" "eqe")
        ;;
    teqe)
        MAKE_ARGS+=("TARGET=tqe" "TARGET_TINY=1" "teqe")
        ;;
    *)
        die "unsupported target '$TARGET'"
        ;;
esac

# ----- build ---------------------------------------------------------------
if ! make "${MAKE_ARGS[@]}" 2>&1; then
    die "make failed"
fi

# ----- post-build ---------------------------------------------------------
# The Makefile produces $(BIN)_g (unstripped) and $(BIN) (stripped copy).
# Strip may fail on foreign-format (ARM) binaries from an x86_64 host,
# so errors are ignored - the binary itself is already valid.
case "$TARGET" in
    teqe) BIN="teqe";  BIN_G="teqe_g"  ;;
    eqe)  BIN="eqe";   BIN_G="eqe_g"   ;;
esac

if [ -n "$EXE" ]; then
    BIN="${BIN}${EXE}"
    BIN_G="${BIN_G}${EXE}"
fi

if [ -f "$BIN_G" ]; then
    cp "$BIN_G" "$BIN" 2>/dev/null || true
    strip -s -R .comment -R .note "$BIN" 2>/dev/null || true
    echo "Built $TARGET successfully -> $BIN"
    file "$BIN" | sed 's/^/  /'
elif [ -f "$BIN" ]; then
    strip -s -R .comment -R .note "$BIN" 2>/dev/null || true
    echo "Built $TARGET successfully -> $BIN"
    file "$BIN" | sed 's/^/  /'
else
    die "expected binary '$BIN' not found after make"
fi
