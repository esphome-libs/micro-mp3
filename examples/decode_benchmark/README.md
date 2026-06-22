# ESP32 MP3 Decode Benchmark

Benchmarks MP3 decoding performance by decoding three 30-second MP3 clips in a loop, reporting per-frame timing statistics (min/max/avg/stddev). Tests 64kbps, 128kbps, and 320kbps bitrates. Demonstrates concurrent decoding with independent decoder instances across up to 4 tasks pinned to alternating cores.

## Features

- Three embedded 30-second test audio clips (public domain):
  - **64 kbps**: stereo (~235KB)
  - **128 kbps**: stereo (~470KB)
  - **320 kbps**: stereo (~1174KB)
- Per-frame timing with statistical analysis
- Setup vs. decode time split, plus per-decoder memory footprint (single-task runs)
- Concurrent decoding demonstration with 1, 2, 3, and 4 independent decode tasks
- Tasks pinned to alternating cores (task 0 → core 0, task 1 → core 1, etc.)
- Pre-configured for maximum performance (240MHz, 360MHz on the P4, PSRAM, `-O2`)

## Building and Flashing

### Prerequisites

- **PlatformIO** (recommended) OR ESP-IDF
- ESP32, ESP32-S3, or ESP32-P4 development board with PSRAM

### Option 1: PlatformIO (Recommended)

```bash
cd examples/decode_benchmark

# Build and upload (choose your target)
pio run -e esp32 -t upload -t monitor
pio run -e esp32s3 -t upload -t monitor
pio run -e esp32p4 -t upload -t monitor
```

The PlatformIO configuration uses the parent microMP3 repository as a component, so no additional setup is required.

### Option 2: Native ESP-IDF

```bash
cd examples/decode_benchmark
idf.py set-target esp32    # or esp32s3, esp32p4
idf.py build
idf.py flash monitor
```

### Configuration Options

#### PlatformIO

The default configuration is optimized for maximum performance. To customize:

1. Edit `sdkconfig.defaults` to change MP3-specific settings
2. Use `pio run -t menuconfig` for full ESP-IDF configuration

To cap the benchmark's task count (for example, on a single-core ESP32 that cannot sustain more than one concurrent stream), set the `DECODE_BENCH_MAX_CONCURRENT_TASKS` build flag:

```ini
build_flags = -DDECODE_BENCH_MAX_CONCURRENT_TASKS=1
```

#### Native ESP-IDF

```bash
idf.py menuconfig
```

Navigate to **Component config → microMP3 Decoder** to adjust:

- Memory placement (PSRAM vs internal RAM for decoder state)

## Expected Output

Each iteration tests all three clips with 1, 2, 3, and 4 concurrent tasks, followed by a summary:

```text
I (1242) DECODE_BENCH: === ESP32 MP3 Decode Benchmark ===
I (1242) DECODE_BENCH: Audio: 30s Beethoven Symphony No. 3 (from 1:00), 48kHz stereo
I (1252) DECODE_BENCH:   MP3 64kbps:  240744 bytes
I (1252) DECODE_BENCH:   MP3 128kbps: 481128 bytes
I (1262) DECODE_BENCH:   MP3 320kbps: 1202280 bytes
I (1262) DECODE_BENCH: Free heap: 8719472 bytes
I (1272) DECODE_BENCH: Free PSRAM: 8386148 bytes
I (1272) DECODE_BENCH: Free Internal: 333324 bytes
I (1282) DECODE_BENCH: Concurrent decode test: up to 4 independent tasks

I (1292) DECODE_BENCH: --- MP3 64kbps (48kHz stereo) - 1 concurrent task ---
I (3292) DECODE_BENCH: Task 0 starting MP3 decode on core 0...
I (3292) DECODE_BENCH: Task 0 finished (1931 ms)
I (3292) DECODE_BENCH: Task 0: Frame (us): min=1440 max=2613 avg=1540.0 sd=57.4 (n=1251)
I (3292) DECODE_BENCH: Task 0: Total: 1931 ms (setup: 0 ms, decode: 1931 ms), 30.0s audio, RTF: 0.064 (15.5x), decode RTF: 0.064 (15.5x), 48000 Hz, 2 ch, 64 kbps, core 0
I (3302) DECODE_BENCH: Task 0: Decoder footprint: 28172 bytes (internal: 0, PSRAM: 28172) (decoder state + PCM buffer)

...

I (16342) DECODE_BENCH: --- Summary (MP3 64kbps (48kHz stereo)) ---
I (16352) DECODE_BENCH:   1 task:     1937 ms
I (16352) DECODE_BENCH:   2 tasks:    2221 ms
I (16362) DECODE_BENCH:   3 tasks:    4480 ms
I (16362) DECODE_BENCH:   4 tasks:    5756 ms

...

I (57162) DECODE_BENCH: All decodes successful: YES
I (57172) DECODE_BENCH: Free heap: 8719168 bytes
I (57172) DECODE_BENCH: Min free heap ever:     8584080 bytes
I (57182) DECODE_BENCH: Min free internal ever: 310620 bytes
I (57182) DECODE_BENCH: Min free PSRAM ever:    8273460 bytes
```

