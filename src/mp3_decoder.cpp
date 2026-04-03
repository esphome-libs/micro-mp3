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

/* MP3 Streaming Decoder Wrapper
 * Implementation of Mp3Decoder class
 */

#include "micro_mp3/mp3_decoder.h"

#include "pvmp3_framedecoder.h"
#include "pvmp3decoder_api.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#else
#include <cstdlib>
#endif

#include <algorithm>
#include <cstring>

namespace micro_mp3 {

// ============================================================================
// ESP32 memory allocation helpers
// ============================================================================

namespace {

void* mp3_malloc(size_t size) {
#ifdef ESP_PLATFORM
#if defined(MP3_DECODER_PREFER_PSRAM)
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#elif defined(MP3_DECODER_PREFER_INTERNAL)
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#elif defined(MP3_DECODER_PSRAM_ONLY)
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#elif defined(MP3_DECODER_INTERNAL_ONLY)
    return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    // Default: prefer PSRAM with fallback to internal RAM
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
#else
    return malloc(size);
#endif
}

void mp3_free(void* ptr) {
#ifdef ESP_PLATFORM
    heap_caps_free(ptr);
#else
    free(ptr);
#endif
}

}  // namespace

// ============================================================================
// Lifecycle
// ============================================================================

Mp3Decoder::Mp3Decoder() {
    // Lazy allocation: all resources allocated on first decode() call
}

Mp3Decoder::~Mp3Decoder() {
    this->reset();
}

// ============================================================================
// Core API
// ============================================================================

Mp3Result Mp3Decoder::decode(const uint8_t* input, size_t input_len, uint8_t* output,
                             size_t output_size, size_t& bytes_consumed, size_t& samples_decoded) {
    bytes_consumed = 0;
    samples_decoded = 0;

    if (!input || !output) {
        return MP3_INPUT_INVALID;
    }

    // Lazy init on first decode call
    if (!this->initialized_) {
        Mp3Result init_result = this->initialize();
        if (init_result != MP3_OK) {
            return init_result;
        }
    }

    // ID3v2 tag skipping: skip non-audio metadata before it reaches the decoder

    // Continue an in-progress ID3v2 tag skip
    if (this->id3_skip_remaining_ > 0) {
        size_t skip = std::min(input_len, this->id3_skip_remaining_);
        bytes_consumed = skip;
        this->id3_skip_remaining_ -= skip;
        return MP3_NEED_MORE_DATA;
    }

    // Detect a new ID3v2 tag at the current input position (direct path only).
    // When partial data is already buffered, the normal decode paths handle it.
    if (this->input_buffer_fill_ == 0) {
        // Quick check for "ID3" signature before committing to full parse
        // NOLINTNEXTLINE(readability-magic-numbers)
        if (input_len >= 3 && input[0] == 0x49 && input[1] == 0x44 && input[2] == 0x33) {
            // NOLINTNEXTLINE(readability-magic-numbers)
            if (input_len < 10) {
                // Not enough bytes to parse the full ID3v2 header -- buffer and wait
                std::memcpy(this->input_buffer_, input, input_len);
                this->input_buffer_fill_ = input_len;
                bytes_consumed = input_len;
                return MP3_NEED_MORE_DATA;
            }
            size_t tag_size = Mp3Decoder::parse_id3v2_tag_size(input, input_len);
            if (tag_size > 0) {
                size_t skip = std::min(input_len, tag_size);
                bytes_consumed = skip;
                this->id3_skip_remaining_ = tag_size - skip;
                return MP3_NEED_MORE_DATA;
            }
        }
        // NOLINTNEXTLINE(readability-magic-numbers)
    } else if (this->input_buffer_fill_ < 10 &&
               this->input_buffer_[0] == 0x49 &&  // NOLINT(readability-magic-numbers)
               (this->input_buffer_fill_ < 2 ||
                this->input_buffer_[1] == 0x44) &&  // NOLINT(readability-magic-numbers)
               (this->input_buffer_fill_ < 3 ||
                this->input_buffer_[2] == 0x33)) {  // NOLINT(readability-magic-numbers)
        // Partial ID3v2 header was buffered from a previous call -- accumulate
        // enough bytes to parse the 10-byte header.
        // NOLINTNEXTLINE(readability-magic-numbers)
        size_t need = 10 - this->input_buffer_fill_;
        size_t to_copy = std::min(need, input_len);
        std::memcpy(this->input_buffer_ + this->input_buffer_fill_, input, to_copy);
        this->input_buffer_fill_ += to_copy;

        // NOLINTNEXTLINE(readability-magic-numbers)
        if (this->input_buffer_fill_ < 10) {
            // Still not enough -- keep waiting
            bytes_consumed = to_copy;
            return MP3_NEED_MORE_DATA;
        }

        size_t tag_size =
            Mp3Decoder::parse_id3v2_tag_size(this->input_buffer_, this->input_buffer_fill_);
        if (tag_size > 0) {
            // The 10-byte header spans previous + current input.
            // Remaining tag bytes = total - header bytes already consumed.
            // NOLINTNEXTLINE(readability-magic-numbers)
            size_t remaining_tag = tag_size - 10;
            this->input_buffer_fill_ = 0;

            // Skip as much of the remaining tag body from current input as possible
            size_t remaining_input = input_len - to_copy;
            size_t extra_skip = std::min(remaining_input, remaining_tag);
            bytes_consumed = to_copy + extra_skip;
            this->id3_skip_remaining_ = remaining_tag - extra_skip;
            return MP3_NEED_MORE_DATA;
        }
        // Not a valid ID3v2 tag -- the to_copy bytes are already in the internal
        // buffer; consume them and let the next call handle via decode_buffered()
        bytes_consumed = to_copy;
        return MP3_NEED_MORE_DATA;
    }

    // Probe: parse the first MP3 frame header to determine stream properties
    // (sample rate, channel count, bitrate) without decoding audio.
    if (!this->probe_done_) {
        return this->run_probe(input, input_len, bytes_consumed);
    }

    if (input_len == 0 && this->input_buffer_fill_ == 0) {
        return MP3_OK;
    }

    // Check output buffer size
    if (output_size < MP3_MIN_OUTPUT_BUFFER_BYTES) {
        return MP3_OUTPUT_BUFFER_TOO_SMALL;
    }

    // Set up the decoder external struct for this frame
    tPVMP3DecoderExternal ext;
    this->setup_decode_ext(ext, output);

    bool frame_decoded = false;
    Mp3Result result = MP3_OK;

    if (this->input_buffer_fill_ == 0) {
        result = this->decode_direct(ext, input, input_len, bytes_consumed, frame_decoded);
    } else {
        result = this->decode_buffered(ext, input, input_len, bytes_consumed, frame_decoded);
    }

    if (result != MP3_OK || !frame_decoded) {
        return result;
    }

    this->finalize_decode(ext, samples_decoded);
    return MP3_OK;
}

// ============================================================================
// Accessors
// ============================================================================

size_t Mp3Decoder::get_samples_per_frame() const {
    if (!this->probe_done_) {
        return 0;
    }
    // NOLINTNEXTLINE(readability-magic-numbers)
    return (this->version_ == MP3_MPEG1) ? 1152 : 576;  // NOLINT(readability-magic-numbers)
}

// ============================================================================
// Mutators
// ============================================================================

void Mp3Decoder::reset() {
    if (this->decoder_memory_) {
        mp3_free(this->decoder_memory_);
        this->decoder_memory_ = nullptr;
    }
    if (this->input_buffer_) {
        mp3_free(this->input_buffer_);
        this->input_buffer_ = nullptr;
    }

    this->input_buffer_fill_ = 0;
    this->expected_frame_length_ = 0;
    this->id3_skip_remaining_ = 0;
    this->sample_rate_ = 0;
    this->bitrate_ = 0;
    this->output_channels_ = 0;
    this->version_ = MP3_MPEG1;
    this->initialized_ = false;
    this->probe_done_ = false;
    this->equalizer_ = MP3_EQ_FLAT;
}

// ============================================================================
// Internal Helpers
// ============================================================================

Mp3Result Mp3Decoder::initialize() {
    uint32_t mem_req = pvmp3_decoderMemRequirements();
    this->decoder_memory_ = mp3_malloc(mem_req);
    if (!this->decoder_memory_) {
        return MP3_ALLOCATION_FAILED;
    }

    this->input_buffer_ = static_cast<uint8_t*>(mp3_malloc(MP3_INPUT_BUFFER_SIZE));
    if (!this->input_buffer_) {
        this->reset();
        return MP3_ALLOCATION_FAILED;
    }

    // Set up the external configuration struct for initialization
    tPVMP3DecoderExternal ext;
    std::memset(&ext, 0, sizeof(ext));

    ext.pInputBuffer = this->input_buffer_;
    ext.inputBufferMaxLength = static_cast<int32>(MP3_INPUT_BUFFER_SIZE);
    ext.inputBufferCurrentLength = 0;
    ext.inputBufferUsedLength = 0;
    ext.equalizerType = flat;
    ext.crcEnabled = 0;
    ext.pOutputBuffer = nullptr;  // Will be set per decode call
    ext.outputFrameSize = 0;

    pvmp3_InitDecoder(&ext, this->decoder_memory_);

    this->input_buffer_fill_ = 0;
    this->id3_skip_remaining_ = 0;
    this->initialized_ = true;
    this->probe_done_ = false;
    this->sample_rate_ = 0;
    this->bitrate_ = 0;
    this->output_channels_ = 0;
    this->version_ = MP3_MPEG1;

    return MP3_OK;
}

Mp3Result Mp3Decoder::run_probe(const uint8_t* input, size_t input_len, size_t& bytes_consumed) {
    // Parse the first MP3 frame header to determine stream properties.
    // No decoding needed -- all info is in the 4-byte header.
    const uint8_t* header_data = nullptr;
    size_t header_data_len = 0;

    if (this->input_buffer_fill_ > 0) {
        // Accumulate enough bytes in the internal buffer to parse the header
        if (this->input_buffer_fill_ < 4) {
            size_t need = 4 - this->input_buffer_fill_;
            size_t to_copy = std::min(need, input_len);
            std::memcpy(this->input_buffer_ + this->input_buffer_fill_, input, to_copy);
            this->input_buffer_fill_ += to_copy;
            bytes_consumed = to_copy;

            if (this->input_buffer_fill_ < 4) {
                return MP3_NEED_MORE_DATA;
            }
        }
        header_data = this->input_buffer_;
        header_data_len = this->input_buffer_fill_;
    } else {
        header_data = input;
        header_data_len = input_len;
    }

    Mp3FrameInfo info = Mp3Decoder::parse_mp3_frame_header(header_data, header_data_len);

    if (info.frame_length == 0) {
        // Not enough data for header -- buffer what we have
        if (this->input_buffer_fill_ == 0) {
            std::memcpy(this->input_buffer_, input, input_len);
            this->input_buffer_fill_ = input_len;
            bytes_consumed = input_len;
        }
        return MP3_NEED_MORE_DATA;
    }

    if (info.frame_length < 0) {
        // Invalid header at current position -- skip one byte to resync
        if (this->input_buffer_fill_ > 0) {
            // Shift buffered data past the invalid byte
            size_t remaining = this->input_buffer_fill_ - 1;
            if (remaining > 0) {
                std::memmove(this->input_buffer_, this->input_buffer_ + 1, remaining);
            }
            this->input_buffer_fill_ = remaining;
        } else {
            bytes_consumed = 1;
        }
        return MP3_NEED_MORE_DATA;
    }

    // Valid header -- extract stream properties
    this->sample_rate_ = info.sample_rate;
    this->output_channels_ = info.channels;
    this->bitrate_ = info.bitrate_kbps;
    this->version_ = info.version;

    size_t frame_size = static_cast<size_t>(info.frame_length);

    // Buffer up to one full frame so the next decode() call can decode from
    // the internal buffer without the caller needing to re-provide the data.
    if (this->input_buffer_fill_ == 0) {
        // No prior buffered data -- copy up to one frame from input
        size_t to_copy = std::min(input_len, frame_size);
        std::memcpy(this->input_buffer_, input, to_copy);
        this->input_buffer_fill_ = to_copy;
        bytes_consumed = to_copy;
    } else {
        // Header bytes were already buffered; append from new input to complete the frame
        size_t already_consumed = bytes_consumed;  // bytes copied for header accumulation
        size_t needed = frame_size - this->input_buffer_fill_;
        size_t remaining_input = input_len - already_consumed;
        size_t to_copy = std::min(needed, remaining_input);
        if (to_copy > 0) {
            std::memcpy(this->input_buffer_ + this->input_buffer_fill_, input + already_consumed,
                        to_copy);
            this->input_buffer_fill_ += to_copy;
        }
        bytes_consumed = already_consumed + to_copy;
    }

    this->expected_frame_length_ = frame_size;

    this->probe_done_ = true;
    return MP3_STREAM_INFO_READY;
}

Mp3FrameInfo Mp3Decoder::parse_mp3_frame_header(const uint8_t* data, size_t len) {
    Mp3FrameInfo info = {0, 0, 0, 0, MP3_MPEG1};

    // Need at least 4 bytes to extract the frame header
    if (len < 4) {
        return info;  // frame_length == 0 means need more data
    }

    // MP3 frame header bit layout:
    //   AAAAAAAA AAABBCCD EEEEFFGH IIJJKLMM
    //   A = 11-bit sync (all 1s)
    //   B = version (00=MPEG2.5, 01=reserved, 10=MPEG2, 11=MPEG1)
    //   C = layer (01=III, 10=II, 11=I)
    //   D = protection (0=CRC, 1=no CRC)
    //   E = bitrate index (4 bits)
    //   F = sample rate index (2 bits)
    //   G = padding (1 bit)
    //   H = private (1 bit)
    //   I = channel mode (00=stereo, 01=joint stereo, 10=dual channel, 11=mono)

    // Check 11-bit sync word: first 8 bits must be 0xFF, next 3 bits must be 0b111
    // NOLINTNEXTLINE(readability-magic-numbers)
    if (data[0] != 0xFF || (data[1] & 0xE0) != 0xE0) {
        info.frame_length = -1;
        return info;
    }

    // Extract version (bits 4-3 of byte 1)
    // NOLINTNEXTLINE(readability-magic-numbers)
    uint8_t version_bits = (data[1] >> 3) & 0x03;

    // 01 is reserved
    if (version_bits == 0x01) {
        info.frame_length = -1;
        return info;
    }

    // Extract layer (bits 2-1 of byte 1) -- must be Layer III (01)
    // NOLINTNEXTLINE(readability-magic-numbers)
    uint8_t layer_bits = (data[1] >> 1) & 0x03;
    if (layer_bits != 0x01) {
        info.frame_length = -1;
        return info;  // Not Layer III
    }

    // Extract bitrate index (bits 7-4 of byte 2)
    // NOLINTNEXTLINE(readability-magic-numbers)
    uint8_t bitrate_index = (data[2] >> 4) & 0x0F;
    // NOLINTNEXTLINE(readability-magic-numbers)
    if (bitrate_index == 0 || bitrate_index == 0x0F) {
        info.frame_length = -1;
        return info;  // Free bitrate or bad index
    }

    // Extract sample rate index (bits 3-2 of byte 2)
    // NOLINTNEXTLINE(readability-magic-numbers)
    uint8_t samplerate_index = (data[2] >> 2) & 0x03;
    if (samplerate_index == 0x03) {
        info.frame_length = -1;
        return info;  // Reserved
    }

    // Extract padding (bit 1 of byte 2)
    // NOLINTNEXTLINE(readability-magic-numbers)
    uint8_t padding = (data[2] >> 1) & 0x01;

    // Extract channel mode (bits 7-6 of byte 3): 11 = mono, everything else = 2 channels
    // NOLINTNEXTLINE(readability-magic-numbers)
    uint8_t channel_mode = (data[3] >> 6) & 0x03;
    info.channels = (channel_mode == 0x03) ? 1 : 2;

    // Bitrate lookup tables (kbps)
    // NOLINTNEXTLINE(readability-magic-numbers)
    static const uint16_t K_BITRATES_MPEG1[15] = {0,   32,  40,  48,  56,  64,  80, 96,
                                                  112, 128, 160, 192, 224, 256, 320};
    // NOLINTNEXTLINE(readability-magic-numbers)
    static const uint16_t K_BITRATES_MPEG2[15] = {0,  8,  16, 24,  32,  40,  48, 56,
                                                  64, 80, 96, 112, 128, 144, 160};

    // Sample rate lookup tables (Hz)
    // NOLINTNEXTLINE(readability-magic-numbers)
    static const uint16_t K_SAMPLE_RATES_MPEG1[3] = {44100, 48000, 32000};
    // NOLINTNEXTLINE(readability-magic-numbers)
    static const uint16_t K_SAMPLE_RATES_MPEG2[3] = {22050, 24000, 16000};
    // NOLINTNEXTLINE(readability-magic-numbers)
    static const uint16_t K_SAMPLE_RATES_MPEG25[3] = {11025, 12000, 8000};

    // NOLINTNEXTLINE(readability-magic-numbers)
    if (version_bits == 0x03) {
        // MPEG1
        info.bitrate_kbps = K_BITRATES_MPEG1[bitrate_index];
        info.sample_rate = K_SAMPLE_RATES_MPEG1[samplerate_index];
        info.version = MP3_MPEG1;
    } else if (version_bits == 0x02) {
        // MPEG2
        info.bitrate_kbps = K_BITRATES_MPEG2[bitrate_index];
        info.sample_rate = K_SAMPLE_RATES_MPEG2[samplerate_index];
        info.version = MP3_MPEG2;
    } else {
        // MPEG2.5 (version_bits == 0x00)
        info.bitrate_kbps = K_BITRATES_MPEG2[bitrate_index];
        info.sample_rate = K_SAMPLE_RATES_MPEG25[samplerate_index];
        info.version = MP3_MPEG2_5;
    }

    if (info.bitrate_kbps == 0 || info.sample_rate == 0) {
        info.frame_length = -1;
        return info;
    }

    // Frame size calculation for Layer III:
    //   MPEG1:     144 * bitrate / sample_rate + padding
    //   MPEG2/2.5:  72 * bitrate / sample_rate + padding
    // NOLINTNEXTLINE(readability-magic-numbers)
    int32_t frame_length = 0;
    if (info.version == MP3_MPEG1) {
        // NOLINTNEXTLINE(readability-magic-numbers)
        frame_length = static_cast<int32_t>((144 * info.bitrate_kbps * 1000) / info.sample_rate +
                                            padding);  // NOLINT(readability-magic-numbers)
    } else {
        // NOLINTNEXTLINE(readability-magic-numbers)
        frame_length = static_cast<int32_t>((72 * info.bitrate_kbps * 1000) / info.sample_rate +
                                            padding);  // NOLINT(readability-magic-numbers)
    }

    // Sanity: minimum frame is 4 bytes (header), maximum is bounded by buffer size
    if (frame_length < 4 || static_cast<size_t>(frame_length) > MP3_INPUT_BUFFER_SIZE) {
        info.frame_length = -1;
        return info;
    }

    info.frame_length = frame_length;
    return info;
}

size_t Mp3Decoder::parse_id3v2_tag_size(const uint8_t* data, size_t len) {
    // ID3v2 header is 10 bytes:
    //   Bytes 0-2: "ID3" signature
    //   Byte 3:    Major version (e.g., 3 for ID3v2.3, 4 for ID3v2.4)
    //   Byte 4:    Revision
    //   Byte 5:    Flags (bit 4 = footer present)
    //   Bytes 6-9: Size as syncsafe integer (each byte uses bits 0-6 only)
    // NOLINTNEXTLINE(readability-magic-numbers)
    if (len < 10) {
        return 0;
    }

    // Check "ID3" signature
    // NOLINTNEXTLINE(readability-magic-numbers)
    if (data[0] != 0x49 || data[1] != 0x44 || data[2] != 0x33) {
        return 0;
    }

    // Major version and revision must not be 0xFF
    // NOLINTNEXTLINE(readability-magic-numbers)
    if (data[3] == 0xFF || data[4] == 0xFF) {
        return 0;
    }

    // Syncsafe integer: high bit of each size byte must be 0
    // NOLINTNEXTLINE(readability-magic-numbers)
    if ((data[6] | data[7] | data[8] | data[9]) & 0x80) {
        return 0;
    }

    // Decode syncsafe size (4 x 7-bit values)
    // NOLINTNEXTLINE(readability-magic-numbers)
    size_t body_size = (static_cast<size_t>(data[6]) << 21) | (static_cast<size_t>(data[7]) << 14) |
                       (static_cast<size_t>(data[8]) << 7) |
                       static_cast<size_t>(data[9]);  // NOLINT(readability-magic-numbers)

    // Total = 10 (header) + body + optional 10 (footer)
    // NOLINTNEXTLINE(readability-magic-numbers)
    size_t total = 10 + body_size;
    // NOLINTNEXTLINE(readability-magic-numbers)
    if (data[5] & 0x10) {
        total += 10;  // Footer present  // NOLINT(readability-magic-numbers)
    }

    return total;
}

void Mp3Decoder::setup_decode_ext(tPVMP3DecoderExternal& ext, uint8_t* output) {
    std::memset(&ext, 0, sizeof(ext));

    ext.inputBufferUsedLength = 0;
    ext.equalizerType = static_cast<e_equalization>(this->equalizer_);
    ext.crcEnabled = 0;
    ext.pOutputBuffer = reinterpret_cast<int16*>(output);

    // Set max output capacity in int16 samples
    // NOLINTNEXTLINE(readability-magic-numbers)
    ext.outputFrameSize = static_cast<int32>(MP3_MIN_OUTPUT_BUFFER_BYTES / sizeof(int16_t));
}

Mp3Result Mp3Decoder::decode_direct(tPVMP3DecoderExternal& ext, const uint8_t* input,
                                    size_t input_len, size_t& bytes_consumed, bool& frame_decoded) {
    frame_decoded = false;

    int32_t mp3_len = Mp3Decoder::parse_mp3_frame_header(input, input_len).frame_length;

    if (mp3_len == 0) {
        // Fewer than 4 bytes available -- can't parse header yet.
        // Buffer what we have and wait for more data.
        std::memcpy(this->input_buffer_, input, input_len);
        this->input_buffer_fill_ = input_len;
        bytes_consumed = input_len;
        return MP3_NEED_MORE_DATA;
    }

    if (mp3_len > 0) {
        size_t frame_size = static_cast<size_t>(mp3_len);

        if (input_len >= frame_size) {
            // Complete frame available -- zero-copy decode.
            // Bound OpenCore's reads to exactly this frame, preventing
            // it from reading past the frame into unrelated data.
            // const_cast is safe: OpenCore only reads from pInputBuffer
            ext.pInputBuffer = const_cast<uint8*>(input);
            ext.inputBufferCurrentLength = static_cast<int32>(frame_size);
            ext.inputBufferMaxLength = static_cast<int32>(frame_size);

            ERROR_CODE status = pvmp3_framedecoder(&ext, this->decoder_memory_);

            if (status == NO_DECODING_ERROR) {
                bytes_consumed = static_cast<size_t>(ext.inputBufferUsedLength);
                frame_decoded = true;
                return MP3_OK;
            }
            if (status == NO_ENOUGH_MAIN_DATA_ERROR) {
                // OpenCore thinks the frame needs more data than we provided.
                // Buffer what we have and let the caller feed more.
                std::memcpy(this->input_buffer_, input, input_len);
                this->input_buffer_fill_ = input_len;
                this->expected_frame_length_ = 0;
                bytes_consumed = input_len;
                return MP3_NEED_MORE_DATA;
            }
            // Genuine decode error -- skip the entire bad frame so the caller
            // advances to the next potential MP3 header
            bytes_consumed = frame_size;
            return MP3_DECODE_ERROR;
        }
        // Incomplete frame -- copy available data to internal buffer
        std::memcpy(this->input_buffer_, input, input_len);
        this->input_buffer_fill_ = input_len;
        this->expected_frame_length_ = frame_size;
        bytes_consumed = input_len;
        return MP3_NEED_MORE_DATA;
    }

    // mp3_len == -1: No valid MP3 sync at position 0.
    // Try to feed the data to OpenCore and let it handle sync scanning.
    // const_cast is safe: OpenCore only reads from pInputBuffer
    ext.pInputBuffer = const_cast<uint8*>(input);
    ext.inputBufferCurrentLength = static_cast<int32>(input_len);
    ext.inputBufferMaxLength = static_cast<int32>(input_len);

    ERROR_CODE status = pvmp3_framedecoder(&ext, this->decoder_memory_);

    if (status == NO_DECODING_ERROR) {
        bytes_consumed = static_cast<size_t>(ext.inputBufferUsedLength);
        frame_decoded = true;
        return MP3_OK;
    }
    if (status == NO_ENOUGH_MAIN_DATA_ERROR ||
        ext.inputBufferUsedLength >= static_cast<int32>(input_len)) {
        // Not enough data -- buffer for next call
        size_t to_copy = std::min(input_len, MP3_INPUT_BUFFER_SIZE);
        std::memcpy(this->input_buffer_, input, to_copy);
        this->input_buffer_fill_ = to_copy;
        bytes_consumed = to_copy;
        return MP3_NEED_MORE_DATA;
    }
    // Skip at least 1 byte to guarantee forward progress
    bytes_consumed =
        (ext.inputBufferUsedLength > 0) ? static_cast<size_t>(ext.inputBufferUsedLength) : 1;
    return MP3_DECODE_ERROR;
}

Mp3Result Mp3Decoder::decode_buffered(tPVMP3DecoderExternal& ext, const uint8_t* input,
                                      size_t input_len, size_t& bytes_consumed,
                                      bool& frame_decoded) {
    frame_decoded = false;

    // Try to determine frame length if we don't know it yet
    if (this->expected_frame_length_ == 0) {
        int32_t mp3_len =
            Mp3Decoder::parse_mp3_frame_header(this->input_buffer_, this->input_buffer_fill_)
                .frame_length;

        if (mp3_len > 0) {
            this->expected_frame_length_ = static_cast<size_t>(mp3_len);
        } else if (mp3_len == 0) {
            // Still not enough header bytes -- copy a small amount and return
            size_t space_available = MP3_INPUT_BUFFER_SIZE - this->input_buffer_fill_;
            size_t to_copy = std::min(
                input_len,
                std::min(space_available, static_cast<size_t>(4) - this->input_buffer_fill_));
            if (to_copy > 0) {
                std::memcpy(this->input_buffer_ + this->input_buffer_fill_, input, to_copy);
                this->input_buffer_fill_ += to_copy;
                bytes_consumed = to_copy;
            }
            return MP3_NEED_MORE_DATA;
        }
        // mp3_len == -1: no valid MP3 sync -- fall through to fill-and-decode below
    }

    if (this->expected_frame_length_ > 0) {
        if (this->input_buffer_fill_ < this->expected_frame_length_) {
            // We know the frame size -- copy only what's needed
            size_t needed = this->expected_frame_length_ - this->input_buffer_fill_;
            size_t space_available = MP3_INPUT_BUFFER_SIZE - this->input_buffer_fill_;
            size_t to_copy = std::min({needed, input_len, space_available});
            if (to_copy > 0) {
                std::memcpy(this->input_buffer_ + this->input_buffer_fill_, input, to_copy);
                this->input_buffer_fill_ += to_copy;
                bytes_consumed = to_copy;
            }

            if (this->input_buffer_fill_ < this->expected_frame_length_) {
                // Still incomplete -- wait for more data
                return MP3_NEED_MORE_DATA;
            }
        }
        // Frame is complete in the internal buffer -- fall through to decode
    } else {
        // No valid MP3 header -- fill buffer and let OpenCore handle it
        size_t space_available = MP3_INPUT_BUFFER_SIZE - this->input_buffer_fill_;
        size_t to_copy = std::min(input_len, space_available);
        if (to_copy > 0) {
            std::memcpy(this->input_buffer_ + this->input_buffer_fill_, input, to_copy);
            this->input_buffer_fill_ += to_copy;
            bytes_consumed = to_copy;
        }
    }

    ext.pInputBuffer = this->input_buffer_;
    ext.inputBufferCurrentLength = static_cast<int32>(this->input_buffer_fill_);
    ext.inputBufferMaxLength = static_cast<int32>(MP3_INPUT_BUFFER_SIZE);

    ERROR_CODE status = pvmp3_framedecoder(&ext, this->decoder_memory_);

    if (status == NO_DECODING_ERROR) {
        this->expected_frame_length_ = 0;

        // Compact the internal buffer past consumed bytes
        if (ext.inputBufferUsedLength > 0 &&
            ext.inputBufferUsedLength <= static_cast<int32>(this->input_buffer_fill_)) {
            size_t used = static_cast<size_t>(ext.inputBufferUsedLength);
            size_t remaining = this->input_buffer_fill_ - used;
            if (remaining > 0) {
                memmove(this->input_buffer_, this->input_buffer_ + used, remaining);
            }
            this->input_buffer_fill_ = remaining;
        }

        frame_decoded = true;
        return MP3_OK;
    }
    if (status == NO_ENOUGH_MAIN_DATA_ERROR) {
        // OpenCore thinks it needs more data -- keep the buffer intact and
        // let the caller provide more input.
        this->expected_frame_length_ = 0;
        return MP3_NEED_MORE_DATA;
    }

    this->expected_frame_length_ = 0;

    // Flush remaining buffered data to avoid retrying corrupt bytes
    this->input_buffer_fill_ = 0;
    return MP3_DECODE_ERROR;
}

void Mp3Decoder::finalize_decode(tPVMP3DecoderExternal& ext, size_t& samples_decoded) {
    // outputFrameSize is the total number of int16 samples written (both channels for stereo).
    // Convert to per-channel sample count.
    size_t total_samples = static_cast<size_t>(ext.outputFrameSize);
    uint8_t num_channels = static_cast<uint8_t>(ext.num_channels);

    // Update stream properties
    if (ext.samplingRate > 0) {
        this->sample_rate_ = static_cast<uint32_t>(ext.samplingRate);
    }

    if (num_channels > 0) {
        this->output_channels_ = num_channels;
    }

    if (ext.bitRate > 0) {
        this->bitrate_ = static_cast<uint32_t>(ext.bitRate);
    }

    this->version_ = static_cast<Mp3Version>(ext.version);

    // Per-channel sample count
    if (num_channels > 0 && total_samples > 0) {
        samples_decoded = total_samples / num_channels;
    } else {
        samples_decoded = total_samples;
    }
}

}  // namespace micro_mp3
