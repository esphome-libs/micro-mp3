// Copyright 2026 Kevin Ahrendt
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/* Unit tests for the Mp3Decoder wrapper.
 *
 * These tests target the wrapper's own logic: header probing, frame
 * synchronization and resync, internal buffering across chunk boundaries,
 * ID3v2 skipping, corrupt-frame recovery, and gapless (Xing/Info/LAME)
 * trimming. The OpenCore fixed-point decode math is validated separately
 * against ffmpeg; here, the all-at-once full-buffer decode of a fixture is the
 * self-consistency reference that the streamed and modified-input decodes must
 * match, so intentional decoder changes don't invalidate golden data.
 *
 * The one exception is decode_accuracy, which checks the recovered tones with a
 * tolerance wide enough to survive benign numerical changes but tight enough to
 * catch a channel swap or a gross DSP regression.
 *
 * Each fixture is a pure sine tone (distinct frequency per stereo channel) of a
 * known duration, so a sample-accurate gapless decoder recovers exactly the
 * original sample count -- see gapless_trim.
 *
 * Usage: test_mp3_decoder <data_dir> [test_name]
 */

#include "micro_mp3/mp3_decoder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using micro_mp3::Mp3Decoder;
using micro_mp3::Mp3Result;
using micro_mp3::Mp3Version;

static std::string g_data_dir;

// Abort the current test on the first failed condition, reporting the line.
#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("    CHECK failed: %s (line %d)\n", #cond, __LINE__); \
            return false;                                                     \
        }                                                                     \
    } while (0)

