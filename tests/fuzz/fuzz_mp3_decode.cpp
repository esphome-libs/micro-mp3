// Copyright 2026 Kevin Ahrendt
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Fuzz harness for micro_mp3::Mp3Decoder.
//
// Feeds raw MP3 bytes through the streaming decoder in variably-sized chunks,
// so libFuzzer's coverage feedback explores both chunk-boundary bugs (probe
// slow path, frame straddling, ID3v2 / resync buffering) and the OpenCore
// fixed-point bitstream decoder.
//
// A configuration byte and a region of chunk-control bytes are consumed from
// the TAIL of the input via FuzzedDataProvider, so the front stays an intact
// MP3 payload. The tail steers options that have no bearing on the bitstream
// itself -- input chunk size, equalizer preset, output buffer size, and a
// reset-and-replay bit -- across runs, while a small set of Tier 1 structural
// invariants is asserted on every decode (see check_oracle below).
//
// Two build modes:
//   1. libFuzzer:  compile with -fsanitize=fuzzer,address,undefined, which
//      exposes LLVMFuzzerTestOneInput. Use with a corpus directory:
//          ./fuzz_mp3_decode corpus_mp3/
//   2. Standalone: compile with FUZZ_STANDALONE defined. Takes file paths on
//      argv for crash reproduction, or with no args runs a torture battery.

#include "micro_mp3/mp3_decoder.h"
#include <fuzzer/FuzzedDataProvider.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using micro_mp3::Mp3Decoder;
using micro_mp3::Mp3Equalizer;
using micro_mp3::Mp3Result;

// The seven equalizer presets, MP3_EQ_FLAT..MP3_EQ_TALK. Used to cycle the
// preset between decode() calls (set_equalizer takes effect on the next call).
static constexpr uint8_t kNumEqualizers = 7;

// Upper bound on the chunk-control bytes pulled from the TAIL (via
// FuzzedDataProvider, alongside the cfg byte). Cycling over them drives the
// streaming chunk sizes and equalizer presets while the front payload stays
// intact for the codec, so libFuzzer mutates stream bytes handed to the
// decoder rather than bytes the harness eats.
static constexpr size_t MAX_CONTROL_BYTES = 64;

// Bound the per-input work so libFuzzer keeps a high exec rate. A pathological
// input (tiny chunks over a large payload) would otherwise spin through one
// call per byte; capping iterations decodes only a prefix, which the coverage
// feedback tolerates.
static constexpr int MAX_ITERATIONS = 8192;
static constexpr size_t MAX_DECODED_BYTES = (1U << 20) * sizeof(int16_t);

// Per-pass options decoded from the cfg byte.
struct PassConfig {
    bool replay;         // re-run the whole payload across a reset()
    bool big_output;     // size the output buffer at 2x the minimum (still valid)
    bool stress_output;  // periodically probe the undersized-output guard
};

// True if `rate` is one of the nine MP3 Layer III sample rates (MPEG 1/2/2.5).
// parse_mp3_frame_header only accepts these, so get_sample_rate() must report
// one of them once the header has been parsed.
static bool is_valid_mp3_rate(uint32_t rate) {
    switch (rate) {
        case 8000:
        case 11025:
        case 12000:
        case 16000:
        case 22050:
        case 24000:
        case 32000:
        case 44100:
        case 48000:
            return true;
        default:
            return false;
    }
}

