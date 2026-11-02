#!/bin/sh
# Yousj OS - QEMU smoke test: build the kernel, boot it in QEMU, capture
# the serial log, and assert the milestone lines kmain must print.
#
# Usage:  tools/qemu-smoke.sh [extra-required-substring ...]
#         Extra arguments are additional substrings that must also
#         appear in the serial log (for future milestones, e.g. M2).
# Exit:   0 if the build succeeds and every assertion holds, else 1.
#         On failure the tail of the serial log is printed.
#
# Environment overrides:
#   YOUSJ_SMOKE_BUILD_DIR  build dir   (default /tmp/yousj-os-smoke-build)
#   YOUSJ_SMOKE_LOG        serial log  (default /tmp/yousj-os-smoke-serial.log)
#   YOUSJ_SMOKE_TIMEOUT    QEMU secs   (default 12; kmain halts forever,
#                                       so timeout ending the run is normal)
#
# Artifacts go to /tmp, not the repo build/, same as run-check.sh: the
# Makefile's BUILD variable is command-line overridable, but its rules
# expect a relative path, so BUILD is passed relative to the repo root.

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(dirname -- "$SCRIPT_DIR")
BUILD_DIR=${YOUSJ_SMOKE_BUILD_DIR:-/tmp/yousj-os-smoke-build}
LOG_FILE=${YOUSJ_SMOKE_LOG:-/tmp/yousj-os-smoke-serial.log}
TIMEOUT_SECS=${YOUSJ_SMOKE_TIMEOUT:-12}

cd "$REPO_ROOT" || {
    echo "qemu-smoke: cannot cd to repo root $REPO_ROOT" >&2
    exit 1
}

if command -v realpath >/dev/null 2>&1; then
    BUILD_DIR=$(realpath -m "$BUILD_DIR")
    BUILD_ARG=$(realpath -m --relative-to="$REPO_ROOT" "$BUILD_DIR")
else
    BUILD_ARG=build
    BUILD_DIR="$REPO_ROOT/build"
fi
LOG_DIR=$(dirname -- "$LOG_FILE")
mkdir -p "$BUILD_DIR" "$LOG_DIR" || exit 1

fail_with_log_tail() {
    echo "qemu-smoke: FAIL - $1" >&2
    if [ -s "$LOG_FILE" ]; then
        echo "----- serial log tail (last 50 lines): $LOG_FILE -----" >&2
        tail -n 50 "$LOG_FILE" >&2
        echo "----- end of serial log tail -----" >&2
    else
        echo "qemu-smoke: serial log is empty or missing: $LOG_FILE" >&2
    fi
    exit 1
}

echo "== Yousj OS qemu-smoke =="
echo "repo:    $REPO_ROOT"
echo "build:   $BUILD_DIR"
echo "serial:  $LOG_FILE"
echo "timeout: ${TIMEOUT_SECS}s"
echo

echo "-- make kernel --"
if ! make kernel BUILD="$BUILD_ARG"; then
    echo "qemu-smoke: FAIL - make kernel failed" >&2
    exit 1
fi
echo

command -v qemu-system-x86_64 >/dev/null 2>&1 || {
    echo "qemu-smoke: FAIL - qemu-system-x86_64 not found in PATH" >&2
    exit 1
}

echo "-- qemu (serial captured, timeout ${TIMEOUT_SECS}s) --"
: > "$LOG_FILE"
timeout "$TIMEOUT_SECS" qemu-system-x86_64 -m 128 \
    -kernel "$BUILD_DIR/yousj-stub.elf" \
    -initrd "$BUILD_DIR/yousj-kernel.elf" \
    -display none -serial stdio -monitor none -no-reboot \
    > "$LOG_FILE" 2> "$LOG_FILE.qemu-err"
qemu_rc=$?
# kmain halts at the end of its self-tests, so timeout (124) is the
# expected way out. 0 can happen on an early guest shutdown/triple
# fault with -no-reboot; the assertions below decide either way.
case "$qemu_rc" in
    0|124) ;;
    *) echo "qemu-smoke: note: qemu exited with code $qemu_rc" ;;
esac
echo "qemu exit: $qemu_rc"
echo

echo "-- assertions --"
missing=0
# Required lines, taken from kernel/arch/x86_64/kmain.c serial output.
for pat in \
    "Yousj OS: kmain reached" \
    "Yousj OS: PMM self-test PASS" \
    "Yousj OS: VMM self-test PASS" \
    "Yousj OS: VMM hw-init PASS" \
    "Yousj OS: thread A iter 0" \
    "Yousj OS: thread B iter 9" \
    "Yousj OS: sched M1 self-test PASS" \
    "Yousj OS: sched M2 self-test PASS" \
    "Yousj OS: ring3 alive, getpid=" \
    "Yousj OS: sched M3 self-test PASS" \
    "Yousj OS: boot self-tests done, init started" \
    "yousj-sh: shell ready" \
    "sh> " \
    "$@"
do
    if grep -Fq -- "$pat" "$LOG_FILE"; then
        echo "  PASS: $pat"
    else
        echo "  MISSING: $pat"
        missing=1
    fi
done

if grep -Fq "FAILED" "$LOG_FILE"; then
    echo "  FAILED-LINE: serial log contains a FAILED self-test line"
    missing=1
fi

echo
if [ "$missing" -ne 0 ]; then
    fail_with_log_tail "one or more assertions failed"
fi

echo "qemu-smoke: ALL PASS (serial log: $LOG_FILE)"
exit 0
