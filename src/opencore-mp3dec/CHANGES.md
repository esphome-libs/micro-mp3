# Changes from Upstream OpenCore MP3 Decoder

Source: OpenCore project, `refs/heads/main`, `codecs_v2/audio/mp3/dec`
(original tarball preserved at repo root as `opencore-refs_heads_main-codecs_v2-audio-mp3-dec.tar.gz`)

## Modified Files

### `pvmp3_framedecoder.cpp`

Two fixes in `fillMainDataBuf()`:

1. Bound the input-side read against the caller's
   `inputBufferCurrentLength`. The original code only bounds-checks
   against `BUFSIZE` (8192), which is pvmp3's internal circular-buffer
   size assumption. The micro-mp3 wrapper hands pvmp3 either a
   caller-owned slice (zero-copy direct path) or the 1536-byte internal
   buffer, so `BUFSIZE` is an overcommit and adversarial side-info can
   drive `offset + temp` past the real allocation. The fix clamps
   `temp` to `inputBufferCurrentLength - offset` so no read ever
   reaches past the caller's slice. Downstream Huffman/dequant parsing
   then produces a garbage-data error that the wrapper reports as
   `MP3_DECODE_ERROR`.

2. Fixed a 1-byte overread in the unrolled fallback loop used when the
   internal main-data buffer wraps (`mainDataStream.offset + temp >=
   BUFSIZE`). The original loop pre-read `tmp1` before the loop and
   then re-read `tmp1` at the end of every iteration, so each pass did
   2 reads from `ptr` but 2 writes to the main data stream. This means
   the final iteration read one byte past `temp`. On well-formed
   streams the input buffer always had trailing slack so it went
   unnoticed; libFuzzer + ASan flagged it on a frame that landed at
   the very end of the caller's allocation. Replaced with a straight
   byte loop that does exactly `temp` reads.

### `pvmp3_decode_header.cpp`

Reject `bitrate_index == 15` ("reserved/invalid" per the MP3 spec) in the
same guard that already rejects free-format (`bitrate_index == 0`). The
`mp3_bitrate` lookup table is declared `int16[3][15]`, so indexing it with
15 is a guaranteed out-of-bounds read. The original parser accepted the
reserved index and the OOB read then manifested in
`pvmp3_get_main_data_size()`. Reachable via the wrapper's buffered path
when the internal parser's sanity check rejected the header but the
old fall-through handed the raw buffer to pvmp3 anyway. Found by UBSan
fuzzing.

### `pvmp3_huffman_parsing.cpp`

Two fixes:

1. Clamp the scale-factor band-index lookups in the long-block branch of
   `pvmp3_huffman_parsing()` to the 23-entry `mp3_sfBandIndex[].l[]` table.
   The side-info fields `region0_count` (4 bits) and `region1_count`
   (3 bits) can combine to produce an index of up to 24 on adversarial
   streams, reading past the end of the table. Found by UBSan fuzzing.

2. Fixed an out-of-bounds write in the count1 "second-chance" decode.
   `pvmp3_huffman_quad_decoding()` writes four lines (`is[i..i+3]`), but the
   second-chance guard only required `i < 576`. That allowed `i == 574`,
   writing `is[576]`/`is[577]` one `int32` past `work_buf_int32[576]` into the
   adjacent `circ_buffer`. `i == 574` is reachable when `big_values` leaves
   `i` at an even, non-multiple-of-4 offset (e.g. `big_values == 285` gives
   `i == 570`, then the first-chance loop advances it to 574) while a malformed
   stream still has count1 bits (`usedBits < grBits`). The write stays within
   the struct on the current layout, but it is UB and fragile to field
   reordering. Tightened the guard to `i <= 576 - 4` so the quad only decodes
   when a full quad fits (the largest decode is `is[572..575]`). This made the
   original post-decode `(i-2) >= 576` overflow-cleanup unreachable, so it was
   removed. Found by code review.

### `pvmp3_dequantize_sample.cpp`

Clamp `temp2` to `[0, 2]` before using it as an index into
`gr_info->subblock_gain[3]` and `scalefac->s[][]` in the short-block
dequantization path. `temp2` is the sub-window index (0..2) derived from a
fixed-point product of side-info values; in spec-compliant streams it is
always in range, but adversarial input drove it past 2 and caused
out-of-bounds reads. Found by UBSan fuzzing.

