#!/usr/bin/env bash
# Generate a seed corpus for fuzz_mp3_decode.
#
# Outputs:
#   seeds_mp3/   MP3 files spanning MPEG1/2/2.5, mono/stereo/joint-stereo,
#                CBR/VBR, Xing/Info/none, with/without ID3v2, a range of
#                bitrates and sample rates, and several content shapes.
#
# Requires: ffmpeg with libmp3lame on PATH.

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

rm -rf seeds_mp3
mkdir -p seeds_mp3

# Strip both ID3 tags by default so the seed starts at the first frame (or the
# Xing/Info header frame); a few seeds below re-enable ID3 explicitly.
NO_ID3=(-id3v2_version 0 -write_id3v1 0)

# Build the lavfi source filter for a content shape. The sources are mono;
# the encode step sets the channel count with -ac.
filter_for() {
    local kind=$1 rate=$2 dur=$3
    case "$kind" in
        tone)    echo "sine=frequency=440:sample_rate=$rate:duration=$dur" ;;
        sweep)   echo "sine=frequency=100:beep_factor=4:sample_rate=$rate:duration=$dur" ;;
        noise)   echo "anoisesrc=r=$rate:d=$dur:amplitude=0.5" ;;
        silence) echo "anullsrc=r=$rate:cl=mono:duration=$dur" ;;
        impulse) echo "aevalsrc=exprs='if(eq(n,100),0.99,0)':s=$rate:d=$dur" ;;
        dc)      echo "aevalsrc=exprs=0.5:s=$rate:d=$dur" ;;
        *) echo "error: unknown content shape $kind" >&2; return 1 ;;
    esac
}

# Encode one MP3 seed.
#   $1 out  $2 rate  $3 channels  $4 dur  $5 content-shape  $6.. extra ffmpeg args
gen_mp3() {
    local out=$1 rate=$2 chans=$3 dur=$4 kind=$5; shift 5
    local filt
    filt="$(filter_for "$kind" "$rate" "$dur")"
    ffmpeg -hide_banner -loglevel error -y -f lavfi -i "$filt" \
        -ac "$chans" -ar "$rate" -t "$dur" -c:a libmp3lame "$@" "seeds_mp3/$out"
}

echo "[seeds] generating MP3 variants..."

# -- Sample-rate / MPEG-version coverage (stereo CBR, Info header, no ID3) -----
# MPEG1 rates.
gen_mp3 sr_44100_mpeg1.mp3 44100 2 1 tone -b:a 128k "${NO_ID3[@]}"
gen_mp3 sr_48000_mpeg1.mp3 48000 2 1 tone -b:a 128k "${NO_ID3[@]}"
gen_mp3 sr_32000_mpeg1.mp3 32000 2 1 tone -b:a 96k  "${NO_ID3[@]}"
# MPEG2 rates.
gen_mp3 sr_24000_mpeg2.mp3 24000 2 1 tone -b:a 64k "${NO_ID3[@]}"
gen_mp3 sr_22050_mpeg2.mp3 22050 2 1 tone -b:a 96k "${NO_ID3[@]}"
gen_mp3 sr_16000_mpeg2.mp3 16000 2 1 tone -b:a 48k "${NO_ID3[@]}"
# MPEG2.5 rates.
gen_mp3 sr_12000_mpeg25.mp3 12000 2 1 tone -b:a 32k "${NO_ID3[@]}"
gen_mp3 sr_11025_mpeg25.mp3 11025 2 1 tone -b:a 32k "${NO_ID3[@]}"
gen_mp3 sr_8000_mpeg25.mp3  8000  1 1 tone -b:a 32k "${NO_ID3[@]}"

# -- Channel modes (44.1 kHz) -------------------------------------------------
gen_mp3 ch_mono_44100.mp3        44100 1 1 tone -b:a 128k "${NO_ID3[@]}"
gen_mp3 ch_stereo_joint.mp3      44100 2 1 tone -b:a 128k -joint_stereo 1 "${NO_ID3[@]}"
gen_mp3 ch_stereo_nonjoint.mp3   44100 2 1 tone -b:a 128k -joint_stereo 0 "${NO_ID3[@]}"

# -- Bitrate spread (44.1 kHz stereo CBR) -------------------------------------
gen_mp3 br_32k.mp3  44100 2 1 tone -b:a 32k  "${NO_ID3[@]}"
gen_mp3 br_128k.mp3 44100 2 1 tone -b:a 128k "${NO_ID3[@]}"
gen_mp3 br_320k.mp3 44100 2 1 tone -b:a 320k "${NO_ID3[@]}"

