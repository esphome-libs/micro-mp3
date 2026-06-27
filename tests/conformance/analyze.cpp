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

/* Error-fingerprint analyzer for the MP3 conformance prototype.
 *
 * Decodes a .bit, compares against a raw S16LE .pcm reference at offset 0, and
 * reports where the error lives: per channel, a |diff| histogram, error vs
 * signal amplitude (the requantization signature -- error growing with sample
 * magnitude points at the |is|^(4/3) path), a per-frame RMS trace (systematic
 * vs sporadic), and the error power spectrum vs the signal spectrum (broadband
 * vs band-localized points at the filterbank).
 */

#include "conformance_common.h"
#include "micro_mp3/mp3_decoder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using conformance::read16le;

// M_PI is a POSIX extension, not ISO C++, so it can be absent under strict
// standard modes. Derive pi portably instead.
const double kPi = std::acos(-1.0);

// In-place iterative radix-2 FFT (re/im interleaved-as-separate arrays).
void fft(std::vector<double>& re, std::vector<double>& im) {
    const size_t n = re.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const double wr = std::cos(ang), wi = std::sin(ang);
        for (size_t i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            for (size_t k = 0; k < len / 2; k++) {
                const double ur = re[i + k], ui = im[i + k];
                const double vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                const double vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ur + vr;
                im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr;
                im[i + k + len / 2] = ui - vi;
                const double ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

// Average power spectrum of one channel (deinterleaved) via 50%-overlap Hann FFT.
void power_spectrum(const std::vector<double>& sig, std::vector<double>& psd) {
    const size_t N = 1024;
    psd.assign(N / 2, 0.0);
    if (sig.size() < N) return;
    std::vector<double> w(N);
    for (size_t i = 0; i < N; i++) w[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (N - 1));
    size_t blocks = 0;
    for (size_t off = 0; off + N <= sig.size(); off += N / 2) {
        std::vector<double> re(N), im(N, 0.0);
        for (size_t i = 0; i < N; i++) re[i] = sig[off + i] * w[i];
        fft(re, im);
        for (size_t k = 0; k < N / 2; k++) psd[k] += re[k] * re[k] + im[k] * im[k];
        blocks++;
    }
    if (blocks) for (auto& v : psd) v /= static_cast<double>(blocks);
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <file.bit> <file.pcm>\n", argv[0]);
        return 2;
    }
    std::vector<uint8_t> bits, ref;
    if (!conformance::read_file(argv[1], bits) || !conformance::read_file(argv[2], ref)) {
        std::fprintf(stderr, "error: cannot read inputs\n");
        return 2;
    }
    std::vector<int16_t> pcm;
    conformance::DecodeInfo info;
    if (!conformance::decode_all(bits, pcm, info)) return 1;
    const uint32_t rate = info.sample_rate;
    const uint8_t ch = info.channels ? info.channels : 1;
    const size_t spf = info.samples_per_frame;

    const size_t ref_n = ref.size() / sizeof(int16_t);
    const size_t n = std::min(ref_n, pcm.size());  // interleaved, offset 0
    std::printf("rate=%u ch=%u samples/ch/frame=%zu  dec/ch=%zu ref/ch=%zu  compared/ch=%zu\n\n",
                rate, ch, spf, pcm.size() / ch, ref_n / ch, n / ch);

    // ---- per channel: hist, RMS, amplitude buckets, spectra ----
    const int AMP_BUCKETS = 8;  // by |ref| in powers: 0, 1-3, ... up to full scale
    for (uint8_t c = 0; c < ch; c++) {
        std::vector<int> hist(8, 0);  // |diff| 0..6, last = >=7
        double sumsq = 0.0;
        int maxd = 0;
        std::vector<double> amp_err(AMP_BUCKETS, 0.0);
        std::vector<size_t> amp_cnt(AMP_BUCKETS, 0);
        std::vector<double> err_sig, ref_sig;
        err_sig.reserve(n / ch);
        ref_sig.reserve(n / ch);
        for (size_t i = c; i < n; i += ch) {
            const int a = pcm[i];
            const int b = read16le(&ref[i * 2]);
            const int d = std::abs(a - b);
            hist[std::min(d, 7)]++;
            sumsq += double(d) * double(d);
            maxd = std::max(maxd, d);
            // amplitude bucket: log2(|ref|) clamped
            int mag = std::abs(b);
            int bkt = 0;
            while (mag > 0 && bkt < AMP_BUCKETS - 1) {
                mag >>= 2;
                bkt++;
            }
            amp_err[bkt] += d;
            amp_cnt[bkt]++;
            err_sig.push_back(double(a - b));
            ref_sig.push_back(double(b));
        }
        const size_t cn = n / ch;
        const double rms = std::sqrt(sumsq / double(cn ? cn : 1));
        const double psnr = (sumsq == 0) ? 99.0 : 10.0 * std::log10(32767.0 * 32767.0 * cn / sumsq);
        std::printf("== channel %u ==  RMS=%.4f LSB  max_diff=%d  PSNR=%.2f dB\n", c, rms, maxd, psnr);
        std::printf("  |diff| histogram: ");
        const char* lbl[8] = {"0", "1", "2", "3", "4", "5", "6", ">=7"};
        for (int k = 0; k < 8; k++)
            std::printf("%s:%.2f%% ", lbl[k], 100.0 * hist[k] / double(cn ? cn : 1));
        std::printf("\n  mean|diff| by |ref| bucket (4^k):  ");
        for (int k = 0; k < AMP_BUCKETS; k++) {
            if (amp_cnt[k])
                std::printf("[<=4^%d n=%zu]=%.3f  ", k, amp_cnt[k], amp_err[k] / double(amp_cnt[k]));
        }
        std::printf("\n");

        // spectra: error PSD vs ref PSD, aggregated to 16 bands, as relative dB
        std::vector<double> epsd, rpsd;
        power_spectrum(err_sig, epsd);
        power_spectrum(ref_sig, rpsd);
        const int BANDS = 16;
        std::printf("  band  freq(Hz)   refPSD(dB)  errPSD(dB)  err-ref(dB)\n");
        const size_t binsPerBand = (epsd.size()) / BANDS;
        double rmax = 1e-30, emax = 1e-30;
        std::vector<double> rb(BANDS, 0), eb(BANDS, 0);
        for (int band = 0; band < BANDS; band++) {
            for (size_t k = band * binsPerBand; k < (band + 1) * binsPerBand && k < epsd.size(); k++) {
                rb[band] += rpsd[k];
                eb[band] += epsd[k];
            }
            rmax = std::max(rmax, rb[band]);
            emax = std::max(emax, eb[band]);
        }
        for (int band = 0; band < BANDS; band++) {
            const double fz = (band + 0.5) / BANDS * (rate / 2.0);
            std::printf("  %4d  %7.0f   %9.1f  %9.1f  %9.1f\n", band, fz,
                        10 * std::log10(rb[band] / rmax + 1e-30),
                        10 * std::log10(eb[band] / emax + 1e-30),
                        10 * std::log10(eb[band] / rb[band] + 1e-30));
        }
        std::printf("\n");
    }

    // ---- per-frame RMS trace (channel 0) ----
    if (spf > 0) {
        const size_t fr = spf;  // per-channel samples per frame
        std::vector<double> frame_psnr;
        for (size_t f0 = 0; (f0 + fr) * ch <= n; f0 += fr) {
            double ss = 0.0;
            for (size_t i = 0; i < fr; i++) {
                const size_t idx = (f0 + i) * ch;  // channel 0
                const int d = pcm[idx] - read16le(&ref[idx * 2]);
                ss += double(d) * double(d);
            }
            frame_psnr.push_back(ss == 0 ? 99.0 : 10.0 * std::log10(32767.0 * 32767.0 * fr / ss));
        }
        std::sort(frame_psnr.begin(), frame_psnr.end());
        const size_t fn = frame_psnr.size();
        if (fn) {
            std::printf("per-frame PSNR (ch0, %zu frames): min=%.1f  p10=%.1f  median=%.1f  p90=%.1f  max=%.1f\n",
                        fn, frame_psnr.front(), frame_psnr[fn / 10], frame_psnr[fn / 2],
                        frame_psnr[fn * 9 / 10], frame_psnr.back());
        }
    }
    return 0;
}