Also fixed the mixed-block long/short boundary. The scaling split was
hardcoded to `2*FILTERBANK_BANDS` (= 36 lines = 2 subbands), which is only
correct when `l[mixstart]` equals 36. For **MPEG-2.5 @ 8 kHz**
mixed blocks `l[mixstart] == l[6] == 72` (4 long subbands), so lines 36..71
are part of the long region but were being dequantized with short-block
scaling against stale sub-window state. The decoder already special-cases
this config in `pvmp3_imdct_synth` (`mixedBlocksLongBlocks = 4`),
`pvmp3_alias_reduction` (`sblim = 3`), and the LSF scale-factor parser
(6 long sfbs), so dequant was inconsistent with the rest of the pipeline.
Both occurrences of `2*FILTERBANK_BANDS` now use
`mp3_sfBandIndex[sfreq].l[mixstart]`, which still evaluates to 36 for every
other rate (no behavior change) and to 72 for MPEG-2.5 @ 8 kHz. No
out-of-bounds; the symptom was audibly wrong output for that rare config.

### `pvmp3_reorder.cpp`

Fixed the same mixed-block boundary in the short-block reorder. `src_line`
(and the matching write cursor `ct`) was hardcoded to 36, the start of the
short region. For MPEG-2.5 @ 8 kHz the short region starts at `3*s[3] == 72`,
so the reorder was de-interleaving from the middle of the long region.
Replaced with `mp3_sfBandIndex[sfreq].s[3] * 3` (== 36 for all other configs).

Also added a guard for malformed streams: if a mixed block decoded no
short-region coefficients (`used_freq_lines <= src_line`), the reorder now
returns early. Otherwise the loop would read the stale tail of
`work_buf_int32[]` (`pvmp3_huffman_parsing()` does not zero beyond
`used_freq_lines`) and grow `used_freq_lines` to the scalefactor-band
boundary, propagating previous-frame values into the IMDCT. Only reachable
on bitstreams that flag a mixed block with an empty short region; valid
encoders never do this.

### `pvmp3_mpeg2_stereo_proc.cpp`

Fixed the same mixed-block boundary in the MPEG-2/2.5 stereo path. The
branch that selects "intensity bound inside vs outside the long blocks" was
keyed on the hardcoded `sb < 36`; both branches already use `l[6]`
internally for the long region, so only the selection was wrong. For
MPEG-2.5 @ 8 kHz an intensity boundary in lines 36..71 took the wrong branch.
Changed to `sb < mp3_sfBandIndex[sfreq].l[6]` (== 36 for all other LSF
configs). The MPEG-1 stereo path is unaffected because it lives in a
separate function (`pvmp3_stereo_proc.cpp`), which `pvmp3_framedecoder`
only calls for `version_x == MPEG_1`; that path keeps its own hardcoded
`36`, correct for MPEG-1's always-2-subband mixed blocks.

### `pvmp3_equalizer.cpp`

Fixed the band stride in the non-flat (preset) branch of `pvmp3_equalizer()`.
Each outer iteration processes exactly two consecutive bands: the first inner
loop handles `band`, then `pt_work_buff++` and the second inner loop handles
`band+1`. The loop must therefore advance by 2, as the flat branch does. The
non-flat branch instead stepped `band += 3`, so of the 18 bands it covered only
`{0,1} {3,4} {6,7} {9,10} {12,13} {15,16}` and skipped bands 2, 5, 8, 11, 14,
and 17. Those `6 * SUBBANDS_NUMBER = 192` of the 576 synthesis-buffer slots were
never written and kept stale prior-frame data, producing glitchy output on every
non-flat preset. Reachable through the wrapper's public `set_equalizer()` API for
any preset other than `MP3_EQ_FLAT`. The write stays in bounds, so there is no
out-of-bounds access; the symptom was audibly wrong output. Changed `band += 3`
to `band += 2`. Found during code review.

### `pvmp3_seek_synch.cpp`

Two fixes plus the removal of a dead function (see also "Removed Functions"):

