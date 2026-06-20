# Changes from Upstream OpenCore MP3 Decoder

Source: OpenCore project, `refs/heads/main`, `codecs_v2/audio/mp3/dec`
(<https://android.googlesource.com/platform/external/opencore/+/refs/heads/main/codecs_v2/audio/mp3/dec/>).
The original source is not vendored in this repo; to review this fork's changes,
diff against the upstream tree linked above.

## Modified Files

### `pvmp3_framedecoder.cpp`

A side-info bounds guard in `pvmp3_framedecoder()`, plus two fixes in
`fillMainDataBuf()`:

1. Before `pvmp3_get_side_info()` runs, the frame is rejected (with
   `NO_ENOUGH_MAIN_DATA_ERROR`) unless the input buffer can hold the 4-byte
   header, the optional 2-byte CRC, the full side information, and the 3-byte
   slack of `getNbits()`'s unconditional 4-byte prefetch window. The downstream
   completeness check (`predicted_frame_size` vs `inputBufferCurrentLength`)
   runs only *after* the side info is parsed, so it cannot guard that read. The
   32-byte header floor in `pvmp3_decode_header.cpp` used to mask this by
   rejecting every frame too small to overrun; lowering it to 5 (to admit valid
   sub-32-byte frames) let a degenerate sub-side-info frame through — e.g. an
   8 kbps MPEG2 stereo frame at 24 kHz, 24 bytes, whose `4 + 2 + 17 = 23`
   header/CRC/side-info bytes leave no room for the prefetch — and the
   zero-copy direct path (buffer bounded to exactly the frame) read past the
   caller's allocation. Valid small frames are unaffected: the smallest real
   one is 24-byte 8 kbps MPEG2 *mono* (9-byte side info), which clears the
   guard. Found by libFuzzer + ASan.
2. In `fillMainDataBuf()`, the input-side read is clamped to
   `inputBufferCurrentLength - offset`. Upstream bounds-checks only against
   `BUFSIZE` (8192; 2048 in this fork, see below), which assumes the caller
   supplies a `BUFSIZE`-byte circular buffer. The wrapper instead passes a
   caller-owned slice or the 1536-byte internal buffer, so `BUFSIZE` overcommits
   and malformed side-info can drive `offset + temp` past the allocation.
   Out-of-range data now produces a downstream parse error, reported as
   `MP3_DECODE_ERROR`.
3. The wrap path in `fillMainDataBuf()` (taken when the main-data ring wraps at
   `BUFSIZE`) is two bulk `pv_memcpy` calls, one per side of the wrap. Upstream's
   unrolled two-at-a-time loop pre-read a byte before the loop and re-read one at
   the end of every iteration, reading one byte past `temp`; the split copy
   reads exactly `temp` bytes.

### `pvmp3_dec_defs.h`

`BUFSIZE` is reduced from 8192 to 2048, and its comment is rewritten. `BUFSIZE`
is the size of the bit-reservoir ring buffer (`mainDataBuffer[BUFSIZE]`), which
holds one frame's main data plus up to 511 bytes back-referenced via
`main_data_begin`. The live window is therefore at most `1441 + 511 = 1952`
bytes, so the ring only needs the next power of two above that. Upstream's 8192
was 4x larger than required (its comment also misdescribed it as the size of the
biggest MP3 frame, which is wrong: the largest compressed frame is 1441 bytes
and 4608 is the decoded PCM output size). 2048 is the smallest power of two that
covers 1952 with ~96 bytes of slack, saving 6 KB per decoder instance. It also
clears the second `BUFSIZE` floor: pvmp3 reuses `BUFSIZE` as the wrap modulus
for input-buffer reads, so it must stay strictly greater than
`MP3_INPUT_BUFFER_SIZE` (1536) for that mask to remain a no-op (the case the
bound check in `fillMainDataBuf()` handles). 2048 satisfies both constraints and
is the floor. The smaller ring only makes `mainDataStream.offset` wrap more
often, splitting `fillMainDataBuf()`'s single `pv_memcpy` into two at the wrap;
decoded output is unchanged.

### `pvmp3_decode_header.cpp`

Two changes:

1. Rejects `bitrate_index == 15` (reserved/invalid per the MP3 spec) in the
   guard that already rejects free-format (`bitrate_index == 0`). The
   `mp3_bitrate` table is `int16[3][15]`, so index 15 is an out-of-bounds read;
   upstream accepted the reserved index and the read surfaced in
   `pvmp3_get_main_data_size()`.
2. The "header complete" floor is lowered from `SYNC_WORD_LNGTH + 21` (32 bytes)
   to 5. The 32-byte floor rejected every valid frame shorter than 32 bytes; the
   smallest Layer III frame is 24 bytes (8 kbps MPEG2 at 24 kHz; 22.05 kHz gives
   26), so low-bitrate mono streams such as speech and TTS had all frames skipped
   and decoded to silence. Five is the minimum the header parse can read (its
   `getNbits(21)` prefetch touches byte 4) and stays below the smallest valid
   frame. The wrapper only calls the decoder with a complete frame buffered, and
   `pvmp3_framedecoder()` then guards the reads this floor no longer covers: a
   side-info bounds check (see that file's entry) before `pvmp3_get_side_info()`,
   and the `predicted_frame_size` vs `inputBufferCurrentLength` check before any
   main-data read.

### `pvmp3_huffman_parsing.cpp`

Two fixes:

1. The scale-factor band-index lookups in the long-block branch are clamped to
   the 23-entry `mp3_sfBandIndex[].l[]` table. `region0_count` (4 bits) and
   `region1_count` (3 bits) can combine to index up to 24, past the table end.
2. An out-of-bounds write in the count1 second-chance decode is fixed.
   `pvmp3_huffman_quad_decoding()` writes four lines (`is[i..i+3]`), but the
   guard only required `i < 576`, allowing `i == 574` to write `is[576..577]`
   one `int32` past `work_buf_int32[576]` into the adjacent `circ_buffer`. The
   guard is now `i <= 576 - 4`, so a quad decodes only when it fits (largest
   `is[572..575]`). This makes the old post-decode `(i-2) >= 576` cleanup dead,
   so it was removed.

### `pvmp3_dequantize_sample.cpp`

Two fixes in the short-block dequantization path:

1. `temp2` (the sub-window index, 0..2) is clamped to `[0, 2]` before indexing
   `gr_info->subblock_gain[3]` and `scalefac->s[][]`. It is derived from a
   fixed-point product of side-info values and can exceed 2 on malformed input,
   causing out-of-bounds reads.
2. The mixed-block long/short boundary uses `mp3_sfBandIndex[sfreq].l[mixstart]`
   instead of the hardcoded `2*FILTERBANK_BANDS` (36). The hardcoded value is
   correct only when `l[mixstart] == 36`; for MPEG-2.5 at 8 kHz it is 72, so
   lines 36..71 belong to the long region but were dequantized with short-block
   scaling. The expression is 36 for every other rate. No out-of-bounds; the
   symptom was audibly wrong output for that configuration.

### `pvmp3_reorder.cpp`

Two fixes:

1. The short-block reorder start uses `mp3_sfBandIndex[sfreq].s[3] * 3` instead
   of the hardcoded 36 (the short-region start). For MPEG-2.5 at 8 kHz the short
   region starts at 72, so the reorder de-interleaved from the middle of the
   long region. The expression is 36 for every other configuration.
2. A guard returns early when a mixed block decoded no short-region coefficients
   (`used_freq_lines <= src_line`). Without it the loop reads the stale tail of
   `work_buf_int32[]` and propagates previous-frame values into the IMDCT.
   Reachable only on streams that flag a mixed block with an empty short region.

### `pvmp3_mpeg2_stereo_proc.cpp`

The MPEG-2/2.5 intensity-bound selection uses `sb < mp3_sfBandIndex[sfreq].l[6]`
instead of the hardcoded `sb < 36`. Both branches already use `l[6]` for the
long region, so only the selection was wrong; for MPEG-2.5 at 8 kHz an intensity
boundary in lines 36..71 took the wrong branch. The expression is 36 for every
other LSF configuration. The MPEG-1 path lives in `pvmp3_stereo_proc.cpp` and is
unaffected (its hardcoded 36 is correct for MPEG-1's two-subband mixed blocks).

### `pvmp3_equalizer.cpp`

The non-flat (preset) branch of `pvmp3_equalizer()` advances `band += 2` instead
of `band += 3`. Each outer iteration processes two consecutive bands, so the
stride must be 2, as in the flat branch. With stride 3, bands 2, 5, 8, 11, 14,
and 17 were never written and kept stale prior-frame data, producing glitchy
output on every non-flat preset reachable through `set_equalizer()`. In bounds;
the symptom was audibly wrong output.

### `pvmp3_seek_synch.cpp`

Two fixes in `pvmp3_header_sync()`, plus removal of a dead function (see Removed
Functions):

1. A 1-byte overread is fixed. The sync-scan loop ended on
   `usedBits < availableBits`, but its body calls `getUpTo9bits()`, which reads
   two bytes (`pBuffer[offset]` and `pBuffer[offset+1]`). The guard is now
   `usedBits + 16 <= availableBits`. The wrapper pre-validates sync at offset 0,
   so the scan is not currently reached; the fix hardens pvmp3 itself.
2. A byte-alignment typo is corrected. Upstream rounded with
   `usedBits = (usedBits + 7) & 8`, which keeps only bit 3 (yielding 0 or 8)
   rather than rounding up to a multiple of 8. Corrected to `& ~7u`. This is a
   logic error, not a memory-safety issue.

`pvmp3_header_sync()` stays; it is reached through `pvmp3_decode_header.cpp`. The
removed `pvmp3_frame_synch()` also carried a `bitrate_index == 15` guard, now
moot because that indexing site is gone; the equivalent check at the live parse
site remains in `pvmp3_decode_header.cpp`.

### `pvmp3_normalize.cpp`

`pvmp3_normalize()` returns `__builtin_clz(x) - 1`, replacing the manual
if/else-and-switch leading-zero count. The intrinsic is equivalent and compiles
to a native `CLZ` instruction on ARM and Xtensa.

### `pvmp3_mpeg2_get_scale_data.cpp`

`new_slen[4]` is zero-initialized at declaration. Both scalefac-compress decode
branches use an `if`/`else-if` chain with no final `else`, so GCC's
`-Wmaybe-uninitialized` flags the later `if (new_slen[i])` read and fails the
xtensa-esp32s3 build under `-Werror`. The unset paths are unreachable here
(`scalefac_compress` is a 9-bit field on the LSF path, so one arm always runs),
and a zero entry routes to the same else branch that writes zeros.

### `s_tmp3dec_file.h`, `pvmp3_reorder.cpp`, `pvmp3_reorder.h`

`Scratch_mem` is enlarged from `int32[168]` to `int32[198]` to fix an
out-of-bounds write in `pvmp3_reorder()`. The reorder loop copies a short
block's three windows into `Scratch_mem[0 .. 3*sfb_lines-1]`. Size 168 covers
only the 44.1 kHz table (widest short band `192-136 = 56`, times 3 = 168). For
MPEG-1 48 kHz the widest short band is `192-126 = 66`, so the loop writes
`Scratch_mem[0..197]`, 30 `int32` past the buffer into the next channel's
`overlap[]` history, and the trailing `pv_memcpy` over-reads by the same amount.
Triggered by any MPEG-1 48 kHz short-block granule with `used_freq_lines > 378`,
which is common. 198 is the largest size any of the nine sample-rate tables
needs.

### `pvmp3_dct_16.cpp`, `pvmp3_dct_9.cpp`, `pvmp3_mdct_18.cpp`, `pvmp3_polyphase_filter_window.cpp` / `.h`, `pvmp3_normalize.h`

Changed only by stripping the `#if defined(PV_ARM_*)` inline-assembly guards,
leaving the C path each file already compiled. See "Platform-specific back-ends
and build files" under Removed Files for the rationale.

### `pvmp3_audio_type_defs.h`, `mp3_mem_funcs.h`

Drop the dependency on PacketVideo's external OSCL library, which is not part of
the decoder directory. `pvmp3_audio_type_defs.h` defines the fixed-width type
aliases (`int8`..`uint64`, via `<stdint.h>`) and `OSCL_UNUSED_ARG` directly,
replacing `#include "oscl_base.h"`. `mp3_mem_funcs.h` expands its `pv_mem*`
macros to the C library `memset` / `memcpy` / `memmove` / `memcmp` (via
`<string.h>`), replacing the `oscl_mem*` aliases from `#include "oscl_mem.h"`.

## Removed Files

### `pvmp3_decoder.cpp`, `pvmp3_decoder.h`

Depend on OSCL headers that are not present, and duplicate functionality already
provided by `pvmp3_framedecoder.cpp`. Their only use was the now-removed
`pvmp3_frame_synch()` call site.

### `mp3_decoder_selection.h`

Defined only `NEW_PV_MP3_DECODER`, which is never referenced; the header was
never included.

### Platform-specific back-ends and build files

The decoder shipped three hand-tuned fixed-point back-ends
(`pv_mp3dec_fxd_op_arm.h`, `pv_mp3dec_fxd_op_arm_gcc.h`,
`pv_mp3dec_fxd_op_msc_evc.h`), ARMv4/v5 and Windows Mobile assembly under
`asm/`, and PacketVideo's build fragments (`make/` and `Android.mk`). All of it
is gated on `PV_ARM_*` macros this fork never defines, so only the C path ever
compiled. The variant headers, `asm/`, `make/`, and `Android.mk` are deleted.
The C-equivalent routines (from `pv_mp3dec_fxd_op_c_equivalent.h`) are folded
into the dispatcher `pv_mp3dec_fxd_op.h`, which no longer selects on platform;
the `C_EQUIVALENT` define is gone. The `#if defined(PV_ARM_*)` assembly guards
are stripped from `pvmp3_dct_16.cpp`, `pvmp3_dct_9.cpp`, `pvmp3_mdct_18.cpp`,
`pvmp3_polyphase_filter_window.cpp` / `.h`, and `pvmp3_normalize.cpp` / `.h`.
Decoded output is byte-for-byte identical. The removed assembly is in the
upstream tree linked at the top of this file.

## Removed Functions

Functions with no callers in any build configuration.

### `pvmp3_frame_synch()` (`pvmp3_seek_synch.cpp` / `.h`)

The sync-and-validate entry point. Its only caller was the removed
`pvmp3_decoder.cpp`; the live decode path uses `pvmp3_framedecoder()` and reaches
`pvmp3_header_sync()` through `pvmp3_decode_header.cpp`. Removing it also dropped
the now-unused `s_tmp3dec_file.h`, `pv_mp3dec_fxd_op.h`, and `pvmp3_tables.h`
includes from `pvmp3_seek_synch.cpp`.

### `fxp_mul32_Q26()`

A Q26 fixed-point multiply helper with no callers in any DSP routine. It was
dropped from `pv_mp3dec_fxd_op.h` when the C-equivalent code was folded in, and
went away with the three deleted platform-variant headers.
