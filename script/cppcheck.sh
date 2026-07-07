#!/bin/bash

# Run cppcheck whole-program analysis on first-party sources
#
# Complements clang-tidy: cppcheck sees every first-party source in one pass,
# so its unusedFunction check can flag functions with no caller anywhere in
# the project -- something a per-translation-unit tool cannot do. The scan
# includes examples/ and tests/ so public API entry points have visible
# callers; anything unusedFunction still flags is dead beyond the API surface.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

if ! command -v cppcheck &> /dev/null; then
    echo "Error: cppcheck not found (brew install cppcheck / apt-get install cppcheck)"
    exit 1
fi

cd "$ROOT_DIR"

# Suppressions:
#   src/opencore-mp3dec -- third-party fork, not linted here (matches the -w
#       build convention and clang-tidy exclusions)
#   useStlAlgorithm -- raw loops are often clearer; stylistic nag
#   functionStatic on the public header -- accessors that return format
#       constants (get_bit_depth, get_bytes_per_sample, ...) are instance
#       methods by API design, matching get_sample_rate()/get_channels()
#   missingInclude* -- system and ESP-IDF headers are not resolvable here;
#       cppcheck analyzes without them
#
# examples/ compiles against ESP-IDF headers cppcheck can't see; that only
# shallows the analysis of those files, it doesn't produce false positives.
cppcheck \
    --enable=warning,style,unusedFunction \
    --std=c++14 \
    --inline-suppr \
    --quiet \
    --error-exitcode=1 \
    --suppress=missingIncludeSystem \
    --suppress=missingInclude \
    --suppress='*:src/opencore-mp3dec/*' \
    --suppress=useStlAlgorithm \
    --suppress='functionStatic:include/micro_mp3/*' \
    -i src/opencore-mp3dec \
    -i tests/build \
    -i tests/fuzz/build \
    -i tests/conformance/build \
    -i host_examples/mp3_to_wav/build \
    -i examples/decode_benchmark/.pio \
    -I include \
    -I src/opencore-mp3dec \
    src \
    host_examples \
    tests \
    examples

echo "cppcheck passed"