// Tier 1 oracle: structural invariants that must hold on every decode(),
// independent of the input. A violation aborts so sanitizers surface it.
static void check_oracle(const Mp3Decoder& dec, Mp3Result result, size_t consumed, size_t offered,
                         size_t samples, size_t output_size, bool header_ready) {
    // Never claim to consume more input than was offered.
    if (consumed > offered) {
        std::abort();
    }
    // Channel count is always 0 (pre-probe) or 1/2 (mono/stereo).
    const uint8_t channels = dec.get_channels();
    if (channels > micro_mp3::MP3_MAX_OUTPUT_CHANNELS) {
        std::abort();
    }
    // Per-channel sample count never exceeds the MPEG1 worst case.
    if (samples > micro_mp3::MP3_MAX_SAMPLES_PER_FRAME) {
        std::abort();
    }
    // Samples can only be emitted once the channel count is known.
    if (samples > 0 && channels == 0) {
        std::abort();
    }
    // The written PCM must fit the output buffer and be a whole number of
    // sample-frames (samples is per channel, so the byte count is intrinsically
    // a multiple of channels * 2; the modulo guards against that ever changing).
    const size_t written = samples * channels * sizeof(int16_t);
    if (written > output_size) {
        std::abort();
    }
    if (channels > 0 && (written % (channels * sizeof(int16_t))) != 0) {
        std::abort();
    }
    // STREAM_INFO_READY is the probe return: no audio yet, only a few header
    // bytes consumed, and a now-valid sample rate.
    if (result == micro_mp3::MP3_STREAM_INFO_READY) {
        if (samples != 0 || consumed > 3) {
            std::abort();
        }
        if (!is_valid_mp3_rate(dec.get_sample_rate())) {
            std::abort();
        }
    }
    // Once the header has been parsed the reported rate stays valid.
    if (header_ready && !is_valid_mp3_rate(dec.get_sample_rate())) {
        std::abort();
    }
}

// One streaming pass: feed `payload` to `dec` in control-byte-sized chunks,
// asserting the Tier 1 oracle on every decode. Factored out so it can run twice
// across a reset() to exercise the re-stream path (free+realloc, re-probe) that
// a single pass misses. `pcm` is reused across passes.
static void run_decode_pass(Mp3Decoder& dec, const std::vector<uint8_t>& payload,
                            const std::vector<uint8_t>& ctrl, const PassConfig& cfg,
                            std::vector<uint8_t>& pcm) {
    size_t off = 0;
    size_t total_decoded = 0;
    int iterations = 0;
    size_t ctrl_idx = 0;
    size_t window_extra = 0;  // grows when a frame needs more contiguous input
    bool header_ready = false;

    // Dedicated output buffer for the undersized-output guard probe, sized at
    // exactly MIN-1 and kept as its own allocation. If decode() ever wrote PCM
    // despite being told the buffer is too small, the write lands past this
    // allocation and ASan traps a heap-buffer-overflow. Passing the full-size
    // pcm buffer (which is >= MIN) would hide such a write.
    std::vector<uint8_t> undersized(micro_mp3::MP3_MIN_OUTPUT_BUFFER_BYTES - 1);

    while (off < payload.size() && iterations < MAX_ITERATIONS && total_decoded < MAX_DECODED_BYTES) {
        // One control byte drives two knobs from disjoint bit-fields: bits 7..3
        // pick a chunk size (1..1985, spanning sub-header to multi-frame), bits
        // 2..0 pick an equalizer preset. Control bytes cycle, so a short tail
        // still yields varied chunk sizes and presets.
        const uint8_t b = ctrl[ctrl_idx++ % ctrl.size()];
        const size_t chunk = 1 + ((static_cast<size_t>(b) >> 3) & 0x1F) * 64;
        dec.set_equalizer(static_cast<Mp3Equalizer>((b & 0x07) % kNumEqualizers));

        size_t avail = std::min(chunk + window_extra, payload.size() - off);

        // Exercise the undersized-output guard. For a normal audio frame the
        // size check at the top of decode() rejects before any state changes,
        // so the probe neither consumes nor writes -- the assert below verifies
        // that. (A metadata path, e.g. an ID3 tag mid-stream, can legitimately
        // consume bytes and return NEED_MORE_DATA before the size check is
        // reached, which is why the assert only constrains the TOO_SMALL case.)
        // The write itself is policed by ASan via the `undersized` allocation.
        if (cfg.stress_output && header_ready && (iterations & 0x03) == 0) {
            size_t c2 = 123;
            size_t s2 = 123;
            Mp3Result r2 = dec.decode(payload.data() + off, avail, undersized.data(),
                                      undersized.size(), c2, s2);
            if (r2 == micro_mp3::MP3_OUTPUT_BUFFER_TOO_SMALL && (c2 != 0 || s2 != 0)) {
                std::abort();
            }
        }

        size_t consumed = 0;
        size_t samples = 0;
        Mp3Result result =
            dec.decode(payload.data() + off, avail, pcm.data(), pcm.size(), consumed, samples);

        check_oracle(dec, result, consumed, avail, samples, pcm.size(), header_ready);

        if (result == micro_mp3::MP3_STREAM_INFO_READY) {
            header_ready = true;
            off += consumed;  // 0 (fast path) or <= 3 (slow path)
            iterations++;
            continue;
        }
        if (result == micro_mp3::MP3_DECODE_ERROR) {
            off += consumed;  // recoverable: advance past the skipped frame
            window_extra = 0;
            iterations++;
            continue;
        }
        if (result < 0) {
            break;  // fatal (allocation failure, invalid input)
        }

        // MP3_OK or MP3_NEED_MORE_DATA.
        if (samples > 0) {
            total_decoded += samples * dec.get_channels() * sizeof(int16_t);
        }
        off += consumed;

        if (consumed == 0 && samples == 0) {
            // No progress on a partial frame: widen the window so the next call
            // sees more contiguous input. (off is unchanged here, so the loop
            // guard `off < payload.size()` still holds -- the payload is drained
            // by the empty-input EOS pass below, not from inside this branch.)
            window_extra += chunk;
        } else {
            window_extra = 0;
        }
        iterations++;
    }

    // Signal end-of-stream by offering empty input, the way a caller drains the
    // decoder at EOF. This drives the input_len == 0 path: a clean frame
    // boundary returns MP3_OK with no samples, while a frame still buffered
    // internally is flushed as PCM. Bounded so a decoder that never settles
    // cannot spin here.
    for (int flush = 0; flush < 4; flush++) {
        size_t consumed = 0;
        size_t samples = 0;
        Mp3Result result = dec.decode(payload.data() + off, 0, pcm.data(), pcm.size(), consumed, samples);
        check_oracle(dec, result, consumed, 0, samples, pcm.size(), header_ready);
        if (result != micro_mp3::MP3_OK && result != micro_mp3::MP3_NEED_MORE_DATA) {
            break;  // fatal or stream-info; nothing more to flush
        }
        if (samples == 0) {
            break;  // buffer drained
        }
        total_decoded += samples * dec.get_channels() * sizeof(int16_t);
    }
}

