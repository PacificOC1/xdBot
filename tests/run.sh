#!/usr/bin/env bash
# Builds and runs the host tests (the parts that don't need the game). From the repo root: tests/run.sh
set -euo pipefail
cd "$(dirname "$0")/.."
out="${TMPDIR:-/tmp}/xdbot-stepper-test"
c++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    tests/stepper_test.cpp src/fwc/Macro.cpp -o "$out"
"$out"
