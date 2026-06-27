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

/* Shared helpers for the MP3 conformance prototype tools (conformance, analyze):
 * file slurping, little-endian sample reads, and whole-bitstream decode. Kept in
 * one place so the two tools cannot drift apart.
 */

#pragma once

#include "micro_mp3/mp3_decoder.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

namespace conformance {

// Read an entire file into a byte buffer. Returns false if it cannot be opened
// or read.
inline bool read_file(const char* path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        return false;
    }
    const std::streamsize size = f.tellg();
    if (size < 0) {
        return false;  // tellg() failed; a negative size would resize() to ~SIZE_MAX
    }
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    return !(size > 0 && !f.read(reinterpret_cast<char*>(out.data()), size));
}

// Read a signed 16-bit little-endian sample.
inline int16_t read16le(const uint8_t* p) {
    return static_cast<int16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}

// Stream metadata gathered while decoding a whole bitstream.
struct DecodeInfo {
    uint32_t sample_rate = 0;
    uint8_t channels = 0;
    size_t samples_per_frame = 0;  // per channel
    size_t decode_errors = 0;      // recoverable MP3_DECODE_ERROR frames skipped
};

// Decode the whole bitstream into an interleaved int16 buffer, recovering from
// bad frames (advancing past them) the way a streaming caller would. Returns
// false on a fatal (non-recoverable) decoder error.
inline bool decode_all(const std::vector<uint8_t>& bitstream, std::vector<int16_t>& pcm,
                       DecodeInfo& info) {
    micro_mp3::Mp3Decoder decoder;
    std::vector<int16_t> scratch(micro_mp3::MP3_MIN_OUTPUT_BUFFER_BYTES / sizeof(int16_t));

    const uint8_t* ptr = bitstream.data();
    size_t remaining = bitstream.size();

    while (remaining > 0) {
        size_t consumed = 0;
        size_t samples = 0;
        const micro_mp3::Mp3Result result =
            decoder.decode(ptr, remaining, reinterpret_cast<uint8_t*>(scratch.data()),
                           scratch.size() * sizeof(int16_t), consumed, samples);

        // MP3_STREAM_INFO_READY (initial probe) and MP3_STREAM_INFO_CHANGED (a
        // mid-stream sample-rate / channel / version switch) are both reported
        // before the frame is decoded, emit no PCM, and just ask the caller to
        // re-read the format. Adopt it and retry the frame; the per-frame append
        // below then uses the updated channel count, so a mono<->stereo switch is
        // emitted in the stream's native layout -- matching ISO references like
        // l3-he_mode that change format mid-file.
        if (result == micro_mp3::MP3_STREAM_INFO_READY ||
            result == micro_mp3::MP3_STREAM_INFO_CHANGED) {
            info.sample_rate = decoder.get_sample_rate();
            info.channels = decoder.get_channels();
            ptr += consumed;
            remaining -= consumed;
            continue;  // no PCM on this call
        }

        if (result == micro_mp3::MP3_DECODE_ERROR) {
            info.decode_errors++;
            if (consumed == 0) {
                break;  // defensive: should not happen, avoids an infinite loop
            }
            ptr += consumed;
            remaining -= consumed;
            continue;
        }

        if (result < 0) {
            std::fprintf(stderr, "fatal decoder error %d\n", static_cast<int>(result));
            return false;
        }

        if (samples > 0) {
            const size_t n = samples * (info.channels ? info.channels : 1);
            pcm.insert(pcm.end(), scratch.data(), scratch.data() + n);
        }

        ptr += consumed;
        remaining -= consumed;

        if (consumed == 0 && samples == 0) {
            break;  // need more data / end of stream
        }
    }

    // Refresh in case channels/rate/frame size were only finalized late.
    if (info.sample_rate == 0) {
        info.sample_rate = decoder.get_sample_rate();
    }
    if (info.channels == 0) {
        info.channels = decoder.get_channels();
    }
    info.samples_per_frame = decoder.get_samples_per_frame();
    return true;
}

}  // namespace conformance
