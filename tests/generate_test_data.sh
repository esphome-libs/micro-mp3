#!/usr/bin/env bash
# Generate the MP3 fixtures used by the unit tests in this directory.
#
# The generated files are checked into data/ so the tests run without any
# encoder installed; rerun this script only when the fixture set needs to
# change. Each stereo fixture carries a distinct tone per channel so a
# channel-swap regression is detectable, and the set spans MPEG1/2/2.5,
# mono/stereo, CBR/VBR, and Xing-present/absent to exercise the wrapper's
# distinct frame paths.
#
# Requires: ffmpeg with libmp3lame (tone generation + MP3 encoding).

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
cd "$here"

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "error: ffmpeg not on PATH" >&2
    exit 1
fi
if ! ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libmp3lame; then
    echo "error: ffmpeg has no libmp3lame encoder" >&2
    exit 1
fi

mkdir -p data

# ID3 tags are stripped from every fixture (-id3v2_version 0 disables the
# ID3v2 tag, -write_id3v1 0 the ID3v1 trailer) so each file starts at the
# first MP3 frame (or the Xing/Info header frame). This keeps the framing tests
# free of container noise; ID3v2 skipping is covered separately by prepending a
# synthetic tag in the test itself.
NO_ID3=(-id3v2_version 0 -write_id3v1 0)

# Encode a mono tone. Args: out rate freq dur [extra ffmpeg args...]
gen_mono() {
    local out=$1 rate=$2 freq=$3 dur=$4
    shift 4
    ffmpeg -hide_banner -loglevel error -y \
        -f lavfi -i "sine=frequency=$freq:sample_rate=$rate:duration=$dur" \
        -c:a libmp3lame "${NO_ID3[@]}" "$@" "data/$out"
}

# Encode a stereo tone with a distinct frequency in each channel. Args:
# out rate freq_l freq_r dur [extra ffmpeg args...]
gen_stereo() {
    local out=$1 rate=$2 fl=$3 fr=$4 dur=$5
    shift 5
    ffmpeg -hide_banner -loglevel error -y \
        -f lavfi -i "sine=frequency=$fl:sample_rate=$rate:duration=$dur" \
        -f lavfi -i "sine=frequency=$fr:sample_rate=$rate:duration=$dur" \
        -filter_complex "[0:a][1:a]join=inputs=2:channel_layout=stereo[a]" \
        -map "[a]" -c:a libmp3lame "${NO_ID3[@]}" "$@" "data/$out"
}

# MPEG1 stereo CBR with the default Info/Xing header frame. Primary workhorse.
gen_stereo sine_stereo_44100.mp3 44100 500 1700 2 -b:a 128k

# MPEG1 mono CBR (mono side-info size differs from stereo).
gen_mono sine_mono_44100.mp3 44100 600 1.5 -b:a 128k

# MPEG2 stereo CBR (version 2, 576 samples/frame; distinct rate for reset-reuse).
gen_stereo sine_stereo_22050.mp3 22050 400 1500 1.5 -b:a 96k

# MPEG2.5 mono CBR (version 2.5, lowest sample rate).
gen_mono sine_mono_8000.mp3 8000 800 1.5 -b:a 32k

# MPEG1 stereo VBR (Xing header with a frame count -> arms gapless end trim).
gen_stereo sine_stereo_44100_vbr.mp3 44100 500 1700 2 -q:a 4

# MPEG1 stereo CBR with the Xing/Info header suppressed (no gapless trimming).
gen_stereo sine_stereo_44100_noxing.mp3 44100 500 1700 2 -b:a 128k -write_xing 0

echo "[test-data] generated:"
ls -l data/*.mp3