The MP3 header probe is a few-byte parse, so `setup` is effectively 0 ms and `decode RTF` tracks the overall RTF; the split is reported for parity with the other codec benchmarks. The decoder footprint lands entirely in PSRAM here because decoder state defaults to PSRAM and `MALLOC_CAP_DEFAULT` resolves to PSRAM under this configuration; it is reported only for single-task runs.

### Output Fields

- **Frame (us)**: Per-frame decode time statistics (min/max/avg/sd in microseconds, n = frame count)
- **Total**: Wall-clock time to decode all audio, split into one-time `setup` (header probe + decoder init) and `decode`
- **RTF**: Real-Time Factor (total_time / audio_duration). RTF < 1 means faster than real-time
- **decode RTF**: Same factor with setup time excluded, so it reflects decode speed alone
- **Nx**: How many times faster than real-time playback (1/RTF)
- **Decoder footprint**: Bytes held by the decoder while running (decoder state + PCM buffer), split by internal RAM vs PSRAM. Logged only for single-task runs, where the global heap counters have no other writers
- **Min free ... ever**: Low-water marks since boot (overall heap, internal RAM, PSRAM). Captured after each iteration, they record the trough during peak concurrency even though every task has since exited
- **core N**: Which CPU core the task ran on

### Performance Scaling

The benchmark shows how performance scales with concurrent tasks. Each stream decodes on a single thread; concurrency comes from running independent decoder instances, one per task. The numbers below were measured on each board. Wall-clock is from the per-clip summary; per-task RTF is the slowest task in that run.

#### ESP32-S3 (240 MHz, octal PSRAM)

**MP3 64kbps (48kHz stereo)**:

| Tasks | Wall-clock | Per-task RTF | Notes |
| ----- | ---------- | ------------ | ----- |
| 1 | 1.9s | 0.064 (15.5x) | Single task on one core |
| 2 | 2.2s | 0.074 (13.6x) | One task per core |
| 3 | 4.5s | ~0.149 (6.7x) | Core 0 has 2 tasks, core 1 has 1 |
| 4 | 5.8s | ~0.191 (5.2x) | Two tasks per core |

**MP3 128kbps (48kHz stereo)**:

| Tasks | Wall-clock | Per-task RTF | Notes |
| ----- | ---------- | ------------ | ----- |
| 1 | 2.5s | 0.082 (12.2x) | Single task on one core |
| 2 | 2.8s | 0.094 (10.7x) | One task per core |
| 3 | 5.6s | ~0.187 (5.3x) | Core 0 has 2 tasks, core 1 has 1 |
| 4 | 6.9s | ~0.228 (4.4x) | Two tasks per core |

**MP3 320kbps (48kHz stereo)**:

