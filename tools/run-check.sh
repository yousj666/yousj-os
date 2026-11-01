#!/bin/sh
# Yousj OS - one-command check: run `make check` and `make test` from
# the repository root and print a combined summary.
#
# Usage:  tools/run-check.sh        (from anywhere; it finds the repo root)
# Exit:   0 if both pass, 1 if either fails.
#
# Build artifacts are redirected to /tmp by default (override with
# YOUSJ_BUILD_DIR) because several developers work in this repo in
# parallel and share the default build/ directory; the Makefile's
# BUILD variable is command-line overridable, so the check/test
# targets themselves run unchanged.
#
# Note: the Makefile's test rule executes ./$(BUILD)/test_pmm, i.e. it
# expects BUILD to be a *relative* path. An absolute /tmp path breaks
# that rule (./ + /tmp/... resolves relative to the repo). So BUILD is
# passed as the path of $BUILD_DIR relative to the repo root, which
# keeps the artifacts physically in /tmp and the rule working.

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(dirname -- "$SCRIPT_DIR")
BUILD_DIR=${YOUSJ_BUILD_DIR:-/tmp/yousj-os-check-build}

cd "$REPO_ROOT" || {
    echo "run-check: cannot cd to repo root $REPO_ROOT" >&2
    exit 1
}

if command -v realpath >/dev/null 2>&1; then
    BUILD_DIR=$(realpath -m "$BUILD_DIR")
    BUILD_ARG=$(realpath -m --relative-to="$REPO_ROOT" "$BUILD_DIR")
else
    # Fallback: no realpath -> use the Makefile default (repo build/).
    BUILD_ARG=build
    BUILD_DIR="$REPO_ROOT/build"
fi

echo "== Yousj OS run-check =="
echo "repo:  $REPO_ROOT"
echo "build: $BUILD_DIR"
echo

check_result="not run"
test_result="not run"
overall=0

echo "-- make check --"
if make check BUILD="$BUILD_ARG"; then
    check_result="PASS"
else
    check_result="FAIL"
    overall=1
fi
echo

echo "-- make test --"
if make test BUILD="$BUILD_ARG"; then
    test_result="PASS"
else
    test_result="FAIL"
    overall=1
fi
echo

echo "== summary =="
echo "make check: $check_result"
echo "make test:  $test_result"
if [ "$overall" -eq 0 ]; then
    echo "run-check: ALL PASS"
else
    echo "run-check: FAILURES PRESENT" >&2
fi
exit "$overall"