// Like CHECK(a == b) but reports both operands' values on failure.
#define CHECK_EQ(a, b)                                                  \
    do {                                                                \
        const long long _va = static_cast<long long>(a);                \
        const long long _vb = static_cast<long long>(b);                \
        if (_va != _vb) {                                               \
            std::printf("    CHECK_EQ failed: %s == %s (%lld vs %lld) " \
                        "(line %d)\n",                                  \
                        #a, #b, _va, _vb, __LINE__);                    \
            return false;                                               \
        }                                                               \
    } while (0)

static std::vector<uint8_t> read_file(const std::string& name) {
    std::ifstream f(g_data_dir + "/" + name, std::ios::binary);
    if (!f) {
        std::printf("    cannot open fixture: %s\n", name.c_str());
        return {};
    }
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

// ============================================================================
// Fixture metadata
//
// bitrate == 0 means "do not assert an exact bitrate" (VBR: the leading Xing
// frame's header bitrate is incidental). orig_samples is the exact per-channel
// PCM length of the source tone; a sample-accurate gapless decode recovers it.
// ============================================================================

struct Fixture {
    const char* file;
    uint32_t sample_rate;
    uint8_t channels;
    Mp3Version version;
    uint32_t bitrate;       // kbps from the first frame header, 0 = don't check
    size_t orig_samples;    // per-channel sample count of the source tone
    bool has_xing;          // true if a Xing/Info gapless header frame is present
};

static const Fixture FIXTURES[] = {
    {"sine_stereo_44100.mp3", 44100, 2, micro_mp3::MP3_MPEG1, 128, 88200, true},
    {"sine_mono_44100.mp3", 44100, 1, micro_mp3::MP3_MPEG1, 128, 66150, true},
    {"sine_stereo_22050.mp3", 22050, 2, micro_mp3::MP3_MPEG2, 96, 33075, true},
    {"sine_mono_8000.mp3", 8000, 1, micro_mp3::MP3_MPEG2_5, 32, 12000, true},
    {"sine_mono_24000_8kbps.mp3", 24000, 1, micro_mp3::MP3_MPEG2, 0, 36000, true},
    {"sine_stereo_44100_vbr.mp3", 44100, 2, micro_mp3::MP3_MPEG1, 0, 88200, true},
    {"sine_stereo_44100_noxing.mp3", 44100, 2, micro_mp3::MP3_MPEG1, 128, 0, false},
};

// ============================================================================
// Decode harness
// ============================================================================

struct DecodeOutput {
    std::vector<int16_t> pcm;  // Interleaved samples across all channels
    uint32_t sample_rate{0};
    uint8_t channels{0};
    Mp3Version version{micro_mp3::MP3_MPEG1};
    uint32_t bitrate{0};
    bool header_ready{false};
    bool errored{false};
    Mp3Result error{micro_mp3::MP3_OK};
    size_t info_ready_events{0};   // count of MP3_STREAM_INFO_READY returns
    size_t info_bytes_consumed{0}; // bytes_consumed reported on the info return
    size_t decode_errors{0};       // count of recoverable MP3_DECODE_ERROR returns
};

// Drive a decoder over `data`, offering at most `chunk` input bytes per call
// (simulating streamed input; the window grows if the decoder makes no progress
// on a partial frame, as a real ring buffer would accumulate). The output
// buffer is always the documented minimum size.
static DecodeOutput decode_stream(Mp3Decoder& dec, const uint8_t* data, size_t len,
                                  size_t chunk = SIZE_MAX) {
    DecodeOutput out;
    std::vector<int16_t> buf(micro_mp3::MP3_MIN_OUTPUT_BUFFER_BYTES / sizeof(int16_t));
    size_t off = 0;
    size_t window = chunk;

    // Generous safety bound: chunk=1 feeding needs at least one call per byte.
    const size_t max_calls = 64 * (len + 1024);
    for (size_t calls = 0; calls < max_calls; calls++) {
        size_t avail = std::min(window, len - off);
        size_t consumed = 0;
        size_t samples = 0;
        Mp3Result result = dec.decode(data + off, avail, reinterpret_cast<uint8_t*>(buf.data()),
                                      buf.size() * sizeof(int16_t), consumed, samples);
        off += consumed;

        if (result == micro_mp3::MP3_STREAM_INFO_READY) {
            out.header_ready = true;
            out.info_ready_events++;
            out.info_bytes_consumed = consumed;
            out.sample_rate = dec.get_sample_rate();
            out.channels = dec.get_channels();
            out.version = dec.get_version();
            out.bitrate = dec.get_bitrate();
            continue;
        }

        if (result == micro_mp3::MP3_DECODE_ERROR) {
            out.decode_errors++;  // Recoverable: the bad frame was already skipped
            continue;
        }

        if (result < 0) {
            out.errored = true;
            out.error = result;
            return out;
        }

        if (samples > 0 && out.channels > 0) {
            const size_t elems = samples * out.channels;
            out.pcm.insert(out.pcm.end(), buf.data(), buf.data() + elems);
        }

        if (consumed == 0 && samples == 0) {
            if (off >= len) {
                // Clean end of stream is signaled by MP3_OK on empty input.
                // Any other result here (notably MP3_NEED_MORE_DATA) means the
                // decoder still has a partial frame buffered that the exhausted
                // input can't complete, i.e. a truncated stream.
                if (result != micro_mp3::MP3_OK) {
                    out.errored = true;
                    out.error = result;
                }
                return out;
            }
            window += chunk;  // No progress on a partial window: offer more input
        } else {
            window = chunk;
        }
    }
    out.errored = true;  // Loop bound exhausted: decoder made no progress
    return out;
}

// Reference decode: whole buffer offered on every call, no chunking.
static DecodeOutput reference_decode(const std::string& name) {
    std::vector<uint8_t> data = read_file(name);
    DecodeOutput out;
    if (data.empty()) {
        out.errored = true;
        return out;
    }
    Mp3Decoder dec;
    return decode_stream(dec, data.data(), data.size());
}

static bool outputs_equal(const DecodeOutput& a, const DecodeOutput& b) {
    return a.sample_rate == b.sample_rate && a.channels == b.channels && a.pcm == b.pcm;
}

// Goertzel power of a single tone, normalized by window length. `stride` and
// `offset` select one channel out of interleaved PCM.
static double goertzel_power(const int16_t* x, size_t n, size_t stride, size_t offset, double freq,
                             double sample_rate) {
    const double w = 2.0 * 3.14159265358979323846 * freq / sample_rate;
    const double coeff = 2.0 * std::cos(w);
    double s1 = 0.0;
    double s2 = 0.0;
    for (size_t i = 0; i < n; i++) {
        const double s0 = static_cast<double>(x[offset + i * stride]) + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return power / static_cast<double>(n);
}

// ============================================================================
// Tests
// ============================================================================

static bool test_stream_info_contract() {
    for (const Fixture& fx : FIXTURES) {
        std::vector<uint8_t> data = read_file(fx.file);
        CHECK(!data.empty());

        Mp3Decoder dec;
        // Accessors report "unknown" before the first decode.
        CHECK(!dec.is_initialized());
        CHECK_EQ(dec.get_sample_rate(), 0);
        CHECK_EQ(dec.get_channels(), 0);
        CHECK_EQ(dec.get_samples_per_frame(), 0);

        DecodeOutput out = decode_stream(dec, data.data(), data.size());
        if (out.errored) {
            std::printf("    %s: decode errored (%d)\n", fx.file, out.error);
            return false;
        }

        CHECK(out.header_ready);
        CHECK_EQ(out.info_ready_events, 1);             // Reported exactly once
        CHECK(out.info_bytes_consumed <= 3);            // Probe doesn't drain the frame body
        CHECK_EQ(out.sample_rate, fx.sample_rate);
        CHECK_EQ(out.channels, fx.channels);
        CHECK_EQ(out.version, fx.version);
        if (fx.bitrate != 0) {
            CHECK_EQ(out.bitrate, fx.bitrate);
        } else {
            CHECK(out.bitrate > 0);
        }

        // samples_per_frame follows the MPEG version once probed.
        const size_t spf = (fx.version == micro_mp3::MP3_MPEG1) ? 1152 : 576;
        CHECK_EQ(dec.get_samples_per_frame(), spf);
        CHECK(dec.is_initialized());

        // Some audio actually came out.
        CHECK(out.pcm.size() > fx.channels * 1000U);
    }
    return true;
}

static bool test_chunked_invariance() {
    for (const Fixture& fx : FIXTURES) {
        DecodeOutput ref = reference_decode(fx.file);
        CHECK(!ref.errored);
        CHECK_EQ(ref.decode_errors, 0);
        CHECK(!ref.pcm.empty());

        std::vector<uint8_t> data = read_file(fx.file);
        CHECK(!data.empty());

        // chunk=1 is the most punishing path (one call per byte, exercising the
        // header slow-path and every partial-frame boundary). Restrict it to the
        // smallest fixture to keep the sanitized run quick.
        const bool tiny = (data.size() < 8000);
        const size_t chunks[] = {tiny ? 1U : 17U, 509U, 4096U};
        for (size_t chunk : chunks) {
            Mp3Decoder dec;
            DecodeOutput out = decode_stream(dec, data.data(), data.size(), chunk);
            if (out.errored || out.decode_errors != 0 || !outputs_equal(ref, out)) {
                std::printf("    mismatch: %s chunk=%zu (errored=%d derr=%zu, ref %zu vs %zu)\n",
                            fx.file, chunk, out.errored ? 1 : 0, out.decode_errors, ref.pcm.size(),
                            out.pcm.size());
                return false;
            }
        }
    }
    return true;
}

static bool test_decode_accuracy() {
    // Verify the recovered tone sits at the encoded frequency in each channel,
    // dominates the other channel's tone (catching a channel swap) and a sweep
    // of decoy frequencies (catching a gross DSP regression), with a plausible
    // amplitude (catching silence or garbage).
    struct ToneCase {
        const char* file;
        uint32_t sample_rate;
        uint8_t channels;
        double tone[2];  // expected tone per channel
    };
    const ToneCase cases[] = {
        {"sine_stereo_44100.mp3", 44100, 2, {500.0, 1700.0}},
        {"sine_mono_44100.mp3", 44100, 1, {600.0, 0.0}},
        {"sine_stereo_22050.mp3", 22050, 2, {400.0, 1500.0}},
        {"sine_mono_24000_8kbps.mp3", 24000, 1, {440.0, 0.0}},
    };
    const double decoys[] = {120.0, 250.0, 1000.0, 2500.0, 3500.0};

    for (const ToneCase& c : cases) {
        DecodeOutput out = reference_decode(c.file);
        CHECK(!out.errored);
        CHECK_EQ(out.channels, c.channels);

        const size_t frames = out.pcm.size() / c.channels;
        CHECK(frames > 24000);
        // Analyze a steady-state window away from the stream edges.
        const size_t start = 4096;
        const size_t n = 16384;
        CHECK(start + n <= frames);

        for (uint8_t ch = 0; ch < c.channels; ch++) {
            const int16_t* base = out.pcm.data() + start * c.channels;
            const double expected = c.tone[ch];
            const double p_expected =
                goertzel_power(base, n, c.channels, ch, expected, c.sample_rate);

            // RMS guards against silence or garbage. The ffmpeg sine source is
            // ~0.12 full-scale, so a clean tone decodes to ~2750 RMS; a floor of
            // 1000 clears that comfortably while still rejecting near-silence.
            double sumsq = 0.0;
            for (size_t i = 0; i < n; i++) {
                const double s = base[ch + i * c.channels];
                sumsq += s * s;
            }
            const double rms = std::sqrt(sumsq / static_cast<double>(n));
            if (rms < 1000.0 || rms > 32768.0) {
                std::printf("    %s ch%u: implausible RMS %.0f\n", c.file, ch, rms);
                return false;
            }

            // The expected tone must dominate every decoy by a wide margin.
            for (double f : decoys) {
                if (std::abs(f - expected) < 50.0) {
                    continue;  // skip decoys too close to the real tone
                }
                const double pf = goertzel_power(base, n, c.channels, ch, f, c.sample_rate);
                if (p_expected < 20.0 * pf) {
                    std::printf("    %s ch%u: tone %.0f power %.3g not >> decoy %.0f power %.3g\n",
                                c.file, ch, expected, p_expected, f, pf);
                    return false;
                }
            }

            // For stereo, the other channel's tone must be far weaker here.
            if (c.channels == 2) {
                const double other = c.tone[1 - ch];
                const double p_other =
                    goertzel_power(base, n, c.channels, ch, other, c.sample_rate);
                if (p_expected < 20.0 * p_other) {
                    std::printf("    %s ch%u: tone %.0f (%.3g) not >> other %.0f (%.3g) "
                                "-- channel swap?\n",
                                c.file, ch, expected, p_expected, other, p_other);
                    return false;
                }
            }
        }
    }
    return true;
}

static bool test_reset_reuse() {
    // Decode stream A, reset, decode a very different stream B (different rate,
    // channel count and MPEG version) on the same decoder; both must match a
    // fresh decode bit for bit.
    DecodeOutput ref_a = reference_decode("sine_stereo_44100.mp3");
    DecodeOutput ref_b = reference_decode("sine_mono_8000.mp3");
    CHECK(!ref_a.errored);
    CHECK(!ref_b.errored);

    std::vector<uint8_t> data_a = read_file("sine_stereo_44100.mp3");
    std::vector<uint8_t> data_b = read_file("sine_mono_8000.mp3");
    CHECK(!data_a.empty());
    CHECK(!data_b.empty());

    Mp3Decoder dec;
    DecodeOutput a = decode_stream(dec, data_a.data(), data_a.size());
    CHECK(outputs_equal(ref_a, a));

    dec.reset();
    CHECK(!dec.is_initialized());
    CHECK_EQ(dec.get_sample_rate(), 0);
    CHECK_EQ(dec.get_channels(), 0);
    CHECK_EQ(dec.get_samples_per_frame(), 0);

    DecodeOutput b = decode_stream(dec, data_b.data(), data_b.size());
    CHECK(outputs_equal(ref_b, b));
    return true;
}

static bool test_error_contract() {
    std::vector<uint8_t> data = read_file("sine_stereo_44100.mp3");
    CHECK(!data.empty());

    std::vector<int16_t> buf(micro_mp3::MP3_MIN_OUTPUT_BUFFER_BYTES / sizeof(int16_t));
    uint8_t* out_ptr = reinterpret_cast<uint8_t*>(buf.data());
    const size_t out_size = buf.size() * sizeof(int16_t);

    // Poison value pre-loaded into the out-params so we can prove the decoder
    // zeroed them even on the early-return error paths.
    const size_t kPoison = 123;

    // Null input and null output are rejected outright, with nothing consumed.
    {
        Mp3Decoder dec;
        size_t consumed = kPoison;
        size_t samples = kPoison;
        Mp3Result r = dec.decode(nullptr, 16, out_ptr, out_size, consumed, samples);
        CHECK_EQ(r, micro_mp3::MP3_INPUT_INVALID);
        CHECK_EQ(consumed, 0);
        CHECK_EQ(samples, 0);
    }
    {
        Mp3Decoder dec;
        size_t consumed = kPoison;
        size_t samples = kPoison;
        Mp3Result r = dec.decode(data.data(), data.size(), nullptr, out_size, consumed, samples);
        CHECK_EQ(r, micro_mp3::MP3_INPUT_INVALID);
        CHECK_EQ(consumed, 0);
        CHECK_EQ(samples, 0);
    }

    // Drive one decoder past the header probe, then exercise the post-probe
    // contract: an undersized output buffer and an empty input.
    {
        Mp3Decoder dec;
        size_t off = 0;
        size_t consumed = 0;
        size_t samples = 0;
        Mp3Result r = micro_mp3::MP3_OK;
        bool probed = false;
        for (int i = 0; i < 1000 && !probed; i++) {
            r = dec.decode(data.data() + off, data.size() - off, out_ptr, out_size, consumed,
                           samples);
            off += consumed;
            CHECK(r >= 0);
            probed = (r == micro_mp3::MP3_STREAM_INFO_READY);
        }
        CHECK(probed);

        // Output buffer below the documented minimum is rejected, consuming
        // nothing and zeroing both out-params.
        consumed = kPoison;
        samples = kPoison;
        r = dec.decode(data.data() + off, data.size() - off, out_ptr,
                       micro_mp3::MP3_MIN_OUTPUT_BUFFER_BYTES - 1, consumed, samples);
        CHECK_EQ(r, micro_mp3::MP3_OUTPUT_BUFFER_TOO_SMALL);
        CHECK_EQ(consumed, 0);
        CHECK_EQ(samples, 0);

        // Empty input with nothing buffered is the EOS signal: MP3_OK, no output.
        consumed = kPoison;
        samples = kPoison;
        r = dec.decode(data.data() + off, 0, out_ptr, out_size, consumed, samples);
        CHECK_EQ(r, micro_mp3::MP3_OK);
        CHECK_EQ(consumed, 0);
        CHECK_EQ(samples, 0);
    }
    return true;
}

// Parse just enough of an MP3 frame header to return its total length, or 0 if
// `data` does not start at a valid Layer III frame. Mirrors the wrapper's own
// header parser; used by the corrupt-frame test to walk frame boundaries.
static size_t frame_length_at(const uint8_t* data, size_t len) {
    if (len < 4 || data[0] != 0xFF || (data[1] & 0xE0) != 0xE0) {
        return 0;
    }
    const uint8_t version_bits = (data[1] >> 3) & 0x03;
    const uint8_t layer_bits = (data[1] >> 1) & 0x03;
    if (version_bits == 0x01 || layer_bits != 0x01) {
        return 0;
    }
    const uint8_t bitrate_index = (data[2] >> 4) & 0x0F;
    const uint8_t samplerate_index = (data[2] >> 2) & 0x03;
    const uint8_t padding = (data[2] >> 1) & 0x01;
    if (bitrate_index == 0 || bitrate_index == 0x0F || samplerate_index == 0x03) {
        return 0;
    }
    static const uint16_t kBitMpeg1[15] = {0,   32,  40,  48,  56,  64,  80, 96,
                                           112, 128, 160, 192, 224, 256, 320};
    static const uint16_t kBitMpeg2[15] = {0,  8,  16, 24,  32,  40,  48, 56,
                                           64, 80, 96, 112, 128, 144, 160};
    static const uint16_t kRateMpeg1[3] = {44100, 48000, 32000};
    static const uint16_t kRateMpeg2[3] = {22050, 24000, 16000};
    static const uint16_t kRateMpeg25[3] = {11025, 12000, 8000};
    uint32_t bitrate = 0;
    uint32_t rate = 0;
    bool mpeg1 = false;
    if (version_bits == 0x03) {
        bitrate = kBitMpeg1[bitrate_index];
        rate = kRateMpeg1[samplerate_index];
        mpeg1 = true;
    } else if (version_bits == 0x02) {
        bitrate = kBitMpeg2[bitrate_index];
        rate = kRateMpeg2[samplerate_index];
    } else {
        bitrate = kBitMpeg2[bitrate_index];
        rate = kRateMpeg25[samplerate_index];
    }
    if (bitrate == 0 || rate == 0) {
        return 0;
    }
    const uint32_t coeff = mpeg1 ? 144 : 72;
    return (coeff * bitrate * 1000) / rate + padding;
}

// decode() accepts an output buffer sized to the stream's actual per-frame
// output (smaller than the MPEG1-stereo worst case for mono or MPEG2/2.5
// streams) and rejects one int16_t below it. get_min_output_buffer_bytes() still
// reports the worst case, which is always safe.
static bool test_per_stream_output_buffer() {
    struct Case {
        const char* file;
        size_t expected_min_bytes;  // get_samples_per_frame() * channels * 2
    };
    const Case cases[] = {
        {"sine_mono_44100.mp3", 1152 * 1 * sizeof(int16_t)},   // MPEG1 mono   = 2304
        {"sine_mono_8000.mp3", 576 * 1 * sizeof(int16_t)},     // MPEG2.5 mono = 1152
        {"sine_stereo_22050.mp3", 576 * 2 * sizeof(int16_t)},  // MPEG2 stereo = 2304
    };

    for (const Case& c : cases) {
        std::vector<uint8_t> data = read_file(c.file);
        CHECK(!data.empty());

        Mp3Decoder dec;

        // The reported minimum is the always-safe worst case, regardless of stream.
        CHECK_EQ(dec.get_min_output_buffer_bytes(), micro_mp3::MP3_MIN_OUTPUT_BUFFER_BYTES);

        // Drive past the header probe with a worst-case buffer.
        std::vector<int16_t> probe_buf(micro_mp3::MP3_MIN_OUTPUT_BUFFER_BYTES / sizeof(int16_t));
        uint8_t* probe_ptr = reinterpret_cast<uint8_t*>(probe_buf.data());
        const size_t probe_size = probe_buf.size() * sizeof(int16_t);

        size_t off = 0;
        size_t consumed = 0;
        size_t samples = 0;
        Mp3Result r = micro_mp3::MP3_OK;
        bool probed = false;
        for (int i = 0; i < 1000 && !probed; i++) {
            r = dec.decode(data.data() + off, data.size() - off, probe_ptr, probe_size, consumed,
                           samples);
            off += consumed;
            CHECK(r >= 0);
            probed = (r == micro_mp3::MP3_STREAM_INFO_READY);
        }
        CHECK(probed);

        // The always-safe minimum does not change after probing.
        CHECK_EQ(dec.get_min_output_buffer_bytes(), micro_mp3::MP3_MIN_OUTPUT_BUFFER_BYTES);

        // This stream's actual per-frame output is strictly smaller than the
        // worst case, and decode() accepts a buffer sized exactly to it.
        const size_t min_bytes = dec.get_samples_per_frame() * dec.get_channels() * sizeof(int16_t);
        CHECK_EQ(min_bytes, c.expected_min_bytes);
        CHECK(min_bytes < micro_mp3::MP3_MIN_OUTPUT_BUFFER_BYTES);

        // One int16 below the per-frame size is rejected, nothing consumed.
        std::vector<int16_t> tight(min_bytes / sizeof(int16_t));
        uint8_t* tight_ptr = reinterpret_cast<uint8_t*>(tight.data());
        {
            size_t c2 = 123;
            size_t s2 = 123;
            r = dec.decode(data.data() + off, data.size() - off, tight_ptr,
                           min_bytes - sizeof(int16_t), c2, s2);
            CHECK_EQ(r, micro_mp3::MP3_OUTPUT_BUFFER_TOO_SMALL);
            CHECK_EQ(c2, 0);
            CHECK_EQ(s2, 0);
        }

        // A buffer of exactly the per-stream minimum decodes the rest of the
        // stream without ever reporting TOO_SMALL, and yields real PCM.
        size_t total_samples = 0;
        const size_t max_calls = 64 * (data.size() + 1024);
        for (size_t calls = 0; calls < max_calls && off < data.size(); calls++) {
            r = dec.decode(data.data() + off, data.size() - off, tight_ptr, min_bytes, consumed,
                           samples);
            CHECK(r != micro_mp3::MP3_OUTPUT_BUFFER_TOO_SMALL);
            CHECK(r >= 0 || r == micro_mp3::MP3_DECODE_ERROR);  // recoverable at worst
            off += consumed;
            total_samples += samples;
            if (consumed == 0 && samples == 0) {
                break;  // no forward progress: end of stream
            }
        }
        CHECK(total_samples > 0);
    }
    return true;
}

// A mid-stream change in sample rate, channel count, or MPEG version is surfaced
// as MP3_STREAM_INFO_CHANGED (recoverable, no PCM, accessors updated) instead of
// feeding new-format audio into a pipeline set up for the old format.
// Concatenating a stereo MPEG1 stream and a mono MPEG2.5 stream changes all three
// at the seam. Both streams carry their own Info header, so the boundary exercises
// two things: clearing the first stream's gapless trim state (which would
// otherwise silence the second), and re-detecting the second stream's own Info
// header so its leading frame is skipped and its delay/padding trimmed. The
// decoder reports the change once; a re-call decodes the new-format frames, and
// the post-change PCM count matches a standalone gapless decode of stream 2.
static bool test_stream_info_change() {
    std::vector<uint8_t> first = read_file("sine_stereo_44100.mp3");  // MPEG1 stereo 44100, Info
    std::vector<uint8_t> second = read_file("sine_mono_8000.mp3");    // MPEG2.5 mono 8000, Info
    CHECK(!first.empty());
    CHECK(!second.empty());
    std::vector<uint8_t> combined = first;
    combined.insert(combined.end(), second.begin(), second.end());

    // Standalone gapless decode of the second stream recovers its exact tone
    // length; the post-change PCM below must match it.
    DecodeOutput ref_second = reference_decode("sine_mono_8000.mp3");
    CHECK(!ref_second.errored);
    const size_t ref_second_samples = ref_second.pcm.size();  // mono: one sample per channel

    Mp3Decoder dec;
    std::vector<int16_t> buf(micro_mp3::MP3_MIN_OUTPUT_BUFFER_BYTES / sizeof(int16_t));
    uint8_t* out = reinterpret_cast<uint8_t*>(buf.data());
    const size_t out_size = buf.size() * sizeof(int16_t);

    bool saw_probe = false;
    size_t change_events = 0;
    uint32_t rate_before = 0;
    uint32_t rate_after = 0;
    uint8_t ch_before = 0;
    uint8_t ch_after = 0;
    Mp3Version ver_before = micro_mp3::MP3_MPEG1;
    Mp3Version ver_after = micro_mp3::MP3_MPEG1;
    size_t before_samples = 0;
    size_t after_samples = 0;

    size_t off = 0;
    const size_t max_calls = 64 * (combined.size() + 1024);
    for (size_t calls = 0; calls < max_calls; calls++) {
        size_t consumed = 0;
        size_t samples = 0;
        Mp3Result r = dec.decode(combined.data() + off, combined.size() - off, out, out_size,
                                 consumed, samples);

        if (r == micro_mp3::MP3_STREAM_INFO_READY) {
            saw_probe = true;
            rate_before = dec.get_sample_rate();
            ch_before = dec.get_channels();
            ver_before = dec.get_version();
            off += consumed;
            continue;
        }
        if (r == micro_mp3::MP3_STREAM_INFO_CHANGED) {
            // Recoverable: no PCM emitted, accessors now hold the new format.
            CHECK_EQ(samples, 0);
            change_events++;
            rate_after = dec.get_sample_rate();
            ch_after = dec.get_channels();
            ver_after = dec.get_version();
            off += consumed;  // 0 on the direct path, or bytes buffered this call
            continue;         // re-call decodes the new-format frame
        }
        if (r == micro_mp3::MP3_DECODE_ERROR) {
            off += consumed;
            continue;
        }
        CHECK(r >= 0);
        off += consumed;
        if (samples > 0) {
            if (change_events == 0) {
                before_samples += samples;
            } else {
                after_samples += samples;
            }
        }
        if (consumed == 0 && samples == 0) {
            break;  // offered all remaining input with no progress: end of stream
        }
    }

    CHECK(saw_probe);
    CHECK_EQ(change_events, 1u);  // exactly one transition, at the seam
    CHECK_EQ(ch_before, 2);
    CHECK_EQ(ch_after, 1);
    CHECK_EQ(rate_before, 44100u);
    CHECK_EQ(rate_after, 8000u);
    CHECK_EQ(ver_before, micro_mp3::MP3_MPEG1);
    CHECK_EQ(ver_after, micro_mp3::MP3_MPEG2_5);
    CHECK(before_samples > 0);  // audio decoded before the change
    CHECK(after_samples > 0);   // audio resumed after recovering from the change
    // The second stream's own Info header was re-detected: its leading frame was
    // skipped and its gapless trim applied, recovering exactly the standalone count.
    CHECK_EQ(after_samples, ref_second_samples);
    return true;
}

static bool test_corrupt_frame_recovery() {
    DecodeOutput ref = reference_decode("sine_stereo_44100.mp3");
    CHECK(!ref.errored);
    CHECK_EQ(ref.decode_errors, 0);
    CHECK(!ref.pcm.empty());

    std::vector<uint8_t> data = read_file("sine_stereo_44100.mp3");
    CHECK(!data.empty());

    // Walk frame boundaries (no ID3 in the fixtures, so the first frame is at 0).
    std::vector<size_t> starts;
    for (size_t pos = 0; pos + 4 <= data.size();) {
        size_t flen = frame_length_at(data.data() + pos, data.size() - pos);
        if (flen < 4) {
            break;
        }
        starts.push_back(pos);
        pos += flen;
    }
    CHECK(starts.size() > 10);

    // Corrupt the side information of a frame ~60% through the stream (the bytes
    // just after the 4-byte header) while leaving its header and the next
    // frame's header intact. A garbled side-info field makes the OpenCore
    // decoder reject the frame, so the wrapper reports a recoverable
    // MP3_DECODE_ERROR, skips it, and resyncs on the next frame.
    const size_t k = starts.size() * 6 / 10;
    CHECK(k + 1 < starts.size());
    for (size_t i = starts[k] + 4; i < starts[k] + 14; i++) {
        data[i] ^= 0xFF;
    }

    Mp3Decoder dec;
    DecodeOutput out = decode_stream(dec, data.data(), data.size());

    // The stream survives: no fatal error, header parsed, and the corruption was
    // reported as recoverable, not fatal.
    CHECK(!out.errored);
    CHECK(out.header_ready);
    CHECK(out.decode_errors >= 1);
    CHECK_EQ(out.sample_rate, ref.sample_rate);
    CHECK_EQ(out.channels, ref.channels);

    // Most of the audio still decodes, and the prefix well before the damage is
    // bit-identical to the clean decode.
    CHECK(out.pcm.size() >= ref.pcm.size() * 8 / 10);
    const size_t prefix = ref.pcm.size() / 4;
    for (size_t i = 0; i < prefix; i++) {
        CHECK_EQ(out.pcm[i], ref.pcm[i]);
    }
    return true;
}

// An 8 kbps MPEG2 stereo frame at 24 kHz is 24 bytes, but its header (4) + CRC
// (2) + side info (17) leave no room for getNbits()'s 4-byte prefetch, which
// reads 2 bytes past the frame. Sizing data to exactly the frame puts it at the
// end of its allocation, so the full-buffer (direct, zero-copy) decode trips
// ASan if the bounds guard in pvmp3_framedecoder() is ever removed. Reproducer:
// tests/fuzz/crashes/sideinfo-overread-decode_direct.bin.
static bool test_sideinfo_overread_guard() {
    // FF F2: sync, MPEG2, Layer III, protection bit clear (CRC present).
    // 14: bitrate_index 1 (8 kbps), samplerate_index 1 (24 kHz), no padding.
    // 00: channel mode stereo, 17-byte side info.
    std::vector<uint8_t> data = {0xFF, 0xF2, 0x14, 0x00};
    data.resize(24, 0x00);  // CRC, side info, one main-data byte
    CHECK_EQ(frame_length_at(data.data(), data.size()), 24);

    Mp3Decoder dec;
    DecodeOutput out = decode_stream(dec, data.data(), data.size());

    // The header probes fine; the frame is skipped as a recoverable error.
    CHECK(!out.errored);
    CHECK(out.header_ready);
    CHECK_EQ(out.sample_rate, 24000);
    CHECK_EQ(out.channels, 2);
    CHECK(out.decode_errors >= 1);
    CHECK(out.pcm.empty());
    return true;
}

static bool test_leading_garbage_resync() {
    DecodeOutput ref = reference_decode("sine_stereo_44100.mp3");
    CHECK(!ref.errored);

    std::vector<uint8_t> file = read_file("sine_stereo_44100.mp3");
    CHECK(!file.empty());

    // Each prefix is non-decodable junk the decoder must skip before resyncing
    // on the real first frame. The 0xFF run is the adversarial case: every byte
    // pair passes the 11-bit sync test but fails full header validation (the
    // path hardened against an out-of-bounds read found by ASan fuzzing).
    std::vector<std::vector<uint8_t>> prefixes = {
        std::vector<uint8_t>(30, 0x00),
        std::vector<uint8_t>(31, 0xFF),
        {0x00, 0x01, 0x02, 'j', 'u', 'n', 'k', 0xFF, 0x00, 0xFF, 0x10, 0x55, 0xAA, 0x7F},
    };

    for (const auto& prefix : prefixes) {
        std::vector<uint8_t> data = prefix;
        data.insert(data.end(), file.begin(), file.end());

        Mp3Decoder dec;
        DecodeOutput out = decode_stream(dec, data.data(), data.size());
        if (out.errored || !outputs_equal(ref, out)) {
            std::printf("    resync failed for prefix size %zu (errored=%d, %zu vs %zu samples)\n",
                        prefix.size(), out.errored ? 1 : 0, out.pcm.size(), ref.pcm.size());
            return false;
        }
    }
    return true;
}

static bool test_id3v2_skip() {
    DecodeOutput ref = reference_decode("sine_stereo_44100.mp3");
    CHECK(!ref.errored);

    std::vector<uint8_t> file = read_file("sine_stereo_44100.mp3");
    CHECK(!file.empty());

    // Build a syntactically valid ID3v2 tag of `body` bytes (zero-filled), with
    // an optional 10-byte footer. The size is a syncsafe integer (7 bits/byte),
    // so body > 127 exercises multi-byte decoding.
    auto make_id3 = [](size_t body, bool footer) {
        std::vector<uint8_t> tag;
        tag.push_back('I');
        tag.push_back('D');
        tag.push_back('3');
        tag.push_back(0x04);                    // major version
        tag.push_back(0x00);                    // revision
        tag.push_back(footer ? 0x10 : 0x00);    // flags (bit 4 = footer present)
        tag.push_back(static_cast<uint8_t>((body >> 21) & 0x7F));
        tag.push_back(static_cast<uint8_t>((body >> 14) & 0x7F));
        tag.push_back(static_cast<uint8_t>((body >> 7) & 0x7F));
        tag.push_back(static_cast<uint8_t>(body & 0x7F));
        tag.insert(tag.end(), body, 0x00);
        if (footer) {
            tag.insert(tag.end(), 10, 0x00);
        }
        return tag;
    };

    struct Case {
        size_t body;
        bool footer;
        size_t chunk;  // SIZE_MAX = whole buffer; small values straddle the tag header
    };
    const Case cases[] = {
        {200, false, SIZE_MAX},  // multi-byte syncsafe size, single shot
        {200, false, 7},         // 10-byte header split across 7-byte chunks
        {64, true, SIZE_MAX},    // footer accounting
        {300, false, 4},         // tag header and body straddle many tiny chunks
    };

    for (const Case& c : cases) {
        std::vector<uint8_t> tag = make_id3(c.body, c.footer);
        std::vector<uint8_t> data = tag;
        data.insert(data.end(), file.begin(), file.end());

        Mp3Decoder dec;
        DecodeOutput out = decode_stream(dec, data.data(), data.size(), c.chunk);
        if (out.errored || !outputs_equal(ref, out)) {
            std::printf("    id3 skip failed (body=%zu footer=%d chunk=%zu): "
                        "errored=%d, %zu vs %zu samples\n",
                        c.body, c.footer ? 1 : 0, c.chunk, out.errored ? 1 : 0, out.pcm.size(),
                        ref.pcm.size());
            return false;
        }
    }
    return true;
}

static bool test_gapless_trim() {
    // The leading Xing/Info header frame must never be emitted as audio, and the
    // encoder delay/padding must be trimmed. Because each fixture is an exact
    // whole-duration tone, a sample-accurate gapless decode lands on precisely
    // the original per-channel sample count.
    size_t xing_emitted = 0;
    for (const Fixture& fx : FIXTURES) {
        if (!fx.has_xing) {
            continue;
        }
        DecodeOutput out = reference_decode(fx.file);
        CHECK(!out.errored);
        const size_t emitted = out.pcm.size() / fx.channels;
        if (emitted != fx.orig_samples) {
            std::printf("    %s: emitted %zu per-channel samples, expected %zu\n", fx.file, emitted,
                        fx.orig_samples);
            return false;
        }
        if (fx.sample_rate == 44100 && fx.channels == 2) {
            xing_emitted = emitted;  // for the no-Xing contrast below
        }
    }
    CHECK(xing_emitted == 88200);

    // Same source audio without a Xing/Info header: no metadata to trim from, so
    // the decoder emits whole frames only (encoder delay, padding and the would-
    // be header frame are all retained), strictly more than the trimmed version.
    DecodeOutput noxing = reference_decode("sine_stereo_44100_noxing.mp3");
    CHECK(!noxing.errored);
    const size_t emitted = noxing.pcm.size() / 2;
    CHECK_EQ(emitted % 1152, 0);          // whole MPEG1 frames, no sample-level trim
    CHECK(emitted > xing_emitted);        // gapless genuinely removed samples
    return true;
}

// ============================================================================
// Runner
// ============================================================================

struct TestCase {
    const char* name;
    bool (*fn)();
};

static const TestCase TESTS[] = {
    {"stream_info_contract", test_stream_info_contract},
    {"chunked_invariance", test_chunked_invariance},
    {"decode_accuracy", test_decode_accuracy},
    {"reset_reuse", test_reset_reuse},
    {"error_contract", test_error_contract},
    {"per_stream_output_buffer", test_per_stream_output_buffer},
    {"stream_info_change", test_stream_info_change},
    {"corrupt_frame_recovery", test_corrupt_frame_recovery},
    {"sideinfo_overread_guard", test_sideinfo_overread_guard},
    {"leading_garbage_resync", test_leading_garbage_resync},
    {"id3v2_skip", test_id3v2_skip},
    {"gapless_trim", test_gapless_trim},
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <data_dir> [test_name]\n", argv[0]);
        return 2;
    }
    g_data_dir = argv[1];
    const char* filter = (argc > 2) ? argv[2] : nullptr;

    int ran = 0;
    int failed = 0;
    for (const TestCase& t : TESTS) {
        if (filter && std::strcmp(filter, t.name) != 0) {
            continue;
        }
        ran++;
        std::printf("[ RUN  ] %s\n", t.name);
        bool ok = t.fn();
        std::printf("[ %s ] %s\n", ok ? "PASS" : "FAIL", t.name);
        if (!ok) {
            failed++;
        }
    }
    if (ran == 0) {
        std::fprintf(stderr, "no test matches '%s'\n", filter);
        return 2;
    }
    std::printf("%d/%d tests passed\n", ran - failed, ran);
    return failed == 0 ? 0 : 1;
}