# -- VBR (Xing header carries a frame count -> arms gapless end trim) ----------
gen_mp3 vbr_q0.mp3 44100 2 2 tone -q:a 0 "${NO_ID3[@]}"
gen_mp3 vbr_q9.mp3 44100 2 2 tone -q:a 9 "${NO_ID3[@]}"

# -- No Xing/Info header frame ------------------------------------------------
gen_mp3 noxing_128k.mp3 44100 2 1 tone -b:a 128k -write_xing 0 "${NO_ID3[@]}"

# -- With ID3v2 (and the ID3v1 trailer) ---------------------------------------
gen_mp3 id3_tagged.mp3 44100 2 1 tone -b:a 128k \
    -id3v2_version 4 -write_id3v1 1 \
    -metadata title="Fuzz Seed" -metadata artist="microMp3" \
    -metadata album="microMp3" -metadata comment="seed corpus entry"

# -- Content shapes (44.1 kHz stereo CBR, no ID3) -----------------------------
for shape in sweep noise silence impulse dc; do
    gen_mp3 "shape_${shape}.mp3" 44100 2 2 "$shape" -b:a 128k "${NO_ID3[@]}"
done

# -- Very short clips (few frames; exercises probe + immediate EOS) ------------
gen_mp3 short_100ms.mp3 44100 2 0.1  tone -b:a 128k "${NO_ID3[@]}"
gen_mp3 short_50ms.mp3  44100 2 0.05 tone -b:a 128k "${NO_ID3[@]}"
gen_mp3 short_mpeg25.mp3 8000 1 0.1  tone -b:a 32k  "${NO_ID3[@]}"

# ---------------------------------------------------------------------------
# Fuzzer config tails.
#
# fuzz_mp3_decode reads its configuration from the BACK of each input
# (FuzzedDataProvider): one cfg byte, then up to 64 chunk-control bytes. A bare
# .mp3 would therefore lose ~65 bytes off its tail to those reads (a tiny seed
# loses most of itself). Appending a config tail keeps the ENTIRE .mp3 intact as
# decoder payload while still giving libFuzzer a mutable region for the options.
# A few variants pre-set cfg so the replay / big-buffer / undersize-stress paths
# are seeded directly rather than found by mutation.
#
# cfg layout (matches the harness): bit0 = replay across reset(), bit1 = 2x
# output buffer, bit2 = periodically probe the undersized-output guard.
# ---------------------------------------------------------------------------

# Emit one raw byte from a decimal value. Octal escape keeps this portable to
# macOS's stock bash 3.2 (whose printf lacks \xHH).
emit_byte() { printf "\\$(printf '%03o' "$1")"; }

# Append a config tail to a file: 64 neutral chunk-control bytes, then the cfg
# byte as the very last byte. 64 control bytes >= the harness MAX_CONTROL_BYTES,
# so every control byte comes from the pad and none is peeled off the real
# stream.
#   $1 file  $2 cfg
append_config_tail() {
    local f=$1 cfg=$2 i
    {
        for ((i=0;i<64;i++)); do emit_byte 32; done  # ~257-byte chunks (1 + 4*64)
        emit_byte "$cfg"
    } >> "$f"
}

# Copy a pristine base seed and give the copy a specific config tail.
#   $1 base.mp3  $2 dstname  $3 cfg
mkvariant() {
    local src=$1 dst=$2 cfg=$3
    [[ -f "$src" ]] || return 0
    cp "$src" "seeds_mp3/$dst"
    append_config_tail "seeds_mp3/$dst" "$cfg"
}

echo "[seeds] appending fuzzer config tails"

# Snapshot the pristine bases before adding variants, so variants are not
# double-tailed by the pass below.
base_list="$(mktemp)"
trap 'rm -f "$base_list"' EXIT
find seeds_mp3 -maxdepth 1 -type f -name '*.mp3' | sort > "$base_list"

# Feature-flag variants off the primary stereo CBR base.
base=seeds_mp3/br_128k.mp3
mkvariant "$base" cfg_replay.mp3        1  # replay across reset()
mkvariant "$base" cfg_big_output.mp3    2  # 2x output buffer
mkvariant "$base" cfg_stress_output.mp3 4  # undersize-guard stress
mkvariant "$base" cfg_all.mp3           7  # all three

# Every pristine base: neutral tail (cfg=0) so the full .mp3 survives as
# payload while libFuzzer still gets a mutable flag region.
while IFS= read -r f; do
    append_config_tail "$f" 0
done < "$base_list"

echo "[seeds] $(ls seeds_mp3 | wc -l | tr -d ' ') MP3 seeds generated"
echo "[seeds] done"
