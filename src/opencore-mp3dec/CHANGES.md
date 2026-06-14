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

Clamp the scale-factor band-index lookups in the long-block branch of
`pvmp3_huffman_parsing()` to the 23-entry `mp3_sfBandIndex[].l[]` table.
The side-info fields `region0_count` (4 bits) and `region1_count`
(3 bits) can combine to produce an index of up to 24 on adversarial
streams, reading past the end of the table. Found by UBSan fuzzing.

### `pvmp3_dequantize_sample.cpp`

Clamp `temp2` to `[0, 2]` before using it as an index into
`gr_info->subblock_gain[3]` and `scalefac->s[][]` in the short-block
dequantization path. `temp2` is the sub-window index (0..2) derived from a
fixed-point product of side-info values; in spec-compliant streams it is
always in range, but adversarial input drove it past 2 and caused
out-of-bounds reads. Found by UBSan fuzzing.

### `pvmp3_seek_synch.cpp`

Two fixes:

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

2. Reject `bitrate_index == 15` ("reserved/invalid" per the MP3 spec)
   in the candidate-frame validation block. The `mp3_bitrate` table is
   declared `int16[3][15]`, so indexing it with 15 is a guaranteed
   out-of-bounds read. The parse-site fix in `pvmp3_decode_header.cpp`
   does NOT cover this site: `pvmp3_header_sync()` indexes the table
   independently before any header is parsed. Found during code review
   of the libFuzzer-driven fix series.

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

## Excluded from Build

- **`asm/*.s`** -- ARM and Windows Mobile assembly. The C-equivalent fixed-point
  routines in `pv_mp3dec_fxd_op_c_equivalent.h` are used on all platforms.