1. Fixed a 1-byte heap-buffer-overflow read in `pvmp3_header_sync()`. The
   sync scan loop terminated on `usedBits < availableBits`, but its body
   calls `getUpTo9bits()` which unconditionally reads two bytes from
   `pBuffer` (`pBuffer[offset]` and `pBuffer[offset+1]`). When `usedBits`
   was within the final byte of the buffer, the read reached one byte
   past the allocation. The fix tightens the guard to
   `usedBits + 16 <= availableBits`, ensuring at least two whole bytes
   are readable before entering the loop body. Originally caught by the
   AddressSanitizer fuzz harness against an earlier wrapper revision
   that handed raw input slices to pvmp3 for sync scanning; the current
   wrapper no longer does so, but the underlying pvmp3 defect remains
   worth fixing for defense in depth.

2. Fixed the byte-alignment typo in `pvmp3_header_sync()`. The upstream
   "byte aligment" step read `usedBits = (usedBits + 7) & 8`, which keeps
   only bit 3 and resets `usedBits` to 0 or 8 instead of rounding up to the
   next multiple of 8 (a typo for `& ~7`). The effect is a logic error,
   rewinding the read cursor toward the buffer start, not a memory-safety
   issue: `& 8` can only yield 0 or 8, both in bounds. The scan is dormant
   in the wrapper, which pre-validates sync at offset 0 before pvmp3 sees
   the buffer. Corrected to `& ~7u`. Found during code review.

`pvmp3_header_sync()` is still reached through `pvmp3_decode_header.cpp`, so
it stays. Its former sibling `pvmp3_frame_synch()` was removed (see below);
that removal also deletes the candidate-frame validation block which had its
own `bitrate_index == 15` out-of-bounds guard. That guard is now moot because
the `mp3_bitrate[version][bitrate_index]` indexing site no longer exists; the
equivalent OOB at the live parse site is still closed in
`pvmp3_decode_header.cpp` (above).

### `pvmp3_normalize.cpp`

Replaced the manual multi-step bit-scan cascade in `pvmp3_normalize()` with
`return __builtin_clz(x) - 1;`. The original used a lookup-table-style if/else
tree followed by a switch statement to count leading zeros. The `__builtin_clz`
intrinsic is equivalent and produces a native `CLZ` instruction on ARM and Xtensa.

### `pvmp3_mpeg2_get_scale_data.cpp`

