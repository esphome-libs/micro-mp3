# ESP32 MP3 Decode Benchmark

Benchmarks MP3 decoding performance by decoding three 30-second MP3 clips in a loop, reporting per-frame timing statistics (min/max/avg/stddev). Tests 64kbps, 128kbps, and 320kbps bitrates. Also demonstrates thread-safe concurrent decoding with up to 4 tasks pinned to alternating cores.

## Features

- Three embedded 30-second test audio clips (public domain):
  - **64 kbps**: stereo (~235KB)
  - **128 kbps**: stereo (~470KB)
  - **320 kbps**: stereo (~1174KB)
- Per-frame timing with statistical analysis
- Thread safety demonstration with 1, 2, 3, and 4 concurrent decode tasks
- Tasks pinned to alternating cores (task 0 → core 0, task 1 → core 1, etc.)
- Pre-configured for maximum performance (240MHz, PSRAM, `-O2`)

## Building and Flashing

### Prerequisites

- **PlatformIO** (recommended) OR ESP-IDF
- ESP32 or ESP32-S3 development board with PSRAM

### Option 1: PlatformIO (Recommended)

```bash
cd examples/decode_benchmark

# Build and upload (choose your target)
pio run -e esp32 -t upload -t monitor
pio run -e esp32s3 -t upload -t monitor
```

The PlatformIO configuration uses the parent micro-mp3 repository as a component, so no additional setup is required.

### Option 2: Native ESP-IDF

```bash
cd examples/decode_benchmark
idf.py set-target esp32    # or esp32s3
idf.py build
idf.py flash monitor
```

### Configuration Options

#### PlatformIO

The default configuration is optimized for maximum performance. To customize:

1. Edit `sdkconfig.defaults` to change MP3-specific settings
2. Use `pio run -t menuconfig` for full ESP-IDF configuration

#### Native ESP-IDF

```bash
idf.py menuconfig
```

Navigate to **Component config → MP3 Decoder** to adjust:

- Memory placement (PSRAM vs internal RAM for decoder state)

## Expected Output

Each iteration tests all three clips with 1, 2, 3, and 4 concurrent tasks, followed by a summary:

```text
I (1231) DECODE_BENCH: === ESP32 MP3 Decode Benchmark ===
I (1231) DECODE_BENCH: Audio: 30s Beethoven Symphony No. 3 (from 1:00), 48kHz stereo
I (1241) DECODE_BENCH:   MP3 64kbps:  240744 bytes
I (1241) DECODE_BENCH:   MP3 128kbps: 481128 bytes
I (1251) DECODE_BENCH:   MP3 320kbps: 1202280 bytes

I (1281) DECODE_BENCH: --- MP3 64kbps (48kHz stereo) - 1 concurrent task ---
I (3261) DECODE_BENCH: Task 0: Frame (us): min=1470 max=2029 avg=1568.3 sd=49.4 (n=1252)
I (3261) DECODE_BENCH: Task 0: Total: 1968 ms (30.0s audio), RTF: 0.066 (15.3x real-time), 48000 Hz, 2 ch, 64 kbps, core 0

...

I (16241) DECODE_BENCH: --- Summary (MP3 64kbps (48kHz stereo)) ---
I (16251) DECODE_BENCH:   1 task:     1974 ms
I (16251) DECODE_BENCH:   2 tasks:    2324 ms
I (16261) DECODE_BENCH:   3 tasks:    4662 ms
I (16261) DECODE_BENCH:   4 tasks:    5795 ms

I (56561) DECODE_BENCH: All decodes successful: YES
I (56571) DECODE_BENCH: Free heap: 17107644 bytes
```

### Output Fields

- **Frame (us)**: Per-frame decode time statistics (min/max/avg/sd in microseconds, n = frame count)
- **Total**: Wall-clock time to decode all audio
- **RTF**: Real-Time Factor (decode_time / audio_duration). RTF < 1 means faster than real-time
- **Nx real-time**: How many times faster than real-time playback (1/RTF)

### Performance Results (ESP32-S3 @ 240MHz, PSRAM)

**MP3 64kbps (48kHz stereo)**:

| Tasks | Wall-clock | Per-task RTF | Notes |
| ----- | ---------- | ------------ | ----- |
| 1 | 2.0s | 0.066 (15.3x) | Single task on one core |
| 2 | 2.3s | 0.077 (13.0x) | One task per core |
| 3 | 4.7s | ~0.155 (6.5x) | Core 0 has 2 tasks, core 1 has 1 |
| 4 | 5.8s | ~0.193 (5.2x) | Two tasks per core |

**MP3 128kbps (48kHz stereo)**:

| Tasks | Wall-clock | Per-task RTF | Notes |
| ----- | ---------- | ------------ | ----- |
| 1 | 2.5s | 0.082 (12.2x) | Single task on one core |
| 2 | 2.9s | 0.097 (10.3x) | One task per core |
| 3 | 5.8s | ~0.191 (5.2x) | Core 0 has 2 tasks, core 1 has 1 |
| 4 | 6.9s | ~0.228 (4.4x) | Two tasks per core |

**MP3 320kbps (48kHz stereo)**:

| Tasks | Wall-clock | Per-task RTF | Notes |
| ----- | ---------- | ------------ | ----- |
| 1 | 3.0s | 0.100 (10.0x) | Single task on one core |
| 2 | 3.7s | 0.122 (8.2x) | One task per core |
| 3 | 7.1s | ~0.235 (4.3x) | Core 0 has 2 tasks, core 1 has 1 |
| 4 | 8.1s | ~0.268 (3.7x) | Two tasks per core |

### Performance Results (ESP32 @ 240MHz, PSRAM)

**MP3 64kbps (48kHz stereo)**:

| Tasks | Wall-clock | Per-task RTF | Notes |
| ----- | ---------- | ------------ | ----- |
| 1 | 9.4s | 0.314 (3.2x) | Single task on one core |
| 2 | 24.5s | ~0.814 (1.2x) | One task per core |
| 3 | 36.4s | ~1.184 (0.8x) | Core 0 has 2 tasks, core 1 has 1 |
| 4 | 55.4s | ~1.842 (0.5x) | Two tasks per core |

**MP3 128kbps (48kHz stereo)**:

| Tasks | Wall-clock | Per-task RTF | Notes |
| ----- | ---------- | ------------ | ----- |
| 1 | 10.5s | 0.351 (2.9x) | Single task on one core |
| 2 | 25.7s | ~0.854 (1.2x) | One task per core |
| 3 | 38.5s | ~1.265 (0.8x) | Core 0 has 2 tasks, core 1 has 1 |
| 4 | 58.2s | ~1.936 (0.5x) | Two tasks per core |

**MP3 320kbps (48kHz stereo)**:

| Tasks | Wall-clock | Per-task RTF | Notes |
| ----- | ---------- | ------------ | ----- |
| 1 | 12.0s | 0.398 (2.5x) | Single task on one core |
| 2 | 27.5s | ~0.914 (1.1x) | One task per core |
| 3 | 41.4s | ~1.376 (0.7x) | Core 0 has 2 tasks, core 1 has 1 |
| 4 | 60.5s | ~2.012 (0.5x) | Two tasks per core |

Higher bitrates increase decode cost due to larger frames — 320kbps takes roughly 27% longer per frame than 64kbps. The ESP32 is roughly 4.8x slower than the ESP32-S3 due to slower PSRAM (quad vs octal) and lack of cache optimizations. A single MP3 stream decodes comfortably in real-time on ESP32, but concurrent streams exceed real-time with 2+ tasks.

### Performance Scaling

With 2 tasks (one per core), total throughput nearly doubles while wall-clock time only increases ~18–21%. The 3-task case is slower because two tasks share one core — the shared-core tasks experience preemption spikes, visible in the high standard deviation and large max frame times.

## Thread Safety

This example demonstrates that each `Mp3Decoder` instance is fully independent. Each FreeRTOS task:

- Creates its own `Mp3Decoder` instance (lazy-initialized on first `decode()` call)
- Allocates its own PCM output buffer on the heap (not the stack)
- Is pinned to a specific core (alternating 0, 1, 0, 1)
- Decodes independently without interference

The concurrent decode shows all tasks running simultaneously with correct results, verifying thread safety for multi-stream applications.

## Regenerating Test Audio

The included test audio uses a public domain recording. To regenerate or use different audio:

```bash
# Download source (e.g., Beethoven Symphony No. 3 from Musopen on Archive.org)
curl -L -o source.flac "https://..."

# Extract 30 seconds starting at 1:00

# MP3 at 64kbps
ffmpeg -i source.flac -ss 60 -t 30 -c:a libmp3lame -b:a 64k src/test_audio_mp3_64k.mp3

# MP3 at 128kbps
ffmpeg -i source.flac -ss 60 -t 30 -c:a libmp3lame -b:a 128k src/test_audio_mp3_128k.mp3

# MP3 at 320kbps
ffmpeg -i source.flac -ss 60 -t 30 -c:a libmp3lame -b:a 320k src/test_audio_mp3_320k.mp3

# Convert each to a C header (edit variable names to match the existing headers)
xxd -i src/test_audio_mp3_64k.mp3 > src/test_audio_mp3_64k.h
xxd -i src/test_audio_mp3_128k.mp3 > src/test_audio_mp3_128k.h
xxd -i src/test_audio_mp3_320k.mp3 > src/test_audio_mp3_320k.h
# xxd derives variable names from the path, so rename e.g. src_test_audio_mp3_64k_mp3 → test_audio_mp3_64k
```

Keep clips ~30 seconds to fit in flash.

## Memory Usage

| Type | Size | Notes |
| ---- | ---- | ----- |
| Flash (audio only) | ~1.9MB | ~235KB (64k) + ~470KB (128k) + ~1174KB (320k) |
| Task stack | ~5KB each | Per FreeRTOS task (`5192` bytes); PCM buffer is heap-allocated separately |
| PCM output buffer | 4.5KB each | Heap-allocated per task (`MP3_MIN_OUTPUT_BUFFER_BYTES` = 4608 bytes) |
| Decoder state | ~23KB | Allocated on first `decode()` call; PSRAM preferred by default |

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
- Formats: MP3 64kbps, 128kbps, 320kbps — all 48kHz stereo

**Timing**: Uses `esp_timer_get_time()` for microsecond precision. Only measures `decoder.decode()` calls that produce samples.