// NOLINTNEXTLINE(readability-identifier-naming): fixed libFuzzer entry point name
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    FuzzedDataProvider fdp(data, size);

    // Decoder configuration, consumed from the TAIL of the input. FuzzedDataProvider
    // integral reads come off the back of the buffer, so the payload PREFIX is
    // preserved: only the trailing cfg/control bytes are peeled off, leaving the
    // front of a real .mp3 seed (or a merged corpus) intact. Up to 65 bytes are
    // peeled (1 cfg byte plus up to 64 chunk-control bytes), so an externally
    // supplied .mp3 loses that much off its tail. Seeds from generate_seeds.sh
    // append a config tail to avoid this, keeping the whole stream intact. An
    // exhausted provider reads 0, i.e. the historical default: single pass,
    // minimum output buffer, no undersize stress.
    const uint8_t cfg = fdp.ConsumeIntegral<uint8_t>();
    PassConfig pcfg;
    pcfg.replay = (cfg & 0x01) != 0;
    pcfg.big_output = (cfg & 0x02) != 0;
    pcfg.stress_output = (cfg & 0x04) != 0;
    // bits 3..7 are an unused mutable region for libFuzzer.

    // Reserve up to 1/8 of the input (capped) for chunk control. Tiny inputs
    // fall back to a single neutral control byte so the decoder still sees the
    // full payload.
    size_t ctrl_len = std::min(MAX_CONTROL_BYTES, fdp.remaining_bytes() / 8);
    std::vector<uint8_t> ctrl;
    ctrl.reserve(ctrl_len + 1);
    for (size_t i = 0; i < ctrl_len; i++) {
        ctrl.push_back(fdp.ConsumeIntegral<uint8_t>());
    }
    if (ctrl.empty()) {
        ctrl.push_back(0x20);  // neutral default: ~257-byte chunks, flat EQ
    }

    std::vector<uint8_t> payload = fdp.ConsumeRemainingBytes<uint8_t>();
    if (payload.empty()) {
        return 0;
    }

    Mp3Decoder dec;
    const size_t out_bytes =
        pcfg.big_output ? 2 * micro_mp3::MP3_MIN_OUTPUT_BUFFER_BYTES
                        : micro_mp3::MP3_MIN_OUTPUT_BUFFER_BYTES;
    std::vector<uint8_t> pcm(out_bytes);

    run_decode_pass(dec, payload, ctrl, pcfg, pcm);

    if (pcfg.replay) {
        // Replay the same payload across a reset() to drive the re-stream path
        // (free + realloc of decoder memory, a second probe) under ASan.
        dec.reset();
        run_decode_pass(dec, payload, ctrl, pcfg, pcm);
    }

    dec.reset();
    return 0;
}