| Tasks | Wall-clock | Per-task RTF | Notes |
| ----- | ---------- | ------------ | ----- |
| 1 | 3.1s | 0.102 (9.8x) | Single task on one core |
| 2 | 3.6s | 0.120 (8.4x) | One task per core |
| 3 | 7.0s | ~0.233 (4.3x) | Core 0 has 2 tasks, core 1 has 1 |
| 4 | 8.1s | ~0.270 (3.7x) | Two tasks per core |

With 2 tasks (one per core), combined throughput nearly doubles while wall-clock time only grows ~15-17%. The 3- and 4-task cases are slower per task because tasks share a core: the contention shows up as large standard deviations and high max frame times. Higher bitrates cost more per frame, with 320kbps running roughly 60% slower per frame than 64kbps.

#### ESP32-P4 (360 MHz, hex PSRAM)

**MP3 64kbps (48kHz stereo)**:

| Tasks | Wall-clock | Per-task RTF | Notes |
| ----- | ---------- | ------------ | ----- |
| 1 | 1.2s | 0.040 (25.0x) | Single task on one core |
| 2 | 1.2s | 0.040 (24.8x) | One task per core, ~50x combined |
| 3 | 2.4s | ~0.081 (12.4x) | Core 0 has 2 tasks, core 1 has 1 |
| 4 | 2.7s | ~0.088 (11.4x) | Two tasks per core |

**MP3 128kbps (48kHz stereo)**:

| Tasks | Wall-clock | Per-task RTF | Notes |
| ----- | ---------- | ------------ | ----- |
| 1 | 1.6s | 0.052 (19.4x) | Single task on one core |
| 2 | 1.6s | 0.052 (19.2x) | One task per core, ~38x combined |
| 3 | 3.1s | ~0.104 (9.6x) | Core 0 has 2 tasks, core 1 has 1 |
| 4 | 3.4s | ~0.114 (8.7x) | Two tasks per core |

**MP3 320kbps (48kHz stereo)**:

| Tasks | Wall-clock | Per-task RTF | Notes |
| ----- | ---------- | ------------ | ----- |
| 1 | 2.0s | 0.065 (15.3x) | Single task on one core |
| 2 | 2.0s | 0.066 (15.1x) | One task per core, ~30x combined |
| 3 | 4.0s | ~0.133 (7.5x) | Core 0 has 2 tasks, core 1 has 1 |
| 4 | 4.5s | ~0.148 (6.7x) | Two tasks per core |

The P4 decodes about 1.6x faster per stream than the S3 and scales nearly linearly to two tasks (one per core), where the second stream barely moves wall-clock time, before bus contention shows up at three and four.

#### ESP32 (240 MHz, quad PSRAM)

The plain single-core-class ESP32 caps the benchmark at two tasks via `-DDECODE_BENCH_MAX_CONCURRENT_TASKS=2` in `platformio.ini`; it cannot sustain real-time with three or more concurrent streams.

**MP3 64kbps (48kHz stereo)**:

| Tasks | Wall-clock | Per-task RTF | Notes |
| ----- | ---------- | ------------ | ----- |
| 1 | 8.2s | 0.274 (3.6x) | Single task on one core |
| 2 | 21.3s | ~0.709 (1.4x) | One task per core |

**MP3 128kbps (48kHz stereo)**:

| Tasks | Wall-clock | Per-task RTF | Notes |
| ----- | ---------- | ------------ | ----- |
| 1 | 9.6s | 0.319 (3.1x) | Single task on one core |
| 2 | 22.8s | ~0.759 (1.3x) | One task per core |

**MP3 320kbps (48kHz stereo)**:

| Tasks | Wall-clock | Per-task RTF | Notes |
| ----- | ---------- | ------------ | ----- |
| 1 | 10.9s | 0.364 (2.7x) | Single task on one core |
| 2 | 24.8s | ~0.826 (1.2x) | One task per core |

A single MP3 stream decodes in real-time on the ESP32 (about 4x slower per stream than the S3, due to slower quad PSRAM and no cache optimizations). Two concurrent streams still clear real-time, but per-task time more than doubles under heavy PSRAM bus contention, so three or more would fall behind.

