#!/usr/bin/env bash
# Fetch the minimp3 ISO conformance vectors (never committed here), build the
# comparison harness, and decode every standard Layer III vector, comparing PCM
# against the reference. Prototype: see conformance.cpp for the metric details.
#
# Vectors are pulled from lieff/minimp3 via a shallow, sparse checkout of just
# the vectors/ directory into ./vectors (gitignored).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

MINIMP3_REPO="${MINIMP3_REPO:-https://github.com/lieff/minimp3.git}"
VECTORS_DIR="$HERE/vectors"

# --- fetch vectors (shallow + sparse: only vectors/) ---------------------------
# Re-fetch when no .bit files are present, not just when the dir is empty: a
# half-populated vectors/ (e.g. .pcm kept but .bit pruned) would otherwise skip
# the fetch and let the run below pass while testing nothing.
if [ -z "$(ls "$VECTORS_DIR"/*.bit 2>/dev/null)" ]; then
    echo ">> fetching conformance vectors from $MINIMP3_REPO"
    tmp="$(mktemp -d)"
    git clone --depth 1 --filter=blob:none --sparse "$MINIMP3_REPO" "$tmp/minimp3"
    git -C "$tmp/minimp3" sparse-checkout set vectors
    mkdir -p "$VECTORS_DIR"
    cp "$tmp/minimp3"/vectors/*.bit "$tmp/minimp3"/vectors/*.pcm "$VECTORS_DIR"/
    rm -rf "$tmp"
    echo ">> fetched $(ls "$VECTORS_DIR"/*.bit | wc -l | tr -d ' ') bitstreams"
fi

# --- build harness -------------------------------------------------------------
cmake -B build >/dev/null
cmake --build build -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" >/dev/null

# --- run -----------------------------------------------------------------------
# Accuracy gate. OpenCore is fixed-point; standard Layer III vectors land at
# ~100-102 dB (max_diff 1-2). 96 dB matches minimp3's gate and is far above the
# ISO 11172-4 "limited accuracy" bound.
MIN_PSNR="${MIN_PSNR:-96}"

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
    line="$(./build/conformance "$b" "$p" --min-psnr "$MIN_PSNR")" || true
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
    echo "RESULT: conformance gate FAILED (>=1 gated Layer III vector below ${MIN_PSNR} dB)"
    exit 1
fi
echo "RESULT: all ${gated} gated Layer III vectors >= ${MIN_PSNR} dB PSNR"