#ifdef FUZZ_STANDALONE

namespace {

std::vector<uint8_t> read_file(const char* path) {
    std::vector<uint8_t> out;
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        return out;
    }
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) {
        out.resize(static_cast<size_t>(n));
        size_t got = std::fread(out.data(), 1, out.size(), f);
        out.resize(got);
    }
    std::fclose(f);
    return out;
}

uint32_t lcg_next(uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return state;
}

// A random blob salted with the markers the wrapper keys off of (MP3 sync
// pairs, "ID3", "Xing", "Info", "LAME"), so the framing and gapless paths
// engage on otherwise-random data.
std::vector<uint8_t> build_random_blob(uint32_t seed, size_t len) {
    std::vector<uint8_t> buf(len);
    uint32_t state = seed;
    for (size_t i = 0; i < len; i++) {
        buf[i] = static_cast<uint8_t>(lcg_next(state) >> 24);
    }
    // Sprinkle sync words. The low byte cycles through the common Layer III
    // second-sync bytes so every MPEG version/layer combination appears.
    static const uint8_t sync1[] = {0xFB, 0xF3, 0xE3, 0xFA, 0xF2, 0xE2};
    for (size_t i = 0; i + 1 < buf.size(); i += 64 + (seed % 128)) {
        buf[i] = 0xFF;
        buf[i + 1] = sync1[(i / 64) % (sizeof(sync1))];
    }
    // Drop a few "ID3", "Xing"/"Info", and "LAME" markers at fixed offsets.
    auto stamp = [&](size_t at, const char* tag) {
        for (size_t k = 0; tag[k] && at + k < buf.size(); k++) {
            buf[at + k] = static_cast<uint8_t>(tag[k]);
        }
    };
    if (len > 4) {
        stamp(0, "ID3");
    }
    if (len > 200) {
        stamp(len / 3, "Xing");
    }
    if (len > 300) {
        stamp(len / 2, "Info");
    }
    if (len > 400) {
        stamp((len * 2) / 3, "LAME");
    }
    return buf;
}

