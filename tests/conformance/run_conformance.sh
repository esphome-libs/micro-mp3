#!/usr/bin/env bash
# Fetch the minimp3 ISO conformance vectors (never committed here), build the
# comparison harness, and decode every standard Layer III vector, comparing PCM
# against the reference. Prototype: see conformance.cpp for the metric details.
#
# Vectors are pulled from lieff/minimp3 via a sparse checkout of just the
# vectors/ directory into ./vectors (gitignored), pinned to a fixed commit
# (MINIMP3_REF) so results are reproducible and the CI cache key is stable.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

MINIMP3_REPO="${MINIMP3_REPO:-https://github.com/lieff/minimp3.git}"
# Pinned minimp3 commit. Bump deliberately (and the CI cache key follows it).
MINIMP3_REF="${MINIMP3_REF:-7b590fdcfa5a79c033e76eacc05d0c3e4c79f536}"
VECTORS_DIR="$HERE/vectors"

# --- fetch vectors (sparse, blob-filtered, pinned commit) ----------------------
# Re-fetch unless BOTH .bit and .pcm files are present, not just when the dir is
# empty: a half-populated vectors/ (e.g. .pcm pruned but .bit kept, or vice
# versa) would otherwise skip the fetch and let the run below silently skip the
# vectors whose reference went missing. Requiring both repairs a partial cache.
# No --depth so the pinned commit is checkoutable; --filter=blob:none keeps the
# clone lean.
if [ -z "$(ls "$VECTORS_DIR"/*.bit 2>/dev/null)" ] || [ -z "$(ls "$VECTORS_DIR"/*.pcm 2>/dev/null)" ]; then
    echo ">> fetching conformance vectors from $MINIMP3_REPO @ ${MINIMP3_REF}"
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT  # clean up even if a clone/checkout below fails early
    git clone --filter=blob:none --sparse "$MINIMP3_REPO" "$tmp/minimp3"
    git -C "$tmp/minimp3" sparse-checkout set vectors
    git -C "$tmp/minimp3" checkout --quiet "$MINIMP3_REF"
    mkdir -p "$VECTORS_DIR"
    cp "$tmp/minimp3"/vectors/*.bit "$tmp/minimp3"/vectors/*.pcm "$VECTORS_DIR"/
    echo ">> fetched $(ls "$VECTORS_DIR"/*.bit | wc -l | tr -d ' ') bitstreams"
fi

# --- build harness -------------------------------------------------------------
cmake -B build >/dev/null
cmake --build build -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" >/dev/null

# --- run -----------------------------------------------------------------------
# Accuracy gate, modeled on ISO/IEC 11172-4 "full accuracy", which has two parts:
#   - an RMS bound: difference RMS < 2^-15/sqrt(12) (~0.29 LSB at 16-bit, ~101 dB
#     PSNR). We check it as PSNR with a 96 dB threshold (minimp3's gate): looser
#     than full accuracy but far tighter than "limited accuracy" (2^-11/sqrt(12),
#     ~4.6 LSB, ~77 dB).
#   - a peak bound: max |diff| <= 2^-14 of full scale = 2 LSB at 16-bit. Enforced
#     via --max-diff so a vector must satisfy BOTH bounds to pass.
# OpenCore is fixed-point yet meets both: standard Layer III vectors land at
# ~101-102 dB with max_diff 1-2.
MIN_PSNR="${MIN_PSNR:-96}"
MAX_DIFF="${MAX_DIFF:-2}"

# Vectors excluded from the accuracy gate, with the reason:
#   *sideinfo*          -- intentionally malformed (robustness, not accuracy)
#   l3-he_free          -- free-format, rejected by design (NODATA)
#   *vbrtag-oob-read    -- adversarial OOB regression vector (NODATA)
#
# l3-he_mode (mid-stream mono<->stereo) is NOT excluded: the harness honors the
# decoder's MP3_STREAM_INFO_CHANGED signal and keeps decoding, emitting each
# section in its native channel layout, so it matches the ISO reference exactly
# (101.8 dB at off=0).
#
# l3-sin1k0db (plain) is NOT excluded: micro_mp3 decodes it bit-exactly, but its
# ISO reference is pre-trimmed by 2 frames and the plain vector carries no
# gapless metadata, so the decoder emits 2 untrimmed lead-in frames and the
# harness aligns via its +/-2-frame search (101.7 dB at off=+2 frames). The
# LAME-tagged twin of the same audio aligns at offset 0 once gapless trimming
# removes those frames.
EXCLUDE_RE='sideinfo|he_free|vbrtag-oob-read'

fail=0
gated=0
echo
printf '%-44s %s\n' "VECTOR" "RESULT"
for b in "$VECTORS_DIR"/l3-*.bit "$VECTORS_DIR"/M2L3_*.bit "$VECTORS_DIR"/ILL2_layer3.bit; do
    [ -e "$b" ] || continue
    p="${b%.bit}.pcm"
    [ -s "$p" ] || continue
    name="$(basename "$b")"
    line="$(./build/conformance "$b" "$p" --min-psnr "$MIN_PSNR" --max-diff "$MAX_DIFF")" || true
    echo "$line"
    if echo "$name" | grep -qE "$EXCLUDE_RE"; then
        continue  # known-divergent / out-of-scope; not gated
    fi
    gated=$((gated + 1))
    case "$line" in
        *" | FAIL") fail=1 ;;
    esac
done

echo
# Guard against a vacuous pass: an empty or .bit-less vectors/ would otherwise
# fall through the loop and report success without testing anything.
if [ "$gated" -eq 0 ]; then
    echo "RESULT: no gated vectors were tested (vectors/ missing or has no .bit files)"
    exit 1
fi
if [ "$fail" -ne 0 ]; then
    echo "RESULT: conformance gate FAILED (>=1 gated vector below ${MIN_PSNR} dB or over ${MAX_DIFF} LSB peak)"
    exit 1
fi
echo "RESULT: all ${gated} gated Layer III vectors >= ${MIN_PSNR} dB PSNR and <= ${MAX_DIFF} LSB peak"
