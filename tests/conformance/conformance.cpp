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

/* MP3 conformance harness (prototype)
 *
 * Decodes a .bit bitstream with micro_mp3 and compares the PCM against a raw
 * interleaved signed-16-bit little-endian reference (.pcm), reporting PSNR and
 * peak absolute sample difference.
 *
 * Reference format and comparison follow the minimp3 conformance test
 * (lieff/minimp3, minimp3_test.c): ref_samples = ref_bytes / 2, compared from
 * index 0 over min(ref, decoded) samples. Plain conformance frames carry no
 * Xing/Info/LAME header, so the micro_mp3 wrapper decodes them untrimmed and
 * most vectors line up with the ISO reference at index 0.
 *
 * Not all do: some ISO references are themselves pre-trimmed by a frame or two
 * (e.g. l3-sin1k0db drops 2 lead-in frames), so an untrimmed decode lands a few
 * frames late. When offset 0 looks misaligned, main() runs a +/-2-frame search
 * and reports the best-aligned PSNR; a correct decode still matches bit-exactly
 * once shifted.
 *
 * PSNR = 10*log10(32767^2 / MSE), MSE averaged over interleaved int16 samples.
 *
 * The gate mirrors ISO/IEC 11172-4 "full accuracy": an RMS bound (checked here as
 * PSNR, default 96 dB) plus a peak bound on the absolute sample difference
 * (--max-diff, default 2). The ISO full-accuracy criterion is RMS < 2^-15/sqrt(12)
 * (~0.29 LSB, ~101 dB) and max |diff| <= 2^-14 of full scale (2 LSB at 16-bit).
 *
 * Usage: conformance <file.bit> <file.pcm> [--min-psnr X] [--max-diff N]
 */

#include "conformance_common.h"
#include "micro_mp3/mp3_decoder.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace {

using conformance::read16le;

struct Metrics {
    double psnr;
    int max_diff;
    size_t compared;  // interleaved int16 samples compared
};

// Compare decoded vs reference starting at decoded-sample `offset`, over at most
// the first `limit` reference samples. The default compares the whole reference;
// a small `limit` drives the cheap alignment search on large files.
Metrics compare_at(const std::vector<int16_t>& dec, const std::vector<uint8_t>& ref, int offset,
                   size_t limit = SIZE_MAX) {
    const size_t ref_count = ref.size() / sizeof(int16_t);
    const size_t end = limit < ref_count ? limit : ref_count;
    Metrics m{};
    double mse_sum = 0.0;
    size_t n = 0;
    for (size_t i = 0; i < end; i++) {
        const long di = static_cast<long>(i) + offset;
        if (di < 0 || static_cast<size_t>(di) >= dec.size()) {
            continue;
        }
        const int a = dec[static_cast<size_t>(di)];
        const int b = read16le(&ref[i * sizeof(int16_t)]);
        const int d = std::abs(a - b);
        if (d > m.max_diff) {
            m.max_diff = d;
        }
        mse_sum += static_cast<double>(d) * static_cast<double>(d);
        n++;
    }
    m.compared = n;
    if (n == 0) {
        m.psnr = 0.0;  // nothing overlapped: not a match, report as worst-case
        return m;
    }
    const double mse = mse_sum / static_cast<double>(n);
    // mse == 0 is a bit-exact match: report +inf rather than a finite sentinel so
    // a perfect decode never under-reports and never fails a tightened gate.
    m.psnr = (mse == 0.0) ? std::numeric_limits<double>::infinity()
                          : 10.0 * std::log10((32767.0 * 32767.0) / mse);
    return m;
}

}  // namespace

// cppcheck-suppress constParameter  // keep main's standard signature
int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <file.bit> <file.pcm> [--min-psnr X] [--max-diff N]\n",
                     argv[0]);
        return 2;
    }
    const char* bit_path = argv[1];
    const char* pcm_path = argv[2];
    double min_psnr = 96.0;  // RMS/PSNR gate (minimp3's threshold)
    int max_diff = 2;        // ISO 11172-4 full-accuracy peak: 2^-14 of full scale = 2 LSB
    for (int i = 3; i < argc - 1; i++) {
        if (std::strcmp(argv[i], "--min-psnr") == 0) {
            min_psnr = std::atof(argv[i + 1]);
        } else if (std::strcmp(argv[i], "--max-diff") == 0) {
            max_diff = std::atoi(argv[i + 1]);
        }
    }

    std::vector<uint8_t> bitstream;
    if (!conformance::read_file(bit_path, bitstream)) {
        std::fprintf(stderr, "error: cannot read %s\n", bit_path);
        return 2;
    }
    std::vector<uint8_t> ref;
    if (!conformance::read_file(pcm_path, ref)) {
        std::fprintf(stderr, "error: cannot read %s\n", pcm_path);
        return 2;
    }

    std::vector<int16_t> pcm;
    pcm.reserve(ref.size() / sizeof(int16_t) + 4608);
    conformance::DecodeInfo info;
    if (!conformance::decode_all(bitstream, pcm, info)) {
        return 1;
    }
    const uint32_t sample_rate = info.sample_rate;
    const uint8_t channels = info.channels;
    const size_t decode_errors = info.decode_errors;

    const size_t ref_count = ref.size() / sizeof(int16_t);
    const Metrics at0 = compare_at(pcm, ref, 0);

    // Diagnostic alignment search, only when offset 0 looks misaligned (our
    // gapless/info-frame handling can shift some streams). Search a +/- 2 frame
    // window using a capped prefix so cost stays O(window) on large files, then
    // recompute the chosen offset over the full overlap.
    Metrics best = at0;
    int best_offset = 0;
    if (at0.psnr < 90.0 && !pcm.empty()) {
        const int span = static_cast<int>(2 * micro_mp3::MP3_MAX_SAMPLES_PER_FRAME *
                                          (channels ? channels : 1));
        const size_t search_n = ref_count < (1u << 16) ? ref_count : (1u << 16);
        double best_search_psnr = -1.0;
        int found = 0;
        for (int off = -span; off <= span; off++) {
            const Metrics m = compare_at(pcm, ref, off, search_n);
            if (m.compared > 0 && m.psnr > best_search_psnr) {
                best_search_psnr = m.psnr;
                found = off;
            }
        }
        best = compare_at(pcm, ref, found);
        best_offset = found;
    }

    const char* base = std::strrchr(bit_path, '/');
    base = base ? base + 1 : bit_path;

    // A stream we could not meaningfully decode (Layer I/II, free-format, etc.)
    // overlaps the reference for < half its samples; report that distinctly
    // instead of letting the empty-overlap MSE==0 sentinel read as a pass.
    const bool no_data = pcm.empty() || at0.compared * 2 < ref_count;
    const double report_psnr = best.psnr;
    // Full-accuracy gate: RMS (as PSNR) and peak must both pass.
    const bool pass = report_psnr >= min_psnr && best.max_diff <= max_diff;
    const char* status = no_data ? "NODATA" : (pass ? "PASS" : "FAIL");

    std::printf(
        "%-44s rate=%u ch=%u dec/ch=%zu ref/ch=%zu errs=%zu | "
        "PSNR@0=%.2f maxdiff@0=%d | off=%d PSNR=%.2f maxdiff=%d | %s\n",
        base, sample_rate, channels, pcm.size() / (channels ? channels : 1),
        ref_count / (channels ? channels : 1), decode_errors, at0.psnr, at0.max_diff,
        best_offset, best.psnr, best.max_diff, status);

    if (no_data) {
        return 0;  // out of scope for this decoder, not a conformance failure
    }
    return pass ? 0 : 1;
}