## Concurrent Decoding

This example runs multiple independent decoder instances in parallel. Each FreeRTOS task:

- Creates its own `Mp3Decoder` instance (lazy-initialized on first `decode()` call)
- Allocates its own PCM output buffer on the heap (not the stack)
- Is pinned to a specific core (alternating 0, 1, 0, 1)
- Decodes independently without interference

All tasks decode simultaneously with correct results, confirming that separate instances do not interfere. A single instance is still not thread-safe: never share one across tasks. To decode multiple streams at once, give each task its own decoder.

## Regenerating Test Audio

The included test audio uses a public domain recording. To regenerate or use different audio:

```bash
# Download source (e.g., Beethoven Symphony No. 3 from Musopen on Archive.org)
curl -L -o source.flac "https://..."

# Extract 30 seconds starting at 1:00

# MP3 at 64kbps
ffmpeg -i source.flac -ss 60 -t 30 -c:a libmp3lame -b:a 64k main/test_audio_mp3_64k.mp3

# MP3 at 128kbps
ffmpeg -i source.flac -ss 60 -t 30 -c:a libmp3lame -b:a 128k main/test_audio_mp3_128k.mp3

# MP3 at 320kbps
ffmpeg -i source.flac -ss 60 -t 30 -c:a libmp3lame -b:a 320k main/test_audio_mp3_320k.mp3

# Convert each to a C header (edit variable names to match the existing headers)
xxd -i main/test_audio_mp3_64k.mp3 > main/test_audio_mp3_64k.h
xxd -i main/test_audio_mp3_128k.mp3 > main/test_audio_mp3_128k.h
xxd -i main/test_audio_mp3_320k.mp3 > main/test_audio_mp3_320k.h
# xxd derives variable names from the path, so rename e.g. main_test_audio_mp3_64k_mp3 to test_audio_mp3_64k
```

Keep clips ~30 seconds to fit in flash.

## Memory Usage

| Type | Size | Notes |
| ---- | ---- | ----- |
| Flash (audio only) | ~1.9MB | ~235KB (64k) + ~470KB (128k) + ~1174KB (320k) |
| Task stack | ~5KB each | Per FreeRTOS task (`5192` bytes); PCM buffer is heap-allocated separately |
| PCM output buffer | 4.5KB each | Heap-allocated per task (`MP3_MIN_OUTPUT_BUFFER_BYTES` = 4608 bytes) |
| Decoder state | ~21KB | Allocated on first `decode()` call; PSRAM preferred by default |
| Decoder footprint | 28172 bytes per stream | Decoder state + internal input buffer + PCM buffer combined. The benchmark logs this per single-task run as "Decoder footprint"; it lands entirely in PSRAM under the default configuration (internal: 0) |

With concurrent tasks the footprint is roughly Nx the single-stream number (about 113KB for 4 tasks), plus a ~5KB stack per task. Per-iteration the benchmark also logs the low-water marks (`Min free heap/internal/PSRAM ever`), which capture the trough during the peak-concurrency phase even after every task has exited.

## Troubleshooting

| Problem | Solution |
| ------- | -------- |
| Watchdog timeout | Disabled by default in `sdkconfig.defaults`; re-check if customizing |
| Stack overflow | PCM buffer must be heap-allocated, not on the FreeRTOS stack |
| Allocation failures | Check PSRAM is enabled; reduce concurrent task count |

## Technical Details

**Test Audio**: Beethoven Symphony No. 3 "Eroica", Op. 55, 30s extract starting at 1:00.

- Performer: Czech National Symphony Orchestra
- Source: [Musopen Collection](https://archive.org/details/MusopenCollectionAsFlac) on Archive.org
- License: Public Domain
- Formats: MP3 64kbps, 128kbps, 320kbps, all 48kHz stereo

**Timing**: Uses `esp_timer_get_time()` for microsecond precision. Only measures `decoder.decode()` calls that produce samples.
