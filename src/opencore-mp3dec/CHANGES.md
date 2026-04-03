# Changes from Upstream OpenCore MP3 Decoder

Source: OpenCore project, `refs/heads/main`, `codecs_v2/audio/mp3/dec`
(original tarball preserved at repo root as `opencore-refs_heads_main-codecs_v2-audio-mp3-dec.tar.gz`)

## Modified Files

### `pvmp3_normalize.cpp`

Replaced the manual multi-step bit-scan cascade in `pvmp3_normalize()` with
`return __builtin_clz(x) - 1;`. The original used a lookup-table-style if/else
tree followed by a switch statement to count leading zeros. The `__builtin_clz`
intrinsic is equivalent, produces a native `CLZ` instruction on ARM and Xtensa,
and saves ~11 KB of flash when compiled with `-O2`.

## Excluded from Build

These files are present in the directory but excluded in `cmake/sources.cmake`:

- **`pvmp3_decoder.cpp`** -- depends on OSCL (Operating System Compatibility
  Library) headers that are not included. All required functionality is available
  through `pvmp3_framedecoder.cpp`.
- **`asm/*.s`** -- ARM and Windows Mobile assembly. The C-equivalent fixed-point
  routines in `pv_mp3dec_fxd_op_c_equivalent.h` are used on all platforms.