Zero-initialize the `new_slen[4]` local at declaration. Both
scalefac-compress decode branches use an `if / else-if` chain with no
final `else`, so GCC's `-Wmaybe-uninitialized` flags the later
`if (new_slen[i])` read. The unset paths cannot be reached: this
function only runs on the MPEG-2/2.5 LSF path, where `scalefac_compress`
is a 9-bit field (0-511), so the `scalefac_comp < 512` and
`int_scalefac_comp <= 255` arms always run. The compiler cannot prove
the bitstream bound, so it warns anyway. The `{0}` default silences the
warning and is safe: a zero entry routes to the existing else branch,
which writes zeros to the scale-factor buffers. Reported as a
`-Werror=maybe-uninitialized` build failure on xtensa-esp32s3 GCC
(issue #7).

### `s_tmp3dec_file.h` / `pvmp3_reorder.cpp` / `pvmp3_reorder.h`

Enlarged the persistent `Scratch_mem` buffer from `int32[168]` to `int32[198]`
to fix an out-of-bounds write in `pvmp3_reorder()`. The reorder loop copies a
short block's three windows into `Scratch_mem[0 .. 3*sfb_lines-1]`, where
`sfb_lines` is the width of the current scale-factor band. The old size 168
covered only the 44.1 kHz table (widest short band `192-136 = 56`, `3*56 = 168`).
For MPEG1 48 kHz (`mp3_sfBandIndex[1]`, short bands `{...,100,126,192}`) the
widest band is `192-126 = 66`, so the loop writes `Scratch_mem[0..197]`, which
is 30 int32 (120 bytes) past the buffer and into the following `perChan[0]`
(its `used_freq_lines` and IMDCT `overlap[]` history). The trailing
`pv_memcpy(&xr[ct], Scratch_mem, sfb_lines*3*sizeof(int32))` over-reads by the
same amount. Any MPEG1 48 kHz stream with a short-block granule
(`block_type == 2`) and `used_freq_lines > 378` triggers it, which is common in
normal encodes. The write stays inside the single decoder allocation, so it is
not heap-metadata corruption, but it corrupts the adjacent channel's overlap
history and trips AddressSanitizer. 198 is the largest value any of the nine
sample-rate tables needs; every other table needs `<= 168` and all other
`Scratch_mem` consumers use fewer entries, so the added 30 slots are otherwise
unused.

## Removed Files

- **`pvmp3_decoder.cpp` / `pvmp3_decoder.h`** -- depended on OSCL (Operating System
  Compatibility Library) headers that are not included. All required functionality
  is available through `pvmp3_framedecoder.cpp`. Deleted from the fork to keep the
  dead `pvmp3_frame_synch` call site from confusing future audits.

- **`mp3_decoder_selection.h`** -- dead. It defined only `NEW_PV_MP3_DECODER`,
  which is never referenced, and the header itself was never included. Removed.

- **`oscl/oscl_base.h` / `oscl/oscl_mem.h`** (and the now-empty `oscl/` directory)
  -- the OSCL compatibility layer had collapsed to two tiny single-consumer headers.
  Their live contents were inlined into the consumers and the indirection removed:
  - The fixed-width type aliases (`int8`..`uint64`) and `OSCL_UNUSED_ARG` from
    `oscl_base.h` moved into `pvmp3_audio_type_defs.h`, its only includer. The
    unused `OSCL_IMPORT_REF` / `OSCL_EXPORT_REF` macros were dropped.
  - The `pv_mem*` macros in `mp3_mem_funcs.h` (its only includer) now expand
    directly to the C library `memset` / `memcpy` / `memmove` / `memcmp` instead
    of going through the `oscl_mem*` aliases. The unused `oscl_malloc` / `oscl_free`
    were dropped.

- **Non-generic fixed-point platforms (`asm/`, `make/`, and the ARM/MSC-EVC
  variant headers)** -- the decoder shipped three hand-tuned fixed-point
  back-ends (`pv_mp3dec_fxd_op_arm.h`, `pv_mp3dec_fxd_op_arm_gcc.h`,
  `pv_mp3dec_fxd_op_msc_evc.h`), the ARMv4/v5 and Windows Mobile assembly in
  `asm/*.s` / `*.asm`, and PacketVideo's own `make/` build fragments. All of it
  is gated on the `PV_ARM_V5/V4`, `PV_ARM_GCC_V5/V4`, and `PV_ARM_MSC_EVC_V5/V4`
  macros, none of which this fork ever defines, so only the C-equivalent path
  ever compiled and `asm/` was never built. Deleted the variant headers, the
  `asm/` folder, and the `make/` folder. The portable C-equivalent routines
  (formerly `pv_mp3dec_fxd_op_c_equivalent.h`) were folded into the dispatcher
  `pv_mp3dec_fxd_op.h`, which no longer selects on platform; the vestigial
  `C_EQUIVALENT` define went with it. The in-source `#if defined(PV_ARM_*)`
  branches that guarded inline assembly were stripped from `pvmp3_dct_16.cpp`,
  `pvmp3_dct_9.cpp`, `pvmp3_mdct_18.cpp`, `pvmp3_polyphase_filter_window.cpp`
  / `.h`, and `pvmp3_normalize.cpp` / `.h`, leaving only the C path each already
  compiled. Decoded output is byte-for-byte identical and all host tests pass;
  the upstream tarball at the repo root preserves the assembly for provenance.

## Removed Functions

Dead code surfaced by the libFuzzer coverage report (`tests/fuzz/coverage.sh`):
functions with zero callers in any build configuration. Removed to shrink the
audit surface; both removals raised corpus coverage of their files to 100%.

- **`pvmp3_frame_synch()`** (`pvmp3_seek_synch.cpp` / `.h`) -- the sync-and-validate
  entry point. Its only caller was the already-deleted `pvmp3_decoder.cpp` (see
  "Removed Files"), so nothing reached it; the live decode path uses
  `pvmp3_framedecoder()` and reaches `pvmp3_header_sync()` directly via
  `pvmp3_decode_header.cpp`. Removing it also dropped the now-redundant
  `s_tmp3dec_file.h`, `pv_mp3dec_fxd_op.h`, and `pvmp3_tables.h` includes from
  `pvmp3_seek_synch.cpp` (they were only needed by the deleted body).

- **`fxp_mul32_Q26()`** (`pv_mp3dec_fxd_op_c_equivalent.h`,
  `pv_mp3dec_fxd_op_arm.h`, `pv_mp3dec_fxd_op_arm_gcc.h`,
  `pv_mp3dec_fxd_op_msc_evc.h`) -- a Q26 fixed-point multiply helper defined in
  all four platform-variant headers but invoked by no DSP routine.