void mutate_in_place(std::vector<uint8_t>& buf, uint32_t& rng_state) {
    if (buf.empty()) {
        return;
    }
    int n = 1 + static_cast<int>((lcg_next(rng_state) >> 24) & 0x07);
    for (int i = 0; i < n; i++) {
        uint32_t r = lcg_next(rng_state);
        size_t pos = r % buf.size();
        uint32_t kind = (r >> 24) & 0x07;
        switch (kind) {
            case 0:
            case 1:
                buf[pos] ^= static_cast<uint8_t>(1u << ((r >> 8) & 0x07));
                break;
            case 2:
            case 3:
                buf[pos] = static_cast<uint8_t>(r >> 16);
                break;
            case 4: {
                static const uint8_t interesting[] = {0x00, 0x01, 0x7F, 0x80,
                                                      0xFF, 0xFE, 0xFB, 0xE3};
                buf[pos] = interesting[(r >> 16) & 0x07];
                break;
            }
            case 5: {
                size_t run = 1 + ((r >> 16) & 0x0F);
                for (size_t k = 0; k < run && pos + k < buf.size(); k++) {
                    buf[pos + k] = 0xFF;  // build sync-pair runs
                }
                break;
            }
            case 6:
                buf[pos] = static_cast<uint8_t>(buf[pos] + 1);
                break;
            default:
                buf[pos] = static_cast<uint8_t>(buf[pos] - 1);
                break;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    // Mutation mode: "./fuzz_mp3_decode -mutate <seedfile>"
    if (argc >= 3 && std::strcmp(argv[1], "-mutate") == 0) {
        std::vector<uint8_t> seed = read_file(argv[2]);
        if (seed.empty()) {
            std::fprintf(stderr, "[fuzz] seed file %s is empty or missing\n", argv[2]);
            return 1;
        }
        const char* iter_env = std::getenv("FUZZ_ITERATIONS");
        const int iters = iter_env ? std::atoi(iter_env) : 2000;
        std::printf("[fuzz] mutation mode: seed=%s (%zu bytes), %d iterations\n", argv[2],
                    seed.size(), iters);

        uint32_t rng_state = 0xC0FFEEu;
        std::vector<uint8_t> scratch;
        scratch.reserve(seed.size());

        LLVMFuzzerTestOneInput(seed.data(), seed.size());

        for (int i = 0; i < iters; i++) {
            scratch = seed;
            mutate_in_place(scratch, rng_state);
            LLVMFuzzerTestOneInput(scratch.data(), scratch.size());
            if ((i + 1) % 200 == 0) {
                std::printf("[fuzz] %d/%d mutated iterations ok\n", i + 1, iters);
            }
        }
        std::printf("[fuzz] mutation fuzzing complete, no sanitizer failures\n");
        return 0;
    }

    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            std::vector<uint8_t> data = read_file(argv[i]);
            std::printf("[fuzz] %s (%zu bytes)\n", argv[i], data.size());
            LLVMFuzzerTestOneInput(data.data(), data.size());
        }
        std::printf("[fuzz] %d file(s) processed cleanly\n", argc - 1);
        return 0;
    }

    std::printf("[fuzz] standalone torture mode\n");

    // Empty / tiny inputs.
    {
        const uint8_t nothing[1] = {0};
        LLVMFuzzerTestOneInput(nothing, 0);
        LLVMFuzzerTestOneInput(nothing, 1);
    }

    // A lone sync byte: the first half of a sync pair with nothing after.
    {
        const uint8_t lone[1] = {0xFF};
        LLVMFuzzerTestOneInput(lone, sizeof(lone));
    }

    // A valid-looking MPEG1 Layer III header (0xFF 0xFB, 128 kbps, 44.1 kHz)
    // truncated immediately after the 4-byte header: the frame body never
    // arrives, so the decoder must wait, not over-read.
    {
        const uint8_t trunc[4] = {0xFF, 0xFB, 0x90, 0x00};
        LLVMFuzzerTestOneInput(trunc, sizeof(trunc));
    }

    // All-0xFF runs: every byte pair passes the 11-bit sync test but fails full
    // header validation, the adversarial resync input. A few lengths around the
    // internal buffer size and the frame-straddle boundary.
    for (size_t len : {2u, 4u, 31u, 1536u, 4096u}) {
        std::vector<uint8_t> ones(len, 0xFF);
        LLVMFuzzerTestOneInput(ones.data(), ones.size());
    }

    // Random blobs salted with sync words / "ID3" / "Xing" / "Info" / "LAME".
    const char* iter_env = std::getenv("FUZZ_ITERATIONS");
    const int kIterations = iter_env ? std::atoi(iter_env) : 200;
    for (int i = 0; i < kIterations; i++) {
        size_t len = 512 + (static_cast<size_t>(i) * 37) % (32 * 1024);
        std::vector<uint8_t> blob = build_random_blob(static_cast<uint32_t>(i) * 2654435761u, len);
        LLVMFuzzerTestOneInput(blob.data(), blob.size());
        if ((i + 1) % 200 == 0) {
            std::printf("[fuzz] %d/%d random iterations ok\n", i + 1, kIterations);
        }
    }

    std::printf("[fuzz] standalone torture complete, no sanitizer failures\n");
    return 0;
}

#endif  // FUZZ_STANDALONE
